#include "hotkey.h"
#include <Preferences.h>

static Hotkey hk[HK_MAX];

// Значения по умолчанию (пока ПК не прислал свои)
static const char* kDefLabel[HK_MAX] = {
    "TaskMgr", "Desktop", "Refresh", "Close",
    "Snap L",  "Snap R",  "PrScr",   "Terminal",
};
static const char* kDefCombo[HK_MAX] = {
    "ctrl+shift+esc", "win+d", "f5", "ctrl+w",
    "win+left", "win+right", "printscreen", "win+r",
};

static void hkSet(int i, const char* label, const char* combo, bool save) {
    if (i < 0 || i >= HK_MAX) return;
    strncpy(hk[i].label, label, HK_LABEL_L - 1); hk[i].label[HK_LABEL_L - 1] = 0;
    strncpy(hk[i].combo, combo, HK_COMBO_L - 1); hk[i].combo[HK_COMBO_L - 1] = 0;
    if (save) {
        Preferences p; p.begin("hotkey", false);
        char k[8];
        snprintf(k, sizeof(k), "l%d", i); p.putString(k, hk[i].label);
        snprintf(k, sizeof(k), "c%d", i); p.putString(k, hk[i].combo);
        p.end();
    }
}

void hkInit() {
    Preferences p; p.begin("hotkey", true);
    bool have = p.getBool("init", false);
    p.end();
    if (!have) {
        for (int i = 0; i < HK_MAX; i++) hkSet(i, kDefLabel[i], kDefCombo[i], true);
        Preferences w; w.begin("hotkey", false);
        w.putBool("init", true);
        w.end();
        return;
    }
    for (int i = 0; i < HK_MAX; i++) {
        Preferences r; r.begin("hotkey", true);
        char k[8];
        snprintf(k, sizeof(k), "l%d", i);
        String l = r.getString(k, "");
        snprintf(k, sizeof(k), "c%d", i);
        String c = r.getString(k, "");
        r.end();
        if (l.length() == 0 && c.length() == 0)
            hkSet(i, kDefLabel[i], kDefCombo[i], false);
        else
            hkSet(i, l.c_str(), c.c_str(), false);
    }
}

bool hkApplyLine(const String& body) {
    int eq = body.indexOf('=');
    int bar = body.indexOf('|');
    if (eq < 0 || bar < 0) return false;
    int idx = body.substring(0, eq).toInt();
    if (idx < 0 || idx >= HK_MAX) return false;
    String label = body.substring(eq + 1, bar);
    String combo = body.substring(bar + 1);
    label.trim(); combo.trim();
    label.replace("|", " ");

    // Без изменений — не трогаем NVS и экран
    if (strcmp(hk[idx].label, label.c_str()) == 0 &&
        strcmp(hk[idx].combo, combo.c_str()) == 0) {
        return false;
    }
    hkSet(idx, label.c_str(), combo.c_str(), true);
    Serial.printf("[hk] %d = %s | %s\n", idx, label.c_str(), combo.c_str());
    return true;
}

const Hotkey& hkGet(int i) {
    if (i < 0 || i >= HK_MAX) i = 0;
    return hk[i];
}
