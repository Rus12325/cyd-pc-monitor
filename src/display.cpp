// ============================================================
//  Экран: дашборд / графики / управление + тач
//  Плата: CYD 2USB (ST7789 320x240 landscape, rotation 1)
//  Тач: XPT2046 на ОТДЕЛЬНОЙ шине VSPI (CLK=25 MOSI=32 MISO=39 CS=33 IRQ=36)
// ============================================================
#include "display.h"
#include "config.h"
#include "relay.h"
#include "alarm.h"
#include "hotkey.h"
#include "launcher.h"
#include "settings.h"
#include <Preferences.h>
#include <string.h>

TFT_eSPI tft;
int uiPage = 0;   // 0=дашборд 1=графики 2=управление 3=ядра 4=клавиши 5=медиа 6=запуск 7=процессы
static const int kNumPages = 8;

// Геометрия табов (8 вкладок в ряд шириной 310)
static const int kTabW = 36;
static const int kTabGap = 3;

// Палитра кнопок/служебного текста (приглушённые тёмно-синие)
static const uint16_t COL_BTN    = 0x10C3;   // обычная кнопка (тёмно-синяя)
static const uint16_t COL_DIM    = 0x5AAB;   // приглушённый текст
static const uint16_t COL_LBLUE  = 0x7DDC;   // светло-голубой текст
static const uint16_t COL_ACCENT = 0x4D7C;   // акцент (линии/точки)
static const uint16_t COL_TRACK  = 0x2104;   // тёмный трек шкал

// Затемнение цвета для «погашенного» светодиода
static uint16_t dimC(uint16_t c, uint8_t div) {
    if (div < 1) div = 1;
    uint16_t r = (c >> 11) & 0x1F;
    uint16_t g = (c >> 5)  & 0x3F;
    uint16_t b = c & 0x1F;
    r = r / div; g = g / div; b = b / div;
    return (r << 11) | (g << 5) | b;
}

// Подсветка нажатой кнопки (KEYS/APPS/MEDIA): до какого millis() горит рамка
static uint32_t gFlashUntil = 0;
static int      gFlashPage  = -1;

// ---------- Тач (отдельная шина!) ----------
static SPIClass touchBus(VSPI);
static XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);

// Калибровка: сырые значения XPT2046 в двух опорных точках экрана
// calData[0..1] = raw в точке (20,20), [2..3] = raw в точке (300,220),
// [4] = флаг swap осей (авто-детект при калибровке)
static uint16_t calData[5] = {300, 300, 3800, 3800, 0};

static const int16_t kZThreshold = 400;   // минимальное "давление" касания

static int lastDrawnPage = -1;

// Анти-мерцание: сколько сэмплов уже нарисовано на GRAPH и состояние шапки.
// Динамика страниц рисуется ТОЛЬКО когда данные реально изменились, а не
// каждый UI_REFRESH_MS «вхолостую» (раньше страницы перерисовывались целиком
// 2 раза в секунду даже при неизменных значениях — экран заметно мигал).
static int  sLastGraphN = -1;   // число нарисованных сэмплов на GRAPH
static bool sLastOn = false;    // ONLINE-статус в шапке
static int  sLastUp = -1;       // последняя показанная секунда аптайма

// ---------------- Служебное ----------------

static uint16_t tempColor(float v) {
    if (isnan(v))  return TFT_DARKGREY;
    float w = settingsWarnC();
    float c = settingsCritC();
    if (v < w)  return TFT_GREEN;
    if (v < c)  return TFT_YELLOW;
    return TFT_RED;
}

static uint16_t loadColor(float v) {
    if (isnan(v))  return TFT_DARKGREY;
    if (v < 55.0f) return TFT_GREEN;
    if (v < 85.0f) return TFT_YELLOW;
    return TFT_RED;
}

static uint16_t voltColor(float v) {
    if (isnan(v))  return TFT_DARKGREY;
    if (v < 10.8f) return TFT_RED;
    if (v < 11.2f) return TFT_YELLOW;
    if (v > 12.8f) return TFT_YELLOW;
    return TFT_GREEN;
}

// Текст с затиранием фона (чтобы не оставались "хвосты" от старых значений)
static void tftText(int font, uint16_t color, int x, int y, const String& s, int w, int h) {
    tft.fillRect(x, y, w, h, TFT_BLACK);
    tft.setTextColor(color, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(font);
    tft.drawString(s, x, y);
}

static String f1(float v) { return isnan(v) ? String("--") : String(v, 1); }
static String f0(float v) { return isnan(v) ? String("--") : String((long)(v + 0.5f)); }

// ---------------- Шапка ----------------

static void drawStatusRow(const PCData& d, bool online) {
    tft.fillRect(5, 2, 310, 15, TFT_BLACK);
    tft.setTextFont(1);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("PC MONITOR", 6, 3);

    uint32_t up = d.uptimeSec;
    char ub[16];
    snprintf(ub, sizeof(ub), "UP %02lu:%02lu:%02lu", (unsigned long)(up / 3600),
             (unsigned long)((up / 60) % 60), (unsigned long)(up % 60));
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(ub, 90, 3);

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(online ? TFT_GREEN : TFT_RED, TFT_BLACK);
    tft.drawString(online ? "ONLINE" : "OFFLINE", 312, 3);
    tft.setTextDatum(TL_DATUM);

    tft.drawFastHLine(0, 19, 320, TFT_NAVY);
}

// ---------------- Табы (6 страниц) ----------------
static const char* kTabNames[kNumPages] = {"DASH", "GRAPH", "CTRL", "CORES", "KEYS", "MEDIA", "APPS", "PROCS"};

static void drawTabs() {
    for (int i = 0; i < kNumPages; i++) {
        int x = 5 + i * (kTabW + kTabGap);
        bool active = (i == uiPage);
        tft.fillRect(x, 217, kTabW, 22, active ? TFT_YELLOW : TFT_NAVY);
        tft.drawRect(x, 217, kTabW, 22, TFT_DARKGREY);
        tft.setTextFont(1);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(active ? TFT_BLACK : TFT_WHITE, active ? TFT_YELLOW : TFT_NAVY);
        tft.drawString(kTabNames[i], x + kTabW / 2, 227);
        tft.setTextDatum(TL_DATUM);
    }
}

// ---------------- Анти-мерцание: умные перерисовки ----------------

static bool feq(float a, float b) {
    if (isnan(a) && isnan(b)) return true;
    return a == b;
}

// Дашборд: перерисовывать, только если изменился хотя бы один видимый датчик
static bool dashNeedsRedraw(const PCData& d, bool online) {
    static float last[18];
    static bool have = false;
    static bool lastOn = false;
    const float cur[18] = {
        d.cpuTemp, d.cpuLoad, d.cpuFreqMHz, d.cpuPowerW,
        d.gpuTemp, d.gpuLoad, d.gpuFreqMHz, d.gpuPowerW,
        d.ramPct, d.ramTotalGB, d.fan1Rpm, d.fan2Rpm,
        d.sysTemp, d.v12Volt, d.diskPct, d.diskFreeGB,
        d.netDnMBps, d.netUpMBps,
    };
    if (!have || online != lastOn) {
        have = true;
        lastOn = online;
        memcpy(last, cur, sizeof(last));
        return true;
    }
    bool need = false;
    for (int i = 0; i < 18; i++)
        if (!feq(cur[i], last[i])) { need = true; break; }
    if (need) memcpy(last, cur, sizeof(last));
    return need;
}

// Графики: перерисовывать при новом сэмпле истории или изменении последнего
static bool graphNeedsRedraw(const History& h) {
    static float last[7];
    static bool have = false;
    const float cur[7] = {
        h.at(h.cpuTemp, 0), h.at(h.cpuLoad, 0), h.at(h.coreAvgTemp, 0),
        h.at(h.ramPct, 0), h.at(h.gpuTemp, 0), h.at(h.gpuLoad, 0),
        h.at(h.diskPct, 0),
    };
    if (!have || h.count != sLastGraphN) {
        have = true;
        sLastGraphN = h.count;
        memcpy(last, cur, sizeof(last));
        return h.count > 0;
    }
    bool need = false;
    for (int i = 0; i < 7; i++)
        if (!feq(cur[i], last[i])) { need = true; break; }
    if (need) memcpy(last, cur, sizeof(last));
    return need;
}

// Страница ядер: цифры/спарклайны рисуем при изменении данных ядер или
// появлении нового сэмпла истории (в офлайне — один раз, показать "--")
static bool coresNeedRedraw(const PCData& d, const History& h, bool online) {
    static float lastT[4], lastL[4];
    static int lastCount = -1, lastHist = -1;
    static bool lastOn = false;
    int n = d.coreCount;
    if (n > 4) n = 4;
    bool dataChanged = (n != lastCount);
    for (int i = 0; i < n; i++)
        if (!feq(d.coreTemp[i], lastT[i]) || !feq(d.coreLoad[i], lastL[i]))
            dataChanged = true;
    bool need;
    if (!online) {
        need = lastOn;                     // переход в офлайн: нарисовать "--"
    } else {
        need = dataChanged || (h.count != lastHist);
    }
    if (need) {
        lastOn = online;
        lastCount = n;
        lastHist = h.count;
        for (int i = 0; i < n; i++) { lastT[i] = d.coreTemp[i]; lastL[i] = d.coreLoad[i]; }
        for (int i = n; i < 4; i++) { lastT[i] = NAN; lastL[i] = NAN; }
    }
    return need;
}

// ---------------- ДАШБОРД ----------------

static void drawMiniBar(int x, int y, int w, int h, float pct) {
    tft.drawRect(x, y, w, h, TFT_DARKGREY);
    tft.fillRect(x + 1, y + 1, w - 2, h - 2, TFT_BLACK);
    int fw = (int)((w - 2) * clampf(pct, 0, 100) / 100.0f);
    if (fw > 0) tft.fillRect(x + 1, y + 1, fw, h - 2, loadColor(pct));
}

static void dashStatic() {
    tft.drawRect(5, 24, 154, 77, TFT_DARKGREY);   // CPU
    tft.drawRect(161, 24, 154, 77, TFT_DARKGREY); // GPU
    tft.drawRect(5, 105, 310, 44, TFT_DARKGREY);  // RAM
    tft.drawRect(5, 153, 150, 60, TFT_DARKGREY);  // вентиляторы
    tft.drawRect(161, 153, 154, 60, TFT_DARKGREY);// 12V + сеть

    tft.setTextFont(1);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.drawString("CPU", 10, 26);
    tft.drawString("GPU", 166, 26);
    tft.drawString("RAM", 10, 107);
    tft.drawString("FANS", 10, 155);
    // Правая нижняя ячейка БЕЗ статичной подписи: 12V или DISK — рисует dashDynamic
}

static void dashDynamic(const PCData& d) {
    // --- CPU / GPU: температура + частота + мощность + загрузка ---
    for (int b = 0; b < 2; b++) {
        int x = b ? 161 : 5;
        float temp    = b ? d.gpuTemp    : d.cpuTemp;
        float load    = b ? d.gpuLoad    : d.cpuLoad;
        float freq    = b ? d.gpuFreqMHz : d.cpuFreqMHz;
        float power   = b ? d.gpuPowerW  : d.cpuPowerW;

        if (isnan(temp)) {
            tftText(4, TFT_DARKGREY, x + 10, 36, "--", 140, 26);
        } else {
            tftText(7, tempColor(temp), x + 10, 30, f1(temp), 140, 48);
        }

        char line[32];
        float ghz = freq / 1000.0f;
        if (!isnan(freq) && !isnan(power))
            snprintf(line, sizeof(line), "%.2fGHz  %.0fW", ghz, power);
        else if (!isnan(freq))
            snprintf(line, sizeof(line), "%.2fGHz", ghz);
        else if (!isnan(power))
            snprintf(line, sizeof(line), "%.0fW", power);
        else
            snprintf(line, sizeof(line), "--");
        tftText(1, TFT_WHITE, x + 10, 88, line, 140, 8);

        tftText(1, TFT_SKYBLUE, x + 10, 97, "L", 8, 8);
        drawMiniBar(x + 22, 97, 60, 6, load);
        tftText(1, TFT_WHITE, x + 90, 97, f0(load) + "%", 48, 8);
    }

    // --- RAM ---
    tftText(4, loadColor(d.ramPct), 58, 108, f0(d.ramPct) + "%", 90, 18);
    String gb = "--";
    if (!isnan(d.ramPct) && !isnan(d.ramTotalGB))
        gb = String(d.ramTotalGB, 1) + "GB";
    tftText(1, TFT_DARKGREY, 240, 107, gb, 72, 8);
    drawMiniBar(10, 127, 300, 8, d.ramPct);

    // --- Вентиляторы (до 2 каналов; второй рисуем только если реально есть) ---
    for (int i = 0; i < 2; i++) {
        float rpm = i == 0 ? d.fan1Rpm : d.fan2Rpm;
        if (i == 1 && isnan(rpm)) {
            tft.fillRect(10, 176, 140, 9, TFT_BLACK);   // строка второго вентилятора
        }
        char l[16];
        snprintf(l, sizeof(l), "%d: %s", i + 1, isnan(rpm) ? "--" : f0(rpm).c_str());
        tftText(1, TFT_WHITE, 10, 166 + i * 10, l, 140, 8);
    }
    // SYS — температура материнки (из LHM)
    char sysl[20];
    if (isnan(d.sysTemp)) snprintf(sysl, sizeof(sysl), "SYS: --");
    else                  snprintf(sysl, sizeof(sysl), "SYS: %s C", f0(d.sysTemp).c_str());
    tftText(1, isnan(d.sysTemp) ? TFT_DARKGREY : tempColor(d.sysTemp), 10, 186, sysl, 140, 9);
    tft.fillRect(10, 196, 140, 9, TFT_BLACK);   // зачистка строки, где раньше был fan2

    // --- Правая нижняя ячейка: 12V (если шина есть) или диск C: ---
    bool hasV12 = !isnan(d.v12Volt);
    tft.fillRect(166, 155, 148, 7, TFT_BLACK);
    tft.setTextFont(1);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    if (hasV12) {
        tft.drawString("12V", 166, 155);
        tft.fillRect(166, 184, 140, 9, TFT_BLACK);   // затереть строку "free"
        if (isnan(d.v12Volt)) tftText(4, TFT_DARKGREY, 166, 162, "--", 140, 18);
        else                  tftText(4, voltColor(d.v12Volt), 166, 162, String(d.v12Volt, 2), 140, 18);
    } else {
        tft.drawString("DISK C", 166, 155);
        if (isnan(d.diskPct)) {
            tftText(4, TFT_DARKGREY, 166, 162, "--", 140, 18);
        } else {
            tftText(4, loadColor(d.diskPct), 166, 162, f0(d.diskPct) + "%", 140, 18);
            String fr = isnan(d.diskFreeGB) ? "?" : String(d.diskFreeGB, 0);
            tftText(1, TFT_WHITE, 166, 184, "free " + fr + " GB", 140, 8);
        }
    }

    String dn = isnan(d.netDnMBps) ? "dn --" : "dn " + String(d.netDnMBps, 1) + " Mb";
    String up = isnan(d.netUpMBps) ? "up --" : "up " + String(d.netUpMBps, 1) + " Mb";
    tftText(1, TFT_WHITE, 166, 196, dn, 140, 8);
    tftText(1, TFT_WHITE, 166, 206, up, 140, 8);
}

// ---------------- ГРАФИКИ ----------------

static void drawGrid(int x0, int y0, int w, int h) {
    for (int k = 25; k <= 75; k += 25) {
        int y = y0 + (int)(k * (h - 2) / 100.0f);
        tft.drawFastHLine(x0, y, w - 1, TFT_NAVY);
    }
    for (int i = 60; i <= 180; i += 60) {
        int x = x0 + 2 + (int)(i * (w - 4) / 239.0f);
        tft.drawFastVLine(x, y0, h - 1, TFT_NAVY);
    }
}

// Универсальный построитель графика (данные передаются через get(i))
template <class Get>
static void plotSeries(int x0, int y0, int w, int h, int count,
                       Get get, uint16_t col) {
    if (count < 2) return;
    const int y1 = y0 + h - 1;
    float sr = (float)(w - 2) / (float)(count - 1);
    for (int i = count - 1; i >= 1; i--) {   // i — индекс от нового к старому
        float raw_a = get(i);
        float raw_b = get(i - 1);
        if (isnan(raw_a) || isnan(raw_b)) continue;   // дыры (офлайн) не рисуем
        float a = clampf(raw_a, 0, 100);
        float b = clampf(raw_b, 0, 100);
        int ax = x0 + 2 + (int)((count - 1 - i) * sr);
        int bx = x0 + 2 + (int)((count - i) * sr);
        int ay = y1 - 1 - (int)(a * (h - 2) / 100.0f);
        int by = y1 - 1 - (int)(b * (h - 2) / 100.0f);
        tft.drawLine(ax, ay, bx, by, col);
    }
}

static void graphStatic() {
    tft.drawRect(5, 25, 310, 90, TFT_DARKGREY);
    tft.drawRect(5, 120, 310, 90, TFT_DARKGREY);
    tftText(1, TFT_DARKGREY, 10, 112, "-120 s", 40, 8);
    tftText(1, TFT_DARKGREY, 270, 207, "now", 40, 8);
}

// Плавный скролл графиков БЕЗ мерцания: полотно с сеткой рисуется один раз,
// а дальше каждый новый сэмпл «сдвигает» кривые — старое изображение стираем
// (повторно рисуем в цвет фона по сохранённой копии gPrev) и поверх рисуем
// свежий кадр. Полная очистка области нужна только при входе на страницу и
// пока буфер истории ещё наполняется (первые 120 с после старта).
static float gPrev[7][History::N];   // тень последнего нарисованного кадра
static int   gPrevN = 0;             // сколько сэмплов в тени

static float histVal(const History& h, int s, int k) {
    switch (s) {
        case 0:  return h.at(h.cpuTemp, k);
        case 1:  return h.at(h.coreAvgTemp, k);
        case 2:  return h.at(h.cpuLoad, k);
        case 3:  return h.at(h.ramPct, k);
        case 4:  return h.at(h.gpuTemp, k);
        case 5:  return h.at(h.gpuLoad, k);
        default: return h.at(h.diskPct, k);
    }
}

static uint16_t gCol(int s) {
    static const uint16_t col[7] = {TFT_RED, TFT_CYAN, TFT_BLUE, TFT_YELLOW,
                                    TFT_GREEN, TFT_MAGENTA, TFT_ORANGE};
    return col[s];
}

static int gPanelY(int s) { return s <= 3 ? 27 : 122; }

// Сетка координат ВСЕГДА по полному окну History::N (правый край — "сейчас",
// слева история растёт по мере наполнения). Благодаря этому позиция сэмпла
// зависит только от его возраста, и кадр можно сдвигать инкрементально с
// самого первого сэмпла — без полных очисток области.
static void graphShadowSync(const History& h) {
    for (int s = 0; s < 7; s++) {
        for (int k = 0; k < History::N; k++) gPrev[s][k] = histVal(h, s, k);
    }
    gPrevN = History::N;
}

// Полная отрисовка при входе на страницу: очистка + сетка + все кривые
static void graphRedrawFull(const History& h) {
    tft.fillRect(7, 27, 306, 84, TFT_BLACK);
    tft.fillRect(7, 122, 306, 84, TFT_BLACK);
    drawGrid(7, 27, 306, 84);
    drawGrid(7, 122, 306, 84);
    for (int s = 0; s < 7; s++)
        plotSeries(7, gPanelY(s), 306, 84, History::N,
                   [&](int i) { return histVal(h, s, i); }, gCol(s));
    graphShadowSync(h);
}

// Сдвиг на один сэмпл: стереть прошлый кадр (в цвет фона по тени) и
// нарисовать текущий. Полотно и сетку не трогаем — мерцания нет.
static void graphScroll(const History& h) {
    if (gPrevN != History::N) { graphRedrawFull(h); return; }  // страховка
    for (int s = 0; s < 7; s++)
        plotSeries(7, gPanelY(s), 306, 84, History::N,
                   [&](int i) { return gPrev[s][i]; }, TFT_BLACK);
    for (int s = 0; s < 7; s++)
        plotSeries(7, gPanelY(s), 306, 84, History::N,
                   [&](int i) { return histVal(h, s, i); }, gCol(s));
    graphShadowSync(h);
}

// ---------------- ЯДРА CPU ----------------

// 2 ряда по 2 крупных блока (до 4 ядер): температура, загрузка,
// спарклайн истории температуры (120 с) и max с момента включения.
static const int kCoresCols = 2;
static const int kCoreBX = 5, kCoreBY = 24, kCoreBW = 154, kCoreBH = 90, kCoreGap = 6;

static void coreXY(int i, int& x, int& y) {
    x = kCoreBX + i % kCoresCols * (kCoreBW + kCoreGap);
    y = kCoreBY + i / kCoresCols * (kCoreBH + kCoreGap);
}

static void coresStatic() {
    int n = PCData::MAX_CORES;
    if (n > 4) n = 4;   // крупные блоки: места хватает на 4
    for (int i = 0; i < n; i++) {
        int x, y; coreXY(i, x, y);
        tft.drawRect(x, y, kCoreBW, kCoreBH, TFT_DARKGREY);
        char h[12];
        snprintf(h, sizeof(h), "CORE %d", i + 1);
        tftText(1, TFT_CYAN, x + 4, y + 3, h, kCoreBW - 8, 8);
        // спарклайн-область (низ блока)
        tft.drawFastHLine(x + 1, y + kCoreBH - 26, kCoreBW - 2, TFT_NAVY);
    }
}

static float coreMaxSinceBoot[PCData::MAX_CORES];
static bool  coreMaxInit = false;

// Спарклайн температуры ядра за 120 с (нормировка 20..100 C)
static void coreSpark(int core, int x0, int y0, int w, int h, const History& hv) {
    if (hv.count < 2) return;
    const int y1 = y0 + h - 1;
    float sr = (float)(w - 1) / (float)(hv.count - 1);
    for (int i = hv.count - 1; i >= 1; i--) {
        float ra = hv.coreAt(core, i);
        float rb = hv.coreAt(core, i - 1);
        if (isnan(ra) || isnan(rb)) continue;
        auto norm = [](float t) {
            float v = (t - 20.0f) / 80.0f;          // 20..100 C -> 0..1
            if (v < 0) v = 0; if (v > 1) v = 1;
            return v;
        };
        int ax = x0 + (int)((hv.count - 1 - i) * sr);
        int bx = x0 + (int)((hv.count - i) * sr);
        int ay = y1 - (int)(norm(ra) * (h - 1));
        int by = y1 - (int)(norm(rb) * (h - 1));
        tft.drawLine(ax, ay, bx, by, tempColor((ra + rb) * 0.5f));
    }
}

static void coresDynamic(const PCData& d, const History& hv) {
    int n = PCData::MAX_CORES;
    if (n > 4) n = 4;
    if (!coreMaxInit) {
        for (int i = 0; i < n; i++) coreMaxSinceBoot[i] = NAN;
        coreMaxInit = true;
    }
    for (int i = 0; i < d.coreCount && i < n; i++) {
        if (isnan(d.coreTemp[i])) continue;
        if (isnan(coreMaxSinceBoot[i]) || d.coreTemp[i] > coreMaxSinceBoot[i])
            coreMaxSinceBoot[i] = d.coreTemp[i];
    }

    for (int i = 0; i < n; i++) {
        int x, y; coreXY(i, x, y);
        bool live = i < d.coreCount;
        float t  = live ? d.coreTemp[i] : NAN;
        float l  = live ? d.coreLoad[i] : NAN;
        float mx = coreMaxSinceBoot[i];

        // Слева — температура (крупно)
        if (isnan(t)) {
            tftText(4, TFT_DARKGREY, x + 5, y + 14, "--", 70, 32);
        } else {
            tftText(4, tempColor(t), x + 5, y + 14, f0(t), 70, 32);
        }
        // Подпись C и справа — max
        tftText(1, TFT_DARKGREY, x + 52, y + 18, "C", 12, 8);
        if (!isnan(mx)) {
            char m[10];
            snprintf(m, sizeof(m), "max %s", f0(mx).c_str());
            tftText(1, TFT_DARKGREY, x + kCoreBW - 48, y + 18, m, 45, 8);
        }
        // Загрузка: бар + %
        tftText(1, TFT_SKYBLUE, x + 5, y + 50, "L", 8, 8);
        drawMiniBar(x + 16, y + 50, 100, 6, l);
        char pct[10];
        snprintf(pct, sizeof(pct), "%s%%", f0(l).c_str());
        tftText(1, TFT_WHITE, x + 122, y + 50, pct, 30, 8);
        // Спарклайн (очищаем только свою область)
        tft.fillRect(x + 1, y + kCoreBH - 24, kCoreBW - 2, 22, TFT_BLACK);
        coreSpark(i, x + 1, y + kCoreBH - 24, kCoreBW - 3, 21, hv);
    }
    // Ядра 5..8 (если вдруг придут) — предупреждение в подвале
    String foot;
    if (d.coreCount > 4) {
        foot = String("showing cores 1-4 of ") + d.coreCount;
    } else if (d.coreCount == 0) {
        foot = "per-core data unavailable (requires LibreHardwareMonitor)";
    }
    if (foot.length()) tftText(1, TFT_DARKGREY, 8, 208, foot, 305, 9);
    else tft.fillRect(8, 208, 305, 9, TFT_BLACK);
}

// ---------------- КЛАВИШИ (KEYS) ----------------

// 2 колонки x 4 ряда, сетка 8 кнопок горячих клавиш
static const int kKeyCols = 2;
static const int kKeyBX = 8, kKeyBY = 34, kKeyBW = 152, kKeyBH = 42, kKeyGapX = 4, kKeyGapY = 3;

static void keyXY(int i, int& x, int& y) {
    x = kKeyBX + i % kKeyCols * (kKeyBW + kKeyGapX);
    y = kKeyBY + i / kKeyCols * (kKeyBH + kKeyGapY);
}

static void keysStatic() {
    // заголовок
    tftText(1, COL_DIM, 8, 25, "tap = run   hold 0.7s = edit combo", 305, 8);
    for (int i = 0; i < HK_MAX; i++) {
        int x, y; keyXY(i, x, y);
        const Hotkey& hk = hkGet(i);
        bool empty = (hk.combo[0] == 0);
        tft.fillRoundRect(x, y, kKeyBW, kKeyBH, 6, empty ? 0x0841 : COL_BTN);
        tft.drawRoundRect(x, y, kKeyBW, kKeyBH, 6, TFT_DARKGREY);
        tft.setTextFont(2);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(empty ? COL_DIM : TFT_WHITE, empty ? 0x0841 : COL_BTN);
        tft.drawString(hk.label[0] ? hk.label : "-", x + kKeyBW / 2, y + 13);
        tft.setTextFont(1);
        tft.setTextColor(empty ? 0x4A69 : COL_LBLUE, empty ? 0x0841 : COL_BTN);
        tft.drawString(hk.combo[0] ? hk.combo : " ", x + kKeyBW / 2, y + 29);
        tft.setTextDatum(TL_DATUM);
    }
}

// ---------------- ЗАПУСК ПРИЛОЖЕНИЙ (APPS) ----------------
// 8 кнопок в той же сетке 2x4, что и KEYS. Настройка (какие приложения)
// выполняется в ПК-приложении — сюда приходят только подписи (AP:).
static void appsStatic() {
    tftText(1, COL_DIM, 8, 25, "tap to launch on the PC", 305, 8);
    for (int i = 0; i < APP_MAX; i++) {
        int x, y; keyXY(i, x, y);
        const AppItem& a = appGet(i);
        bool empty = (a.label[0] == 0);
        tft.fillRoundRect(x, y, kKeyBW, kKeyBH, 6, empty ? 0x0841 : COL_BTN);
        tft.drawRoundRect(x, y, kKeyBW, kKeyBH, 6, TFT_DARKGREY);
        tft.setTextFont(2);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(empty ? COL_DIM : TFT_WHITE, empty ? 0x0841 : COL_BTN);
        tft.drawString(empty ? "-" : a.label, x + kKeyBW / 2, y + 13);
        if (!empty) {
            tft.setTextFont(1);
            tft.setTextColor(COL_LBLUE, COL_BTN);
            tft.drawString("RUN", x + kKeyBW / 2, y + 29);
        }
        tft.setTextDatum(TL_DATUM);
    }
}

// ---------------- МЕДИА (MEDIA) ----------------
// Сверху — «сейчас играет» (трек с ПК), снизу — реальный уровень громкости.
static const int kMediaCols = 3;
static const int kMediaBX = 8, kMediaBY = 47, kMediaBW = 99, kMediaBH = 52, kMediaGapX = 5, kMediaGapY = 6;
static const char* kMediaNames[kMediaCols * 2] = {
    "MUTE", "VOL-", "VOL+",
    "PREV", "PLAY", "NEXT",
};
static const char* kMediaSym[kMediaCols * 2] = {
    " ", "-", "+",
    "<|", ">", ">|",
};
static void mediaXY(int i, int& x, int& y) {
    x = kMediaBX + i % kMediaCols * (kMediaBW + kMediaGapX);
    y = kMediaBY + i / kMediaCols * (kMediaBH + kMediaGapY);
}

static void mediaStatic() {
    // «сейчас играет»: рамка и метка NOW (цветная точка-«эфир»)
    tft.drawRoundRect(5, 24, 310, 20, 4, TFT_DARKGREY);
    tft.fillCircle(12, 34, 3, COL_ACCENT);
    tftText(1, COL_DIM, 18, 30, "NOW PLAYING", 90, 8);
    // кнопки транспорта
    for (int i = 0; i < kMediaCols * 2; i++) {
        int x, y; mediaXY(i, x, y);
        bool big = (i == 4);
        uint16_t bg = big ? 0x03EF : COL_BTN;
        tft.fillRoundRect(x, y, kMediaBW, kMediaBH, 8, bg);
        tft.drawRoundRect(x, y, kMediaBW, kMediaBH, 8, TFT_DARKGREY);
        tft.setTextFont(2);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, bg);
        tft.drawString(kMediaNames[i], x + kMediaBW / 2, big ? y + 17 : y + 15);
        tft.setTextFont(1);
        tft.setTextColor(COL_LBLUE, bg);
        tft.drawString(kMediaSym[i], x + kMediaBW / 2, y + 35);
        tft.setTextDatum(TL_DATUM);
    }
    // Заголовок громкости (значение и шкалу рисует mediaDynamic)
    tftText(1, COL_DIM, 10, 162, "VOLUME", 50, 8);
}

// Полоса громкости: скруглённый трек + зонная заливка. Рисуется в mediaDynamic,
// чтобы при каждом изменении уровня перерисовывать аккуратно и без хвостов.
static void drawVolumeBar(int x, int y, int w, int h, int pct, bool muted) {
    tft.fillRoundRect(x, y, w, h, h / 2, COL_TRACK);
    if (muted) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int fw = (w - 4) * pct / 100;
    if (fw <= 0) return;
    // Зонный градиент: до 55% голубой, до 85% зелёный, дальше жёлтый->красный
    for (int px = 0; px < fw; px++) {
        float pos = 100.0f * px / (float)(w - 4);
        uint16_t c = (pos < 55) ? COL_LBLUE : (pos < 85) ? TFT_GREEN : (pos < 95) ? TFT_YELLOW : TFT_RED;
        tft.drawFastVLine(x + 2 + px, y + 2, h - 4, c);
    }
}

// Изменился ли трек/громкость (перерисовываем динамику только тогда)
static bool mediaNeedsRedraw(const PCData& d) {
    static char lastTrack[52] = "";
    static int lastVol = 999;
    static bool have = false;
    int vol = isnan(d.volume) ? -999 : (int)(d.volume + 0.5f);
    bool need = !have || strcmp(lastTrack, d.mediaTrack) != 0 || vol != lastVol;
    have = true;
    if (need) {
        strncpy(lastTrack, d.mediaTrack, sizeof(lastTrack) - 1);
        lastTrack[sizeof(lastTrack) - 1] = 0;
        lastVol = vol;
    }
    return need;
}

static void mediaDynamic(const PCData& d) {
    // Строка трека (обрезаем до ширины; затираем старую)
    bool haveTrack = d.mediaTrack[0] != 0;
    tft.fillRect(110, 25, 200, 18, TFT_BLACK);
    if (haveTrack) {
        String t = String(d.mediaTrack);
        if (t.length() > 17) t = t.substring(0, 16) + "…";   // ~17 симв. font2
        tft.setTextFont(2);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString(t, 112, 26);
    } else {
        tftText(1, TFT_DARKGREY, 112, 28, "no media / player not found", 200, 10);
    }

    // Уровень: % (или MUTED) и заполнение шкалы
    float v = d.volume;
    bool muted = false;
    if (isnan(v)) { v = 0; }
    else if (v < 0) { muted = true; v = -v; }
    if (v > 100) v = 100;

    // Процент крупно справа
    tftText(4, muted ? TFT_RED : TFT_SKYBLUE, 232, 160, muted ? "MUTED" : f0(v) + "%", 84, 20);
    // Шкала-пилюля (перерисовываем трек целиком, чтобы не было хвостов)
    drawVolumeBar(10, 178, 214, 12, (int)(v + 0.5f), muted);
    // Уровень в процентах + метка (приглушённая) под шкалой
    tftText(1, COL_DIM, 10, 195, muted ? "muted" : "volume level", 120, 8);
}

// ---------------- ПРОЦЕССЫ (PROCS) ----------------
// Топ-5 процессов по CPU (данные приходят строкой TOP: раз в секунду).
// Строки перерисовываются точечно (только изменившиеся), чтобы не мигать.
static const int kProcRows = PCData::PROC_TOP;
static const int kProcRowY0 = 46, kProcRowH = 28, kProcRowGap = 3;
static const int kProcNameX = 8,  kProcNameW = 150;
static const int kProcCpuX  = 168, kProcCpuW  = 38;
static const int kProcMemX  = 214, kProcMemW  = 44;
static const int kProcKillX = 262, kProcKillW = 52;

static int procRowY(int i) { return kProcRowY0 + i * (kProcRowH + kProcRowGap); }

static PCData sProcData;              // последние данные (для kill-подтверждения)
static int     killRow = -1;          // ряд в режиме подтверждения
static long    killPid = 0;
static uint32_t killUntil = 0;
static bool    lastKillDraw[kProcRows];   // как нарисована кнопка KILL
static char    lastPName[kProcRows][21];
static int     lastPCpu[kProcRows], lastPMem[kProcRows];
static long    lastPPid[kProcRows];
static bool    pInit = false;

static void procKillCell(int i, bool armed) {
    int x = kProcKillX, y = procRowY(i), w = kProcKillW, h = kProcRowH;
    if (armed) {
        tft.fillRoundRect(x, y, w, h, 6, TFT_RED);
        tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);
        tft.setTextColor(TFT_BLACK, TFT_RED);
    } else {
        tft.fillRoundRect(x, y, w, h, 6, COL_BTN);
        tft.drawRoundRect(x, y, w, h, 6, TFT_DARKGREY);
        tft.setTextColor(TFT_RED, COL_BTN);
    }
    tft.setTextFont(1);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(armed ? "SURE?" : "KILL", x + w / 2, y + h / 2 + 1);
    tft.setTextDatum(TL_DATUM);
    lastKillDraw[i] = armed;
}

static void procStatic() {
    tftText(1, COL_DIM, 8, 26, "top-5 by CPU   ·   kill = 2 taps", 300, 8);
    tftText(1, COL_DIM, kProcNameX, 37, "PROCESS", 60, 8);
    tftText(1, COL_DIM, kProcCpuX, 37, "CPU", 24, 8);
    tftText(1, COL_DIM, kProcMemX, 37, "MEM", 24, 8);
    tftText(1, COL_DIM, kProcKillX, 37, "KILL", 24, 8);
    tft.drawFastHLine(5, 44, 310, COL_ACCENT);
    for (int i = 0; i < kProcRows; i++) {
        procKillCell(i, false);
        lastPName[i][0] = 0;
        lastPCpu[i] = -1;
        lastPMem[i] = -1;
        lastPPid[i] = -1;
    }
    pInit = true;
    killRow = -1;
}

// Перерисовать изменённые ячейки списка процессов
static void procDynamic(const PCData& d) {
    sProcData = d;
    // подтверждение устарело или процесс исчез/сменился — снимаем
    if (killRow >= 0) {
        bool stale = killRow >= kProcRows ||
                     (millis() >= killUntil) ||
                     (long)sProcData.procPid[killRow] != killPid;
        if (stale) killRow = -1;
    }
    for (int i = 0; i < kProcRows; i++) {
        long pid = (long)d.procPid[i];
        int  cpu = isnan(d.procCpu[i]) ? -1 : (int)(d.procCpu[i] + 0.5f);
        int  mem = isnan(d.procMem[i]) ? -1 : (int)(d.procMem[i] + 0.5f);
        bool rowChg = !pInit || pid != lastPPid[i] || cpu != lastPCpu[i] || mem != lastPMem[i] ||
                      strcmp(lastPName[i], d.procName[i]) != 0;
        bool armed = (i == killRow);
        if (rowChg) {
            // фон строки + разделитель
            int y = procRowY(i);
            tft.fillRect(kProcNameX, y, kProcKillX - kProcNameX - 2, kProcRowH, TFT_BLACK);
            // имя
            if (pid > 0 && d.procName[i][0]) {
                String n = String(d.procName[i]);
                if (n.length() > 22) n = n.substring(0, 21) + "…";
                tftText(1, TFT_WHITE, kProcNameX, y + 3, n, kProcNameW, 10);
                // мини-шкала CPU под именем (0..100% -> цвет)
                int bw = (kProcNameW - 6) * clampf((float)cpu, 0, 100) / 100;
                tft.fillRect(kProcNameX, y + 16, kProcNameW - 6, 5, COL_TRACK);
                if (bw > 0)
                    tft.fillRect(kProcNameX, y + 16, bw, 5, cpu < 0 ? COL_DIM : loadColor((float)cpu));
            } else {
                tftText(1, COL_DIM, kProcNameX, y + 10, "-", kProcNameW, 10);
                tft.fillRect(kProcNameX, y + 16, kProcNameW - 6, 5, TFT_BLACK);
            }
            // cpu / mem (по центру строки)
            char c[10];
            snprintf(c, sizeof(c), "%s%%", cpu < 0 ? "--" : String(cpu).c_str());
            tftText(1, cpu < 0 ? COL_DIM : loadColor((float)cpu), kProcCpuX, y + 10, c, kProcCpuW, 10);
            snprintf(c, sizeof(c), "%s", mem < 0 ? "--" : String(mem).c_str());
            tftText(1, mem < 0 ? COL_DIM : TFT_WHITE, kProcMemX, y + 10, c, kProcMemW, 10);
        }
        if (rowChg || lastKillDraw[i] != armed) procKillCell(i, armed);
        strncpy(lastPName[i], d.procName[i], 20);
        lastPName[i][20] = 0;
        lastPCpu[i] = cpu;
        lastPMem[i] = mem;
        lastPPid[i] = pid;
    }
    pInit = true;
}

long displayProcKillTap(int row) {
    if (row < 0 || row >= kProcRows) return -1;
    long pid = (long)sProcData.procPid[row];
    if (pid <= 0) return -1;
    if (killRow == row && killPid == pid && millis() < killUntil) {
        killRow = -1;                 // второй тап — завершаем
        killPid = 0;
        return pid;
    }
    killRow = row;                    // первый тап — подтверждение
    killPid = pid;
    killUntil = millis() + 2500;
    procDynamic(sProcData);
    return -1;
}

// ---------------- РЕДАКТОР КЛАВИШ НА БОРТУ ----------------
// Открывается долгим удержанием кнопки на странице KEYS (~0.7 с).
// Модальный: табы и переключение страниц недоступны до SAVE/CANCEL.
// Состав комбинации: модификаторы (CTRL/ALT/SHIFT/WIN) + одна клавиша.
static int     editSlot = -1;       // редактируемый слот (0..HK_MAX-1), -1 = выкл
static int     editCat = 0;         // категория: 0 A-Z, 1 0-9, 2 F1-12, 3 служебные
static int     editPage = 0;        // страница категории (по 21 клавише)
static uint8_t editMods = 0;        // бит0 ctrl, 1 alt, 2 shift, 3 win
static String  editBase;            // выбранная основная клавиша (токен, напр. "f5")
static uint32_t editFlashUntil = 0; // до этого millis() показываем подсказку красным

static const char* kModNames[4] = {"CTRL", "ALT", "SHIFT", "WIN"};
static const char* kModTok[4]   = {"ctrl", "alt", "shift", "win"};
static const char* kCatNames[4] = {"A-Z", "0-9", "F1-12", "SPEC"};
static const char* kSpecTok[18] = {
    "esc", "tab", "space", "enter", "backspace", "delete", "insert", "home", "end",
    "pgup", "pgdn", "up", "down", "left", "right", "printscreen", "pause", "capslock"
};
static const char* kSpecDisp[18] = {
    "Esc", "Tab", "Spc", "Ent", "Bksp", "Del", "Ins", "Home", "End",
    "PgUp", "PgDn", "Up", "Dn", "Lt", "Rt", "PrScr", "Paus", "Caps"
};
static const int kCatCount[4] = {26, 10, 12, 18};
static const int kGridCols = 7, kGridRows = 3, kGridPage = 21;

static int catPages(int cat) {
    return (kCatCount[cat] + kGridPage - 1) / kGridPage;
}

// Клавиша в ячейке (cat, page, cell). false = пустая ячейка.
static bool keyAt(int cat, int page, int cell, String& disp, String& tok) {
    int idx = page * kGridPage + cell;
    if (cat < 0 || cat > 3 || idx >= kCatCount[cat]) return false;
    if (cat == 0) {
        tok = String((char)('a' + idx));
        disp = String((char)('A' + idx));
    } else if (cat == 1) {
        tok = String((char)('0' + idx));
        disp = tok;
    } else if (cat == 2) {
        tok = String("f") + (idx + 1);
        disp = String("F") + (idx + 1);
    } else {
        if (idx >= 18) return false;
        tok = kSpecTok[idx];
        disp = kSpecDisp[idx];
    }
    return true;
}

// Короткая подпись для label кнопки (из токена комбинации)
static String friendlyToken(const String& tok) {
    if (tok.length() == 1) {
        char c = tok.charAt(0);
        if (c >= 'a' && c <= 'z') return String((char)(c - 32));
        if (c >= '0' && c <= '9') return tok;
    }
    if (tok.length() > 1 && (tok[0] == 'f' || tok[0] == 'F')) {
        bool num = true;
        for (unsigned i = 1; i < tok.length(); i++)
            if (!isdigit((unsigned char)tok[i])) { num = false; break; }
        if (num) return String("F") + tok.substring(1);
    }
    for (int i = 0; i < 18; i++)
        if (tok == kSpecTok[i]) return kSpecDisp[i];
    String u = tok; u.toUpperCase(); return u;
}

static uint8_t modBit(const String& t) {
    if (t == "ctrl" || t == "control") return 1;
    if (t == "alt") return 2;
    if (t == "shift") return 4;
    if (t == "win") return 8;
    return 0;
}

// Собрать строку комбинации из состояния редактора
static String buildCombo() {
    String c;
    for (int i = 0; i < 4; i++) {
        if (editMods & (1 << i)) {
            if (c.length()) c += "+";
            c += kModTok[i];
        }
    }
    if (editBase.length()) {
        if (c.length()) c += "+";
        c += editBase;
    }
    return c;
}

// Категория/страница под токен основной клавиши (чтобы редактор открывался
// на уже назначенной клавише). false = токен вне каталога.
static bool baseView(const String& tok, int& cat, int& page) {
    if (tok.length() == 1) {
        char c = tolower(tok.charAt(0));
        if (c >= 'a' && c <= 'z') { cat = 0; page = (c - 'a') / kGridPage; return true; }
        if (c >= '0' && c <= '9') { cat = 1; page = 0; return true; }
    }
    if (tok.length() > 1 && (tok[0] == 'f' || tok[0] == 'F')) {
        String n = tok.substring(1);
        bool num = true;
        for (unsigned i = 0; i < n.length(); i++)
            if (!isdigit((unsigned char)n[i])) { num = false; break; }
        if (num) {
            int v = n.toInt();
            if (v >= 1 && v <= 12) { cat = 2; page = 0; return true; }
        }
    }
    for (int i = 0; i < 18; i++)
        if (tok == kSpecTok[i]) { cat = 3; page = i / kGridPage; return true; }
    return false;
}

// Геометрия редактора (вся область y=21..239, табы скрыты)
static const int ED_CHIP_X0 = 8, ED_CHIP_W = 72, ED_CHIP_STEP = 78;
static const int ED_CHIP_Y0 = 52, ED_CHIP_H = 18;
static const int ED_CAT_Y0  = 73;
static const int ED_GRID_X0 = 6, ED_GRID_W = 43, ED_GRID_STEP = 45;
static const int ED_GRID_Y0 = 96, ED_GRID_H = 30, ED_GRID_ROW = 33;
static const int ED_BAR_Y = 218, ED_BAR_H = 20;

// Кнопка-чип (активная заливается onCol, неактивная — navy)
static void editChip(int x, int y, int w, int h, const char* s,
                     bool on, uint16_t onCol, uint16_t txtOn, uint16_t txtOff) {
    uint16_t bg = on ? onCol : COL_BTN;
    tft.fillRoundRect(x, y, w, h, 5, bg);
    tft.drawRoundRect(x, y, w, h, 5, on ? TFT_WHITE : TFT_DARKGREY);
    tft.setTextFont(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(on ? txtOn : txtOff, bg);
    tft.drawString(s, x + w / 2, y + h / 2 + 1);
    tft.setTextDatum(TL_DATUM);
}

// Полная перерисовка редактора (не трогает шапку y<=19)
static void editDrawAll() {
    tft.fillRect(0, 21, 320, 218, TFT_BLACK);
    if (editSlot < 0) return;
    const Hotkey& cur = hkGet(editSlot);

    // Шапка редактора: номер слота + старая подпись
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "EDIT #%d   was: %s", editSlot + 1,
             cur.label[0] ? cur.label : "-");
    tftText(1, TFT_DARKGREY, 8, 22, hdr, 300, 8);

    // Черновик комбинации (крупно) либо подсказка
    String combo = buildCombo();
    if (millis() < editFlashUntil) {
        tftText(2, TFT_RED, 8, 35, "pick a key first!", 300, 16);
    } else if (combo.length() == 0) {
        tftText(2, TFT_DARKGREY, 8, 35, "tap a key below", 300, 16);
    } else {
        tftText(2, TFT_YELLOW, 8, 35, combo, 300, 16);
    }

    // Модификаторы (переключатели)
    for (int i = 0; i < 4; i++)
        editChip(ED_CHIP_X0 + i * ED_CHIP_STEP, ED_CHIP_Y0, ED_CHIP_W, ED_CHIP_H,
                 kModNames[i], editMods & (1 << i), TFT_YELLOW, TFT_BLACK, TFT_WHITE);
    // Категории клавиш
    for (int i = 0; i < 4; i++)
        editChip(ED_CHIP_X0 + i * ED_CHIP_STEP, ED_CAT_Y0, ED_CHIP_W, ED_CHIP_H,
                 kCatNames[i], i == editCat, TFT_CYAN, TFT_BLACK, TFT_WHITE);

    // Сетка клавиш 7x3
    for (int cell = 0; cell < kGridPage; cell++) {
        int col = cell % kGridCols, row = cell / kGridCols;
        int x = ED_GRID_X0 + col * ED_GRID_STEP;
        int y = ED_GRID_Y0 + row * ED_GRID_ROW;
        String disp, tok;
        if (!keyAt(editCat, editPage, cell, disp, tok)) {
            tft.fillRect(x, y, ED_GRID_W, ED_GRID_H, TFT_BLACK);
            continue;
        }
        bool sel = (tok == editBase);
        uint16_t bg = sel ? 0x03EF : COL_BTN;
        tft.fillRoundRect(x, y, ED_GRID_W, ED_GRID_H, 5, bg);
        tft.drawRoundRect(x, y, ED_GRID_W, ED_GRID_H, 5, sel ? TFT_YELLOW : TFT_DARKGREY);
        tft.setTextDatum(MC_DATUM);
        tft.setTextFont(disp.length() <= 2 ? 2 : 1);
        tft.setTextColor(TFT_WHITE, bg);
        tft.drawString(disp, x + ED_GRID_W / 2, y + ED_GRID_H / 2);
        tft.setTextDatum(TL_DATUM);
    }

    // Нижняя панель: CANCEL | < > (страницы) | SAVE
    editChip(8, ED_BAR_Y, 64, ED_BAR_H, "CANCEL", false, TFT_MAROON, TFT_WHITE, TFT_RED);
    int pages = catPages(editCat);
    bool canPrev = editPage > 0;
    bool canNext = editPage + 1 < pages;
    editChip(84, ED_BAR_Y, 36, ED_BAR_H, "<", canPrev, TFT_YELLOW, TFT_BLACK,
             canPrev ? TFT_WHITE : TFT_DARKGREY);
    editChip(124, ED_BAR_Y, 36, ED_BAR_H, ">", canNext, TFT_YELLOW, TFT_BLACK,
             canNext ? TFT_WHITE : TFT_DARKGREY);
    if (pages > 1) {
        char pg[16];
        snprintf(pg, sizeof(pg), "%s %d/%d", kCatNames[editCat], editPage + 1, pages);
        tftText(1, TFT_DARKGREY, 164, ED_BAR_Y + 5, pg, 78, 10);
    } else {
        tft.fillRect(164, ED_BAR_Y, 78, ED_BAR_H, TFT_BLACK);
    }
    editChip(246, ED_BAR_Y, 68, ED_BAR_H, "SAVE", true, TFT_GREEN, TFT_BLACK, TFT_WHITE);
}

// Возврат на страницу KEYS (рисуем сразу, чтобы не ждать следующего кадра)
static void editClose() {
    editSlot = -1;
    tft.fillScreen(TFT_BLACK);
    keysStatic();
    drawTabs();
}

bool displayEditActive() {
    return editSlot >= 0;
}

void displayEditOpen(int slot) {
    if (slot < 0 || slot >= HK_MAX) return;
    editSlot = slot;
    editMods = 0;
    editBase = "";
    const Hotkey& c = hkGet(slot);
    // Разбираем текущую комбинацию: модификаторы + основная клавиша
    String combo(c.combo);
    int pos = 0;
    while (true) {
        int plus = combo.indexOf('+', pos);
        String tok = (plus < 0) ? combo.substring(pos) : combo.substring(pos, plus);
        tok.trim();
        if (tok.length()) {
            uint8_t b = modBit(tok);
            if (b) editMods |= b;
            else if (editBase.length() == 0) editBase = tok;
        }
        if (plus < 0) break;
        pos = plus + 1;
    }
    editCat = 0;
    editPage = 0;
    int cv = 0, pv = 0;
    if (baseView(editBase, cv, pv)) { editCat = cv; editPage = pv; }
    editFlashUntil = 0;
    editDrawAll();
}

int displayEditTap(UIAction a) {
    if (editSlot < 0) return -1;

    if (a == UI_EDIT_CANCEL) {
        editClose();
        return -1;
    }
    if (a == UI_EDIT_PG_PREV) {
        if (editPage > 0) { editPage--; editDrawAll(); }
        return -1;
    }
    if (a == UI_EDIT_PG_NEXT) {
        if (editPage + 1 < catPages(editCat)) { editPage++; editDrawAll(); }
        return -1;
    }
    if (a >= UI_EDIT_MOD_CTRL && a <= UI_EDIT_MOD_WIN) {
        int b = a - UI_EDIT_MOD_CTRL;
        editMods ^= (1u << b);
        editDrawAll();
        return -1;
    }
    if (a >= UI_EDIT_CAT_0 && a <= UI_EDIT_CAT_3) {
        int c = a - UI_EDIT_CAT_0;
        if (c != editCat) { editCat = c; editPage = 0; editDrawAll(); }
        return -1;
    }
    if (a >= UI_EDIT_KEY_0 && a <= UI_EDIT_KEY_20) {
        int cell = a - UI_EDIT_KEY_0;
        String disp, tok;
        if (keyAt(editCat, editPage, cell, disp, tok)) {
            editBase = tok;
            editDrawAll();
        }
        return -1;
    }
    if (a == UI_EDIT_SAVE) {
        if (editBase.length() == 0) {
            editFlashUntil = millis() + 1200;
            editDrawAll();
            return -1;
        }
        const Hotkey& cur = hkGet(editSlot);
        String combo = buildCombo();
        // Комбинация не менялась — сохраняем и прежнюю подпись (не превращаем
        // «TaskMgr» в «Esc»), и main не шлёт на ПК лишний HKCFG.
        String body;
        if (combo == cur.combo)
            body = String(editSlot) + "=" + String(cur.label) + "|" + combo;
        else
            body = String(editSlot) + "=" + friendlyToken(editBase) + "|" + combo;
        int saved = editSlot;
        bool changed = hkApplyLine(body);   // сохранение в NVS (с пропуском дублей)
        editClose();
        return changed ? saved : -1;        // changed: main отправит HKCFG на ПК
    }
    return -1;
}

// ---------------- УПРАВЛЕНИЕ ----------------

static void relayButton(int x, int y, int w, const char* label, bool on) {
    uint16_t bg = on ? TFT_GREEN : COL_BTN;
    tft.fillRoundRect(x, y, w, 40, 8, bg);
    tft.drawRoundRect(x, y, w, 40, 8, on ? TFT_WHITE : TFT_DARKGREY);
    // «светодиод» состояния
    uint16_t led = on ? TFT_WHITE : dimC(TFT_GREEN, 8);
    tft.fillCircle(x + 12, y + 20, 5, led);
    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(on ? TFT_BLACK : TFT_WHITE, bg);
    tft.drawString(label, x + w / 2 + 4, y + 13);
    tft.setTextFont(1);
    tft.setTextColor(on ? TFT_BLACK : COL_DIM, bg);
    tft.drawString(on ? "ON" : "OFF", x + w / 2 + 4, y + 31);
    tft.setTextDatum(TL_DATUM);
}

static void cmdButton(int x, int y, const char* label) {
    tft.fillRoundRect(x, y, 96, 40, 8, COL_BTN);
    tft.drawRoundRect(x, y, 96, 40, 8, TFT_DARKGREY);
    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, COL_BTN);
    tft.drawString(label, x + 48, y + 20);
    tft.setTextDatum(TL_DATUM);
}

// Кнопка-переключатель на 2 строки (заголовок font2 + подпись font1)
static void ctrlButton(int x, int y, int w, const char* top, const char* sub,
                       bool on, uint16_t onColor) {
    uint16_t bg = on ? onColor : COL_BTN;
    tft.fillRoundRect(x, y, w, 30, 7, bg);
    tft.drawRoundRect(x, y, w, 30, 7, on ? TFT_WHITE : TFT_DARKGREY);
    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(on ? TFT_BLACK : TFT_WHITE, bg);
    tft.drawString(top, x + w / 2, y + 9);
    tft.setTextFont(1);
    tft.setTextColor(on ? TFT_BLACK : COL_DIM, bg);
    tft.drawString(sub, x + w / 2, y + 22);
    tft.setTextDatum(TL_DATUM);
}

static void ctrlStatic() {
    tftText(1, TFT_ORANGE, 8, 28, "RELAYS", 40, 8);
    for (int i = 0; i < NUM_RELAYS; i++)
        relayButton(8 + i * 154, 40, 150, RELAY_NAMES[i], relayGetState(i));

    // AUTO / ALARM (звук) / TOUCH-калибровка — три кнопки в ряд
    bool autoOn = relayGetAuto();
    bool snd = alarmsGetEnabled();
    ctrlButton(8,   90, 98, "AUTO",  autoOn ? "ON" : "OFF", autoOn, TFT_YELLOW);
    ctrlButton(110, 90, 98, "ALARM", snd ? "SOUND ON" : "MUTED", snd, TFT_ORANGE);
    ctrlButton(212, 90, 98, "TOUCH", "CALIBRATE", false, 0);

    tftText(1, TFT_SKYBLUE, 8, 128, "PC CONTROL", 70, 8);
    cmdButton(8, 136, "SLEEP");
    cmdButton(110, 136, "REBOOT");
    cmdButton(212, 136, "SHUTDN");

    // Пороги — редактируются в ПК-приложении (кнопка «Пороги…»)
    char tl[40];
    snprintf(tl, sizeof(tl), "AUTO R1>=%d/<=%d R2>=%d/<=%d",
             (int)settingsRelayOn(0), (int)settingsRelayOff(0),
             (int)settingsRelayOn(1), (int)settingsRelayOff(1));
    tftText(1, COL_DIM, 8, 186, tl, 300, 8);
    snprintf(tl, sizeof(tl), "WARN>=%d ALARM>=%d (RGB+buzz)",
             (int)settingsWarnC(), (int)settingsCritC());
    tftText(1, COL_DIM, 8, 197, tl, 300, 8);
    tftText(1, COL_DIM, 8, 208, "sleep/reboot/shutdown -> PC", 300, 8);
}

// ---------------- Тач: калибровка и чтение ----------------

static void drawCrosshair(int x, int y, uint16_t col) {
    tft.fillScreen(TFT_BLACK);
    tft.drawFastHLine(x - 12, y, 25, col);
    tft.drawFastVLine(x, y - 12, 25, col);
    tft.drawCircle(x, y, 8, col);
}

// Блокирующий мастер: 2 опорные точки, авто-детект swap осей, сохранение в NVS.
// Нажатия требуют устойчивого удержания (~0.1 c) и полного отпускания между
// точками — шум по z не может «проскочить» калибровку.

// Ждать стабильное нажатие (z > порога непрерывно >= 80 мс)
static bool waitPressStable() {
    uint32_t held = 0, last = 0;
    while (true) {
        TS_Point p = touch.getPoint();
        uint32_t now = millis();
        if (p.z > kZThreshold) {
            if (held == 0 || now - last > 80) held = now;  // новый отсчёт при дребезге
            else if (now - held >= 80) return true;
        } else {
            held = 0;
        }
        last = now;
        delay(5);
    }
}

// Ждать, что палец отпущен (z <= порога непрерывно >= 120 мс)
static bool waitReleaseStable() {
    uint32_t rel = 0;
    while (true) {
        TS_Point p = touch.getPoint();
        uint32_t now = millis();
        if (p.z <= kZThreshold) {
            if (rel == 0) rel = now;
            else if (now - rel >= 120) return true;
        } else {
            rel = 0;
        }
        delay(5);
    }
}

// Усреднение n замеров удержания (подавление шума одиночного сэмпла)
static TS_Point sampleHeld(int n) {
    long sx = 0, sy = 0;
    int ok = 0;
    for (int i = 0; i < n; i++) {
        TS_Point p = touch.getPoint();
        if (p.z > kZThreshold) { sx += p.x; sy += p.y; ok++; }
        delay(15);
    }
    TS_Point r;
    r.x = ok ? (int)(sx / ok) : 0;
    r.y = ok ? (int)(sy / ok) : 0;
    r.z = 0;
    return r;
}

void displayCalibrateAndSave() {
    int16_t raw[4];
    TS_Point p;

    Serial.println("[cal] wizard start, tap 1");
    drawCrosshair(20, 20, TFT_MAGENTA);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Touch 1/2: hold the +", 20, 30);
    waitPressStable();
    p = sampleHeld(6);
    raw[0] = p.x; raw[1] = p.y;
    Serial.printf("[cal] got 1: x=%d y=%d\n", raw[0], raw[1]);
    waitReleaseStable();

    Serial.println("[cal] tap 2");
    drawCrosshair(300, 220, TFT_MAGENTA);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Touch 2/2: hold the +", 100, 100);
    waitPressStable();
    p = sampleHeld(6);
    raw[2] = p.x; raw[3] = p.y;
    Serial.printf("[cal] got 2: x=%d y=%d\n", raw[2], raw[3]);
    waitReleaseStable();

    // Если разброс по "X" мал, а по "Y" велик — оси на панели переставлены
    int dx = abs(raw[2] - raw[0]);
    int dy = abs(raw[3] - raw[1]);

    // ВАЖНО: копируем РЕАЛЬНЫЕ точки в calData. Раньше здесь оставались
    // значения по умолчанию {300,300,3800,3800}, поэтому карта тача была
    // «фейковой» и нижние табы (GRAPHS/CTRL) не попадали под палец.
    calData[0] = (uint16_t)raw[0];
    calData[1] = (uint16_t)raw[1];
    calData[2] = (uint16_t)raw[2];
    calData[3] = (uint16_t)raw[3];
    calData[4] = (dy > dx) ? 1 : 0;

    // Касания почти совпали (мимо креста / шум) — не сохраняем, повторяем мастер
    if (dx < 200 && dy < 200) {
        Serial.printf("[cal] retry: dx=%d dy=%d\n", dx, dy);
        tft.fillScreen(TFT_BLACK);
        displayCalibrateAndSave();
        return;
    }

    Preferences pv;
    pv.begin("tft", false);
    pv.putBytes("cal", calData, sizeof(calData));
    pv.end();
    Serial.printf("[cal] saved: %d %d %d %d swap=%d\n",
                  raw[0], raw[1], raw[2], raw[3], calData[4]);

    tft.fillScreen(TFT_BLACK);
    // Явное подтверждение, что калибровка завершилась
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("CAL OK", 160, 104);
    tft.setTextFont(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("tap DASH / GRAPHS / CTRL", 160, 132);
    tft.setTextDatum(TL_DATUM);
    delay(1000);
    displayRefreshStatic();   // перерисовать UI на следующем кадре
}

void displayCalLoad() {
    // Мастер калибровки запускается при КАЖДОМ включении CYD (пользователь
    // хочет заново калибровать тач при каждой загрузке). Значение из NVS всё
    // равно читается: если мастер прервут/не пройдут до конца — останется
    // прошлая рабочая калибровка.
    Preferences p;
    p.begin("tft", true);
    size_t n = p.getBytes("cal", calData, sizeof(calData));
    p.end();
    if (n == sizeof(calData)) {
        Serial.printf("[cal] stored: %u %u %u %u swap=%u\n",
                      calData[0], calData[1], calData[2], calData[3], calData[4]);
    } else {
        Serial.println("[cal] no stored calibration");
    }
    Serial.println("[cal] wizard at boot");
    displayCalibrateAndSave();
}

bool displayGetTouch(int16_t& sx, int16_t& sy) {
    TS_Point p = touch.getPoint();
    if (p.z <= kZThreshold) return false;

    long rx = p.x, ry = p.y;
    long ax = calData[0], ay = calData[1];   // точка 1 — экран (20, 20)
    long bx = calData[2], by = calData[3];   // точка 2 — экран (300, 220)
    if (calData[4]) {                        // оси панели переставлены
        long t = rx; rx = ry; ry = t;
        t = ax; ax = ay; ay = t;
        t = bx; bx = by; by = t;
    }

    // ВАЖНО: маппим между точками В ПОРЯДКЕ их калибровки (не min/max!).
    // У многих панелей одна из осей «перевёрнута» (raw растёт вверх/влево):
    // min/max выбрасывал направление, и низ экрана (табы GRAPHS/CTRL)
    // оказывался вверху — кнопки внизу не работали.
    long lo = min(ax, bx), hi = max(ax, bx);
    if (hi == lo) hi = lo + 1;
    sx = (int16_t)map(constrain(rx, lo, hi), ax, bx, 0, 319);

    lo = min(ay, by); hi = max(ay, by);
    if (hi == lo) hi = lo + 1;
    sy = (int16_t)map(constrain(ry, lo, hi), ay, by, 0, 239);
    return true;
}

// ---------------- Публичное API ----------------

void displayInit() {
    // Подсветку поднимаем ДО tft.init(), иначе TFT_eSPI пишет в неё как в
    // непроинициализированный пин (в логе: "IO 21 is not set as GPIO")
#ifdef TFT_BL
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON == HIGH ? HIGH : LOW);
#endif

    tft.init();
    tft.setRotation(1);   // 320x240 landscape
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);

    // Тач на СВОЕЙ шине VSPI (TFT живёт на HSPI)
    touchBus.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, -1);
    touch.begin(touchBus);
    touch.setRotation(0);   // работаем с сырыми координатами
}

void displayRefreshStatic() {
    lastDrawnPage = -1;   // перерисовать статику на следующем обновлении
}

void displayUpdate(const PCData& d, const History& h, bool online) {
    bool editing = (uiPage == 4 && editSlot >= 0);
    bool pageNew = (uiPage != lastDrawnPage);
    if (pageNew) {
        // Полная очистка экрана при смене страницы: статические отрисовки
        // рисуют только свои рамки/кнопки, и без очистки за новой страницей
        // остаётся «мусор» от предыдущей.
        tft.fillScreen(TFT_BLACK);
        lastDrawnPage = uiPage;
        if (editing) {
            editDrawAll();
        } else {
            if (uiPage == 0) dashStatic();
            else if (uiPage == 1) graphStatic();
            else if (uiPage == 2) ctrlStatic();
            else if (uiPage == 3) coresStatic();
            else if (uiPage == 4) keysStatic();
            else if (uiPage == 5) mediaStatic();
            else if (uiPage == 6) appsStatic();
            else if (uiPage == 7) procStatic();
            drawTabs();
        }
    }
    // Динамика рисуется ТОЛЬКО когда содержимое реально изменилось.
    // Статичные страницы (CTRL, MEDIA, KEYS-список, редактор) после отрисовки
    // не трогаем вовсе — это убирает мерцание целиком.
    switch (uiPage) {
        case 0: if (pageNew || dashNeedsRedraw(d, online)) dashDynamic(d); break;
        case 1:
            // Плавный скролл: полная отрисовка только при входе на страницу,
            // дальше каждый новый сэмпл лишь сдвигает кривые на шаг влево
            // (стирание прошлого кадра + отрисовка текущего, без очистки полотна)
            if (pageNew) {
                graphRedrawFull(h);
            } else if (graphNeedsRedraw(h)) {
                graphScroll(h);
            }
            break;
        case 2: break;
        case 3: if (pageNew || coresNeedRedraw(d, h, online)) coresDynamic(d, h); break;
        case 4: {
            // KEYS: статика + перерисовка по refresh/tap. В редакторе гасим
            // красную подсказку "pick a key first!" по таймеру (раньше это
            // делала частая перерисовка всего редактора).
            if (editing && editFlashUntil != 0 &&
                (int32_t)(millis() - editFlashUntil) >= 0) {
                editFlashUntil = 0;
                editDrawAll();
            }
            break;
        }
        case 5: if (pageNew || mediaNeedsRedraw(d)) mediaDynamic(d); break;
        case 6: break;   // APPS — статика
        case 7: procDynamic(d); break;   // PROCS — точечная перерисовка строк
    }
    // Шапка — только при смене страницы, ONLINE-статуса или секунды аптайма
    if (pageNew || online != sLastOn || (int)d.uptimeSec != sLastUp) {
        drawStatusRow(d, online);
        sLastOn = online;
        sLastUp = (int)d.uptimeSec;
    }
}

// Визуальный отклик: яркая рамка вокруг кнопки на страницах KEYS/APPS/MEDIA
// (их нажатие не меняет состояние экрана — без отклика тап «немой»).
// Рамку убирает ближайший displayUpdate после истечения gFlashUntil.
void displayPressFlash(int x, int y) {
    if (uiPage != 4 && uiPage != 5 && uiPage != 6) return;
    if (uiPage == 4 && editSlot >= 0) return;   // редактор рисует сам
    int fx = 0, fy = 0, fw = 0, fh = 0;
    if (uiPage == 4 || uiPage == 6) {
        for (int i = 0; i < (uiPage == 4 ? HK_MAX : APP_MAX); i++) {
            int x0, y0; keyXY(i, x0, y0);
            if (x >= x0 && x < x0 + kKeyBW && y >= y0 && y < y0 + kKeyBH) {
                fx = x0; fy = y0; fw = kKeyBW; fh = kKeyBH;
                break;
            }
        }
    } else {
        for (int i = 0; i < kMediaCols * 2; i++) {
            int x0, y0; mediaXY(i, x0, y0);
            if (x >= x0 && x < x0 + kMediaBW && y >= y0 && y < y0 + kMediaBH) {
                fx = x0; fy = y0; fw = kMediaBW; fh = kMediaBH;
                break;
            }
        }
    }
    if (!fw || !fh) return;
    // Двойная яркая рамка — заметно даже на светлых кнопках
    tft.drawRoundRect(fx - 1, fy - 1, fw + 2, fh + 2, 9, TFT_WHITE);
    tft.drawRoundRect(fx, fy, fw, fh, 8, TFT_YELLOW);
    gFlashUntil = millis() + 130;
    gFlashPage = uiPage;
}

// Рамка «дожила» свой срок -> пора перерисовать страницу (снять подсветку)
bool displayFlashExpired() {
    if (gFlashUntil != 0 && (int32_t)(millis() - gFlashUntil) >= 0) {
        gFlashUntil = 0;
        gFlashPage = -1;
        displayRefreshStatic();   // следующий displayUpdate уберёт рамку
        return true;
    }
    return false;
}

UIAction displayHit(int x, int y) {
    // Модальный редактор клавиш (страница KEYS, долгое удержание):
    // табы и переключение страниц недоступны, пока не нажат SAVE/CANCEL.
    if (uiPage == 4 && editSlot >= 0) {
        if (y >= ED_BAR_Y - 2) {   // нижняя панель CANCEL/< >/SAVE
            if (x >= 8 && x < 72)  return UI_EDIT_CANCEL;
            if (x >= 84 && x < 120) return UI_EDIT_PG_PREV;
            if (x >= 124 && x < 160) return UI_EDIT_PG_NEXT;
            if (x >= 246)           return UI_EDIT_SAVE;
            return UI_NONE;
        }
        if (y >= ED_CHIP_Y0 && y < ED_CHIP_Y0 + ED_CHIP_H) {
            int m = (x - ED_CHIP_X0) / ED_CHIP_STEP;
            if (m < 0) m = 0; if (m > 3) m = 3;
            return (UIAction)(UI_EDIT_MOD_CTRL + m);
        }
        if (y >= ED_CAT_Y0 && y < ED_CAT_Y0 + ED_CHIP_H) {
            int m = (x - ED_CHIP_X0) / ED_CHIP_STEP;
            if (m < 0) m = 0; if (m > 3) m = 3;
            return (UIAction)(UI_EDIT_CAT_0 + m);
        }
        if (y >= ED_GRID_Y0 && y < ED_GRID_Y0 + kGridRows * ED_GRID_ROW) {
            int col = (x - ED_GRID_X0) / ED_GRID_STEP;
            int row = (y - ED_GRID_Y0) / ED_GRID_ROW;
            if (col < 0) col = 0; if (col >= kGridCols) col = kGridCols - 1;
            if (row < 0) row = 0; if (row >= kGridRows) row = kGridRows - 1;
            return (UIAction)(UI_EDIT_KEY_0 + row * kGridCols + col);
        }
        return UI_NONE;
    }
    // Табы внизу (y 217..239). Небольшой допуск вверх (y >= 211): если карта
    // калибровки чуть «сжата» по вертикали, нажатие на таб всё равно сработает.
    if (y >= 211) {
        int t = (x - 5) / (kTabW + kTabGap);
        if (t < 0) t = 0; if (t > kNumPages - 1) t = kNumPages - 1;
        return (UIAction)(UI_PAGE_DASH + t);
    }
    if (uiPage == 3) return UI_NONE;   // страница ядер — только табы

    // Страница клавиш: сетка 2x4
    if (uiPage == 4) {
        for (int i = 0; i < HK_MAX; i++) {
            int x0, y0; keyXY(i, x0, y0);
            if (x >= x0 && x < x0 + kKeyBW && y >= y0 && y < y0 + kKeyBH)
                return (UIAction)(UI_HK_0 + i);
        }
        return UI_NONE;
    }
    // Страница медиа: сетка 3x2
    if (uiPage == 5) {
        for (int i = 0; i < kMediaCols * 2; i++) {
            int x0, y0; mediaXY(i, x0, y0);
            if (x >= x0 && x < x0 + kMediaBW && y >= y0 && y < y0 + kMediaBH)
                return (UIAction)(UI_MEDIA_MUTE + i);
        }
        return UI_NONE;
    }
    // Страница запуска приложений: сетка 2x4 (та же, что у KEYS)
    if (uiPage == 6) {
        for (int i = 0; i < APP_MAX; i++) {
            int x0, y0; keyXY(i, x0, y0);
            if (x >= x0 && x < x0 + kKeyBW && y >= y0 && y < y0 + kKeyBH)
                return (UIAction)(UI_APP_0 + i);
        }
        return UI_NONE;
    }
    // Страница процессов: зона KILL справа в каждом ряду
    if (uiPage == 7) {
        for (int i = 0; i < kProcRows; i++) {
            int ry = procRowY(i);
            if (y >= ry && y < ry + kProcRowH) {
                if (x >= kProcKillX && x < kProcKillX + kProcKillW)
                    return (UIAction)(UI_PROC_KILL_0 + i);
                return UI_NONE;
            }
        }
        return UI_NONE;
    }
    if (uiPage != 2) return UI_NONE;

    // реле (2 кнопки по 150 px)
    if (y >= 40 && y < 80) {
        int r = (x - 8) / 154;
        if (r >= 0 && r < NUM_RELAYS) return (UIAction)(UI_RELAY_1 + r);
    }
    // AUTO / ALARM / TOUCH
    if (y >= 90 && y < 120) {
        int t = (x - 8) / 102;
        if (t < 0) t = 0;
        if (t > 2) t = 2;
        if (t == 0) return UI_AUTO;
        if (t == 1) return UI_ALARM;
        return UI_CALIBRATE;
    }
    // команды ПК
    if (y >= 136 && y < 176) {
        int c = (x - 8) / 102;
        if (c >= 0 && c < 3) return (UIAction)(UI_CMD_SLEEP + c);
    }
    return UI_NONE;
}
