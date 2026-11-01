// NWNRagdoll - dismemberment
//
// A killing blow that is a critical hit with a slashing weapon (swords, axes, claws ...), or one heavy
// enough to take half the victim's hit points at once, may cut off a limb or the head: the game's
// rules decide the blow, the mod only shows it. The server's combat round says what each blow was
// (single player: the server runs in this process), noted as the blow is resolved. The ragdoll then lets the cut bone
// fly free; the blood is the game's own combat blood effects (visualeffects.2da VFX_COM_BLOOD_*,
// VFX_COM_CHUNK_*), played at the wound.
#include "gore.h"
#include "game.h"
#include "log.h"
#include "phys.h"
#include <windows.h>
#include "MinHook.h"
#include <cstring>
#include <cstdio>
#include <map>
#include <string>

using namespace G;

namespace {
struct Cfg {
    int enabled = 1;
    float critChance = 1.0f;     // a critical hit with a slashing weapon
    float heavyChance = 0.5f;    // a slashing blow of half the victim's hit points or more
    float headShare = 0.35f;     // of cuts: the head
    float armShare = 0.45f;      //          an arm (the rest: a leg)
    int spurts = 1;              // the wound pulses blood for a moment
} C;

// CNWCArea::ApplyFireAndForgetVisualEffect(area, vfx, Vector*, holder*) - the holder is a fresh map head
// the function takes over (what its callers build with 0x1403668b0)
typedef int (*ApplyFF_t)(void* area, uint16_t vfx, const V3* pos, void* holder);
typedef void* (*MakeHead_t)(void* holder);
ApplyFF_t fApplyFF = nullptr;
MakeHead_t fMakeHead = nullptr;
typedef void* (*GetWeapon_t)(void* round, int type);
GetWeapon_t fAttackWeapon = nullptr;
typedef void* (*GetBaseItem_t)(void* arr, int id);
GetBaseItem_t fBaseItem = nullptr;
void** pRules = nullptr;


int VfxId(int kind, char blood) {
    // kind: 0 small, 1 large, 2 critical spurt, 3 chunks
    int col = blood == 'G' ? 1 : blood == 'Y' ? 2 : 0;
    static const int SP[3][3] = {{109, 110, 111}, {112, 113, 114}, {115, 116, 117}};
    static const int CH[3] = {122, 124, 126};
    if (kind == 3) return CH[col];
    return SP[kind < 0 ? 0 : kind > 2 ? 2 : kind][col];
}
void Fire(int vfx, V3 p) {
    void* area = PlayerArea();
    if (!area || !fApplyFF || !fMakeHead) return;
    struct { void* head; size_t size; } holder{nullptr, 0};
    holder.head = fMakeHead(&holder);
    fApplyFF(area, (uint16_t)vfx, &p, &holder);
}
}  // namespace

bool Gore_Enabled() { return C.enabled != 0; }

void Gore_Spurt(V3 p, int size, char blood) {
    if (!C.enabled || blood == 'N' || blood == 'W') return;
    if (size > 0 || C.spurts) Fire(VfxId(size, blood), p);
}
void Gore_Chunks(V3 p, char blood) {
    if (!C.enabled || blood == 'N' || blood == 'W') return;
    Fire(VfxId(3, blood), p);
}

namespace {

// the last blow each creature took (server side, as the attack is resolved: the combat round is reset
// soon after, before the client shows the death)
struct Blow { uint32_t attacker; int result, dmg, baseItem, wtype; double t; };
std::map<uint32_t, Blow> g_blows;
CRITICAL_SECTION g_cs;

typedef void (*PostDamage_t)(void* attacker, void* target);
PostDamage_t o_PostMelee = nullptr, o_PostRanged = nullptr;

void Record(void* attacker, void* target) {
    if (!attacker || !target) return;
    uint32_t tid = F<uint32_t>(target, 8);                               // CGameObject::m_idSelf
    void* round = Readable(attacker, 0x888) ? F<void*>(attacker, 0x880) : nullptr;   // m_pcCombatRound
    if (!round || !Readable(round, 50 * 0xf0)) return;
    // the latest resolved attack at this target: one that did damage (-1: none of that type)
    const char* ad = nullptr;
    auto Dmg = [](const char* a) { int d = 0; for (int k = 0; k < 32; k++) { short v = F<short>(a, 0x16 + 2 * k); if (v > 0) d += v; } return d; };
    for (int i = 0; i < 50; i++) {
        const char* a = (const char*)round + i * 0xf0;                  // CNWSCombatAttackData
        if (F<uint32_t>(a, 4) == tid && (Dmg(a) > 0 || F<int>(a, 0x68))) ad = a;
    }
    if (!ad) return;
    Blow b{};
    b.attacker = F<uint32_t>(attacker, 8);
    b.result = F<uint8_t>(ad, 0x78);
    b.dmg = Dmg(ad);
    b.baseItem = -1;
    void* item = fAttackWeapon ? fAttackWeapon(round, F<uint8_t>(ad, 0x56)) : nullptr;
    if (item && Readable(item, 0x48)) {
        b.baseItem = F<int>(item, 0x40);
        void* arr = pRules && *pRules ? F<void*>(*pRules, 0x20) : nullptr;
        void* bi = arr && fBaseItem ? fBaseItem(arr, b.baseItem) : nullptr;
        if (bi && Readable(bi, 0x10)) b.wtype = F<uint8_t>(bi, 9);
    }
    b.t = P::Now();
    EnterCriticalSection(&g_cs);
    g_blows[tid] = b;
    LeaveCriticalSection(&g_cs);
}
void hk_PostMelee(void* a, void* t) { o_PostMelee(a, t); Record(a, t); }
void hk_PostRanged(void* a, void* t) { o_PostRanged(a, t); Record(a, t); }
}  // namespace

const char* Gore_Cut(void* cre) {
    if (!C.enabled || !cre) return nullptr;
    uint32_t id = Id(cre);
    Blow b{};
    bool have = false;
    EnterCriticalSection(&g_cs);
    auto it = g_blows.find(id & 0x7fffffffu);
    if (it != g_blows.end()) { b = it->second; have = true; g_blows.erase(it); }
    LeaveCriticalSection(&g_cs);
    if (!have || P::Now() - b.t > 6.0) return nullptr;
    bool crit = b.result == 3 || b.result == 10;                         // AttackResult: critical, devastating
    void* st = F<void*>(cre, OFS::Cre_Stats);
    int maxHp = st ? F<short>(st, OFS::Stats_MaxHP) : 0;
    bool slashing = b.wtype == 3 || b.wtype == 4;                        // baseitems WeaponType slashing, piercing-slashing
    bool heavy = maxHp > 0 && b.dmg * 2 >= maxHp;
    float chance = !slashing ? 0.0f : crit ? C.critChance : heavy ? C.heavyChance : 0.0f;
    float roll = P::Rand(0, 1);
    if (roll >= chance) return nullptr;
    float r = P::Rand(0, 1);
    bool left = P::Rand(0, 1) < 0.5f;
    if (r < C.headShare) return "head_g";
    if (r < C.headShare + C.armShare) return P::Rand(0, 1) < 0.6f ? (left ? "lforearm_g" : "rforearm_g") : (left ? "lbicep_g" : "rbicep_g");
    return P::Rand(0, 1) < 0.6f ? (left ? "lshin_g" : "rshin_g") : (left ? "lthigh_g" : "rthigh_g");
}

void Gore_LoadConfig(const char* ini) {
    auto f = [&](const char* k, float def) { char b[32]; GetPrivateProfileStringA("Gore", k, "", b, sizeof b, ini); return b[0] ? (float)atof(b) : def; };
    C.enabled = GetPrivateProfileIntA("Gore", "Dismemberment", 1, ini);
    C.critChance = f("CriticalChance", 1.0f);
    C.heavyChance = f("HeavyBlowChance", 0.5f);
    C.headShare = f("Heads", 0.35f);
    C.armShare = f("Arms", 0.45f);
    C.spurts = GetPrivateProfileIntA("Gore", "Spurts", 1, ini);
    Log("gore: dismemberment %d (critical %.2f, heavy %.2f; heads %.2f arms %.2f) spurts %d", C.enabled, C.critChance, C.heavyChance,
        C.headShare, C.armShare, C.spurts);
}

void Gore_Install() {
    HMODULE exe = GetModuleHandleA(nullptr);
    fApplyFF = (ApplyFF_t)A(0x1407d8f60);
    fMakeHead = (MakeHead_t)A(0x1403668b0);
    fAttackWeapon = (GetWeapon_t)GetProcAddress(exe, "?GetCurrentAttackWeapon@CNWSCombatRound@@QEAAPEAVCNWSItem@@H@Z");
    fBaseItem = (GetBaseItem_t)GetProcAddress(exe, "?GetBaseItem@CNWBaseItemArray@@QEBAPEAVCNWBaseItem@@H@Z");
    pRules = (void**)GetProcAddress(exe, "?g_pRules@@3PEAVCNWRules@@EA");
    // the two unexported functions are checked by their first bytes
    const uint8_t ff[] = {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x56, 0x57, 0x41, 0x56};
    const uint8_t mh[] = {0x48, 0x89, 0x4c, 0x24, 0x08, 0x48, 0x83, 0xec, 0x38};
    if (memcmp((void*)fApplyFF, ff, sizeof ff) != 0 || memcmp((void*)fMakeHead, mh, sizeof mh) != 0) {
        Log("gore: effect functions not found - no blood effects");
        fApplyFF = nullptr;
    }
    InitializeCriticalSection(&g_cs);
    void* pm = (void*)GetProcAddress(exe, "?ResolvePostMeleeDamage@CNWSCreature@@IEAAXPEAVCNWSObject@@@Z");
    void* pr = (void*)GetProcAddress(exe, "?ResolvePostRangedDamage@CNWSCreature@@IEAAXPEAVCNWSObject@@@Z");
    bool hm = pm && MH_CreateHook(pm, (void*)hk_PostMelee, (void**)&o_PostMelee) == MH_OK && MH_EnableHook(pm) == MH_OK;
    bool hr = pr && MH_CreateHook(pr, (void*)hk_PostRanged, (void**)&o_PostRanged) == MH_OK && MH_EnableHook(pr) == MH_OK;
    Log("gore: blow hooks melee %d ranged %d", hm, hr);
    Log("gore: effects %p weapon %p baseitem %p rules %p", (void*)fApplyFF, (void*)fAttackWeapon, (void*)fBaseItem, (void*)pRules);
}
