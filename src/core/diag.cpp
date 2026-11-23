#include "core/diag.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace wv {

bool          g_perfOn = false;
PerfAcc       g_perfAcc;
LARGE_INTEGER g_perfFreq{};

LONGLONG PerfNow() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

double PerfMs(LONGLONG t) {
    return g_perfFreq.QuadPart ? (double)t * 1000.0 / (double)g_perfFreq.QuadPart : 0.0;
}

void PerfAppend(const char* line) {
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return;
    std::wstring f = std::wstring(dir) + L"winvclip_perf.log";
    if (FILE* fp = _wfopen(f.c_str(), L"a")) {
        std::fputs(line, fp);
        std::fclose(fp);
    }
}

void PerfInit() {
    // 环境变量在某些 shell 里 start 时不会传递给子进程，所以命令行参数优先
    const wchar_t* cl = GetCommandLineW();
    bool on = (GetEnvironmentVariableW(L"WINVCLIP_PERF", nullptr, 0) != 0);
    if (cl && wcsstr(cl, L"--perf")) on = true;
    if (!on) return;
    QueryPerformanceFrequency(&g_perfFreq);
    g_perfOn = g_perfFreq.QuadPart != 0;
}

void WarmLog(const char* fmt, int a, int b) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    PerfAppend(buf);
}

void Trace(const char* fmt, ...) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("WV_TRACE"); on = (e && *e && *e != '0') ? 1 : 0; }
    if (!on) return;
    wchar_t path[MAX_PATH];
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"winvclip-trace.txt");
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

}  // namespace wv
