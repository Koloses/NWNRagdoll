// NWNRagdoll - shared physics helpers: engine access, walkmesh ground, explosions
#include "phys.h"
#include "game.h"
#include "log.h"
#include <windows.h>
#include <MinHook.h>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include <map>

using namespace G;

namespace P {
namespace {
typedef void (*GetWorld_t)(void* part, V3* pos, Q4* q);
GetWorld_t fGetWorld = nullptr;
typedef void* (*GetCreature_t)(void* srv, uint32_t id);
GetCreature_t fSrvCreature = nullptr;
typedef void* (*GetGameObject_t)(void* srv, uint32_t id);
GetGameObject_t fSrvObject = nullptr;
typedef void (*SetPos_t)(void* obj, const V3* pos, int);
SetPos_t fSrvSetPosition = nullptr;
void** p_AppManager = nullptr;

typedef void* (*GetTile_t)(void* area, const V3* pos);
GetTile_t fGetTile = nullptr;               // CNWCArea::GetTile(Vector) 0x1407d9d00
typedef void (*Rot_t)(void* tile, float x, float y, float* ox, float* oy);
Rot_t fToCanonical = nullptr;
typedef int (*FindTri_t)(void* mesh, int type, int start, float x, float y, int skip);
FindTri_t fFindTri = nullptr;
typedef float (*Height_t)(void* mesh, const V3* p);
Height_t fMeshHeight = nullptr;

LARGE_INTEGER g_freq;
uint32_t g_rng = 0x9e3779b9u;
}  // namespace

double Now() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_freq.QuadPart;
}
float Rand(float a, float b) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return a + (b - a) * (float)(g_rng & 0xffffff) / (float)0xffffff;
}

bool Init() {
    HMODULE exe = GetModuleHandleA(nullptr);
    QueryPerformanceFrequency(&g_freq);
    g_rng ^= (uint32_t)GetTickCount();
    p_AppManager = (void**)GetProcAddress(exe, "?g_pAppManager@@3PEAVCAppManager@@EA");
    fToCanonical = (Rot_t)GetProcAddress(exe, "?RotateRealToCanonicalTile@CNWTile@@QEAAXMMPEAM0@Z");
    fFindTri = (FindTri_t)GetProcAddress(exe, "?FindTriangle@CNWTileSurfaceMesh@@QEAAHHHMMH@Z");
    fMeshHeight = (Height_t)GetProcAddress(exe, "?ComputeHeight@CNWTileSurfaceMesh@@QEAAMVVector@@@Z");
    fGetTile = (GetTile_t)A(0x1407d9d00);            // not exported (CNWCArea::GetTile)
    fGetWorld = (GetWorld_t)GetProcAddress(exe, "?GetWorld@Part@@UEAAXAEAVVector@@AEAVQuaternion@@@Z");
    fSrvCreature = (GetCreature_t)GetProcAddress(exe, "?GetCreatureByGameObjectID@CServerExoApp@@QEAAPEAVCNWSCreature@@I@Z");
    fSrvObject = (GetGameObject_t)GetProcAddress(exe, "?GetGameObject@CServerExoApp@@QEAAPEAVCGameObject@@I@Z");
    fSrvSetPosition = (SetPos_t)GetProcAddress(exe, "?SetPosition@CNWSObject@@QEAAXVVector@@H@Z");
    Log("engine: getworld %p srvcreature %p srvobject %p setpos %p ground %p %p %p", (void*)fGetWorld, (void*)fSrvCreature,
        (void*)fSrvObject, (void*)fSrvSetPosition, (void*)fToCanonical, (void*)fFindTri, (void*)fMeshHeight);
    return fGetWorld != nullptr;
}

void* ServerApp() { return p_AppManager && *p_AppManager ? F<void*>(*p_AppManager, 8) : nullptr; }
void* ServerCreature(uint32_t clientId) {
    void* s = ServerApp();
    if (!s || !fSrvCreature || !F<void*>(s, 8) || G::Loading()) return nullptr;
    return fSrvCreature(s, clientId & 0x7fffffffu);
}
void* ServerObject(uint32_t clientId) {
    void* s = ServerApp();
    if (!s || !fSrvObject || !F<void*>(s, 8) || G::Loading()) return nullptr;
    void* g = fSrvObject(s, clientId & 0x7fffffffu);
    return g ? VCall<void* (*)(void*)>(g, 0x20)(g) : nullptr;     // AsNWSObject
}
bool SetServerPosition(uint32_t clientId, V3 p) {
    void* o = ServerObject(clientId);
    if (!o || !fSrvSetPosition) return false;
    fSrvSetPosition(o, &p, 0);
    return true;
}

void GetWorld(void* part, V3& p, Q4& q) { fGetWorld(part, &p, &q); }
const char* PartName(void* p) { void* n = p ? F<void*>(p, 8) : nullptr; return n ? (const char*)n + 0x38 : ""; }
void* ObjectGob(void* obj) {
    void* ab = obj ? F<void*>(obj, 0x78) : nullptr;
    if (!ab || !Readable(ab, 0xd0)) return nullptr;
    void* g = F<void*>(ab, 0xc8);
    return g && strcmp(ClassName(g), "Gob") == 0 ? g : nullptr;
}
float GobScale(void* gob) { return VCall<float (*)(void*)>(gob, 0x2d0)(gob); }
void SetGobTransform(void* gob, V3 p, Q4 q) { F<V3>(gob, 0xa8) = p; F<Q4>(gob, 0xb4) = q; }

void* MdlTriMesh(void* part) {
    const char* cn = ClassName(part);
    if (!strstr(cn, "TriMesh") && !strstr(cn, "Skin") && !strstr(cn, "Dangly")) return nullptr;
    void* n = F<void*>(part, 8);
    return n && Readable(n, 0xf0) ? n : nullptr;
}
bool GobExtent(void* gob, V3& mn, V3& mx) {
    // from the part tree's own local transforms, so the result is in the gob's frame whatever the
    // gob's placement; gobs attached to its parts (weapon heads / hilts) are included at their part
    struct T { V3 p; Q4 q; float s; };
    std::map<void*, T> frames;
    bool any = false;
    mn = V3{1e9f, 1e9f, 1e9f}; mx = V3{-1e9f, -1e9f, -1e9f};
    struct G { void* gob; T base; };
    std::vector<G> gobs{{gob, T{V3{}, Q4{}, 1.0f}}};
    for (size_t gi = 0; gi < gobs.size() && gi < 8; gi++) {
        struct S { void* p; T parent; int depth; };
        std::vector<S> st{{F<void*>(gobs[gi].gob, 0x90), gobs[gi].base, 0}};
        int guard = 0;
        while (!st.empty() && guard++ < 300) {
            S s = st.back(); st.pop_back();
            if (!s.p || s.depth > 40 || !Readable(s.p, 0x58)) continue;
            T t;
            t.p = s.parent.p + Rot(s.parent.q, F<V3>(s.p, 0x10) * s.parent.s);
            t.q = s.parent.q * Norm(F<Q4>(s.p, 0x1c));
            t.s = s.parent.s * F<float>(s.p, 0x2c);
            if (!(t.s > 0.001f && t.s < 100.0f)) t.s = s.parent.s;
            frames[s.p] = t;
            void** ch = F<void**>(s.p, 0x38); int n = F<int>(s.p, 0x40);
            for (int i = 0; ch && i < n && i < 64; i++) st.push_back({ch[i], t, s.depth + 1});
            void* m = MdlTriMesh(s.p);
            if (!m) continue;
            V3 bmin = F<V3>(m, 0xc0), bmax = F<V3>(m, 0xcc);
            if (!(bmin.x <= bmax.x && bmin.y <= bmax.y && bmin.z <= bmax.z) || Len(bmax - bmin) > 20.0f) continue;
            for (int c = 0; c < 8; c++) {
                V3 corner{c & 1 ? bmax.x : bmin.x, c & 2 ? bmax.y : bmin.y, c & 4 ? bmax.z : bmin.z};
                V3 l = t.p + Rot(t.q, corner * t.s);
                mn = V3{fminf(mn.x, l.x), fminf(mn.y, l.y), fminf(mn.z, l.z)};
                mx = V3{fmaxf(mx.x, l.x), fmaxf(mx.y, l.y), fmaxf(mx.z, l.z)};
                any = true;
            }
        }
        void** al = F<void**>(gobs[gi].gob, 0x4a8); int an = F<int>(gobs[gi].gob, 0x4b0);
        for (int i = 0; al && i < an && i < 8; i++) {
            void* c = al[i];
            if (!c || !Readable(c, 0x458)) continue;
            void* beh = F<void*>(c, 0x450);
            void* part = beh && Readable(beh, 0x38) ? F<void*>(beh, 0x30) : nullptr;
            auto it = frames.find(part);
            if (it != frames.end()) gobs.push_back({c, it->second});
        }
    }
    return any;
}

// ---- ground ----
bool GroundAt(void* area, float x, float y, float& h) {
    if (!area || !fGetTile || !fToCanonical || !fFindTri || !fMeshHeight) return false;
    V3 p{x, y, 0};
    void* tile = fGetTile(area, &p);
    if (!tile) return false;
    void* td = F<void*>(tile, 0x40);
    void* mesh = td ? F<void*>(td, 8) : nullptr;
    if (!mesh) return false;
    float cx, cy;
    fToCanonical(tile, x, y, &cx, &cy);
    if (fFindTri(mesh, 0, 0, cx, cy, -1) < 0) return false;
    V3 c{cx, cy, 0};
    h = fMeshHeight(mesh, &c) + F<float>(tile, 0x10);
    return true;
}

// walkmesh heights on a lattice of the area, kept until the area changes: bodies that start again (kicked,
// thrown by a blast) and bodies near each other sample the same points
namespace {
void* g_latArea = nullptr;
float g_latCell = 0;
std::unordered_map<uint64_t, float> g_lattice;          // (i, j) -> height, NAN = no walkmesh
bool LatticeGround(void* area, int i, int j, float cell, float& h) {
    if (area != g_latArea || cell != g_latCell) { g_lattice.clear(); g_latArea = area; g_latCell = cell; }
    uint64_t k = ((uint64_t)(uint32_t)i << 32) | (uint32_t)j;
    auto it = g_lattice.find(k);
    if (it == g_lattice.end()) {
        float v = 0;
        if (!GroundAt(area, i * cell, j * cell, v)) v = NAN;
        if (g_lattice.size() > 400000) g_lattice.clear();
        it = g_lattice.emplace(k, v).first;
    }
    if (std::isnan(it->second)) return false;
    h = it->second;
    return true;
}
}  // namespace

b3MeshData* AddGround(b3WorldId world, void* area, V3 center, float refZ, int N, float cell) {
    std::vector<b3Vec3> v(N * N);
    std::vector<char> walk(N * N, 0);
    // the grid sits on the lattice (cell multiples) nearest to centring it on 'center'
    int i0 = (int)lroundf((center.x - cell * (N - 1) * 0.5f) / cell), j0 = (int)lroundf((center.y - cell * (N - 1) * 0.5f) / cell);
    float x0 = i0 * cell, y0 = j0 * cell;
    int walkable = 0;
    for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
        float h = refZ;
        bool ok = LatticeGround(area, i0 + i, j0 + j, cell, h);
        if (ok && fabsf(h - refZ) > 3.0f) ok = false;   // another floor
        walk[j * N + i] = ok; walkable += ok;
        v[j * N + i] = b3Vec3{x0 + i * cell, y0 + j * cell, ok ? h : refZ};
    }
    b3MeshData* mesh = nullptr;
    if (walkable > 4) {
        // what is not walkable (walls, furniture) is raised, using the height of a walkable neighbour
        for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
            if (walk[j * N + i]) continue;
            float best = refZ; float bd = 1e9f;
            for (int dj = -3; dj <= 3; dj++) for (int di = -3; di <= 3; di++) {
                int ii = i + di, jj = j + dj;
                if (ii < 0 || jj < 0 || ii >= N || jj >= N || !walk[jj * N + ii]) continue;
                float dd = (float)(di * di + dj * dj);
                if (dd < bd) { bd = dd; best = v[jj * N + ii].z; }
            }
            v[j * N + i].z = best + 1.2f;
        }
        std::vector<int32_t> idx;
        for (int j = 0; j + 1 < N; j++) for (int i = 0; i + 1 < N; i++) {
            int a = j * N + i, b = a + 1, c = a + N, e = c + 1;
            idx.insert(idx.end(), {a, b, e, a, e, c});
        }
        b3MeshDef md = {};
        md.vertices = v.data(); md.indices = idx.data();
        md.vertexCount = N * N; md.triangleCount = (int)idx.size() / 3;
        md.useMedianSplit = true; md.identifyEdges = true;
        mesh = b3CreateMesh(&md, nullptr, 0);
    }
    b3BodyDef bd = b3DefaultBodyDef();
    b3BodyId g = b3CreateBody(world, &bd);
    b3ShapeDef sd = b3DefaultShapeDef();
    sd.baseMaterial.friction = 0.9f;
    if (mesh) b3CreateMeshShape(g, &sd, mesh, b3Vec3{1, 1, 1});
    // beyond the sampled area (or with no walkmesh at all): a flat floor at the lowest walkable height
    float low = refZ;
    for (int k = 0; k < N * N; k++) if (walk[k]) low = fminf(low, v[k].z);
    b3BoxHull box = b3MakeOffsetBoxHull(40.0f, 40.0f, 0.5f, b3Vec3{center.x, center.y, low - (mesh ? 0.52f : 0.5f)});
    b3CreateHullShape(g, &sd, &box.base);
    return mesh;
}

// ---- explosions ----
namespace {
struct BlastKind { int id; float radius, speed; bool implode; };
// visualeffects.2da fire-and-forget effects that hit with force
const BlastKind KINDS[] = {
    {22, 6.5f, 5.0f, false},    // VFX_FNF_FIREBALL
    {23, 7.0f, 3.5f, false},    // FIRESTORM
    {24, 6.0f, 4.0f, true},     // IMPLOSION
    {28, 8.0f, 6.0f, false},    // METEOR_SWARM
    {183, 6.0f, 4.0f, false},   // SOUND_BURST
    {184, 3.0f, 3.0f, false},   // STRIKE_HOLY
    {231, 7.0f, 4.0f, false},   // ICESTORM
    {257, 5.0f, 4.5f, false}, {258, 5.0f, 4.5f, false}, {259, 5.0f, 4.5f, false},
    {260, 5.0f, 5.0f, false}, {261, 5.0f, 4.0f, false}, {262, 5.0f, 4.5f, false},   // GAS_EXPLOSION_*
    {459, 5.0f, 5.0f, false},   // ELECTRIC_EXPLOSION
    {477, 5.0f, 5.0f, false},   // MYSTICAL_EXPLOSION
    {494, 5.0f, 5.0f, false},   // DRAGBREATHGROUND
};
typedef uint64_t (*LoadEffect_t)(void* vfx, int id);
LoadEffect_t o_LoadEffect = nullptr;
struct PendingFx { void* vfx; const BlastKind* k; double t; };
std::vector<PendingFx> g_pending;
std::vector<Blast> g_blasts;
uint32_t g_serial = 0;

uint64_t hk_LoadEffect(void* vfx, int id) {
    uint64_t r = o_LoadEffect(vfx, id);
    if ((r & 0xffffffff) != 0) for (const BlastKind& k : KINDS) if (k.id == id) { g_pending.push_back({vfx, &k, Now()}); break; }
    return r;
}
}  // namespace

void InstallBlastHook() {
    void* p = (void*)A(0x1408711d0);                  // CNWCVisualEffect::LoadEffect(unsigned int)
    if (MH_CreateHook(p, (void*)hk_LoadEffect, (void**)&o_LoadEffect) == MH_OK && MH_EnableHook(p) == MH_OK) Log("blast hook ready");
    else Log("blast hook failed");
}

void PollBlasts() {
    double now = Now();
    // the effect gets its position right after it is loaded (same frame)
    for (auto& pf : g_pending) {
        if (!Readable(pf.vfx, 0x48)) continue;
        Vec3 p = Pos(pf.vfx);
        if (p.x == 0 && p.y == 0 && p.z == 0) continue;
        Blast b{V3{p.x, p.y, p.z}, pf.k->radius, pf.k->speed, pf.k->implode, now, ++g_serial};
        g_blasts.push_back(b);
        Log("blast: effect %d at %.1f %.1f %.1f", pf.k->id, p.x, p.y, p.z);
    }
    g_pending.clear();
    while (!g_blasts.empty() && now - g_blasts.front().t > 3.0) g_blasts.erase(g_blasts.begin());
}
const std::vector<Blast>& Blasts() { return g_blasts; }

V3 BlastVelocity(const Blast& b, V3 at) {
    V3 d = at - b.p;
    float h = sqrtf(d.x * d.x + d.y * d.y);
    if (h > b.radius) return V3{};
    float k = 1.0f - h / b.radius;
    k = 0.35f + 0.65f * k;
    V3 dir = h > 0.05f ? V3{d.x / h, d.y / h, 0} : V3{0, 0, 0};
    if (b.implode) return V3{-dir.x * b.speed * k, -dir.y * b.speed * k, 1.0f * k};
    return V3{dir.x * b.speed * k, dir.y * b.speed * k, b.speed * 0.5f * k};
}
}  // namespace P
