// ============================================================
//  Тревоги: RGB-индикатор + зуммер (неблокирующие паттерны)
//  CYD 2USB: RGB 4/16/17 (активный LOW), зуммер 26 через PAM8002A
//  Зуммер ведётся PWM-тоном (ledc). Совместимо с Arduino-ESP32
//  ядрами 2.x (ledcSetup/ledcAttachPin) и 3.x (ledcAttach по пину).
// ============================================================
#include "alarm.h"
#include "config.h"
#include "settings.h"
#include <Preferences.h>

// --- Настройки паттернов (мс). Меняйте под себя. ---
static const uint32_t kBuzzFreqHz = 2730;       // тон зуммера

// --- Совместимость LEDC: ядро 2.x vs 3.x ---
#if ESP_ARDUINO_VERSION_MAJOR >= 3
// Ядро 3.x: ledcAttach(pin, freq) сам выбирает канал;
// ledcWrite(pin, duty) — по пину.
static void buzzInit() { ledcAttach(BUZZ_PIN, kBuzzFreqHz, 8); }
static void buzzDuty(uint32_t d) { ledcWrite(BUZZ_PIN, d); }
#else
// Ядро 2.x: фиксированный канал + ledcSetup/ledcAttachPin.
static const uint8_t kBuzzChan = 0;
static void buzzInit() {
    ledcSetup(kBuzzChan, kBuzzFreqHz, 8);
    ledcAttachPin(BUZZ_PIN, kBuzzChan);
}
static void buzzDuty(uint32_t d) { ledcWrite(kBuzzChan, d); }
#endif
static const uint8_t  kBuzzDuty   = 128;        // 50% от 255 (8 бит)
static const uint16_t kAlarmOnMs  = 250;        // ALARM: сигнал 250 мс
static const uint16_t kAlarmOffMs = 250;        //        пауза 250 мс
static const uint16_t kLedAlarmMs = 300;        // ALARM: период мигания красного
static const uint16_t kLedOffMs   = 800;        // OFFLINE: период мигания синего
// OFFLINE: последовательность зуммера «бип-бип...пауза» (один цикл)
static const uint16_t kOffBeep1   = 120;        // бип 1
static const uint16_t kOffPause1  = 120;        // пауза
static const uint16_t kOffBeep2   = 120;        // бип 2
static const uint32_t kOffPause2  = 10000;      // долгая пауза до след. цикла

enum Mode : int { M_NORMAL = 0, M_WARN, M_ALARM, M_OFFLINE };

static bool    soundEnabled = true;   // звук вкл (MUTE)
static int     curMode = -1;          // текущий режим
static bool    ledLit  = true;        // фаза светодиода
static uint32_t ledStepAt = 0;        // когда менять фазу LED
static bool    buzzLit = false;       // фаза зуммера
static uint32_t buzzStepAt = 0;       // когда менять фазу зуммера
static uint8_t buzzSeq = 0;           // шаг последовательности OFFLINE

// RGB активный LOW: true = светится -> пин LOW
static void rgbWrite(bool r, bool g, bool b) {
    digitalWrite(RGB_R_PIN, r ? LOW : HIGH);
    digitalWrite(RGB_G_PIN, g ? LOW : HIGH);
    digitalWrite(RGB_B_PIN, b ? LOW : HIGH);
}

static void buzzSet(bool on) {
    buzzLit = on;
    if (on && soundEnabled) buzzDuty(kBuzzDuty);
    else                    buzzDuty(0);
}

void alarmsInit() {
    pinMode(RGB_R_PIN, OUTPUT);
    pinMode(RGB_G_PIN, OUTPUT);
    pinMode(RGB_B_PIN, OUTPUT);
    rgbWrite(false, false, false);        // всё погашено при старте

    pinMode(BUZZ_PIN, OUTPUT);
    buzzInit();
    buzzDuty(0);

    // Звук вкл/выкл — храним в NVS (переживает перезагрузку).
    // Открываем на запись: namespace создаётся при первом старте,
    // чтобы не было шумной ошибки nvs_open NOT_FOUND в логе.
    Preferences p;
    p.begin("alarm", false);
    soundEnabled = p.getBool("sound", true);
    p.putBool("sound", soundEnabled);
    p.end();
}

bool alarmsGetEnabled() { return soundEnabled; }

void alarmsSetEnabled(bool en) {
    soundEnabled = en;
    if (!en) buzzSet(false);              // немедленно замолчать
    Preferences p;
    p.begin("alarm", false);
    p.putBool("sound", soundEnabled);
    p.end();
}

bool alarmsActive() { return curMode == M_ALARM || curMode == M_OFFLINE; }

void alarmsTick(const PCData& d, bool online) {
    uint32_t now = millis();

    // --- Определяем режим (NaN-safe: NaN >= порог = false) ---
    float mx = NAN;
    if (!isnan(d.cpuTemp)) mx = d.cpuTemp;
    if (!isnan(d.gpuTemp)) mx = isnan(mx) ? d.gpuTemp : max(mx, d.gpuTemp);

    float warnC = settingsWarnC();
    float critC = settingsCritC();

    Mode m = online ? M_NORMAL : M_OFFLINE;
    if (online && !isnan(mx)) {
        if (mx >= critC) m = M_ALARM;
        else if (mx >= warnC) m = M_WARN;
    }

    if (m != curMode) {                   // смена режима: сброс фаз
        curMode = m;
        ledLit = true;  ledStepAt = now;
        buzzSeq = 0;
        buzzSet(m == M_ALARM);            // ALARM начинает со звука
        buzzStepAt = now;
    }

    // --- LED: цвет по режиму; мигание для ALARM/OFFLINE ---
    bool r = false, g = false, b = false;
    uint16_t period = 0;                  // 0 = горит постоянно
    switch (curMode) {
        case M_NORMAL:  g = true; break;
        case M_WARN:    r = true; g = true; break;      // янтарный
        case M_ALARM:   r = true; period = kLedAlarmMs; break;
        case M_OFFLINE: b = true; period = kLedOffMs;   break;
        default: break;
    }
    if (period) {
        if (now - ledStepAt >= period) {
            ledStepAt = now;
            ledLit = !ledLit;
        }
        if (!ledLit) r = g = b = false;
    }
    rgbWrite(r, g, b);

    // --- Зуммер: паттерны по режиму ---
    if (!online && curMode == M_OFFLINE) {
        // последовательность: бип-пауза-бип-долгая пауза
        static const uint32_t waits[] = {kOffBeep1, kOffPause1, kOffBeep2, kOffPause2};
        uint32_t wait = waits[buzzSeq & 3];
        if (now - buzzStepAt >= wait) {
            buzzStepAt = now;
            buzzSeq = (buzzSeq + 1) & 3;
            buzzSet(buzzSeq < 3 && (buzzSeq & 1) == 0);  // шаги 0 и 2 = звук
        }
    } else if (curMode == M_ALARM) {
        if (now - buzzStepAt >= (buzzLit ? kAlarmOnMs : kAlarmOffMs)) {
            buzzStepAt = now;
            buzzSet(!buzzLit);
        }
    } else {
        if (buzzLit) buzzSet(false);      // NORMAL/WARN — тихо
    }
}
