#pragma once
// ============================================================
// Горячие клавиши (страница KEYS): до 8 кнопок.
// Каждая кнопка: короткая подпись (label) и комбинация клавиш
// (combo), например "Ctrl+Shift+Esc". Настройки хранятся во
// флеш-памяти (NVS) и обновляются с ПК строкой:
//   HK:<номер>=<label>|<combo>        (через USB-Serial)
// ============================================================
#include <Arduino.h>

#define HK_MAX     8
#define HK_LABEL_L 17
#define HK_COMBO_L 25

struct Hotkey {
    char label[HK_LABEL_L];
    char combo[HK_COMBO_L];
};

// Загрузить конфиг из NVS (или установить значения по умолчанию)
void hkInit();

// Разобрать строку "N=label|combo" и сохранить (NVS + RAM).
// Возвращает true, если что-то изменилось (нужна перерисовка).
bool hkApplyLine(const String& body);

// Получить кнопку по номеру (0..HK_MAX-1)
const Hotkey& hkGet(int i);
