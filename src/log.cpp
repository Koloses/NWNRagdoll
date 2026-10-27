#include "log.h"
#include <windows.h>
#include <cstdio>
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_cs;
static ULONGLONG g_flushed = 0;
void Log_Init(const char* path) {
    InitializeCriticalSection(&g_cs);
    g_log = fopen(path, "w");
    if (g_log) setvbuf(g_log, nullptr, _IOFBF, 1 << 16);
}
void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    ULONGLONG now = GetTickCount64();                     // to disk at most once a second, not per line
    if (now - g_flushed >= 1000) { fflush(g_log); g_flushed = now; }
    LeaveCriticalSection(&g_cs);
}
void Log_Flush() {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    if (GetTickCount64() - g_flushed >= 1000) { fflush(g_log); g_flushed = GetTickCount64(); }
    LeaveCriticalSection(&g_cs);
}
