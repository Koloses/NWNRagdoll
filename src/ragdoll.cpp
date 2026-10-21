// NWNRagdoll - ragdoll physics for dying creatures (client-side visuals, Box3D)
//
// The engine animates a creature's model as a tree of Parts (local position / rotation, w-first
// quaternions). Gob::Animate writes the animated local transforms each frame; right after it we put
// back the pose frozen at the moment of death and replace the rotations of the main bones with the
// rigid bodies of a Box3D ragdoll. When the ragdoll has settled the final pose is kept for good.
// In single player (the server runs in this process) the corpse object is moved under the body so
// looting and selection match what is on screen; against a remote server the body is held close
// to the corpse position instead.
#include "ragdoll.h"
#include "game.h"
#include "qmath.h"
#include "log.h"
#include <windows.h>
#include <MinHook.h>
#include <box3d/box3d.h>
#include <cstring>
#include <cmath>
#include <vector>
#include <map>
#include <string>

using namespace G;

namespace {
// ---- settings ----
struct Cfg {
    int enabled = 1;
    int reposition = 1;          // single player: move the corpse object under the body
    float pinRadius = 0.6f;      // remote server: how far the body may end up from the corpse position
    float push = 1.2f;           // backwards velocity of the upper body at death (m/s)
    float settleTime = 6.0f;     // seconds of simulation at most
    int maxActive = 10;          // ragdolls simulated at the same time
    float range = 45.0f;         // only creatures this close to the player
    int debug = 0;
    char devFile[260] = "";      // testing: "kill <client id hex>" commands
} C;

// ---- engine ----
typedef void (*Animate_t)(void* gob, float dt);
Animate_t o_GobAnimate = nullptr, o_SceneAnimate = nullptr;
typedef void (*GetWorld_t)(void* part, V3* pos, Q4* q);
GetWorld_t fGetWorld = nullptr;
typedef void* (*GetCreature_t)(void* srv, uint32_t id);
GetCreature_t fSrvCreature = nullptr;
typedef void (*SetPos_t)(void* obj, const V3* pos, int);
SetPos_t fSrvSetPosition = nullptr;
typedef int (*GetDead_t)(void* obj);
GetDead_t fSrvGetDead = nullptr;
void** p_AppManager = nullptr;

// ---- ground: the area's tile walkmeshes ----
typedef void* (*GetTile_t)(void* area, const V3* pos);
GetTile_t fGetTile = nullptr;               // CNWCArea::GetTile(Vector) 0x1407d9d00
typedef void (*Rot_t)(void* tile, float x, float y, float* ox, float* oy);
Rot_t fToCanonical = nullptr;
typedef int (*FindTri_t)(void* mesh, int type, int start, float x, float y, int skip);
FindTri_t fFindTri = nullptr;
typedef float (*Height_t)(void* mesh, const V3* p);
Height_t fMeshHeight = nullptr;
// height of the walkmesh at (x, y); false where nothing is walkable
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

void* ServerApp() { return p_AppManager && *p_AppManager ? F<void*>(*p_AppManager, 8) : nullptr; }
void* ServerCreature(uint32_t clientId) {
    void* s = ServerApp();
    if (!s || !fSrvCreature || !F<void*>(s, 8)) return nullptr;
    return fSrvCreature(s, clientId & 0x7fffffffu);
}

const char* PartName(void* p) { void* n = F<void*>(p, 8); return n ? (const char*)n + 0x38 : ""; }
void* CreatureGob(void* cre) {
    void* ab = cre ? F<void*>(cre, 0x78) : nullptr;
    if (!ab || !Readable(ab, 0xd0)) return nullptr;
    void* g = F<void*>(ab, 0xc8);
    return g && strcmp(ClassName(g), "Gob") == 0 ? g : nullptr;
}
float GobScale(void* gob) { return VCall<float (*)(void*)>(gob, 0x2d0)(gob); }

bool ClientDead(void* cre) {
    void* st = F<void*>(cre, OFS::Cre_Stats);
    if (!st) return false;
    if (F<uint8_t>(st, 0x8e) == 5) return true;                 // damage level "dead"
    void* pl = Player();
    bool exact = cre == pl || (pl && F<uint32_t>(cre, 0x270) == Id(pl));
    return exact && F<short>(st, OFS::Stats_MaxHP) > 0 && F<short>(st, OFS::Stats_CurHP) <= -10;
}

// ---- model snapshot ----
struct PartRec { void* part; int parent; V3 lp; Q4 lq; float s; int bone = -1; };
struct Bone {
    int part;                    // index into parts
    int parent = -1;             // parent bone
    b3BodyId body = b3_nullBodyId;
    V3 wp; Q4 wq;                // world transform (live while simulating, final afterwards)
    bool root = false;
};
struct Doll {
    uint32_t id = 0;
    void* gob = nullptr;
    std::vector<PartRec> parts;
    std::vector<Bone> bones;
    b3WorldId world = b3_nullWorldId;
    double t = 0, calm = 0;
    bool done = false;
    V3 anchor;                   // corpse position at death
    float ground = 0;
    b3MeshData* mesh = nullptr;  // ground around the body
    bool moved = false;          // server corpse repositioned
};
std::map<uint32_t, Doll*> g_dolls;          // by creature id
std::map<void*, Doll*> g_byGob;
std::map<uint32_t, bool> g_dead;            // last known dead state
LARGE_INTEGER g_freq, g_last;
double g_acc = 0;

void Collect(Doll& d, void* p, int parent, int depth) {
    if (!p || depth > 48 || d.parts.size() > 400 || !Readable(p, 0x58)) return;
    PartRec r; r.part = p; r.parent = parent; r.lp = F<V3>(p, 0x10); r.lq = F<Q4>(p, 0x1c); r.s = F<float>(p, 0x2c);
    int me = (int)d.parts.size();
    d.parts.push_back(r);
    void** ch = F<void**>(p, 0x38); int n = F<int>(p, 0x40);
    for (int i = 0; ch && i < n && i < 128; i++) Collect(d, ch[i], me, depth + 1);
}
int FindPart(const Doll& d, const char* name) {
    for (size_t i = 0; i < d.parts.size(); i++) if (_stricmp(PartName(d.parts[i].part), name) == 0) return (int)i;
    return -1;
}

// world transforms of all parts from their locals (parts are in parent-before-child order)
struct WT { V3 p; Q4 q; float s; };
void ComputeWorld(Doll& d, std::vector<WT>& w, bool useBones) {
    w.resize(d.parts.size());
    V3 rp; Q4 rq;
    fGetWorld(d.parts[0].part, &rp, &rq);
    float gs = GobScale(d.gob);
    for (size_t i = 0; i < d.parts.size(); i++) {
        PartRec& r = d.parts[i];
        if (r.parent < 0) { w[i] = {rp, rq, gs * r.s}; continue; }
        const WT& pw = w[r.parent];
        if (useBones && r.bone >= 0) {
            const Bone& b = d.bones[r.bone];
            // local rotation (and the root bone's position) that put the part where its body is
            Q4 lq = Norm(Conj(pw.q) * b.wq);
            F<Q4>(r.part, 0x1c) = lq;
            if (b.root) {
                V3 lp = Rot(Conj(pw.q), b.wp - pw.p) * (1.0f / (pw.s > 1e-4f ? pw.s : 1.0f));
                F<V3>(r.part, 0x10) = lp;
            }
            w[i] = {b.root ? b.wp : pw.p + Rot(pw.q, r.lp * pw.s), b.wq, pw.s * r.s};
            continue;
        }
        w[i] = {pw.p + Rot(pw.q, F<V3>(r.part, 0x10) * pw.s), pw.q * F<Q4>(r.part, 0x1c), pw.s * r.s};
    }
}

// ---- skeleton template (the standard NWN humanoid bone names; other skeletons use a generic chain) ----
struct Tmpl { const char* name; const char* parent; const char* tip; float radius; float cone; float twist; };
const Tmpl HUMANOID[] = {
    {"rootdummy", nullptr, nullptr, 0.11f, 0, 0},
    {"torso_g", "rootdummy", "neck_g", 0.13f, 0.45f, 0.35f},
    {"head_g", "torso_g", "head", 0.10f, 0.7f, 0.7f},
    {"lbicep_g", "torso_g", "lforearm_g", 0.055f, 1.7f, 0.8f},
    {"lforearm_g", "lbicep_g", "lhand_g", 0.045f, 1.4f, 0.3f},
    {"rbicep_g", "torso_g", "rforearm_g", 0.055f, 1.7f, 0.8f},
    {"rforearm_g", "rbicep_g", "rhand_g", 0.045f, 1.4f, 0.3f},
    {"lthigh_g", "rootdummy", "lshin_g", 0.07f, 1.2f, 0.4f},
    {"lshin_g", "lthigh_g", "lfoot_g", 0.055f, 1.4f, 0.2f},
    {"rthigh_g", "rootdummy", "rshin_g", 0.07f, 1.2f, 0.4f},
    {"rshin_g", "rthigh_g", "rfoot_g", 0.055f, 1.4f, 0.2f},
};

b3Vec3 BV(V3 v) { return b3Vec3{v.x, v.y, v.z}; }
V3 VB(b3Vec3 v) { return V3{v.x, v.y, v.z}; }
b3Quat BQ(Q4 q) { b3Quat r; r.v = b3Vec3{q.x, q.y, q.z}; r.s = q.w; return r; }
Q4 QB(b3Quat q) { return Q4{q.s, q.v.x, q.v.y, q.v.z}; }

void DestroyWorld(Doll& d) {
    if (b3World_IsValid(d.world)) b3DestroyWorld(d.world);
    d.world = b3_nullWorldId;
    if (d.mesh) { b3DestroyMesh(d.mesh); d.mesh = nullptr; }
    for (auto& b : d.bones) b.body = b3_nullBodyId;
}

bool StartDoll(void* cre, void* gob) {
    Doll* dp = new Doll();
    Doll& d = *dp;
    d.id = Id(cre); d.gob = gob;
    Collect(d, F<void*>(gob, 0x90), -1, 0);
    if (d.parts.size() < 3) { delete dp; return false; }
    std::vector<WT> w;
    ComputeWorld(d, w, false);
    Vec3 cp = Pos(cre);
    d.anchor = V3{cp.x, cp.y, cp.z};
    d.ground = cp.z;

    // bones: humanoid template, or every "_g" part with length as a generic chain
    struct Def { int part; int parentBone; int tip; float radius, cone, twist; };
    std::vector<Def> defs;
    bool humanoid = FindPart(d, "rootdummy") >= 0 && FindPart(d, "torso_g") >= 0 && FindPart(d, "lthigh_g") >= 0;
    float gs = GobScale(gob);
    if (humanoid) {
        for (const Tmpl& t : HUMANOID) {
            int p = FindPart(d, t.name);
            if (p < 0) continue;
            int parentBone = -1;
            if (t.parent) for (size_t k = 0; k < defs.size(); k++) if (_stricmp(PartName(d.parts[defs[k].part].part), t.parent) == 0) parentBone = (int)k;
            if (t.parent && parentBone < 0) continue;
            defs.push_back({p, parentBone, t.tip ? FindPart(d, t.tip) : -1, t.radius * gs, t.cone, t.twist});
        }
    } else {
        std::vector<int> boneOf(d.parts.size(), -1);
        // root: the "rootdummy" of the skeleton, or the first part below the model root that has children
        int rootPart = -1;
        for (size_t i = 1; i < d.parts.size() && rootPart < 0; i++) {
            std::string n = PartName(d.parts[i].part);
            for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
            if (n.find("rootdummy") != std::string::npos) rootPart = (int)i;
        }
        if (rootPart < 0) for (size_t i = 1; i < d.parts.size(); i++) if (d.parts[i].parent == 0 && F<int>(d.parts[i].part, 0x40) > 0) { rootPart = (int)i; break; }
        if (rootPart < 0) { delete dp; return false; }
        boneOf[rootPart] = 0;
        defs.push_back({rootPart, -1, -1, 0.08f * gs, 0, 0});
        for (size_t i = rootPart + 1; i < d.parts.size(); i++) {
            // only the root's subtree
            int a = d.parts[i].parent; bool inside = false;
            for (; a >= 0; a = d.parts[a].parent) if (a == rootPart) { inside = true; break; }
            if (!inside) continue;
            std::string n = PartName(d.parts[i].part);
            for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
            const char* cn = ClassName(d.parts[i].part);
            bool mesh = strstr(cn, "TriMesh") || strstr(cn, "Skin") || strstr(cn, "Dangly");
            bool named = n.size() > 2 && n.compare(n.size() - 2, 2, "_g") == 0;
            bool helper = n.find("impact") != std::string::npos || n.find("conjure") != std::string::npos ||
                          n.find("dummy") != std::string::npos || n == "head" || n.find("_fx") != std::string::npos ||
                          n.find("shadow") != std::string::npos || strstr(cn, "Emitter") || strstr(cn, "Light");
            if (helper || (!mesh && !named)) continue;
            int pb = -1;
            for (int b = d.parts[i].parent; b >= 0; b = d.parts[b].parent) if (boneOf[b] >= 0) { pb = boneOf[b]; break; }
            if (pb < 0) continue;
            if (defs.size() >= 24) break;
            boneOf[i] = (int)defs.size();
            defs.push_back({(int)i, pb, -1, 0.05f * gs, 0.8f, 0.5f});
        }
        // tips: the first child bone that is away from the bone's origin
        for (size_t k = 0; k < defs.size(); k++)
            for (size_t j = k + 1; j < defs.size(); j++)
                if (defs[j].parentBone == (int)k && Len(w[defs[j].part].p - w[defs[k].part].p) > 0.05f * gs) { defs[k].tip = defs[j].part; break; }
    }
    if (defs.size() < 2) { delete dp; return false; }

    b3WorldDef wd = b3DefaultWorldDef();
    wd.gravity = b3Vec3{0, 0, -9.81f};
    d.world = b3CreateWorld(&wd);
    // ground: the walkmesh around the body as a height grid; what is not walkable (walls, furniture)
    // is raised so limbs do not sink into it
    {
        const int N = 29; const float cell = 0.25f;           // 7 x 7 m
        std::vector<b3Vec3> v(N * N);
        std::vector<char> walk(N * N, 0);
        void* area = F<void*>(cre, OFS::Obj_Area);
        float x0 = cp.x - cell * (N - 1) * 0.5f, y0 = cp.y - cell * (N - 1) * 0.5f;
        int walkable = 0;
        for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
            float h = d.ground;
            bool ok = GroundAt(area, x0 + i * cell, y0 + j * cell, h);
            if (ok && fabsf(h - d.ground) > 3.0f) ok = false;   // another floor
            walk[j * N + i] = ok; walkable += ok;
            v[j * N + i] = b3Vec3{x0 + i * cell, y0 + j * cell, ok ? h : d.ground};
        }
        if (walkable > 4) {
            // blocked points: the height of a walkable neighbour plus a wall
            for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
                if (walk[j * N + i]) continue;
                float best = d.ground; float bd = 1e9f;
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
            d.mesh = b3CreateMesh(&md, nullptr, 0);
        }
        b3BodyDef bd = b3DefaultBodyDef();
        b3BodyId g = b3CreateBody(d.world, &bd);
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.baseMaterial.friction = 0.9f;
        if (d.mesh) b3CreateMeshShape(g, &sd, d.mesh, b3Vec3{1, 1, 1});
        else {
            b3BoxHull box = b3MakeOffsetBoxHull(25.0f, 25.0f, 0.5f, b3Vec3{cp.x, cp.y, d.ground - 0.5f});
            b3CreateHullShape(g, &sd, &box.base);
        }
    }
    Q4 facing = w[0].q;
    V3 back = Rot(facing, V3{0, -1, 0});               // models face +y
    for (size_t k = 0; k < defs.size(); k++) {
        const Def& df = defs[k];
        Bone b; b.part = df.part; b.parent = df.parentBone; b.root = k == 0;
        b.wp = w[df.part].p; b.wq = w[df.part].q;
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = BV(b.wp);
        bd.rotation = BQ(b.wq);
        bd.linearDamping = 0.1f;
        bd.angularDamping = 0.8f;
        // the upper body falls backwards a little, as if hit from the front
        float up = Len(b.wp - V3{cp.x, cp.y, cp.z});
        bd.linearVelocity = BV(back * (C.push * fminf(1.0f, up / 1.4f)));
        b.body = b3CreateBody(d.world, &bd);
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.filter.groupIndex = -1;                    // no collisions inside the ragdoll
        sd.baseMaterial.friction = 0.8f;
        sd.baseMaterial.rollingResistance = 0.05f;
        sd.density = 1000.0f;
        V3 tipLocal;
        if (df.tip >= 0) tipLocal = Rot(Conj(b.wq), w[df.tip].p - b.wp);
        else if (b.root) {
            // pelvis: across the hips
            int lt = FindPart(d, "lthigh_g"), rt = FindPart(d, "rthigh_g");
            if (lt >= 0 && rt >= 0) {
                V3 a = Rot(Conj(b.wq), w[lt].p - b.wp), c = Rot(Conj(b.wq), w[rt].p - b.wp);
                b3Capsule cap{BV(a), BV(c), df.radius};
                b3CreateCapsuleShape(b.body, &sd, &cap);
                d.bones.push_back(b);
                continue;
            }
            tipLocal = V3{0, 0, 0.15f * gs};
        } else tipLocal = V3{0, 0, -0.18f * gs};
        float len = Len(tipLocal);
        if (len < 0.02f) { b3Sphere sp{b3Vec3{0, 0, 0}, df.radius}; b3CreateSphereShape(b.body, &sd, &sp); }
        else {
            V3 dir = tipLocal * (1.0f / len);
            float r = fminf(df.radius, len * 0.45f);
            b3Capsule cap{BV(dir * r), BV(tipLocal - dir * r), r};
            b3CreateCapsuleShape(b.body, &sd, &cap);
        }
        d.bones.push_back(b);
        // joint to the parent bone at this bone's origin; z axes along the child bone
        if (df.parentBone >= 0) {
            Bone& pb = d.bones[df.parentBone];
            V3 axisW = Rot(b.wq, len > 0.02f ? tipLocal * (1.0f / len) : V3{0, 0, 1});
            Q4 frameW = FromTo(V3{0, 0, 1}, axisW);
            b3SphericalJointDef jd = b3DefaultSphericalJointDef();
            jd.base.bodyIdA = pb.body;
            jd.base.bodyIdB = b.body;
            jd.base.localFrameA.p = BV(Rot(Conj(pb.wq), b.wp - pb.wp));
            jd.base.localFrameA.q = BQ(Norm(Conj(pb.wq) * frameW));
            jd.base.localFrameB.p = b3Vec3{0, 0, 0};
            jd.base.localFrameB.q = BQ(Norm(Conj(b.wq) * frameW));
            jd.base.collideConnected = false;
            jd.enableConeLimit = true;
            jd.coneAngle = df.cone;
            jd.enableTwistLimit = true;
            jd.lowerTwistAngle = -df.twist;
            jd.upperTwistAngle = df.twist;
            jd.enableSpring = true;                    // a little muscle tone left
            jd.hertz = 0.6f;
            jd.dampingRatio = 0.4f;
            jd.targetRotation = BQ(Q4{});
            b3CreateSphericalJoint(d.world, &jd);
        }
    }
    for (size_t k = 0; k < d.bones.size(); k++) d.parts[d.bones[k].part].bone = (int)k;
    g_dolls[d.id] = dp;
    g_byGob[gob] = dp;
    Log("ragdoll: %08x '%s' %s, %d parts, %d bones", d.id, PartName(d.parts[0].part), humanoid ? "humanoid" : "generic", (int)d.parts.size(), (int)d.bones.size());
    return true;
}

void FreeDoll(Doll* d, bool restore = false) {
    if (!d) return;
    // give the animation back (creature alive again): restore the frozen locals once
    if (restore && g_byGob.count(d->gob)) {
        for (auto& r : d->parts) if (Readable(r.part, 0x30)) { F<V3>(r.part, 0x10) = r.lp; F<Q4>(r.part, 0x1c) = r.lq; }
    }
    DestroyWorld(*d);
    g_byGob.erase(d->gob);
    g_dolls.erase(d->id);
    delete d;
}

// the creature's model was rebuilt (equipment taken from the corpse ...): pose the new one the same way
bool Rebind(Doll& d, void* gob) {
    std::vector<std::string> names;
    std::vector<Bone> old = d.bones;
    for (auto& b : old) names.push_back(PartName(d.parts[b.part].part));
    g_byGob.erase(d.gob);
    d.gob = gob;
    d.parts.clear();
    Collect(d, F<void*>(gob, 0x90), -1, 0);
    d.bones.clear();
    for (size_t k = 0; k < old.size(); k++) {
        int p = FindPart(d, names[k].c_str());
        if (p < 0) continue;
        Bone b = old[k]; b.part = p; b.body = b3_nullBodyId;
        b.parent = -1;
        d.bones.push_back(b);
        d.parts[p].bone = (int)d.bones.size() - 1;
    }
    g_byGob[gob] = &d;
    Log("ragdoll: %08x model rebuilt, %d bones kept", d.id, (int)d.bones.size());
    return !d.bones.empty() && d.bones[0].root;
}

void Settle(Doll& d) {
    d.done = true;
    DestroyWorld(d);
    // single player: put the corpse object under the body (looting and selection follow)
    if (C.reposition && !d.moved) {
        void* sc = ServerCreature(d.id);
        if (sc && fSrvSetPosition && d.bones.size()) {
            V3 b = d.bones[0].wp;
            V3 np{b.x, b.y, d.anchor.z};
            V3 off = np - d.anchor;
            float l = sqrtf(off.x * off.x + off.y * off.y);
            if (l > 2.0f) { np = d.anchor + off * (2.0f / l); np.z = d.anchor.z; }
            if (l > 0.05f) { fSrvSetPosition(sc, &np, 0); Log("ragdoll: %08x corpse moved %.2f m", d.id, l); }
            d.moved = true;
        }
    }
}

void Step(Doll& d, float dt) {
    if (d.done || !b3World_IsValid(d.world)) return;
    b3World_Step(d.world, dt, 4);
    d.t += dt;
    float maxV = 0;
    for (auto& b : d.bones) {
        b3WorldTransform tf = b3Body_GetTransform(b.body);
        b.wp = VB(tf.p); b.wq = Norm(QB(tf.q));
        V3 lv = VB(b3Body_GetLinearVelocity(b.body)), av = VB(b3Body_GetAngularVelocity(b.body));
        maxV = fmaxf(maxV, fmaxf(Len(lv), Len(av) * 0.15f));
    }
    // against a remote server the corpse stays where it is: keep the body over it
    bool canMove = C.reposition && ServerCreature(d.id) != nullptr;
    if (!canMove && d.bones.size()) {
        Bone& r = d.bones[0];
        V3 off = r.wp - d.anchor; off.z = 0;
        float l = Len(off);
        if (l > C.pinRadius) {
            V3 v = VB(b3Body_GetLinearVelocity(r.body));
            V3 n = off * (1.0f / l);
            float out = Dot(v, n);
            if (out > 0) v = v - n * out;
            v = v - n * ((l - C.pinRadius) * 4.0f);
            b3Body_SetLinearVelocity(r.body, BV(v));
        }
    }
    d.calm = maxV < 0.08f ? d.calm + dt : 0;
    if ((d.t > 0.8 && d.calm > 0.5) || d.t > C.settleTime) {
        Settle(d);
        if (C.debug) {
            Log("ragdoll: %08x settled after %.1f s (ground %.2f)", d.id, d.t, d.ground);
            for (auto& b : d.bones) Log("ragdoll:   %-12s %.2f %.2f %.2f", PartName(d.parts[b.part].part), b.wp.x, b.wp.y, b.wp.z);
        }
    }
}

void DevCommands() {
    if (!C.devFile[0]) return;
    FILE* f = fopen(C.devFile, "r");
    if (!f) return;
    char line[128] = {};
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    DeleteFileA(C.devFile);
    if (strncmp(line, "cam ", 4) == 0) {      // cam <zoom> <pitch> <yaw>
        float z = 0, p = 0, y = 0;
        void* in = Internal();
        if (in && sscanf(line + 4, "%f %f %f", &z, &p, &y) == 3) {
            Log("dev: cam was %.2f %.2f %.2f", F<float>(in, OFS::In_CamZoom), F<float>(in, OFS::In_CamPitch), F<float>(in, OFS::In_CamYaw));
            F<float>(in, OFS::In_CamZoom) += z; F<float>(in, OFS::In_CamPitch) += p; F<float>(in, OFS::In_CamYaw) += y;
        }
        return;
    }
    if (strncmp(line, "ground", 6) == 0) {
        void* pl = Player(); void* area = PlayerArea();
        if (pl) {
            Vec3 p = Pos(pl);
            for (int dy = -6; dy <= 6; dy++) {
                char row[256] = {}; int k = 0;
                for (int dx = -8; dx <= 8; dx++) {
                    float h = 0; bool ok = GroundAt(area, p.x + dx * 0.5f, p.y + dy * 0.5f, h);
                    k += sprintf(row + k, ok ? "%6.2f " : "  ---  ", h);
                }
                Log("ground y%+d: %s", dy, row);
            }
            Log("player z %.2f", p.z);
        }
        return;
    }
    if (strncmp(line, "kill ", 5) == 0) {
        uint32_t id = (uint32_t)strtoul(line + 5, nullptr, 16);
        void* sc = ServerCreature(id);
        typedef void (*dmg_t)(void*, int);
        static dmg_t fDmg = (dmg_t)GetProcAddress(GetModuleHandleA(nullptr), "?DoDamage@CNWSCreature@@UEAAXH@Z");
        Log("dev: kill %08x -> %p", id, sc);
        if (sc && fDmg) fDmg(sc, 1000);
    }
}

// once per rendered frame, before the scene animates: watch deaths, step the simulations
void Update() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = g_last.QuadPart ? (double)(now.QuadPart - g_last.QuadPart) / g_freq.QuadPart : 0;
    if (g_last.QuadPart && dt < 0.002) return;            // other scenes of the same frame
    g_last = now;
    if (dt > 0.25) dt = 0.25;
    DevCommands();
    void* pl = Player();
    void* area = PlayerArea();
    static void* lastArea = nullptr;
    if (area != lastArea) { lastArea = area; g_dead.clear(); }
    // forget creatures that are gone (area change, destroyed corpses)
    for (auto it = g_dolls.begin(); it != g_dolls.end();) {
        Doll* d = it->second; ++it;
        void* c = GetObj(d->id);
        if (!c || F<void*>(c, OFS::Obj_Area) != area) { FreeDoll(d); continue; }
        void* g = CreatureGob(c);
        if (g != d->gob) {
            if (g && d->done && Rebind(*d, g)) continue;
            g_byGob.erase(d->gob); d->gob = nullptr;     // old model is gone: nothing to restore
            FreeDoll(d);
        }
    }
    if (!pl || !area) { g_dead.clear(); return; }
    int n = F<int>(area, OFS::Area_ObjCount);
    uint32_t* ids = F<uint32_t*>(area, OFS::Area_ObjIds);
    Vec3 pp = Pos(pl);
    int active = 0;
    for (auto& kv : g_dolls) if (!kv.second->done) active++;
    for (int i = 0; ids && i < n && i < 100000; i++) {
        void* o = GetObj(ids[i]);
        if (!o || Type(o) != OT_CREATURE) continue;
        uint32_t id = ids[i];
        void* sc = ServerCreature(id);
        bool dead = sc && fSrvGetDead ? fSrvGetDead(sc) != 0 : ClientDead(o);
        auto it = g_dead.find(id);
        bool known = it != g_dead.end();
        bool was = known && it->second;
        g_dead[id] = dead;
        if (dead && known && !was && C.enabled) {
            Vec3 p = Pos(o);
            float dx = p.x - pp.x, dy = p.y - pp.y;
            void* gob = CreatureGob(o);
            if (gob && dx * dx + dy * dy < C.range * C.range && active < C.maxActive && !g_dolls.count(id)) {
                if (StartDoll(o, gob)) active++;
            }
        }
        if (!dead && was) { auto d = g_dolls.find(id); if (d != g_dolls.end()) { Log("ragdoll: %08x alive again", id); FreeDoll(d->second, true); } }
    }
    // fixed 60 Hz steps
    g_acc += dt;
    int steps = 0;
    while (g_acc >= 1.0 / 60 && steps < 4) {
        for (auto& kv : g_dolls) Step(*kv.second, 1.0f / 60);
        g_acc -= 1.0 / 60; steps++;
    }
    if (steps == 4) g_acc = 0;
}

void hk_SceneAnimate(void* scene, float dt) {
    Update();
    o_SceneAnimate(scene, dt);
}

void hk_GobAnimate(void* gob, float dt) {
    o_GobAnimate(gob, dt);
    auto it = g_byGob.find(gob);
    if (it == g_byGob.end()) return;
    Doll& d = *it->second;
    // freeze the pose from the moment of death, then pose the bones from the bodies
    for (auto& r : d.parts) { F<V3>(r.part, 0x10) = r.lp; F<Q4>(r.part, 0x1c) = r.lq; }
    std::vector<WT> w;
    ComputeWorld(d, w, true);
}

int IniInt(const char* sec, const char* key, int def, const char* ini) { return GetPrivateProfileIntA(sec, key, def, ini); }
float IniFloat(const char* sec, const char* key, float def, const char* ini) {
    char b[64]; GetPrivateProfileStringA(sec, key, "", b, sizeof b, ini);
    return b[0] ? (float)atof(b) : def;
}
}  // namespace

void Ragdoll_LoadConfig(const char* ini) {
    C.enabled = IniInt("Ragdoll", "Enabled", 1, ini);
    C.reposition = IniInt("Ragdoll", "MoveCorpseSinglePlayer", 1, ini);
    C.pinRadius = IniFloat("Ragdoll", "MultiplayerPinRadius", 0.6f, ini);
    C.push = IniFloat("Ragdoll", "DeathPush", 1.2f, ini);
    C.settleTime = IniFloat("Ragdoll", "MaxSimulationSeconds", 6.0f, ini);
    C.maxActive = IniInt("Ragdoll", "MaxActive", 10, ini);
    C.range = IniFloat("Ragdoll", "Range", 45.0f, ini);
    C.debug = IniInt("Ragdoll", "Debug", 0, ini);
    GetPrivateProfileStringA("Developer", "CommandFile", "", C.devFile, sizeof C.devFile, ini);
    Log("config: enabled %d moveCorpse %d pin %.2f push %.2f maxTime %.1f maxActive %d range %.0f",
        C.enabled, C.reposition, C.pinRadius, C.push, C.settleTime, C.maxActive, C.range);
}

void Ragdoll_Install() {
    HMODULE exe = GetModuleHandleA(nullptr);
    QueryPerformanceFrequency(&g_freq);
    p_AppManager = (void**)GetProcAddress(exe, "?g_pAppManager@@3PEAVCAppManager@@EA");
    fToCanonical = (Rot_t)GetProcAddress(exe, "?RotateRealToCanonicalTile@CNWTile@@QEAAXMMPEAM0@Z");
    fFindTri = (FindTri_t)GetProcAddress(exe, "?FindTriangle@CNWTileSurfaceMesh@@QEAAHHHMMH@Z");
    fMeshHeight = (Height_t)GetProcAddress(exe, "?ComputeHeight@CNWTileSurfaceMesh@@QEAAMVVector@@@Z");
    {   // not exported: check its first bytes
        static const uint8_t sig[8] = {0};
        uint8_t* p = (uint8_t*)A(0x1407d9d00);
        char hex[40]; for (int i = 0; i < 8; i++) sprintf(hex + i * 3, "%02x ", p[i]);
        Log("GetTile bytes %s", hex);
        fGetTile = (GetTile_t)p;
        (void)sig;
    }
    Log("ground fns %p %p %p", (void*)fToCanonical, (void*)fFindTri, (void*)fMeshHeight);
    fGetWorld = (GetWorld_t)GetProcAddress(exe, "?GetWorld@Part@@UEAAXAEAVVector@@AEAVQuaternion@@@Z");
    fSrvCreature = (GetCreature_t)GetProcAddress(exe, "?GetCreatureByGameObjectID@CServerExoApp@@QEAAPEAVCNWSCreature@@I@Z");
    fSrvSetPosition = (SetPos_t)GetProcAddress(exe, "?SetPosition@CNWSObject@@QEAAXVVector@@H@Z");
    fSrvGetDead = (GetDead_t)GetProcAddress(exe, "?GetDead@CNWSObject@@QEAAHXZ");
    void* ga = (void*)GetProcAddress(exe, "?Animate@Gob@@UEAAXM@Z");
    void* sa = (void*)GetProcAddress(exe, "?Animate@Scene@@UEAAXM@Z");
    Log("exports: getworld %p srvcreature %p setpos %p getdead %p gob %p scene %p", (void*)fGetWorld, (void*)fSrvCreature,
        (void*)fSrvSetPosition, (void*)fSrvGetDead, ga, sa);
    if (!fGetWorld || !ga || !sa) { Log("required exports missing - disabled"); return; }
    if (MH_CreateHook(ga, (void*)hk_GobAnimate, (void**)&o_GobAnimate) != MH_OK || MH_EnableHook(ga) != MH_OK ||
        MH_CreateHook(sa, (void*)hk_SceneAnimate, (void**)&o_SceneAnimate) != MH_OK || MH_EnableHook(sa) != MH_OK) {
        Log("hooks failed - disabled"); return;
    }
    Log("ready");
}
