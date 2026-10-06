#pragma once
// ============================================================
// Тревоги: RGB-индикатор (GPIO4/16/17, активный LOW) + зуммер (GPIO26, PWM)
// Режимы (авто-детект каждый тик):
//   0 NORMAL  — ПК онлайн, температуры в норме:  зелёный
//   1 WARN    — t >= TEMP_WARN_C:                  жёлтый (янтарный)
//   2 ALARM   — t >= TEMP_ALARM_C:                 красный мигает + сигнал
//   3 OFFLINE — нет данных от ПК:                  синий мигает + двойной сигнал
// Звук можно отключить кнопкой ALARM на странице CTRL (сохраняется в NVS).
// ============================================================
#include <Arduino.h>
#include "pc_data.h"

void alarmsInit();
// Вызывать каждый loop (неблокирующий, все тайминги по millis())
void alarmsTick(const PCData& d, bool online);

bool alarmsGetEnabled();          // звук вкл/выкл
void alarmsSetEnabled(bool en);   // переключение с экрана (сохраняется в NVS)
bool alarmsActive();              // true = режим тревоги (ALARM или OFFLINE)
