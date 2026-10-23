#include "game.h"
#include "log.h"
#include <MinHook.h>
#include <windows.h>
#include <cmath>

namespace G {
uintptr_t base = 0;
static void** g_ppAppManager = nullptr;   // CAppManager** (address of the exported global)

bool Init() {
    base = (uintptr_t)GetModuleHandleA(nullptr);
    g_ppAppManager = (void**)GetProcAddress((HMODULE)base, "?g_pAppManager@@3PEAVCAppManager@@EA");
    Log("image base %p, g_pAppManager @ %p", (void*)base, g_ppAppManager);
    return g_ppAppManager != nullptr;
}

void* App() {
    if (!g_ppAppManager || !*g_ppAppManager) return nullptr;
    return *(void**)(*g_ppAppManager);          // CAppManager->m_pClientExoApp
}
void* Internal() {
    void* a = App();
    return a ? F<void*>(a, 8) : nullptr;
}
// Loading a game or module tears the object tables down and rebuilds them while the loading screen
// keeps rendering (and animating): lookups then read freed memory. No lookups while the server loads,
// nor for a moment after.
static volatile LONG g_loadDepth = 0;
static volatile ULONGLONG g_loadEnd = 0;
typedef int (*LoadFn_t)(void*, void*, void*, void*, void*, void*, void*);
static LoadFn_t o_LoadGame = nullptr, o_LoadModule = nullptr;     // 5 and 7 register/stack-sized arguments
static int hk_LoadGame(void* a, void* b, void* c, void* d, void* e, void* f, void* g) {
    InterlockedIncrement(&g_loadDepth);
    int r = o_LoadGame(a, b, c, d, e, f, g);
    g_loadEnd = GetTickCount64(); InterlockedDecrement(&g_loadDepth);
    return r;
}
static int hk_LoadModule(void* a, void* b, void* c, void* d, void* e, void* f, void* g) {
    InterlockedIncrement(&g_loadDepth);
    int r = o_LoadModule(a, b, c, d, e, f, g);
    g_loadEnd = GetTickCount64(); InterlockedDecrement(&g_loadDepth);
    return r;
}
void InstallLoadGuard() {
    HMODULE exe = GetModuleHandleA(nullptr);
    void* lg = (void*)GetProcAddress(exe, "?LoadGame@CServerExoAppInternal@@QEAAHIAEAVCExoString@@0PEAVCNWSPlayer@@@Z");
    void* lm = (void*)GetProcAddress(exe, "?LoadModule@CServerExoAppInternal@@QEAAHVCExoString@@UCUUID@@HPEAVCNWSPlayer@@HAEBUAdvertisement@NWSync@@@Z");
    if (lg && MH_CreateHook(lg, (void*)hk_LoadGame, (void**)&o_LoadGame) == MH_OK) MH_EnableHook(lg); else o_LoadGame = nullptr;
    if (lm && MH_CreateHook(lm, (void*)hk_LoadModule, (void**)&o_LoadModule) == MH_OK) MH_EnableHook(lm); else o_LoadModule = nullptr;
    Log("load guard: %s", o_LoadGame && o_LoadModule ? "ready" : "hooks failed");
}
bool Loading() {
    if (g_loadDepth > 0 || GetTickCount64() - g_loadEnd < 1500) return true;
    void* s = g_ppAppManager && *g_ppAppManager ? F<void*>(*g_ppAppManager, 8) : nullptr;   // CServerExoApp
    void* si = s ? F<void*>(s, 8) : nullptr;
    return si && Readable((char*)si + 0x10140, 4) && F<int>(si, 0x10140) != 0;             // GetLoadingModule
}
static bool ObjectsReady() {
    if (Loading()) return false;
    void* in = Internal();
    void* arr = in ? F<void*>(in, 0x28) : nullptr;
    if (!arr || !Readable(arr, 0x20)) return false;
    void* buckets = F<void*>(arr, 0);
    uint32_t n = F<uint32_t>(arr, 0x1c);
    return buckets && n != 0 && n <= (1u << 22) && Readable(buckets, (size_t)n * 8);
}
typedef void* (*fn_ptr1)(void*);
typedef void* (*fn_ptr2u)(void*, uint32_t);
void* Player() {
    void* in = Internal();
    if (!in || !ObjectsReady()) return nullptr;
    return ((fn_ptr1)A(VA::Internal_GetPlayerCreature))(in);
}
void* Module() {
    void* in = Internal();
    return in ? F<void*>(in, OFS::In_Module) : nullptr;
}
void* Camera() {
    void* m = Module();
    return m ? F<void*>(m, OFS::Mod_Camera) : nullptr;
}
void* GetObj(uint32_t id) {
    void* a = App();
    if (!a || id == 0x7f000000 || !ObjectsReady()) return nullptr;
    void* go = ((fn_ptr2u)A(VA::App_GetGameObject))(a, id);
    if (!go) return nullptr;
    return VCall<void* (*)(void*)>(go, OFS::V_AsNWCObject)(go);
}
void* PlayerArea() {
    void* p = Player();
    return p ? F<void*>(p, OFS::Obj_Area) : nullptr;
}
int HandleInputEvent(int id, int value) {
    void* in = Internal();
    if (!in) return 0;
    typedef int (*fn)(void*, int, int, int, int);
    return ((fn)A(VA::HandleInputEvent))(in, id, value, 0, 0);
}
int DefaultAction(void* obj) {
    void* in = Internal();
    if (!in || !obj) return 0;
    typedef int (*fn)(void*, void*);
    return ((fn)A(VA::PerformDefaultActionOnObject))(in, obj);
}
float CameraYaw() {
    void* c = Camera();
    if (!c) return 0.f;
    typedef float (*fn)(void*);
    return ((fn)A(VA::Camera_GetYaw))(c);
}
bool WorldToScreen(const Vec3& w, float& sx, float& sy) {
    void* c = Camera();
    if (!c) return false;
    typedef Vec3* (*fn)(void*, Vec3*, Vec3*, bool);
    Vec3 in = w, out = {0, 0, 0};
    ((fn)A(VA::Camera_MapToScreen))(c, &out, &in, true);
    sx = out.x; sy = out.y;          // normalized, origin top-left
    return true;
}
void SetOrientation(void* cre, const Vec3& dir) {
    Vec3 d = dir;
    VCall<void (*)(void*, Vec3*)>(cre, OFS::V_SetOrientation)(cre, &d);
}
}

// MSVC RTTI: class name of a polymorphic object (e.g. "CPanelQuickBar"), or "" if unknown
namespace G {
static bool InImage(uintptr_t p) {
    static uintptr_t lo = 0, hi = 0;
    if (!lo) {
        lo = base;
        IMAGE_DOS_HEADER* d = (IMAGE_DOS_HEADER*)base;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(base + d->e_lfanew);
        hi = base + nt->OptionalHeader.SizeOfImage;
    }
    return p >= lo && p < hi;
}
bool Readable(const void* p, size_t n) {
    if (!p || (uintptr_t)p < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD pr = mbi.Protect & 0xff;
    if (!(pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}
const char* ClassName(void* obj) {
    static char buf[128];
    buf[0] = 0;
    if (!obj || ((uintptr_t)obj & 7) || !Readable(obj, 8)) return buf;
    uintptr_t vt = *(uintptr_t*)obj;
    if (!InImage(vt)) return buf;
    uintptr_t col = *(uintptr_t*)(vt - 8);
    if (!InImage(col)) return buf;
    int32_t tdRva = *(int32_t*)(col + 12);
    uintptr_t td = base + (uint32_t)tdRva;
    if (!InImage(td)) return buf;
    const char* n = (const char*)(td + 16);
    if (n[0] != '.' || n[1] != '?') return buf;
    const char* s = n + 4;   // skip ".?AV"
    int i = 0;
    while (s[i] && s[i] != '@' && i < 120) { buf[i] = s[i]; i++; }
    buf[i] = 0;
    return buf;
}
}
