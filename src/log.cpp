#include "log.h"
#include <windows.h>
#include <cstdio>
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_cs;
void Log_Init(const char* path) {
    InitializeCriticalSection(&g_cs);
    g_log = fopen(path, "w");
}
void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_cs);
}
