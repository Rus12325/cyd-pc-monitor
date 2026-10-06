#pragma once
// ============================================================
// Данные ПК и кольцевой буфер истории для графиков
// ============================================================
#include <Arduino.h>

struct PCData {
    // Датчики (NaN = данные не пришли)
    float cpuTemp   = NAN;   // град C
    float gpuTemp   = NAN;
    float cpuLoad   = NAN;   // 0..100 %
    float gpuLoad   = NAN;
    float cpuFreqMHz= NAN;
    float gpuFreqMHz= NAN;
    float cpuPowerW = NAN;
    float gpuPowerW = NAN;
    float ramPct    = NAN;
    float ramTotalGB= NAN;
    float fan1Rpm   = NAN;
    float fan2Rpm   = NAN;
    float netDnMBps = NAN;
    float netUpMBps = NAN;
    float v12Volt   = NAN;
    float diskPct   = NAN;    // занятость системного диска, %
    float diskFreeGB= NAN;    // свободно на системном диске, ГБ
    uint32_t uptimeSec = 0;   // аптайм ПК

    // Температуры/нагрузка по ядрам CPU (c0t.. / c0l..), NaN = нет данных.
    enum { MAX_CORES = 8 };
    float coreTemp[MAX_CORES];
    float coreLoad[MAX_CORES];
    int   coreCount = 0;      // сколько ядер реально пришло
    float sysTemp   = NAN;    // температура материнки (SYS, из LHM)

    // Медиа (строка MNOW:vol|title): громкость Windows (отрицательная = mute)
    float volume  = NAN;
    char  mediaTrack[52];

    // Топ-5 процессов (строка TOP:): имя | cpu% | память МБ | pid
    enum { PROC_TOP = 5 };
    char    procName[PROC_TOP][21];
    float   procCpu[PROC_TOP];
    float   procMem[PROC_TOP];
    int32_t procPid[PROC_TOP];
    int     procCount = 0;

    bool   hasData  = false;  // хоть одно поле получено
    uint32_t lastUpdate = 0;  // millis() ESP при последнем обновлении

    PCData() {
        for (int i = 0; i < MAX_CORES; i++) { coreTemp[i] = NAN; coreLoad[i] = NAN; }
        mediaTrack[0] = 0;
        for (int i = 0; i < PROC_TOP; i++) {
            procName[i][0] = 0;
            procCpu[i] = NAN;
            procMem[i] = NAN;
            procPid[i] = 0;
        }
    }

    void reset() { *this = PCData(); }
};

// Кольцевой буфер: 120 сэмплов @ 1 c = 120 c истории
struct History {
    static const int N = 120;
    float cpuTemp[N], gpuTemp[N], cpuLoad[N], gpuLoad[N];
    float ramPct[N], diskPct[N];
    float coreAvgTemp[N];     // средняя температура по ядрам
    // История каждого ядра (для спарклайнов на странице CORES)
    float coreHist[PCData::MAX_CORES][N];
    int idx = 0, count = 0;

    void push(const PCData& d);
    // k=0 — последний сэмпл, k>0 — назад в прошлое
    float at(const float* arr, int k) const {
        if (k >= count) return NAN;
        return arr[(idx - 1 - k + N) % N];
    }
    // k=0 — последний сэмпл ядра c
    float coreAt(int c, int k) const {
        if (c < 0 || c >= PCData::MAX_CORES || k >= count) return NAN;
        return coreHist[c][(idx - 1 - k + N) % N];
    }
};

// Ограничение с учётом NaN (NaN -> lo)
static inline float clampf(float v, float lo, float hi) {
    if (isnan(v)) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
