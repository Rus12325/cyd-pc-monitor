#pragma once
// ============================================================
// Экран: дашборд, графики, управление, ядра, клавиши, медиа + тач
// CYD 2USB: ST7789 (TFT_eSPI) + XPT2046 на ОТДЕЛЬНОЙ шине SPI
// ============================================================
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "pc_data.h"

extern TFT_eSPI tft;
extern int uiPage;   // 0=дашборд 1=графики 2=управление 3=ядра 4=клавиши 5=медиа 6=запуск приложений

// Действия кнопок/табов
enum UIAction : int {
    UI_NONE = 0,
    UI_PAGE_DASH,
    UI_PAGE_GRAPHS,
    UI_PAGE_CTRL,
    UI_PAGE_CORES,
    UI_PAGE_KEYS,
    UI_PAGE_MEDIA,
    UI_PAGE_APPS,
    UI_PAGE_PROCS,
    UI_RELAY_1,
    UI_RELAY_2,
    UI_AUTO,
    UI_ALARM,
    UI_CALIBRATE,
    UI_CMD_SLEEP,
    UI_CMD_REBOOT,
    UI_CMD_SHUTDOWN,
    // Медиа-клавиши (страница MEDIA)
    UI_MEDIA_MUTE,
    UI_MEDIA_VOLDN,
    UI_MEDIA_VOLUP,
    UI_MEDIA_PREV,
    UI_MEDIA_PLAY,
    UI_MEDIA_NEXT,
    // Горячие клавиши (страница KEYS) — индекс = a - UI_HK_0
    UI_HK_0,
    UI_HK_1,
    UI_HK_2,
    UI_HK_3,
    UI_HK_4,
    UI_HK_5,
    UI_HK_6,
    UI_HK_7,
    // Редактор горячих клавиш НА БОРТУ (открывается удержанием кнопки KEYS)
    UI_EDIT_CANCEL,
    UI_EDIT_SAVE,
    UI_EDIT_PG_PREV,
    UI_EDIT_PG_NEXT,
    UI_EDIT_MOD_CTRL,
    UI_EDIT_MOD_ALT,
    UI_EDIT_MOD_SHIFT,
    UI_EDIT_MOD_WIN,
    UI_EDIT_CAT_0,     // A-Z
    UI_EDIT_CAT_1,     // 0-9
    UI_EDIT_CAT_2,     // F1..F12
    UI_EDIT_CAT_3,     // служебные
    UI_EDIT_KEY_0,     // сетка 7x3: индекс = a - UI_EDIT_KEY_0
    UI_EDIT_KEY_1,
    UI_EDIT_KEY_2,
    UI_EDIT_KEY_3,
    UI_EDIT_KEY_4,
    UI_EDIT_KEY_5,
    UI_EDIT_KEY_6,
    UI_EDIT_KEY_7,
    UI_EDIT_KEY_8,
    UI_EDIT_KEY_9,
    UI_EDIT_KEY_10,
    UI_EDIT_KEY_11,
    UI_EDIT_KEY_12,
    UI_EDIT_KEY_13,
    UI_EDIT_KEY_14,
    UI_EDIT_KEY_15,
    UI_EDIT_KEY_16,
    UI_EDIT_KEY_17,
    UI_EDIT_KEY_18,
    UI_EDIT_KEY_19,
    UI_EDIT_KEY_20,
    // Быстрый запуск приложений (страница APPS) — индекс = a - UI_APP_0
    UI_APP_0,
    UI_APP_1,
    UI_APP_2,
    UI_APP_3,
    UI_APP_4,
    UI_APP_5,
    UI_APP_6,
    UI_APP_7,
    // Вкладка PROCS: «завершить процесс» для строки 0..4
    UI_PROC_KILL_0,
    UI_PROC_KILL_1,
    UI_PROC_KILL_2,
    UI_PROC_KILL_3,
    UI_PROC_KILL_4,
};

// Обработка нажатия «завершить» на строке i вкладки PROCS.
// Первый тап — подтверждение (кнопка краснеет), второй в течение ~2.5 с —
// возвращает pid процесса (main отправит KILL:pid). Иначе -1.
long displayProcKillTap(int row);

void displayInit();
void displayCalLoad();            // загрузка калибровки тача из NVS (или запуск мастера)
void displayCalibrateAndSave();   // повторная калибровка + сохранение

// Полная отрисовка текущей страницы + динамики (вызывать в цикле)
void displayUpdate(const PCData& d, const History& h, bool online);

// Перерисовать статику (состояния кнопок) — после изменения реле/авто/клавиш
void displayRefreshStatic();

// Чтение тача: true, если было касание (координаты уже в системе экрана)
bool displayGetTouch(int16_t& x, int16_t& y);

// Хит-тест: вернуть действие по координатам тача
UIAction displayHit(int x, int y);

// Визуальный отклик на нажатие: яркая рамка вокруг кнопки на ~130 мс.
// Вызывать из обработчика тача сразу после того, как действие распознано.
void displayPressFlash(int x, int y);

// true, когда подсветка нажатия истекла (пора перерисовать страницу без рамки)
bool displayFlashExpired();

// ---- Редактор горячих клавиш на борту (страница KEYS) ----
// Открывается долгим удержанием кнопки KEYS (~0.7 с).
bool displayEditActive();
void displayEditOpen(int slot);              // начать редактирование слота
// Обработать тап внутри редактора. Возвращает номер слота (>=0), если
// пользователь нажал SAVE (настройка уже в NVS) — main отправляет HKCFG на ПК.
int  displayEditTap(UIAction a);
