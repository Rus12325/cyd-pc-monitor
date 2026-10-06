#pragma once
// ============================================================
// Управление реле: ручной режим + авто по порогам (гистерезис)
// Реле 1 (GPIO22) — CPU, реле 2 (GPIO27) — GPU
// ============================================================
#include "pc_data.h"
#include <Arduino.h>

void relayInit();

// Ручное переключение реле r (0..NUM_RELAYS-1) — блокирует авто для него
void relaySetManual(int r, bool on);
void relayToggleManual(int r);
bool relayGetState(int r);

// Авто-режим: вкл/выкл глобально
void relaySetAuto(bool en);
bool relayGetAuto();

// Вызывать периодически: гистерезис по температуре
// online=false => состояния не меняются (держим последние)
void relayUpdate(const PCData& d, bool online);
