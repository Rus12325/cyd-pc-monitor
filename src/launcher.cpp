#include "launcher.h"
#include <Preferences.h>

static AppItem app[APP_MAX];

// Подписи по умолчанию — синхронизированы с DEFAULT_APPS в pc/engine.py,
// чтобы вкладка выглядела осмысленно ещё до первого подключения ПК.
static const char* kDefLabel[APP_MAX] = {
    "Explorer", "Terminal", "Notepad", "Calc",
    "TaskMgr",  "Paint",    "Browser", "Downloads",
};

static void appSet(int i, const char* label, bool save) {
    if (i < 0 || i >= APP_MAX) return;
    strncpy(app[i].label, label, APP_LABEL_L - 1);
    app[i].label[APP_LABEL_L - 1] = 0;
    if (save) {
        Preferences p;
        p.begin("launch", false);
        char k[8];
        snprintf(k, sizeof(k), "l%d", i);
        p.putString(k, app[i].label);
        p.end();
    }
}

void appInit() {
    Preferences p;
    p.begin("launch", true);
    bool have = p.getBool("init", false);
    p.end();
    if (!have) {
        for (int i = 0; i < APP_MAX; i++) appSet(i, kDefLabel[i], true);
        Preferences w;
        w.begin("launch", false);
        w.putBool("init", true);
        w.end();
        return;
    }
    for (int i = 0; i < APP_MAX; i++) {
        Preferences r;
        r.begin("launch", true);
        char k[8];
        snprintf(k, sizeof(k), "l%d", i);
        String l = r.getString(k, "");
        r.end();
        appSet(i, l.length() ? l.c_str() : kDefLabel[i], false);
    }
}

bool appApplyLine(const String& body) {
    int eq = body.indexOf('=');
    if (eq < 0) return false;
    int idx = body.substring(0, eq).toInt();
    if (idx < 0 || idx >= APP_MAX) return false;
    String label = body.substring(eq + 1);
    label.trim();
    label.replace("|", " ");
    if (label.length() == 0) return false;

    if (strcmp(app[idx].label, label.c_str()) == 0) return false;
    appSet(idx, label.c_str(), true);
    Serial.printf("[app] %d = %s\n", idx, label.c_str());
    return true;
}

const AppItem& appGet(int i) {
    if (i < 0 || i >= APP_MAX) i = 0;
    return app[i];
}
