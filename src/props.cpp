// NWNRagdoll - loose objects: dropped items tumble to the ground, bashed barrels and crates break into
// pieces, blasts throw both around.
//
// Items: an item that appears on the ground while you are in the area (dropped by you, by a dying
// creature, thrown by a script) falls from a little above its spot with a spin and comes to rest
// on the walkmesh. In single player the server item is moved to where it lies; against a remote
// server it is kept within a small radius of its real position.
// Debris: when a placeable is destroyed (single player: the server says it is dead), wooden / stone /
// metal chunk models of the game fly from where it stood, lie there for a while and sink away.
#include "props.h"
#include "game.h"
#include "log.h"
#include <windows.h>
#include <box3d/box3d.h>
#include <cstring>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace G;
using P::BV; using P::VB; using P::BQ; using P::QB;

namespace {
struct Cfg {
    int items = 1;
    int debris = 1;
    float range = 30.0f;
    float pinRadius = 0.5f;
    float debrisSeconds = 20.0f;
    int debug = 0;
} C;

typedef void* (*NewAur_t)(const char* model, const char* tag, void* file);
NewAur_t fNewAur = nullptr;
typedef void (*SetScene_t)(void* gob, void* scene);
SetScene_t fSetScene = nullptr;
typedef int (*GetDead_t)(void* obj);
GetDead_t fSrvGetDead = nullptr;

// ---- one rigid body with its own little world ----
struct Body {
    b3WorldId world = b3_nullWorldId;
    b3BodyId body = b3_nullBodyId;
    b3MeshData* mesh = nullptr;
    V3 wp; Q4 wq;
    double t = 0, calm = 0;
    bool done = false;
};
void FreeBody(Body& b) {
    if (b3World_IsValid(b.world)) b3DestroyWorld(b.world);
    b.world = b3_nullWorldId;
    if (b.mesh) { b3DestroyMesh(b.mesh); b.mesh = nullptr; }
}

// ---- items ----
struct Item {
    uint32_t id; void* gob;
    V3 center, half;             // box in the gob frame
    V3 origin;                   // where the game put it
    Body b;
    bool active = false;
};
std::map<uint32_t, Item> g_items;
std::set<uint32_t> g_knownItems;             // items that were there when we arrived
void* g_area = nullptr;
double g_areaTime = 0;

void ItemBox(void* gob, V3& center, V3& half) {
    V3 mn, mx;
    if (P::GobExtent(gob, mn, mx)) { center = (mn + mx) * 0.5f; half = (mx - mn) * 0.5f; }
    else { center = V3{0, 0, 0.05f}; half = V3{0.1f, 0.1f, 0.05f}; }
    half = V3{fminf(fmaxf(half.x, 0.02f), 1.0f), fminf(fmaxf(half.y, 0.02f), 1.0f), fminf(fmaxf(half.z, 0.02f), 1.0f)};
}

void StartBody(Body& b, void* area, V3 p, Q4 q, V3 v, V3 w, V3 center, V3 half, float refZ, float density = 400.0f) {
    FreeBody(b);
    b3WorldDef wd = b3DefaultWorldDef();
    wd.gravity = b3Vec3{0, 0, -9.81f};
    b.world = b3CreateWorld(&wd);
    b.mesh = P::AddGround(b.world, area, p, refZ, 21, 0.25f);
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position = BV(p); bd.rotation = BQ(q);
    bd.linearVelocity = BV(v); bd.angularVelocity = BV(w);
    bd.angularDamping = 0.4f;
    b.body = b3CreateBody(b.world, &bd);
    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = density;
    sd.baseMaterial.friction = 0.6f;
    sd.baseMaterial.restitution = 0.2f;
    b3BoxHull box = b3MakeOffsetBoxHull(half.x, half.y, half.z, BV(center));
    b3CreateHullShape(b.body, &sd, &box.base);
    b.wp = p; b.wq = q; b.t = 0; b.calm = 0; b.done = false;
}

bool StepBody(Body& b, float dt, float maxT, const V3* pin, float pinR) {
    if (b.done || !b3World_IsValid(b.world)) return false;
    b3World_Step(b.world, dt, 4);
    b.t += dt;
    b3WorldTransform tf = b3Body_GetTransform(b.body);
    b.wp = VB(tf.p); b.wq = Norm(QB(tf.q));
    V3 lv = VB(b3Body_GetLinearVelocity(b.body)), av = VB(b3Body_GetAngularVelocity(b.body));
    if (pin) {
        V3 off = b.wp - *pin; off.z = 0;
        float l = Len(off);
        if (l > pinR) {
            V3 n = off * (1.0f / l);
            float out = Dot(lv, n);
            if (out > 0) lv = lv - n * out;
            lv = lv - n * ((l - pinR) * 4.0f);
            b3Body_SetLinearVelocity(b.body, BV(lv));
        }
    }
    float m = fmaxf(Len(lv), Len(av) * 0.1f);
    b.calm = m < 0.05f ? b.calm + dt : 0;
    if ((b.t > 0.4 && b.calm > 0.3) || b.t > maxT) {
        b.done = true;
        FreeBody(b);
        return true;                      // settled now
    }
    return false;
}

void StartItem(Item& it, void* area, V3 v, V3 w, float lift) {
    Vec3 op = Pos(GetObj(it.id));
    it.origin = V3{op.x, op.y, op.z};
    V3 p = F<V3>(it.gob, 0xa8);
    Q4 q = Norm(F<Q4>(it.gob, 0xb4));
    if (it.b.done || it.active) p = it.b.wp, q = it.b.wq;      // already simulated: from where it lies
    p.z += lift;
    StartBody(it.b, area, p, q, v, w, it.center, it.half, op.z);
    it.active = true;
}

void SettleItem(Item& it) {
    if (!P::ServerApp()) return;
    V3 np{it.b.wp.x, it.b.wp.y, it.origin.z};
    float h;
    if (P::GroundAt(g_area, np.x, np.y, h)) np.z = h;
    V3 off = np - it.origin;
    if (sqrtf(off.x * off.x + off.y * off.y) > 0.05f && P::SetServerPosition(it.id, np)) {
        if (C.debug) Log("props: item %08x moved %.2f m", it.id, Len(off));
        it.origin = np;
    }
}

// ---- debris ----
struct Chunk { void* gob; V3 center, half; Body b; double born; bool inUse = false; float sink = 0; };
std::vector<Chunk> g_pool;                  // chunk models are reused, never deleted
struct Placeable { void* gob; std::string model; V3 p; Q4 q; V3 mn, mx; bool dead = false; };
std::map<uint32_t, Placeable> g_placeables;

const char* ChunkModel(const std::string& m, int i) {
    std::string s = m; for (auto& c : s) c = (char)tolower((unsigned char)c);
    auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
    if (has("stone") || has("statue") || has("rock") || has("pillar") || has("column") || has("altar") || has("tomb") || has("brazier")) {
        static const char* M[] = {"fx_rockchunksml", "fx_rockchunk", "fx_rockchunkmed", "fx_rockchunk"};
        return M[i % 4];
    }
    if (has("urn") || has("pot") || has("vase") || has("jar") || has("bottle") || has("glass")) {
        static const char* M[] = {"fx_shard01", "fx_shard02"};
        return M[i % 2];
    }
    if (has("anvil") || has("iron") || has("metal") || has("cage")) {
        static const char* M[] = {"plc_chunk_m01", "fx_shard02"};
        return M[i % 2];
    }
    static const char* M[] = {"plc_chunk_w01", "fx_chunkm", "fx_chunksm", "fx_chunkm", "fx_chunksm", "plc_chunk_w01", "fx_chunkm"};
    return M[i % 7];
}

Chunk* GetChunk(const char* model, void* scene) {
    for (auto& c : g_pool)
        if (!c.inUse && _stricmp(P::PartName(F<void*>(c.gob, 0x90)), model) == 0) return &c;
    if (g_pool.size() >= 48 || !fNewAur) return nullptr;
    if (g_pool.capacity() < 64) g_pool.reserve(64);
    void* g = fNewAur(model, "body", nullptr);
    if (!g) { Log("props: model %s did not load", model); return nullptr; }
    P::SetGobTransform(g, V3{}, Q4{});
    Chunk c; c.gob = g;
    ItemBox(g, c.center, c.half);
    g_pool.push_back(c);
    if (C.debug) Log("props: chunk %s box %.2f %.2f %.2f", model, c.half.x, c.half.y, c.half.z);
    (void)scene;
    return &g_pool.back();
}

void Shatter(const Placeable& pl, void* area) {
    void* pg = Player() ? P::ObjectGob(Player()) : nullptr;
    void* scene = pg ? F<void*>(pg, 0x88) : nullptr;
    if (!scene || !fSetScene) return;
    V3 size = pl.mx - pl.mn;
    float vol = fmaxf(0.05f, size.x * size.y * size.z);
    int n = (int)fminf(14.0f, fmaxf(5.0f, vol * 10.0f));
    V3 c = (pl.mn + pl.mx) * 0.5f;
    Log("props: %s breaks into %d pieces", pl.model.c_str(), n);
    for (int i = 0; i < n; i++) {
        Chunk* ch = GetChunk(ChunkModel(pl.model, i), scene);
        if (!ch) break;
        ch->inUse = true; ch->born = P::Now(); ch->sink = 0;
        V3 p{P::Rand(pl.mn.x, pl.mx.x), P::Rand(pl.mn.y, pl.mx.y), P::Rand(pl.mn.z + 0.1f, fmaxf(pl.mn.z + 0.15f, pl.mx.z))};
        V3 out = p - c; out.z = 0;
        float l = Len(out);
        out = l > 0.01f ? out * (1.0f / l) : V3{P::Rand(-1, 1), P::Rand(-1, 1), 0};
        V3 v = out * P::Rand(1.0f, 3.0f) + V3{0, 0, P::Rand(1.5f, 3.5f)};
        V3 w{P::Rand(-10, 10), P::Rand(-10, 10), P::Rand(-10, 10)};
        Q4 q = Norm(Q4{P::Rand(-1, 1), P::Rand(-1, 1), P::Rand(-1, 1), P::Rand(-1, 1)});
        StartBody(ch->b, area, p, q, v, w, ch->center, ch->half, pl.p.z, 500.0f);
        P::SetGobTransform(ch->gob, p, q);
        fSetScene(ch->gob, scene);
    }
}

void TrackPlaceables(void* area) {
    if (!C.debris || !P::ServerApp()) return;           // needs the server's view of what was destroyed
    void* pl = Player();
    if (!pl) return;
    Vec3 pp = Pos(pl);
    int n = F<int>(area, OFS::Area_ObjCount);
    uint32_t* ids = F<uint32_t*>(area, OFS::Area_ObjIds);
    std::set<uint32_t> seen;
    for (int i = 0; ids && i < n; i++) {
        void* o = GetObj(ids[i]);
        if (!o || Type(o) != OT_PLACEABLE) continue;
        Vec3 p = Pos(o);
        if ((p.x - pp.x) * (p.x - pp.x) + (p.y - pp.y) * (p.y - pp.y) > C.range * C.range) continue;
        void* g = P::ObjectGob(o);
        if (!g) continue;
        seen.insert(ids[i]);
        auto it = g_placeables.find(ids[i]);
        if (it == g_placeables.end()) {
            Placeable rec; rec.gob = g; rec.model = P::PartName(F<void*>(g, 0x90));
            V3 mn, mx;
            if (!P::GobExtent(g, mn, mx)) continue;
            rec.p = F<V3>(g, 0xa8); rec.q = F<Q4>(g, 0xb4);
            // world box of the placeable
            V3 a{1e9f, 1e9f, 1e9f}, b{-1e9f, -1e9f, -1e9f};
            for (int k = 0; k < 8; k++) {
                V3 cr{k & 1 ? mx.x : mn.x, k & 2 ? mx.y : mn.y, k & 4 ? mx.z : mn.z};
                V3 wv = rec.p + Rot(rec.q, cr);
                a = V3{fminf(a.x, wv.x), fminf(a.y, wv.y), fminf(a.z, wv.z)};
                b = V3{fmaxf(b.x, wv.x), fmaxf(b.y, wv.y), fmaxf(b.z, wv.z)};
            }
            rec.mn = a; rec.mx = b;
            it = g_placeables.emplace(ids[i], rec).first;
        }
        void* so = P::ServerObject(ids[i]);
        bool dead = so && fSrvGetDead && fSrvGetDead(so) != 0;
        if (dead && !it->second.dead) { it->second.dead = true; Shatter(it->second, area); }
    }
    // gone without dying (scripts, area streaming): just forget
    for (auto it = g_placeables.begin(); it != g_placeables.end();) {
        if (!seen.count(it->first)) it = g_placeables.erase(it); else ++it;
    }
}

void UpdateChunks() {
    double now = P::Now();
    for (auto& c : g_pool) {
        if (!c.inUse) continue;
        if (c.b.done && now - c.born > C.debrisSeconds) {
            // sink into the ground, then put the model away
            c.sink += 0.004f;
            c.b.wp.z -= 0.004f;
            if (c.sink > 0.4f) { if (fSetScene) fSetScene(c.gob, nullptr); c.inUse = false; continue; }
        }
        P::SetGobTransform(c.gob, c.b.wp, c.b.wq);
    }
}

void TrackItems(void* area) {
    if (!C.items) return;
    void* pl = Player();
    if (!pl) return;
    Vec3 pp = Pos(pl);
    double now = P::Now();
    bool arriving = now - g_areaTime < 3.0;
    int n = F<int>(area, OFS::Area_ObjCount);
    uint32_t* ids = F<uint32_t*>(area, OFS::Area_ObjIds);
    std::set<uint32_t> seen;
    for (int i = 0; ids && i < n; i++) {
        void* o = GetObj(ids[i]);
        if (!o || Type(o) != OT_ITEM) continue;
        uint32_t id = ids[i];
        seen.insert(id);
        if (arriving) { g_knownItems.insert(id); continue; }
        if (g_knownItems.count(id) || g_items.count(id)) continue;
        void* g = P::ObjectGob(o);
        if (!g) continue;                                   // model not loaded yet: next frame
        g_knownItems.insert(id);
        Vec3 p = Pos(o);
        if ((p.x - pp.x) * (p.x - pp.x) + (p.y - pp.y) * (p.y - pp.y) > C.range * C.range) continue;
        Item it; it.id = id; it.gob = g;
        ItemBox(g, it.center, it.half);
        V3 v{P::Rand(-0.7f, 0.7f), P::Rand(-0.7f, 0.7f), P::Rand(0.5f, 1.5f)};
        V3 w{P::Rand(-6, 6), P::Rand(-6, 6), P::Rand(-6, 6)};
        auto& ref = g_items.emplace(id, it).first->second;
        StartItem(ref, area, v, w, 0.6f);
        if (C.debug) Log("props: item %08x '%s' tumbles, box %.2f %.2f %.2f", id, P::PartName(F<void*>(g, 0x90)), it.half.x, it.half.y, it.half.z);
    }
    for (auto it = g_items.begin(); it != g_items.end();) {
        void* o = seen.count(it->first) ? GetObj(it->first) : nullptr;
        if (!o || P::ObjectGob(o) != it->second.gob) { FreeBody(it->second.b); it = g_items.erase(it); }
        else ++it;
    }
    for (auto it = g_knownItems.begin(); it != g_knownItems.end();) {
        if (!seen.count(*it)) it = g_knownItems.erase(it); else ++it;
    }
}
}  // namespace

void Props_LoadConfig(const char* ini) {
    C.items = GetPrivateProfileIntA("Physics", "ItemsTumble", 1, ini);
    C.debris = GetPrivateProfileIntA("Physics", "Debris", 1, ini);
    char b[64]; GetPrivateProfileStringA("Physics", "DebrisSeconds", "20", b, sizeof b, ini);
    C.debrisSeconds = (float)atof(b);
    C.debug = GetPrivateProfileIntA("Ragdoll", "Debug", 0, ini);
    Log("config: items %d debris %d (%.0f s)", C.items, C.debris, C.debrisSeconds);
}

void Props_Install() {
    HMODULE exe = GetModuleHandleA(nullptr);
    fNewAur = (NewAur_t)GetProcAddress(exe, "?NewCAurObject@@YAPEAVCAurObject@@PEBD0PEAU_iobuf@@@Z");
    fSetScene = (SetScene_t)GetProcAddress(exe, "?SetScene@Gob@@QEAAXPEAVScene@@@Z");
    fSrvGetDead = (GetDead_t)GetProcAddress(exe, "?GetDead@CNWSObject@@QEAAHXZ");
    Log("props: newaurobject %p setscene %p", (void*)fNewAur, (void*)fSetScene);
    if (!fNewAur || !fSetScene) C.debris = 0;
}

void Props_Update(void* area) {
    if (area != g_area) {
        // the old scene goes away with its models: forget everything (pooled chunks too)
        for (auto& kv : g_items) FreeBody(kv.second.b);
        g_items.clear(); g_knownItems.clear(); g_placeables.clear();
        for (auto& c : g_pool) FreeBody(c.b);
        g_pool.clear();
        g_area = area; g_areaTime = P::Now();
    }
    if (!area) return;
    TrackItems(area);
    TrackPlaceables(area);
    UpdateChunks();
    for (auto& kv : g_items) if (kv.second.active) P::SetGobTransform(kv.second.gob, kv.second.b.wp, kv.second.b.wq);
}

void Props_Step(float dt) {
    bool remote = P::ServerApp() == nullptr;
    for (auto& kv : g_items) {
        Item& it = kv.second;
        if (!it.active) continue;
        if (StepBody(it.b, dt, 5.0f, remote ? &it.origin : nullptr, C.pinRadius)) SettleItem(it);
    }
    for (auto& c : g_pool) if (c.inUse) StepBody(c.b, dt, 6.0f, nullptr, 0);
}

void Props_AfterAnimate(void* gob) {
    if (g_items.empty()) return;
    for (auto& kv : g_items) if (kv.second.gob == gob && kv.second.active) { P::SetGobTransform(gob, kv.second.b.wp, kv.second.b.wq); break; }
}

void Props_Blast(const P::Blast& bl) {
    if (!g_area) return;
    // items on the ground near the blast, simulated before or not
    int n = F<int>(g_area, OFS::Area_ObjCount);
    uint32_t* ids = F<uint32_t*>(g_area, OFS::Area_ObjIds);
    for (int i = 0; ids && i < n && C.items; i++) {
        void* o = GetObj(ids[i]);
        if (!o || Type(o) != OT_ITEM) continue;
        V3 p = V3{Pos(o).x, Pos(o).y, Pos(o).z};
        auto it = g_items.find(ids[i]);
        if (it != g_items.end() && it->second.active) p = it->second.b.wp;
        V3 v = P::BlastVelocity(bl, p);
        if (Len(v) < 0.01f) continue;
        if (it == g_items.end()) {
            void* g = P::ObjectGob(o);
            if (!g) continue;
            Item rec; rec.id = ids[i]; rec.gob = g; ItemBox(g, rec.center, rec.half);
            it = g_items.emplace(ids[i], rec).first;
            g_knownItems.insert(ids[i]);
        }
        StartItem(it->second, g_area, v * 1.3f, V3{P::Rand(-10, 10), P::Rand(-10, 10), P::Rand(-10, 10)}, 0.05f);
    }
    for (auto& c : g_pool) {
        if (!c.inUse) continue;
        V3 v = P::BlastVelocity(bl, c.b.wp);
        if (Len(v) < 0.01f) continue;
        Body old = c.b;
        StartBody(c.b, g_area, old.wp, old.wq, v * 1.4f, V3{P::Rand(-12, 12), P::Rand(-12, 12), P::Rand(-12, 12)}, c.center, c.half, old.wp.z, 500.0f);
        c.born = P::Now();
    }
}

void Props_DevCommand(const char* line) {
    if (strncmp(line, "ext ", 4) == 0) {          // ext <object id>: mesh boxes of its model
        void* o = GetObj((uint32_t)strtoul(line + 4, nullptr, 16));
        void* g = o ? P::ObjectGob(o) : nullptr;
        if (!g) return;
        std::vector<void*> st{F<void*>(g, 0x90)};
        while (!st.empty()) {
            void* p = st.back(); st.pop_back();
            if (!p || !Readable(p, 0x58)) continue;
            void** ch = F<void**>(p, 0x38); int k = F<int>(p, 0x40);
            for (int i = 0; ch && i < k && i < 64; i++) st.push_back(ch[i]);
            void* m = F<void*>(p, 8);
            V3 a = m ? F<V3>(m, 0xc0) : V3{}, b = m ? F<V3>(m, 0xcc) : V3{};
            float r = m ? F<float>(m, 0xd8) : 0; V3 c = m ? F<V3>(m, 0xdc) : V3{};
            Log("dev: part '%s' %s mdl %s  %.3f %.3f %.3f / %.3f %.3f %.3f  r %.3f c %.3f %.3f %.3f", P::PartName(p), ClassName(p), m ? ClassName(m) : "",
                a.x, a.y, a.z, b.x, b.y, b.z, r, c.x, c.y, c.z);
        }
        return;
    }
    if (strncmp(line, "models", 6) == 0) {         // sizes of candidate debris models
        const char* names[] = {"fx_chunklg", "fx_chunklg01", "fx_chunkm", "fx_chunksm", "fx_rockchunk", "fx_rockchunksml", "fx_rockchunkmed",
                               "fx_shard01", "fx_shard02", "plc_chunk_w01", "plc_chunk_w02", "plc_chunk_s01", "plc_chunk_m01", "tbw_chunk01", "dag_chunk_g01", "m_chunk_r"};
        for (const char* n : names) {
            void* g = fNewAur ? fNewAur(n, "body", nullptr) : nullptr;
            V3 a, b;
            if (g) { P::SetGobTransform(g, V3{}, Q4{}); if (P::GobExtent(g, a, b)) Log("dev: %-16s %.2f %.2f %.2f .. %.2f %.2f %.2f", n, a.x, a.y, a.z, b.x, b.y, b.z); else Log("dev: %s no mesh", n); }
            else Log("dev: %s not loaded", n);
        }
        return;
    }
    if (strncmp(line, "debris", 6) == 0) {         // debris: a crate's worth of pieces next to the player
        void* pl = Player();
        if (!pl) return;
        Vec3 p = Pos(pl);
        Vec3 o = F<Vec3>(pl, OFS::Obj_Orient);
        Placeable rec; rec.model = "plc_crate";
        V3 c{p.x + o.x * 1.5f, p.y + o.y * 1.5f, p.z};
        rec.p = c; rec.mn = c + V3{-0.35f, -0.35f, 0}; rec.mx = c + V3{0.35f, 0.35f, 0.7f};
        Shatter(rec, g_area);
        return;
    }
    if (strncmp(line, "shatter ", 8) == 0) {      // shatter <placeable id>: break it as if destroyed
        uint32_t id = (uint32_t)strtoul(line + 8, nullptr, 16);
        auto it = g_placeables.find(id);
        if (it == g_placeables.end()) { Log("dev: placeable %08x not tracked", id); return; }
        Shatter(it->second, g_area);
    }
}
