#include "settings.h"
#include "config.h"
#include <Preferences.h>

// Текущие значения (RAM-копия). Стартуют с дефолтов config.h, затем
// перезаписываются из NVS, если ПК уже присылал свою конфигурацию.
static float gR1On  = RELAY1_ON_TEMP;
static float gR1Off = RELAY1_OFF_TEMP;
static float gR2On  = RELAY2_ON_TEMP;
static float gR2Off = RELAY2_OFF_TEMP;
static float gWarn  = TEMP_WARN_C;
static float gCrit  = TEMP_ALARM_C;

static float clampT(float v) {
    if (v < 20)  v = 20;
    if (v > 110) v = 110;
    return v;
}

// Проверка «гистерезисной» связки: у реле off должен быть < on
// (иначе при t между порогами реле начнёт дребезжать). У тревог
// warn должен быть < crit. Нарушения чиним молча и сообщаем, что
// значение было скорректировано (возвращаем true — надо перерисовать).
static bool fixPair() {
    bool fixed = false;
    if (gR1Off >= gR1On) { gR1Off = gR1On - 1; if (gR1Off < 20) gR1Off = 20; fixed = true; }
    if (gR2Off >= gR2On) { gR2Off = gR2On - 1; if (gR2Off < 20) gR2Off = 20; fixed = true; }
    if (gWarn  >= gCrit) { gWarn  = gCrit - 1; if (gWarn  < 20) gWarn  = 20; fixed = true; }
    return fixed;
}

void settingsInit() {
    Preferences p;
    p.begin("thr", true);
    gR1On  = p.getFloat("r1on",  RELAY1_ON_TEMP);
    gR1Off = p.getFloat("r1off", RELAY1_OFF_TEMP);
    gR2On  = p.getFloat("r2on",  RELAY2_ON_TEMP);
    gR2Off = p.getFloat("r2off", RELAY2_OFF_TEMP);
    gWarn  = p.getFloat("warn",  TEMP_WARN_C);
    gCrit  = p.getFloat("crit",  TEMP_ALARM_C);
    p.end();
    fixPair();
    Serial.printf("[thr] load: R1 %d/%d R2 %d/%d W%d A%d\n",
                  (int)gR1On, (int)gR1Off, (int)gR2On, (int)gR2Off,
                  (int)gWarn, (int)gCrit);
}

float settingsRelayOn(int r)  { return r == 0 ? gR1On  : gR2On; }
float settingsRelayOff(int r) { return r == 0 ? gR1Off : gR2Off; }
float settingsWarnC()         { return gWarn; }
float settingsCritC()         { return gCrit; }

bool settingsApplyLine(const String& body) {
    // body = "r1on=60,r1off=55,..."
    bool changed = false;
    int pos = 0;
    const int len = body.length();
    while (pos < len) {
        int eq = body.indexOf('=', pos);
        if (eq < 0) break;
        int comma = body.indexOf(',', eq);
        if (comma < 0) comma = len;
        String k = body.substring(pos, eq);
        String v = body.substring(eq + 1, comma);
        k.trim(); v.trim();
        if (k.length() && v.length()) {
            char c0 = v.charAt(0);
            if (isdigit((unsigned char)c0) || c0 == '-' || c0 == '+' || c0 == '.') {
                float val = clampT(v.toFloat());
                float* dst = NULL;
                if      (k == "r1on")  dst = &gR1On;
                else if (k == "r1off") dst = &gR1Off;
                else if (k == "r2on")  dst = &gR2On;
                else if (k == "r2off") dst = &gR2Off;
                else if (k == "warn")  dst = &gWarn;
                else if (k == "crit")  dst = &gCrit;
                if (dst && *dst != val) { *dst = val; changed = true; }
            }
        }
        pos = comma + 1;
    }
    if (fixPair()) changed = true;
    if (changed) {
        Preferences p;
        p.begin("thr", false);
        p.putFloat("r1on",  gR1On);
        p.putFloat("r1off", gR1Off);
        p.putFloat("r2on",  gR2On);
        p.putFloat("r2off", gR2Off);
        p.putFloat("warn",  gWarn);
        p.putFloat("crit",  gCrit);
        p.end();
        Serial.printf("[thr] set: R1 %d/%d R2 %d/%d W%d A%d\n",
                      (int)gR1On, (int)gR1Off, (int)gR2On, (int)gR2Off,
                      (int)gWarn, (int)gCrit);
    }
    return changed;
}
