#include "protocol.h"
#include "hotkey.h"
#include <string.h>

bool protocolParseLine(const String& line, PCData& d) {
    bool any = false;
    int pos = 0;
    const int len = line.length();

    while (pos < len) {
        int eq = line.indexOf('=', pos);
        if (eq < 0) break;
        int comma = line.indexOf(',', eq);
        if (comma < 0) comma = len;

        String k = line.substring(pos, eq);
        String v = line.substring(eq + 1, comma);
        k.trim();
        v.trim();
        if (k.length() > 0 && v.length() > 0) {
            // toFloat() возвращает 0 для нечисловой строки ("abc" -> 0),
            // а 0 — валидное значение (напр. fan=0), поэтому требуем,
            // чтобы первый символ был цифрой/знаком/точкой.
            char c0 = v.charAt(0);
            if (isdigit((unsigned char)c0) || c0 == '-' || c0 == '+' || c0 == '.') {
                float val = v.toFloat();
                any = true;
                if      (k == "cpu_temp")    d.cpuTemp    = val;
                else if (k == "gpu_temp")    d.gpuTemp    = val;
                else if (k == "cpu_load")    d.cpuLoad    = val;
                else if (k == "gpu_load")    d.gpuLoad    = val;
                else if (k == "cpu_freq")    d.cpuFreqMHz = val;
                else if (k == "gpu_freq")    d.gpuFreqMHz = val;
                else if (k == "cpu_power")   d.cpuPowerW  = val;
                else if (k == "gpu_power")   d.gpuPowerW  = val;
                else if (k == "ram_pct")     d.ramPct     = val;
                else if (k == "ram_total_gb")d.ramTotalGB = val;
                else if (k == "fan1")        d.fan1Rpm    = val;
                else if (k == "fan2")        d.fan2Rpm    = val;
                else if (k == "net_dn")      d.netDnMBps  = val;
                else if (k == "net_up")      d.netUpMBps  = val;
                else if (k == "v12")         d.v12Volt    = val;
                else if (k == "disk_pct")    d.diskPct    = val;
                else if (k == "disk_free_gb")d.diskFreeGB = val;
                else if (k == "sys_temp")    d.sysTemp    = val;
                else if (k == "uptime")      d.uptimeSec  = (uint32_t)val;
                else if (k == "c0t" || k == "c1t" || k == "c2t" || k == "c3t" ||
                         k == "c4t" || k == "c5t" || k == "c6t" || k == "c7t") {
                    int ci = k.charAt(1) - '0';
                    d.coreTemp[ci] = val;
                    if (ci + 1 > d.coreCount) d.coreCount = ci + 1;
                }
                else if (k == "c0l" || k == "c1l" || k == "c2l" || k == "c3l" ||
                         k == "c4l" || k == "c5l" || k == "c6l" || k == "c7l") {
                    int ci = k.charAt(1) - '0';
                    d.coreLoad[ci] = val;
                }
            }
        }
        pos = comma + 1;
    }
    if (any) {
        d.hasData = true;
        d.lastUpdate = millis();
    }
    return any;
}

void protocolSendCommand(const char* name) {
    Serial.print("CMD:");
    Serial.println(name);
}

void protocolSendMedia(const char* action) {
    Serial.print("MEDIA:");
    Serial.println(action);
}

void protocolSendKey(int index) {
    Serial.print("KEY:");
    Serial.println(index);
}

void protocolSendHkCfg(int index) {
    if (index < 0 || index >= HK_MAX) return;
    const Hotkey& h = hkGet(index);
    Serial.print("HKCFG:");
    Serial.print(index);
    Serial.print('=');
    Serial.print(h.label);
    Serial.print('|');
    Serial.println(h.combo);
}

void protocolSendApp(int index) {
    Serial.print("APP:");
    Serial.println(index);
}

void protocolParseMediaNow(const String& line, PCData& d) {
    // line = "MNOW:<vol>|<track>" (или просто "MNOW:<vol>")
    String body = line.startsWith("MNOW:") ? line.substring(5) : line;
    int bar = body.indexOf('|');
    String v = bar < 0 ? body : body.substring(0, bar);
    v.trim();
    d.volume = NAN;
    if (v.length() && (isdigit((unsigned char)v.charAt(0)) || v.charAt(0) == '-'))
        d.volume = v.toFloat();
    String t = bar < 0 ? "" : body.substring(bar + 1);
    t.trim();
    t.replace("|", " ");
    if (t.length() >= sizeof(d.mediaTrack)) t = t.substring(0, sizeof(d.mediaTrack) - 2) + "…";
    strncpy(d.mediaTrack, t.c_str(), sizeof(d.mediaTrack) - 1);
    d.mediaTrack[sizeof(d.mediaTrack) - 1] = 0;
    d.hasData = true;
    d.lastUpdate = millis();
}

void protocolParseTop(const String& line, PCData& d) {
    // line = "TOP:<seg>;<seg>;..." где seg = "name|cpu|mem|pid" (всегда 5)
    String body = line.startsWith("TOP:") ? line.substring(4) : line;
    int count = 0;
    int pos = 0;
    while (count < PCData::PROC_TOP) {
        int semi = body.indexOf(';', pos);
        String seg = (semi < 0) ? body.substring(pos) : body.substring(pos, semi);
        seg.trim();
        if (seg.length()) {
            int b1 = seg.indexOf('|');
            int b2 = b1 < 0 ? -1 : seg.indexOf('|', b1 + 1);
            int b3 = b2 < 0 ? -1 : seg.indexOf('|', b2 + 1);
            String name = b1 < 0 ? "" : seg.substring(0, b1);
            String scpu = (b1 < 0 || b2 < 0) ? "" : seg.substring(b1 + 1, b2);
            String smem = (b2 < 0 || b3 < 0) ? "" : seg.substring(b2 + 1, b3);
            String spid = (b3 < 0) ? "" : seg.substring(b3 + 1);
            name.trim();
            strncpy(d.procName[count], name.c_str(), 20);
            d.procName[count][20] = 0;
            d.procCpu[count] = scpu.toFloat();
            d.procMem[count] = smem.toFloat();
            d.procPid[count] = (int32_t)spid.toInt();
            count++;
        }
        if (semi < 0) break;
        pos = semi + 1;
    }
    // лишние строки от прошлого кадра затираем
    for (int i = count; i < PCData::PROC_TOP; i++) {
        d.procName[i][0] = 0;
        d.procCpu[i] = NAN;
        d.procMem[i] = NAN;
        d.procPid[i] = 0;
    }
    d.procCount = count;
    d.hasData = true;
    d.lastUpdate = millis();
}

void protocolSendKill(long pid) {
    if (pid <= 0) return;
    Serial.print("KILL:");
    Serial.println(pid);
}
