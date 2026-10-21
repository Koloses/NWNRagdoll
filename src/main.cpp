// NWNRagdoll - client-side ragdoll physics for dying creatures in NWN:EE (Win64 8193.37)
#include <windows.h>
#include <cstdio>
#include <MinHook.h>
#include "log.h"
#include "game.h"
#include "ragdoll.h"

void Proxy_Load();
static char g_dir[MAX_PATH];

static bool SupportedBuild() {
    auto* d = (IMAGE_DOS_HEADER*)GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS64*)((char*)d + d->e_lfanew);
    return nt->FileHeader.TimeDateStamp == 0x6782a73f && nt->OptionalHeader.SizeOfImage == 0x1711000;
}

static void Init() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    if (_stricmp(base, "nwmain.exe") != 0) return;
    strcpy(g_dir, exe);
    *strrchr(g_dir, '\\') = 0;
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\NWNRagdoll.log", g_dir);
    Log_Init(path);
    Log("NWNRagdoll 0.1 (built " __DATE__ " " __TIME__ ")");
    if (!SupportedBuild()) { Log("unsupported nwmain.exe build - disabled"); return; }
    snprintf(path, sizeof path, "%s\\NWNRagdoll.ini", g_dir);
    Ragdoll_LoadConfig(path);
    if (!G::Init()) { Log("game globals not found"); return; }
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { Log("MinHook init failed"); return; }
    Ragdoll_Install();
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        Proxy_Load();
        Init();
    }
    return TRUE;
}
