// NWNRagdoll - blood pools under the dead
//
// The game's own floor blood models (dag_floorblood1..6) are placed on the walkmesh where a body comes
// to rest and spread out over a few seconds like a liquid finding its level. They stay until you leave
// the area. Only red-blooded creatures bleed (appearance.2da BLOODCOLR).
#include "blood.h"
#include "game.h"
#include "log.h"
#include "phys.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace G;

namespace {
struct Cfg {
    int enabled = 1;
    int max = 48;                // pools in one area
    float size = 1.0f;           // scale of a full pool
} C;

typedef void* (*NewAur_t)(const char* model, const char* tag, void* file);
typedef void (*SetScene_t)(void* gob, void* scene);
typedef void (*FinalScale_t)(void* gob);
typedef void (*StrCtor_t)(void* s, const char* c);
typedef void (*StrDtor_t)(void* s);
typedef int (*Entry_t)(void* tda, int row, const void* col, void* out);
NewAur_t fNewAur = nullptr;
SetScene_t fSetScene = nullptr;
FinalScale_t fFinalScale = nullptr;
StrCtor_t fStr = nullptr;
StrDtor_t fStrFree = nullptr;
Entry_t fEntry = nullptr;
void** pRules = nullptr;

struct Pool {
    void* gob;
    V3 p; Q4 q;
    float from, to;              // model scale
    double t0; float seconds;
};
std::vector<Pool> g_pools;
void* g_area = nullptr;
size_t g_next = 0;              // oldest pool, reused when the area is full

float Ease(float x) { x = fminf(fmaxf(x, 0.0f), 1.0f); return 1.0f - (1.0f - x) * (1.0f - x) * (1.0f - x); }

void SetScale(void* gob, float s) {
    F<float>(gob, 0x4a0) = s;
    if (fFinalScale) fFinalScale(gob);
}
}  // namespace

char Blood_Color(void* cre) {
    if (!cre || !pRules || !*pRules || !fStr || !fEntry) return 0;
    void* app = F<void*>(cre, 0x300);                     // CNWCCreatureAppearance
    if (!app || !Readable(app, 0xd0)) return 0;
    int row = F<uint16_t>(app, 0xcc);
    void* arrays = F<void*>(*pRules, 0x130);
    void* tda = arrays ? F<void*>(arrays, 0x100) : nullptr;  // appearance.2da
    if (!tda) return 0;
    struct ExoStr { char* p; uint32_t len, cap; } col{}, out{};
    fStr(&col, "BLOODCOLR");
    char r = 0;
    if (fEntry(tda, row, &col, &out) && out.p && out.p[0]) r = (char)toupper((unsigned char)out.p[0]);
    if (fStrFree) { fStrFree(&col); fStrFree(&out); }
    return r;
}

void Blood_Spill(V3 p, float size, float seconds) {
    if (!C.enabled || !fNewAur || !fSetScene || !g_area) return;
    void* pg = Player() ? P::ObjectGob(Player()) : nullptr;
    void* scene = pg ? F<void*>(pg, 0x88) : nullptr;
    if (!scene) return;
    float h;
    if (P::GroundAt(g_area, p.x, p.y, h)) p.z = h;
    Pool* pl;
    if ((int)g_pools.size() < C.max) {
        char name[32];
        snprintf(name, sizeof name, "dag_floorblood%d", 1 + (int)P::Rand(0, 5.99f));
        void* g = fNewAur(name, "blood", nullptr);
        if (!g) { Log("blood: model %s did not load", name); return; }
        g_pools.push_back(Pool{g});
        pl = &g_pools.back();
    } else {
        pl = &g_pools[g_next % g_pools.size()];
        g_next++;
        fSetScene(pl->gob, nullptr);
    }
    float yaw = P::Rand(0, 6.2832f);
    pl->p = V3{p.x, p.y, p.z + 0.015f + P::Rand(0, 0.006f)};       // pools overlap: no two at the same height
    pl->q = Q4{cosf(yaw * 0.5f), 0, 0, sinf(yaw * 0.5f)};
    pl->to = size * C.size * P::Rand(0.85f, 1.15f);
    pl->from = pl->to * 0.12f;
    pl->t0 = P::Now(); pl->seconds = seconds;
    P::SetGobTransform(pl->gob, pl->p, pl->q);
    SetScale(pl->gob, pl->from);
    fSetScene(pl->gob, scene);
}

void Blood_Update(void* area) {
    if (area != g_area) {
        g_pools.clear();                                    // the old scene took its models along
        g_next = 0;
        g_area = area;
    }
    double now = P::Now();
    for (auto& pl : g_pools) {
        float x = pl.seconds > 0 ? (float)((now - pl.t0) / pl.seconds) : 1.0f;
        if (x > 1.05f) continue;
        SetScale(pl.gob, pl.from + (pl.to - pl.from) * Ease(x));
        P::SetGobTransform(pl.gob, pl.p, pl.q);
    }
}

void Blood_LoadConfig(const char* ini) {
    C.enabled = GetPrivateProfileIntA("Blood", "Enabled", 1, ini);
    C.max = GetPrivateProfileIntA("Blood", "MaxPools", 48, ini);
    char b[32]; GetPrivateProfileStringA("Blood", "PoolSize", "", b, sizeof b, ini);
    if (b[0]) C.size = (float)atof(b);
    if (C.max < 1) C.max = 1;
    Log("blood: enabled %d max %d size %.2f", C.enabled, C.max, C.size);
}

void Blood_Install() {
    HMODULE exe = GetModuleHandleA(nullptr);
    fNewAur = (NewAur_t)GetProcAddress(exe, "?NewCAurObject@@YAPEAVCAurObject@@PEBD0PEAU_iobuf@@@Z");
    fSetScene = (SetScene_t)GetProcAddress(exe, "?SetScene@Gob@@QEAAXPEAVScene@@@Z");
    fFinalScale = (FinalScale_t)GetProcAddress(exe, "?CalculateFinalScale@Gob@@QEAAXXZ");
    fStr = (StrCtor_t)GetProcAddress(exe, "??0CExoString@@QEAA@PEBD@Z");
    fStrFree = (StrDtor_t)GetProcAddress(exe, "??1CExoString@@QEAA@XZ");
    fEntry = (Entry_t)GetProcAddress(exe, "?GetCExoStringEntry@C2DA@@QEAAHHAEBVCExoString@@PEAV2@@Z");
    pRules = (void**)GetProcAddress(exe, "?g_pRules@@3PEAVCNWRules@@EA");
    if (!fNewAur || !fSetScene || !fFinalScale) { Log("blood: exports missing - off"); C.enabled = 0; }
}
