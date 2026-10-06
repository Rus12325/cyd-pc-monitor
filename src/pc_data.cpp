#include "pc_data.h"

void History::push(const PCData& d) {
    cpuTemp[idx] = d.cpuTemp;
    gpuTemp[idx] = d.gpuTemp;
    cpuLoad[idx] = d.cpuLoad;
    gpuLoad[idx] = d.gpuLoad;
    ramPct[idx]  = d.ramPct;
    diskPct[idx] = d.diskPct;
    // средняя температура по ядрам (для графика)
    if (d.coreCount > 0) {
        float s = 0; int n = 0;
        for (int i = 0; i < d.coreCount; i++)
            if (!isnan(d.coreTemp[i])) { s += d.coreTemp[i]; n++; }
        coreAvgTemp[idx] = n ? s / n : NAN;
    } else {
        coreAvgTemp[idx] = NAN;
    }
    // История каждого ядра; если данных по ядру нет (офлайн/меньше ядер) —
    // пишем NaN, чтобы на спарклайне была "дыра", а не ноль.
    for (int c = 0; c < PCData::MAX_CORES; c++) {
        coreHist[c][idx] = (c < d.coreCount) ? d.coreTemp[c] : NAN;
    }
    idx = (idx + 1) % N;
    if (count < N) count++;
}
