#pragma once
// ============================================================
// Конфигурация: пины, пороги, протокол
// Плата: ESP32-2432S028 ("CYD"), вариант 2USB (micro-USB + USB-C)
//
// ВАЖНО: вариант 2USB распаян иначе, чем классическая R-версия!
//   - TFT: ST7789 (не ILI9341), SPI1/HSPI: MISO=12 MOSI=13 SCLK=14 CS=15 DC=2, BL=21
//   - Тач XPT2046 на ОТДЕЛЬНОЙ шине VSPI: CLK=25 MOSI=32 MISO=39 CS=33 IRQ=36
//   - Занято платой: SD картридер, RGB-светодиод (4/16/17), зуммер (26), LDR (34)
//   - RGB и зуммер ИСПОЛЬЗУЮТСЯ нами как индикация/тревоги (см. alarm.h)
//   - Свободные GPIO на разъёмах: 22, 27, 35 (35 — только вход)
// ============================================================
#include <Arduino.h>

// ---------- Пины реле (GPIO -> оптопара -> реле/SSR) ----------
// Единственные свободные GPIO платы; GPIO35 — input-only, но для выхода
// модуля реле (просто HIGH/LOW) не подходит => используем как вход/статус.
// Итог: 2 полноценных выхода реле + 1 вход (например, концевик дверцы).
#define RELAY1_PIN 22      // реле 1: вентиляторы корпуса        (JST, выход)
#define RELAY2_PIN 27      // реле 2: доп. охлаждение GPU        (P3,  выход)
#define AUX_IN_PIN 35      // AUX: вход (концевик/кнопка)        (P3,  input-only)

// На плате физически доступно только 2 свободных выхода -> NUM_RELAYS = 2.
// Третий "канал" AUX_IN_PIN можно завести как дискретный вход в будущем.
#define NUM_RELAYS 2

// ---------- RGB-индикатор (активный LOW: LOW = светится) и зуммер ----------
// RGB: 4=R, 16=G, 17=B (common anode, логика инвертирована)
// Зуммер: 26 — PAM8002A (класс-D) на 2-pin JST P4; тон = PWM ~2.7 кГц
#define RGB_R_PIN 4
#define RGB_G_PIN 16
#define RGB_B_PIN 17
#define BUZZ_PIN  26

// ---------- Тач XPT2046 (отдельная шина VSPI, не разделяется с TFT) ----------
#define TOUCH_CLK_PIN 25
#define TOUCH_MISO_PIN 39
#define TOUCH_MOSI_PIN 32
#define TOUCH_CS_PIN  33
#define TOUCH_IRQ_PIN 36   // можно оставить неподключённым, опрос по z-pressure

// ---------- Пороги авто-управления (гистерезис) ----------
// Реле 1 (R1 CASE): CPU — вкл при >= 60 C, выкл при <= 55 C
#define RELAY1_ON_TEMP  60.0f
#define RELAY1_OFF_TEMP 55.0f
// Реле 2 (R2 GPU): GPU — вкл при >= 65 C, выкл при <= 60 C
#define RELAY2_ON_TEMP  65.0f
#define RELAY2_OFF_TEMP 60.0f

#define AUTO_RELAY_DEFAULT true   // режим AUTO по умолчанию

// ---------- Протокол / тайминги ----------
#define SERIAL_BAUD       115200
#define DATA_TIMEOUT_MS   3000    // нет данных дольше -> OFFLINE
#define UI_REFRESH_MS     1000    // период обновления экрана (данные приходят 1/с)

// ---------- Цветовые зоны и тревоги ----------
#define TEMP_WARN_C  60.0f   // < 60 зелёный, до 75 жёлтый, дальше красный
#define TEMP_ALARM_C 75.0f

// ---------- Подписи реле для экрана ----------
static const char* const RELAY_NAMES[NUM_RELAYS] = {"R1 CASE", "R2 GPU"};
