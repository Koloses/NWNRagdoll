#pragma once
#include <cstdarg>
void Log_Init(const char* path);
void Log(const char* fmt, ...);
void Log_Flush();                // once per frame: writes out what waited
