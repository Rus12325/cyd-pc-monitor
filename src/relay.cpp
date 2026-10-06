#include "relay.h"
#include "config.h"
#include "settings.h"

static bool state[NUM_RELAYS]  = {false, false};
static bool locked[NUM_RELAYS] = {false, false}; // ручной режим для реле
static bool autoMode = AUTO_RELAY_DEFAULT;

static const int kPins[NUM_RELAYS] = {RELAY1_PIN, RELAY2_PIN};

static void apply(int r) {
    digitalWrite(kPins[r], state[r] ? HIGH : LOW);
}

void relayInit() {
    for (int i = 0; i < NUM_RELAYS; i++) {
        pinMode(kPins[i], OUTPUT);
        state[i] = false;
        apply(i);
    }
    // AUX-вход (GPIO35, input-only) — reserved для концевика/кнопки
    pinMode(AUX_IN_PIN, INPUT);
}

void relaySetManual(int r, bool on) {
    if (r < 0 || r >= NUM_RELAYS) return;
    locked[r] = true;          // блокируем авто для этого реле
    state[r] = on;
    apply(r);
}

void relayToggleManual(int r) {
    relaySetManual(r, !relayGetState(r));
}

bool relayGetState(int r) {
    if (r < 0 || r >= NUM_RELAYS) return false;
    return state[r];
}

void relaySetAuto(bool en) {
    autoMode = en;
    if (en) {
        // при включении AUTO снимаем блокировки
        for (int i = 0; i < NUM_RELAYS; i++) locked[i] = false;
    }
}

bool relayGetAuto() { return autoMode; }

void relayUpdate(const PCData& d, bool online) {
    if (!autoMode || !online) return;

    // Реле 1 — по температуре CPU, реле 2 — по температуре GPU
    const float* temps[NUM_RELAYS] = {&d.cpuTemp, &d.gpuTemp};

    for (int i = 0; i < NUM_RELAYS; i++) {
        if (locked[i]) continue;
        float t = *temps[i];
        if (isnan(t)) continue;    // нет данных — не дёргаем

        float onT = settingsRelayOn(i);
        float offT = settingsRelayOff(i);
        if (!state[i] && t >= onT)         { state[i] = true;  apply(i); }
        else if (state[i] && t <= offT)    { state[i] = false; apply(i); }
    }
}
