// ============================================================
//  PC Monitor for ESP32-2432S028 (CYD, 2USB variant)
//  Дашборд температур/нагрузки ПК + графики + частичное
//  управление (реле + команды ПК по USB-Serial)
// ============================================================
#include <Arduino.h>
#include "esp_task_wdt.h"
#include "config.h"
#include "pc_data.h"
#include "protocol.h"
#include "display.h"
#include "relay.h"
#include "alarm.h"
#include "hotkey.h"
#include "launcher.h"
#include "settings.h"

PCData pc;
History hist;
static uint32_t lastUi = 0;
static bool touched = false;
static uint32_t pressSince = 0;      // millis() начала текущего нажатия
static bool pressDidAction = false;  // нажатие попало в кнопку?
static const uint32_t CAL_PRESS_MS = 3000;  // удержание 3 с -> мастер калибровки тача
static int hkHoldIndex = -1;         // удержание кнопки KEYS (0..7) для редактора
static const uint32_t HK_HOLD_MS = 700;    // долгое удержание KEYS -> редактор на борту

// ---------- Приём данных от ПК (буферизованный построчно) ----------
static String rxBuf;

static void pollSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            String line = rxBuf;
            rxBuf = "";
            line.trim();
            if (line.length() == 0) continue;
            if (line.startsWith("HK:")) {
                // настройка горячей клавиши с ПК
                if (hkApplyLine(line.substring(3)))
                    displayRefreshStatic();
            } else if (line.startsWith("AP:")) {
                // настройка подписи вкладки APPS с ПК
                if (appApplyLine(line.substring(3)))
                    displayRefreshStatic();
            } else if (line.startsWith("THR:")) {
                // настройка порогов реле/тревог с ПК (кнопка «Пороги…»)
                if (settingsApplyLine(line.substring(4)))
                    displayRefreshStatic();
            } else if (line == "HKREQ") {
                // ПК при старте просит конфиг клавиш: экран — источник истины
                // (редактор на борту может изменить слоты, пока ПК был выключен)
                for (int i = 0; i < HK_MAX; i++) protocolSendHkCfg(i);
            } else if (line.startsWith("MNOW:")) {
                protocolParseMediaNow(line, pc);
            } else if (line.startsWith("TOP:")) {
                protocolParseTop(line, pc);
            } else {
                protocolParseLine(line, pc);
            }
        } else if (c != '\r') {
            rxBuf += c;
        }
    }
}

// ---------- Обработка тача ----------
// Возвращает true, если нажатие попало в кнопку/таб (совершено действие).
static bool handleTouch(int x, int y) {
    UIAction a = displayHit(x, y);
    if (a == UI_NONE) return false;

    // Визуальный отклик: яркая рамка на кнопках KEYS/APPS/MEDIA
    displayPressFlash(x, y);

    // Модальный редактор клавиш (страница KEYS, долгое удержание кнопки):
    // отрисовка и логика — в display.cpp, отсюда только уведомление ПК,
    // чтобы его таблица комбинаций не разошлась с настройкой на экране.
    if (a >= UI_EDIT_CANCEL && a <= UI_EDIT_KEY_20) {
        int saved = displayEditTap(a);
        if (saved >= 0) protocolSendHkCfg(saved);
        return true;
    }

    switch (a) {
        case UI_PAGE_DASH:
        case UI_PAGE_GRAPHS:
        case UI_PAGE_CTRL:
        case UI_PAGE_CORES:
        case UI_PAGE_KEYS:
        case UI_PAGE_MEDIA:
        case UI_PAGE_APPS:
        case UI_PAGE_PROCS:
            uiPage = (int)(a - UI_PAGE_DASH);
            displayRefreshStatic();
            break;
        // Медиа-клавиши (страница MEDIA) -> на ПК
        case UI_MEDIA_MUTE:  protocolSendMedia("mute");  break;
        case UI_MEDIA_VOLDN: protocolSendMedia("voldn"); break;
        case UI_MEDIA_VOLUP: protocolSendMedia("volup"); break;
        case UI_MEDIA_PREV:  protocolSendMedia("prev");  break;
        case UI_MEDIA_PLAY:  protocolSendMedia("play");  break;
        case UI_MEDIA_NEXT:  protocolSendMedia("next");  break;
        // Горячие клавиши (страница KEYS) -> на ПК
        case UI_HK_0: case UI_HK_1: case UI_HK_2: case UI_HK_3:
        case UI_HK_4: case UI_HK_5: case UI_HK_6: case UI_HK_7:
            protocolSendKey((int)(a - UI_HK_0));
            break;
        // Быстрый запуск приложений (страница APPS) -> на ПК
        case UI_APP_0: case UI_APP_1: case UI_APP_2: case UI_APP_3:
        case UI_APP_4: case UI_APP_5: case UI_APP_6: case UI_APP_7:
            protocolSendApp((int)(a - UI_APP_0));
            break;
        // Завершение процесса (страница PROCS): 1-й тап — подтверждение,
        // 2-й в течение 2.5 с — отправляем KILL:pid на ПК
        case UI_PROC_KILL_0: case UI_PROC_KILL_1: case UI_PROC_KILL_2:
        case UI_PROC_KILL_3: case UI_PROC_KILL_4: {
            long pid = displayProcKillTap((int)(a - UI_PROC_KILL_0));
            if (pid > 0) protocolSendKill(pid);
            break;
        }
        case UI_RELAY_1: case UI_RELAY_2:
            relayToggleManual(a - UI_RELAY_1);
            displayRefreshStatic();
            break;
        case UI_AUTO:
            relaySetAuto(!relayGetAuto());
            displayRefreshStatic();
            break;
        case UI_ALARM:
            alarmsSetEnabled(!alarmsGetEnabled());
            displayRefreshStatic();
            break;
        case UI_CALIBRATE:
            displayCalibrateAndSave();
            break;
        case UI_CMD_SLEEP:
            protocolSendCommand("sleep");
            break;
        case UI_CMD_REBOOT:
            protocolSendCommand("reboot");
            break;
        case UI_CMD_SHUTDOWN:
            protocolSendCommand("shutdown");
            break;
        default:
            break;
    }
    return true;
}

void setup() {
    Serial.begin(SERIAL_BAUD);

    // Аппаратный сторожевой таймер: если main loop зависнет (бесконечный цикл
    // в обработке тача и т.п.) — контроллер сам перезагрузится через 8 с.
    esp_task_wdt_init(8, true);   // panic: true -> полный ресет при срабатывании
    esp_task_wdt_add(NULL);       // наблюдаем за текущей (loop) задачей

    displayInit();
    displayCalLoad();     // калибровка тача (первый запуск — мастер на экране)
    settingsInit(); // пороги реле/тревог из NVS (страница CTRL)
    relayInit();
    alarmsInit();
    hkInit();     // горячие клавиши (NVS, страница KEYS)
    appInit();    // подписи быстрого запуска (NVS, страница APPS)

    Serial.println("PC-MONITOR ready");
}

void loop() {
    esp_task_wdt_reset();   // кормим сторожевой таймер
    pollSerial();

    bool online = pc.hasData && (millis() - pc.lastUpdate) < DATA_TIMEOUT_MS;

    // Тревоги (RGB + зуммер) — каждый цикл, неблокирующе
    alarmsTick(pc, online);

    if (millis() - lastUi >= UI_REFRESH_MS) {
        lastUi = millis();

        if (online) {
            hist.push(pc);
        } else {
            PCData empty;              // NaN — дыры в графике
            hist.push(empty);
        }

        relayUpdate(pc, online);
    }

    // Экран рисуем каждый цикл: внутренние guard'ы перерисовывают только
    // изменившиеся элементы, поэтому это дёшево и даёт мгновенный отклик
    // на тап (переключение страниц, реле, подсветка кнопки).
    displayUpdate(pc, hist, online);
    displayFlashExpired();   // подсветка нажатия истекла -> перерисовать страницу

    // Тач: действие — по нажатию; долгое удержание (3 с) в пустом месте —
    // самолечение: повторная калибровка тача (работает даже при «кривой»
    // сохранённой калибровке, т.к. отсчёт идёт по факту нажатия, а не по карте)
    int16_t x = 0, y = 0;
    if (displayGetTouch(x, y)) {
        if (!touched) {
            touched = true;
            pressSince = millis();
            pressDidAction = false;
            hkHoldIndex = -1;
            // На странице KEYS (вне редактора) короткий тап = выполнить клавишу,
            // долгое удержание (0.7 с) = редактор комбинации на борту. Поэтому
            // действие не шлём сразу — ждём отпускания или порога удержания.
            if (uiPage == 4 && !displayEditActive()) {
                UIAction a = displayHit(x, y);
                if (a >= UI_HK_0 && a <= UI_HK_7) {
                    hkHoldIndex = (int)(a - UI_HK_0);
                    displayPressFlash(x, y);   // отклик сразу по нажатию
                } else {
                    pressDidAction = handleTouch(x, y);
                }
            } else {
                pressDidAction = handleTouch(x, y);
            }
        } else if (hkHoldIndex >= 0 && !pressDidAction &&
                   (millis() - pressSince) >= HK_HOLD_MS) {
            // Порог удержания пройден — открываем редактор прямо под пальцем
            displayEditOpen(hkHoldIndex);
            hkHoldIndex = -1;
            pressDidAction = true;   // чтобы отпускание не запустило калибровку
        }
    } else {
        if (touched) {
            if (hkHoldIndex >= 0) {
                // Короткий тап (отпустили до порога) — выполняем клавишу
                protocolSendKey(hkHoldIndex);
                hkHoldIndex = -1;
            } else if (!pressDidAction &&
                       (millis() - pressSince) >= CAL_PRESS_MS) {
                displayCalibrateAndSave();
            }
            touched = false;
        }
    }
}
