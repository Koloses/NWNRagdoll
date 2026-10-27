// NWNRagdoll - ragdoll physics for dying creatures (client-side visuals, Box3D)
//
// The engine animates a creature's model as a tree of Parts (local position / rotation, w-first
// quaternions). When a creature dies its death animation plays for a moment; then the pose of that
// instant is frozen and the main bones are handed to a Box3D ragdoll that starts with the animation's
// own motion. Gob::Animate writes the animated local transforms each frame; right after it we put
// back the frozen pose and replace the rotations of the bones with the rigid bodies. When the body
// has settled the final pose is kept for good. Weapons and shields leave the hands and fall on their own.
// In single player (the server runs in this process) the corpse object is moved under the body so
// looting and selection match what is on screen; against a remote server the body is held close
// to the corpse position instead. Explosions wake settled bodies up again.
#include "ragdoll.h"
#include "phys.h"
#include "props.h"
#include "cloth.h"
#include "game.h"
#include "qmath.h"
#include "log.h"
#include "blood.h"
#include <windows.h>
#include <MinHook.h>
#include <box3d/box3d.h>
#include <cstring>
#include <cmath>
#include <vector>
#include <map>
#include <string>

using namespace G;
using P::BV; using P::VB; using P::BQ; using P::QB;

namespace {
// ---- settings ----
struct Cfg {
    int enabled = 1;
    int reposition = 1;          // single player: move the corpse object under the body
    float pinRadius = 0;         // remote server: how far the body may end up from the corpse position (0: free)
    float push = 2.0f;           // velocity added away from the killer at death (m/s, at head height)
    float animLead = 0.24f;      // seconds of the death animation before the physics takes over
    float muscle = 1.0f;         // strength of the fading muscle tone (0 = limp at once)
    float settleTime = 6.0f;     // seconds of simulation at most
    int maxActive = 10;          // ragdolls simulated at the same time
    float range = 45.0f;         // only creatures this close to the player
    int dropWeapons = 1;         // weapons and shields fall from the hands
    int explosions = 1;          // blasts push bodies
    int kick = 1;                // walking into a body pushes it
    float kickSpeed = 2.0f;      // m/s added at the foot
    int keepCorpses = 1;         // dead creatures stay instead of fading away
    int maxCorpses = 60;         // kept per area; the oldest fade when there are more
    int debug = 0;
    char devFile[260] = "";      // testing commands
} C;

typedef void (*Animate_t)(void* gob, float dt);
Animate_t o_GobAnimate = nullptr, o_SceneAnimate = nullptr;
typedef int (*AttachControl_t)(void* beh, float dt);
AttachControl_t o_AttachControl = nullptr;
typedef int (*GetDead_t)(void* obj);
GetDead_t fSrvGetDead = nullptr;

void* CreatureGob(void* cre) { return P::ObjectGob(cre); }

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
enum JointKind { J_NONE, J_BALL, J_HINGE };
enum : uint64_t { CAT_GROUND = 1, CAT_CORE = 2, CAT_ARM = 4, CAT_LEG = 8, CAT_DROP = 16, CAT_LIMB = 32 };
struct Bone {
    int part;                    // index into parts
    int parent = -1;             // parent bone
    bool root = false;
    // shape (body local)
    bool capsule = true; V3 a, b; float radius = 0.05f;
    uint64_t cat = CAT_CORE;
    // joint to the parent (frames in body-local space, fixed at the death pose)
    JointKind joint = J_NONE;
    b3Transform frameA, frameB;
    float cone = 0, twist = 0, lower = 0, upper = 0;
    // simulation
    b3BodyId body = b3_nullBodyId;
    b3JointId jid = b3_nullJointId;
    V3 wp; Q4 wq;                // world transform (live while simulating, final afterwards)
    V3 v, w;                     // velocities to start with
};
struct Drop {                    // a weapon / shield that left the hand
    void* gob;                   // the attached Gob
    V3 center, half;             // box in the gob's frame
    b3BodyId body = b3_nullBodyId;
    V3 wp; Q4 wq; V3 v, w;
};
struct Doll {
    uint32_t id = 0;
    void* gob = nullptr;
    std::vector<PartRec> parts;
    std::vector<Bone> bones;
    std::vector<Drop> drops;
    b3WorldId world = b3_nullWorldId;
    double t = 0, calm = 0;
    bool done = false;
    float muscle = 0;            // spring stiffness at the start of this simulation
    V3 anchor;                   // corpse position (server) / where the body is held (remote)
    float ground = 0;
    b3MeshData* mesh = nullptr;
    bool moved = false;
    uint32_t lastBlast = 0;
    double lastKick = 0;
    char blood = 0;              // BLOODCOLR of the creature
    // a settled body's pose: the bone parts' locals as computed for this root placement (reused every frame)
    struct Pose { bool valid = false; V3 rp; Q4 rq; std::vector<V3> lp; std::vector<Q4> lq; } pose;
    bool bled = false;
    float scale = 1;
};
std::map<uint32_t, Doll*> g_dolls;          // by creature id
std::map<void*, Doll*> g_byGob;
std::map<void*, Drop*> g_dropByGob;
std::map<uint32_t, bool> g_dead;            // last known dead state
double g_lastUpdate = 0;
double g_acc = 0;

// death animation playing before the ragdoll: the last animated pose gives the starting velocities
struct WT { V3 p; Q4 q; float s; };
struct Pending { uint32_t id; void* gob; double t0; double lastT = 0; double moveT = 0; std::vector<WT> prev, first; };
std::map<void*, Pending> g_pending;         // by gob

void Collect(Doll& d, void* p, int parent, int depth) {
    if (!p || depth > 48 || d.parts.size() > 400 || !Readable(p, 0x58)) return;
    PartRec r; r.part = p; r.parent = parent; r.lp = F<V3>(p, 0x10); r.lq = F<Q4>(p, 0x1c); r.s = F<float>(p, 0x2c);
    int me = (int)d.parts.size();
    d.parts.push_back(r);
    void** ch = F<void**>(p, 0x38); int n = F<int>(p, 0x40);
    for (int i = 0; ch && i < n && i < 128; i++) Collect(d, ch[i], me, depth + 1);
}
int FindPart(const Doll& d, const char* name) {
    for (size_t i = 0; i < d.parts.size(); i++) if (_stricmp(P::PartName(d.parts[i].part), name) == 0) return (int)i;
    return -1;
}

// world transforms of all parts from their current locals (parts are in parent-before-child order);
// with bones: the bone parts' locals are first set so they land where their bodies are
void ComputeWorld(Doll& d, std::vector<WT>& w, bool useBones) {
    w.resize(d.parts.size());
    V3 rp; Q4 rq;
    P::GetWorld(d.parts[0].part, rp, rq);
    float gs = P::GobScale(d.gob);
    for (size_t i = 0; i < d.parts.size(); i++) {
        PartRec& r = d.parts[i];
        if (r.parent < 0) { w[i] = {rp, rq, gs * r.s}; continue; }
        const WT& pw = w[r.parent];
        if (useBones && r.bone >= 0) {
            const Bone& b = d.bones[r.bone];
            Q4 lq = Norm(Conj(pw.q) * b.wq);
            F<Q4>(r.part, 0x1c) = lq;
            if (b.root) {
                V3 lp = Rot(Conj(pw.q), b.wp - pw.p) * (1.0f / (pw.s > 1e-4f ? pw.s : 1.0f));
                F<V3>(r.part, 0x10) = lp;
            }
            w[i] = {b.root ? b.wp : pw.p + Rot(pw.q, F<V3>(r.part, 0x10) * pw.s), b.wq, pw.s * r.s};
            continue;
        }
        w[i] = {pw.p + Rot(pw.q, F<V3>(r.part, 0x10) * pw.s), pw.q * F<Q4>(r.part, 0x1c), pw.s * r.s};
    }
}
// the same from a raw part tree, in Collect order (pending deaths: no Doll yet)
void TreeWorld(void* root, std::vector<WT>& w, float gs) {
    w.clear();
    struct S { void* p; int par; int depth; };
    std::vector<S> st{{root, -1, 0}};
    while (!st.empty()) {
        S s = st.back(); st.pop_back();
        if (!s.p || s.depth > 48 || w.size() > 400 || !Readable(s.p, 0x58)) continue;
        int me = (int)w.size();
        WT t;
        if (s.par < 0) { V3 rp; Q4 rq; P::GetWorld(s.p, rp, rq); t = {rp, rq, gs * F<float>(s.p, 0x2c)}; }
        else {
            const WT pw = w[s.par];
            t = {pw.p + Rot(pw.q, F<V3>(s.p, 0x10) * pw.s), pw.q * F<Q4>(s.p, 0x1c), pw.s * F<float>(s.p, 0x2c)};
        }
        w.push_back(t);
        void** ch = F<void**>(s.p, 0x38); int n = F<int>(s.p, 0x40);
        for (int i = (n < 128 ? n : 128) - 1; ch && i >= 0; i--) st.push_back({ch[i], me, s.depth + 1});
    }
}

// ---- skeleton template (the standard NWN humanoid bone names; other skeletons use a generic chain) ----
struct Tmpl { const char* name; const char* parent; const char* tip; float radius; float cone; float twist; JointKind joint; uint64_t cat; };
const Tmpl HUMANOID[] = {
    {"rootdummy", nullptr, nullptr, 0.11f, 0, 0, J_NONE, CAT_CORE},
    {"torso_g", "rootdummy", "neck_g", 0.13f, 0.45f, 0.35f, J_BALL, CAT_CORE},
    {"head_g", "torso_g", "head", 0.10f, 0.7f, 0.7f, J_BALL, CAT_CORE},
    {"lbicep_g", "torso_g", "lforearm_g", 0.055f, 1.5f, 0.8f, J_BALL, CAT_ARM},
    {"lforearm_g", "lbicep_g", "lhand_g", 0.045f, 0, 0, J_HINGE, CAT_ARM},
    {"rbicep_g", "torso_g", "rforearm_g", 0.055f, 1.5f, 0.8f, J_BALL, CAT_ARM},
    {"rforearm_g", "rbicep_g", "rhand_g", 0.045f, 0, 0, J_HINGE, CAT_ARM},
    {"lthigh_g", "rootdummy", "lshin_g", 0.07f, 1.1f, 0.4f, J_BALL, CAT_LEG},
    {"lshin_g", "lthigh_g", "lfoot_g", 0.055f, 0, 0, J_HINGE, CAT_LEG},
    {"rthigh_g", "rootdummy", "rshin_g", 0.07f, 1.1f, 0.4f, J_BALL, CAT_LEG},
    {"rshin_g", "rthigh_g", "rfoot_g", 0.055f, 0, 0, J_HINGE, CAT_LEG},
};
uint64_t MaskOf(uint64_t cat) {
    // hips and shoulders overlap the torso: those pairs never collide; limbs collide with each other
    switch (cat) {
    case CAT_CORE: return CAT_GROUND | CAT_ARM;
    case CAT_ARM: return CAT_GROUND | CAT_CORE | CAT_ARM | CAT_LEG;
    case CAT_LEG: return CAT_GROUND | CAT_ARM | CAT_LEG;
    case CAT_DROP: return CAT_GROUND;
    // limbs of a generic skeleton (beetles, spiders ...) start packed together: touching each other they push
    // apart hard enough to throw the body away, so they only meet the ground
    case CAT_LIMB: return CAT_GROUND;
    }
    return CAT_GROUND;
}

void DestroyWorld(Doll& d) {
    if (b3World_IsValid(d.world)) b3DestroyWorld(d.world);
    d.world = b3_nullWorldId;
    if (d.mesh) { b3DestroyMesh(d.mesh); d.mesh = nullptr; }
    for (auto& b : d.bones) { b.body = b3_nullBodyId; b.jid = b3_nullJointId; }
    for (auto& x : d.drops) x.body = b3_nullBodyId;
}

// where the killing blow came from: the server's "last damager" in single player; online, the closest
// creature on the other side (your party for enemies, the nearest hostile for everyone else)
bool KillerPos(void* cre, V3& out) {
    Vec3 cp = Pos(cre);
    void* sc = P::ServerCreature(Id(cre));
    if (sc) {
        void* so = VCall<void* (*)(void*)>(sc, 0x20)(sc);         // AsNWSObject
        uint32_t dmg = so ? F<uint32_t>(so, 0x198) : 0x7f000000;
        if (dmg != 0x7f000000 && dmg != (Id(cre) & 0x7fffffffu)) {
            void* o = P::ServerObject(dmg);
            if (o) { out = F<V3>(o, 0xa4); if (C.debug) Log("ragdoll: killer %08x", dmg); return true; }
        }
    }
    void* pl = Player();
    void* area = PlayerArea();
    if (!pl || !area) return false;
    bool enemy = F<uint8_t>(cre, OFS::Cre_Reaction) >= 2;
    int n = F<int>(area, OFS::Area_ObjCount);
    uint32_t* ids = F<uint32_t*>(area, OFS::Area_ObjIds);
    float best = 1e9f;
    for (int i = 0; ids && i < n; i++) {
        void* o = GetObj(ids[i]);
        if (!o || o == cre || Type(o) != OT_CREATURE || ClientDead(o)) continue;
        bool party = o == pl || F<uint32_t>(o, 0x270) == Id(pl);
        bool hostile = F<uint8_t>(o, OFS::Cre_Reaction) >= 2;
        if (enemy ? !party : !hostile) continue;
        Vec3 p = Pos(o);
        float dd = (p.x - cp.x) * (p.x - cp.x) + (p.y - cp.y) * (p.y - cp.y);
        if (dd < best && dd < 30.0f * 30.0f) { best = dd; out = V3{p.x, p.y, p.z}; }
    }
    return best < 1e9f;
}

// the body topples nearly in one piece at first, arms looser
float MuscleScale(const Bone& b) { return b.cat == CAT_ARM ? 0.5f : 1.0f; }

// ---- simulation world: ground, bodies, joints, dropped weapons ----
void BuildWorld(Doll& d, void* area, bool springs) {
    b3WorldDef wd = b3DefaultWorldDef();
    wd.gravity = b3Vec3{0, 0, -9.81f};
    d.world = b3CreateWorld(&wd);
    V3 c = d.bones.empty() ? d.anchor : d.bones[0].wp;
    d.mesh = P::AddGround(d.world, area, V3{c.x, c.y, d.ground}, d.ground, springs ? 29 : 45, 0.25f);
    for (auto& b : d.bones) {
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = BV(b.wp);
        bd.rotation = BQ(b.wq);
        bd.linearVelocity = BV(b.v);
        bd.angularVelocity = BV(b.w);
        bd.linearDamping = 0.1f;
        bd.angularDamping = 1.0f;
        b.body = b3CreateBody(d.world, &bd);
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.filter.categoryBits = b.cat;
        sd.filter.maskBits = MaskOf(b.cat);
        sd.baseMaterial.friction = b.cat == CAT_LEG ? 0.5f : 0.8f;
        sd.baseMaterial.rollingResistance = 0.05f;
        sd.density = b.cat == CAT_LIMB ? 400.0f : 1000.0f;
        if (b.capsule) { b3Capsule cap{BV(b.a), BV(b.b), b.radius}; b3CreateCapsuleShape(b.body, &sd, &cap); }
        else { b3Sphere sp{b3Vec3{0, 0, 0}, b.radius}; b3CreateSphereShape(b.body, &sd, &sp); }
    }
    for (auto& b : d.bones) {
        if (b.parent < 0 || b.joint == J_NONE) continue;
        Bone& pb = d.bones[b.parent];
        float hz = springs ? 2.5f * d.muscle * MuscleScale(b) : 0.0f;
        if (b.joint == J_HINGE) {
            b3RevoluteJointDef jd = b3DefaultRevoluteJointDef();
            jd.base.bodyIdA = pb.body; jd.base.bodyIdB = b.body;
            jd.base.localFrameA = b.frameA; jd.base.localFrameB = b.frameB;
            jd.base.collideConnected = false;
            jd.enableLimit = true; jd.lowerAngle = b.lower; jd.upperAngle = b.upper;
            jd.enableSpring = hz > 0; jd.hertz = hz; jd.dampingRatio = 0.7f; jd.targetAngle = 0;
            b.jid = b3CreateRevoluteJoint(d.world, &jd);
        } else {
            b3SphericalJointDef jd = b3DefaultSphericalJointDef();
            jd.base.bodyIdA = pb.body; jd.base.bodyIdB = b.body;
            jd.base.localFrameA = b.frameA; jd.base.localFrameB = b.frameB;
            jd.base.collideConnected = false;
            jd.enableConeLimit = true; jd.coneAngle = b.cone;
            jd.enableTwistLimit = true; jd.lowerTwistAngle = -b.twist; jd.upperTwistAngle = b.twist;
            jd.enableSpring = hz > 0; jd.hertz = hz; jd.dampingRatio = 0.7f;
            jd.targetRotation = BQ(Q4{});
            b.jid = b3CreateSphericalJoint(d.world, &jd);
        }
    }
    for (auto& x : d.drops) {
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = BV(x.wp); bd.rotation = BQ(x.wq);
        bd.linearVelocity = BV(x.v); bd.angularVelocity = BV(x.w);
        bd.angularDamping = 0.3f;
        x.body = b3CreateBody(d.world, &bd);
        b3ShapeDef sd = b3DefaultShapeDef();
        sd.filter.categoryBits = CAT_DROP; sd.filter.maskBits = MaskOf(CAT_DROP);
        sd.baseMaterial.friction = 0.7f;
        sd.baseMaterial.restitution = 0.15f;
        sd.density = 600.0f;
        b3BoxHull box = b3MakeOffsetBoxHull(x.half.x, x.half.y, x.half.z, BV(x.center));
        b3CreateHullShape(x.body, &sd, &box.base);
    }
    d.t = 0; d.calm = 0; d.done = false;
}

// muscle tone fades over the first second
void UpdateMuscles(Doll& d) {
    if (d.muscle <= 0) return;
    float k = 1.0f - (float)(d.t / 1.6);
    float base = k > 0 ? 2.5f * d.muscle * k : 0.0f;
    for (auto& b : d.bones) {
        if (B3_IS_NULL(b.jid)) continue;
        float hz = base * MuscleScale(b);
        if (b.joint == J_HINGE) { if (hz > 0) b3RevoluteJoint_SetSpringHertz(b.jid, hz); else b3RevoluteJoint_EnableSpring(b.jid, false); }
        else { if (hz > 0) b3SphericalJoint_SetSpringHertz(b.jid, hz); else b3SphericalJoint_EnableSpring(b.jid, false); }
    }
    if (base <= 0) d.muscle = 0;
}

// the weapons in the hands (and a shield on the forearm) leave them
bool HandAttachment(void* c, const char** name) {
    if (!c || !Readable(c, 0x458)) return false;
    void* beh = F<void*>(c, 0x450);
    void* part = beh && Readable(beh, 0x38) ? F<void*>(beh, 0x30) : nullptr;
    const char* pn = part && Readable(part, 0x10) ? P::PartName(part) : "";
    if (name) *name = pn;
    return !_stricmp(pn, "rhand") || !_stricmp(pn, "lhand") || !_stricmp(pn, "lforearm");
}
void CollectDrops(Doll& d, V3 push) {
    if (!C.dropWeapons) return;
    void** list = F<void**>(d.gob, 0x4a8); int n = F<int>(d.gob, 0x4b0);
    for (int i = 0; list && i < n && i < 16; i++) {
        void* c = list[i];
        const char* pn = "";
        if (!HandAttachment(c, &pn)) continue;
        Drop x; x.gob = c;
        x.wp = F<V3>(c, 0xa8); x.wq = Norm(F<Q4>(c, 0xb4));
        V3 mn, mx;
        if (P::GobExtent(c, mn, mx)) {
            x.center = (mn + mx) * 0.5f;
            x.half = (mx - mn) * 0.5f;
        } else { x.center = V3{0, 0, 0.35f}; x.half = V3{0.04f, 0.04f, 0.4f}; }
        // thin parts still need some thickness to rest on the ground
        x.half = V3{fmaxf(x.half.x, 0.02f), fmaxf(x.half.y, 0.02f), fmaxf(x.half.z, 0.02f)};
        x.v = push * 0.7f + V3{P::Rand(-0.6f, 0.6f), P::Rand(-0.6f, 0.6f), P::Rand(0.8f, 1.6f)};
        x.w = V3{P::Rand(-4, 4), P::Rand(-4, 4), P::Rand(-4, 4)};
        if (C.debug) Log("ragdoll: drop '%s' from %s, box %.2f %.2f %.2f at %.2f %.2f %.2f", P::PartName(F<void*>(c, 0x90)), pn,
                         x.half.x, x.half.y, x.half.z, x.center.x, x.center.y, x.center.z);
        d.drops.push_back(x);
    }
}

// a blast that went off near here a moment ago
const P::Blast* RecentBlast(V3 at, double within) {
    if (!C.explosions) return nullptr;
    double now = P::Now();
    const P::Blast* best = nullptr;
    for (auto& b : P::Blasts()) {
        if (now - b.t > within) continue;
        V3 d = at - b.p;
        if (d.x * d.x + d.y * d.y > b.radius * b.radius) continue;
        best = &b;
    }
    return best;
}

// angular velocity taking q0 to q1 in dt
V3 AngVel(Q4 q0, Q4 q1, float dt) {
    Q4 dq = Norm(q1 * Conj(q0));
    if (dq.w < 0) dq = Q4{-dq.w, -dq.x, -dq.y, -dq.z};
    float s = sqrtf(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
    if (s < 1e-6f || dt <= 0) return V3{};
    float ang = 2.0f * atan2f(s, dq.w);
    return V3{dq.x / s, dq.y / s, dq.z / s} * (ang / dt);
}
V3 ClampLen(V3 v, float m) { float l = Len(v); return l > m ? v * (m / l) : v; }

// prevW: world transforms of the part tree one animation frame before (Collect order), or null
bool StartDoll(void* cre, void* gob, const std::vector<WT>* prevW, float prevDt) {
    Doll* dp = new Doll();
    Doll& d = *dp;
    d.id = Id(cre); d.gob = gob;
    Collect(d, F<void*>(gob, 0x90), -1, 0);
    if (d.parts.size() < 3) { delete dp; return false; }
    std::vector<WT> w;
    ComputeWorld(d, w, false);
    if (prevW && prevW->size() != w.size()) prevW = nullptr;
    Vec3 cp = Pos(cre);
    d.anchor = V3{cp.x, cp.y, cp.z};
    d.ground = cp.z;
    d.muscle = C.muscle;
    d.blood = Blood_Color(cre);

    struct Def { int part; int parentBone; int tip; float radius, cone, twist; JointKind joint; uint64_t cat; };
    std::vector<Def> defs;
    bool humanoid = FindPart(d, "rootdummy") >= 0 && FindPart(d, "torso_g") >= 0 && FindPart(d, "lthigh_g") >= 0;
    float gs = P::GobScale(gob);
    d.scale = gs;
    if (humanoid) {
        for (const Tmpl& t : HUMANOID) {
            int p = FindPart(d, t.name);
            if (p < 0) continue;
            int parentBone = -1;
            if (t.parent) for (size_t k = 0; k < defs.size(); k++) if (_stricmp(P::PartName(d.parts[defs[k].part].part), t.parent) == 0) parentBone = (int)k;
            if (t.parent && parentBone < 0) continue;
            defs.push_back({p, parentBone, t.tip ? FindPart(d, t.tip) : -1, t.radius * gs, t.cone, t.twist, t.joint, t.cat});
        }
    } else {
        std::vector<int> boneOf(d.parts.size(), -1);
        int rootPart = -1;
        for (size_t i = 1; i < d.parts.size() && rootPart < 0; i++) {
            std::string n = P::PartName(d.parts[i].part);
            for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
            if (n.find("rootdummy") != std::string::npos) rootPart = (int)i;
        }
        if (rootPart < 0) for (size_t i = 1; i < d.parts.size(); i++) if (d.parts[i].parent == 0 && F<int>(d.parts[i].part, 0x40) > 0) { rootPart = (int)i; break; }
        if (rootPart < 0) { delete dp; return false; }
        boneOf[rootPart] = 0;
        defs.push_back({rootPart, -1, -1, 0.08f * gs, 0, 0, J_NONE, CAT_CORE});
        for (size_t i = rootPart + 1; i < d.parts.size(); i++) {
            int a = d.parts[i].parent; bool inside = false;
            for (; a >= 0; a = d.parts[a].parent) if (a == rootPart) { inside = true; break; }
            if (!inside) continue;
            std::string n = P::PartName(d.parts[i].part);
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
            // generic skeletons: limbs collide with each other but not with the body they hang from
            defs.push_back({(int)i, pb, -1, 0.05f * gs, 0.8f, 0.5f, J_BALL, CAT_LIMB});
        }
        for (size_t k = 0; k < defs.size(); k++)
            for (size_t j = k + 1; j < defs.size(); j++)
                if (defs[j].parentBone == (int)k && Len(w[defs[j].part].p - w[defs[k].part].p) > 0.05f * gs) { defs[k].tip = defs[j].part; break; }
    }
    if (defs.size() < 2) { delete dp; return false; }

    // starting motion: the death animation's own motion, plus a push away from the killer (or a blast)
    V3 away = Rot(w[0].q, V3{0, -1, 0});               // models face +y: backwards
    V3 kp;
    if (KillerPos(cre, kp)) {
        V3 a{cp.x - kp.x, cp.y - kp.y, 0};
        float l = Len(a);
        if (l > 0.05f) away = a * (1.0f / l);
    }
    const P::Blast* blast = RecentBlast(d.anchor, 2.5);
    if (blast) d.lastBlast = blast->serial;
    V3 lateral = Rot(w[0].q, V3{1, 0, 0});
    for (size_t k = 0; k < defs.size(); k++) {
        const Def& df = defs[k];
        Bone b; b.part = df.part; b.parent = df.parentBone; b.root = k == 0; b.cat = df.cat; b.joint = df.joint;
        b.cone = df.cone; b.twist = df.twist; b.radius = df.radius;
        b.wp = w[df.part].p; b.wq = w[df.part].q;
        // toppling: the whole body turns over its feet, away from the killer
        float up = fmaxf(0.0f, b.wp.z - d.anchor.z);
        float tip = fminf(1.0f, up / (1.4f * gs));
        V3 axis = Cross(V3{0, 0, 1}, away);
        V3 v = Cross(axis, b.wp - d.anchor) * (C.push / (1.4f * gs));
        V3 av{};
        if (prevW && prevDt > 0.004f) {
            const WT& pw = (*prevW)[df.part];
            v = v + ClampLen((b.wp - pw.p) * (1.0f / prevDt), 6.0f);
            av = ClampLen(AngVel(pw.q, b.wq, prevDt), 12.0f);
        }
        if (blast) v = v + P::BlastVelocity(*blast, b.wp) * (0.7f + 0.3f * tip);
        b.v = v; b.w = av;
        // shape
        V3 tipLocal;
        float len = 0;
        if (df.tip >= 0) tipLocal = Rot(Conj(b.wq), w[df.tip].p - b.wp);
        else if (b.root) {
            int lt = FindPart(d, "lthigh_g"), rt = FindPart(d, "rthigh_g");
            if (lt >= 0 && rt >= 0) {
                b.a = Rot(Conj(b.wq), w[lt].p - b.wp); b.b = Rot(Conj(b.wq), w[rt].p - b.wp);
                b.capsule = true;
                d.bones.push_back(b);
                continue;
            }
            tipLocal = V3{0, 0, 0.15f * gs};
        } else tipLocal = V3{0, 0, -0.18f * gs};
        len = Len(tipLocal);
        if (len < 0.02f) b.capsule = false;
        else {
            V3 dir = tipLocal * (1.0f / len);
            float r = fminf(df.radius, len * 0.45f);
            b.radius = r; b.a = dir * r; b.b = tipLocal - dir * r;
        }
        // generic skeletons: the part's own mesh gives the shape (a beetle's body is one wide mesh, not a
        // stick to the next part); a body much lighter than its legs makes the joints throw it around
        if (!humanoid) {
            void* m = P::MdlTriMesh(d.parts[df.part].part);
            V3 mn = m ? F<V3>(m, 0xc0) : V3{}, mx = m ? F<V3>(m, 0xcc) : V3{};
            float sc = w[df.part].s > 0.001f ? w[df.part].s : gs;
            V3 ext = (mx - mn) * sc;
            if (m && ext.x > 0.02f && ext.y > 0.02f && ext.z > 0.0f && Len(ext) < 6.0f) {
                V3 c = (mn + mx) * (0.5f * sc);
                float e[3] = {ext.x, ext.y, ext.z};
                int ax = e[0] >= e[1] && e[0] >= e[2] ? 0 : e[1] >= e[2] ? 1 : 2;
                float other = fmaxf(e[(ax + 1) % 3], e[(ax + 2) % 3]);
                float r = fmaxf(0.03f, fminf(0.5f * other * 0.8f, 0.5f * e[ax]));
                float half = fmaxf(0.0f, 0.5f * e[ax] - r);
                V3 dir{ax == 0 ? 1.0f : 0, ax == 1 ? 1.0f : 0, ax == 2 ? 1.0f : 0};
                b.capsule = true; b.radius = r; b.a = c - dir * half; b.b = c + dir * half;
            }
        }
        // joint frames at this bone's origin
        if (df.parentBone >= 0) {
            Bone& pb = d.bones[df.parentBone];
            V3 dirW = Rot(b.wq, len > 0.02f ? tipLocal * (1.0f / len) : V3{0, 0, 1});
            Q4 frameW;
            if (df.joint == J_HINGE) {
                // knees and elbows bend one way only: hinge across the limb, flexion positive
                V3 parentDir = b.wp - pb.wp;
                float pl = Len(parentDir);
                parentDir = pl > 1e-4f ? parentDir * (1.0f / pl) : dirW;
                bool knee = df.cat == CAT_LEG;
                V3 axis = Cross(parentDir, dirW);
                float bend = atan2f(Len(axis), Dot(parentDir, dirW));
                if (bend > 0.25f) axis = axis * (1.0f / Len(axis));
                else {
                    // nearly straight: knees fold backwards, elbows forwards
                    axis = knee ? lateral * -1.0f : lateral;
                    axis = axis - parentDir * Dot(axis, parentDir);
                    float al = Len(axis);
                    axis = al > 1e-3f ? axis * (1.0f / al) : Rot(pb.wq, V3{1, 0, 0});
                    bend = atan2f(Dot(Cross(parentDir, dirW), axis), Dot(parentDir, dirW));
                }
                frameW = FromTo(V3{0, 0, 1}, axis);
                b.lower = fminf(-0.03f - bend, 0.0f);
                b.upper = fmaxf(2.5f - bend, 0.0f);
            } else frameW = FromTo(V3{0, 0, 1}, dirW);
            b.frameA.p = BV(Rot(Conj(pb.wq), b.wp - pb.wp));
            b.frameA.q = BQ(Norm(Conj(pb.wq) * frameW));
            b.frameB.p = b3Vec3{0, 0, 0};
            b.frameB.q = BQ(Norm(Conj(b.wq) * frameW));
        }
        d.bones.push_back(b);
    }
    for (size_t k = 0; k < d.bones.size(); k++) d.parts[d.bones[k].part].bone = (int)k;
    CollectDrops(d, away * C.push);
    BuildWorld(d, F<void*>(cre, OFS::Obj_Area), d.muscle > 0);
    if (C.debug > 1) for (size_t k = 0; k < d.bones.size(); k++) {
        const Bone& b = d.bones[k];
        Log("ragdoll:   bone %d %-24s parent %d at %.2f %.2f %.2f v %.2f %.2f %.2f w %.1f r %.3f len %.3f joint %d", (int)k, P::PartName(d.parts[b.part].part), b.parent,
            b.wp.x, b.wp.y, b.wp.z, b.v.x, b.v.y, b.v.z, Len(b.w), b.radius, b.capsule ? Len(b.b - b.a) : 0.0f, (int)b.joint);
    }
    g_dolls[d.id] = dp;
    g_byGob[gob] = dp;
    for (auto& x : dp->drops) g_dropByGob[x.gob] = &x;
    Log("ragdoll: %08x '%s' %s, %d parts, %d bones, %d dropped%s%s", d.id, P::PartName(d.parts[0].part), humanoid ? "humanoid" : "generic",
        (int)d.parts.size(), (int)d.bones.size(), (int)d.drops.size(), prevW ? ", animated start" : "", blast ? ", blast" : "");
    return true;
}

void ForgetDrops(Doll& d) {
    for (auto& x : d.drops) { auto it = g_dropByGob.find(x.gob); if (it != g_dropByGob.end() && it->second == &x) g_dropByGob.erase(it); }
}

void FreeDoll(Doll* d, bool restore = false) {
    if (!d) return;
    if (restore && g_byGob.count(d->gob)) {
        for (auto& r : d->parts) if (Readable(r.part, 0x30)) { F<V3>(r.part, 0x10) = r.lp; F<Q4>(r.part, 0x1c) = r.lq; }
    }
    DestroyWorld(*d);
    ForgetDrops(*d);
    g_byGob.erase(d->gob);
    g_dolls.erase(d->id);
    delete d;
}

// the creature's model was rebuilt (equipment taken from the corpse ...): pose the new one the same way
bool Rebind(Doll& d, void* gob) {
    std::vector<std::string> names;
    std::vector<Bone> old = d.bones;
    for (auto& b : old) names.push_back(P::PartName(d.parts[b.part].part));
    g_byGob.erase(d.gob);
    d.gob = gob;
    d.parts.clear();
    Collect(d, F<void*>(gob, 0x90), -1, 0);
    d.bones.clear();
    for (size_t k = 0; k < old.size(); k++) {
        int p = FindPart(d, names[k].c_str());
        if (p < 0) continue;
        Bone b = old[k]; b.part = p; b.body = b3_nullBodyId; b.jid = b3_nullJointId;
        b.parent = -1; b.joint = J_NONE;          // joints are not rebuilt for this model
        d.bones.push_back(b);
        d.parts[p].bone = (int)d.bones.size() - 1;
    }
    // weapons on the new model: keep the old resting places in the same order
    std::vector<Drop> oldDrops = d.drops;
    ForgetDrops(d);
    d.drops.clear();
    void** list = F<void**>(gob, 0x4a8); int n = F<int>(gob, 0x4b0);
    size_t k = 0;
    for (int i = 0; list && i < n && i < 16 && k < oldDrops.size(); i++) {
        void* c = list[i];
        if (!HandAttachment(c, nullptr)) continue;
        Drop x = oldDrops[k++]; x.gob = c; x.body = b3_nullBodyId;
        d.drops.push_back(x);
    }
    for (auto& x : d.drops) g_dropByGob[x.gob] = &x;
    g_byGob[gob] = &d;
    Log("ragdoll: %08x model rebuilt, %d bones, %d dropped kept", d.id, (int)d.bones.size(), (int)d.drops.size());
    return !d.bones.empty() && d.bones[0].root;
}

void Settle(Doll& d) {
    d.done = true;
    DestroyWorld(d);
    if (C.reposition && d.bones.size() && P::ServerCreature(d.id)) {
        V3 b = d.bones[0].wp;
        V3 np{b.x, b.y, d.anchor.z};
        V3 off = np - d.anchor;
        float l = sqrtf(off.x * off.x + off.y * off.y);
        float maxMove = d.lastBlast ? 8.0f : 2.5f;
        if (l > maxMove) { np = d.anchor + off * (maxMove / l); np.z = d.anchor.z; }
        if (l > 0.05f && P::SetServerPosition(d.id, np)) {
            Log("ragdoll: %08x corpse moved %.2f m", d.id, l);
            d.anchor = np;
        }
        d.moved = true;
    }
}

// the body is on the ground: a pool spreads under the torso, a few drops land around the head
void Bleed(Doll& d) {
    d.bled = true;
    if (!Blood_Bleeds(d.blood)) return;
    V3 torso = d.bones[0].wp, head = torso;
    float floor = d.ground;                       // where the walkmesh can't be found: not above the body's lowest point
    for (auto& b : d.bones) floor = fminf(floor, b.wp.z - b.radius);
    for (auto& b : d.bones) {
        const char* n = P::PartName(d.parts[b.part].part);
        if (_stricmp(n, "torso_g") == 0) torso = b.wp;
        if (_stricmp(n, "head_g") == 0) head = b.wp;
    }
    Blood_Spill(torso, 0.55f * d.scale, P::Rand(6.0f, 9.0f), d.blood, floor);
    int drops = 1 + (int)P::Rand(0, 2.99f);
    for (int i = 0; i < drops; i++)
        Blood_Spill(head + V3{P::Rand(-0.4f, 0.4f), P::Rand(-0.4f, 0.4f), 0} * d.scale, P::Rand(0.08f, 0.15f) * d.scale, P::Rand(0.8f, 2.0f), d.blood, floor);
}

void Step(Doll& d, float dt) {
    if (d.done || !b3World_IsValid(d.world)) return;
    UpdateMuscles(d);
    b3World_Step(d.world, dt, 4);
    d.t += dt;
    // a body never moves like this on its own: a bad contact or joint would fling it off
    for (auto& b : d.bones) {
        V3 lv = VB(b3Body_GetLinearVelocity(b.body)), av = VB(b3Body_GetAngularVelocity(b.body));
        if (Len(lv) > 12.0f) b3Body_SetLinearVelocity(b.body, BV(ClampLen(lv, 12.0f)));
        if (Len(av) > 30.0f) b3Body_SetAngularVelocity(b.body, BV(ClampLen(av, 30.0f)));
    }
    float maxV = 0;
    for (auto& b : d.bones) {
        b3WorldTransform tf = b3Body_GetTransform(b.body);
        b.wp = VB(tf.p); b.wq = Norm(QB(tf.q));
        V3 lv = VB(b3Body_GetLinearVelocity(b.body)), av = VB(b3Body_GetAngularVelocity(b.body));
        maxV = fmaxf(maxV, fmaxf(Len(lv), Len(av) * 0.15f));
    }
    for (auto& x : d.drops) {
        b3WorldTransform tf = b3Body_GetTransform(x.body);
        x.wp = VB(tf.p); x.wq = Norm(QB(tf.q));
        V3 lv = VB(b3Body_GetLinearVelocity(x.body)), av = VB(b3Body_GetAngularVelocity(x.body));
        maxV = fmaxf(maxV, fmaxf(Len(lv), Len(av) * 0.1f));
    }
    // against a remote server the corpse stays where it is: keep the body over it
    bool canMove = C.reposition && P::ServerCreature(d.id) != nullptr;
    if (!canMove && C.pinRadius > 0 && d.bones.size()) {
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
    if (!d.bled && d.t > 0.35 && d.bones.size()) Bleed(d);
    d.calm = maxV < 0.08f ? d.calm + dt : 0;
    if ((d.t > 0.8 && d.calm > 0.5) || d.t > C.settleTime) {
        Settle(d);
        if (C.debug) {
            Log("ragdoll: %08x settled after %.1f s (ground %.2f, corpse at %.2f %.2f)", d.id, d.t, d.ground, d.anchor.x, d.anchor.y);
            for (auto& b : d.bones) Log("ragdoll:   %-12s %.2f %.2f %.2f", P::PartName(d.parts[b.part].part), b.wp.x, b.wp.y, b.wp.z);
            for (auto& x : d.drops) Log("ragdoll:   drop at %.2f %.2f %.2f", x.wp.x, x.wp.y, x.wp.z);
        }
    }
}

// a blast near a body: wake it (or add to its motion)
void ApplyBlast(Doll& d, const P::Blast& bl, void* area) {
    if (d.lastBlast == bl.serial || d.bones.empty()) return;
    V3 c = d.bones[0].wp;
    V3 dd = c - bl.p;
    if (dd.x * dd.x + dd.y * dd.y > bl.radius * bl.radius) return;
    d.lastBlast = bl.serial;
    if (d.done) {
        bool joints = true;
        for (size_t k = 1; k < d.bones.size(); k++) if (d.bones[k].joint == J_NONE) joints = false;
        if (!joints) return;                      // model was rebuilt: the skeleton is not jointed any more
        for (auto& b : d.bones) { b.v = P::BlastVelocity(bl, b.wp) * P::Rand(0.85f, 1.15f); b.w = V3{P::Rand(-2, 2), P::Rand(-2, 2), P::Rand(-2, 2)}; }
        for (auto& x : d.drops) { x.v = P::BlastVelocity(bl, x.wp) * 1.3f; x.w = V3{P::Rand(-8, 8), P::Rand(-8, 8), P::Rand(-8, 8)}; }
        d.muscle = 0;
        BuildWorld(d, area, false);
        Log("ragdoll: %08x thrown by a blast", d.id);
    } else {
        for (auto& b : d.bones) b3Body_SetLinearVelocity(b.body, BV(VB(b3Body_GetLinearVelocity(b.body)) + P::BlastVelocity(bl, b.wp)));
        for (auto& x : d.drops) b3Body_SetLinearVelocity(x.body, BV(VB(b3Body_GetLinearVelocity(x.body)) + P::BlastVelocity(bl, x.wp) * 1.3f));
    }
}

// something walking through a body: the parts near its feet are kicked along
bool Jointed(const Doll& d) {
    for (size_t k = 1; k < d.bones.size(); k++) if (d.bones[k].joint == J_NONE) return false;
    return true;
}
void Kick(Doll& d, V3 from, V3 at, V3 vel, void* area) {
    double now = P::Now();
    if (d.bones.empty() || now - d.lastKick < 0.6) return;
    { V3 o = d.bones[0].wp - at; float r = 3.0f * fmaxf(1.0f, d.scale); if (o.x * o.x + o.y * o.y > r * r) return; }   // nowhere near (most bodies)
    V3 dir{vel.x, vel.y, 0};
    float speed = Len(dir);
    if (speed < 0.01f) return;
    dir = dir * (1.0f / speed);
    // closest approach of the walker's path this frame to a part lying on the ground
    float best = 1e9f;
    V3 seg = at - from; seg.z = 0;
    float sl = Dot(seg, seg);
    for (auto& b : d.bones) {
        V3 o = b.wp - from;
        if (o.z > 0.6f || o.z < -0.5f) continue;
        o.z = 0;
        float u = sl > 1e-6f ? fminf(fmaxf(Dot(o, seg) / sl, 0.0f), 1.0f) : 1.0f;
        if (u >= 1.0f && Dot(b.wp - at, dir) < -0.1f) continue;   // already behind the walker
        V3 c = o - seg * u;
        float dd = c.x * c.x + c.y * c.y;
        if (dd < best) { best = dd; }
    }
    float r = 0.4f;
    if (C.debug > 1 && best < 4.0f) Log("ragdoll: %08x walker at %.2f m (%.1f m/s)", d.id, sqrtf(best), speed);
    if (best > r * r) return;
    if (d.done && !Jointed(d)) return;                        // model was rebuilt: no skeleton to move
    d.lastKick = now;
    float k = C.kickSpeed * fminf(1.6f, 0.6f + speed / 4.0f);
    auto push = [&](V3 p, float reach) {
        V3 o = p - at; o.z = 0;
        float l = Len(o);
        float f = fmaxf(0.0f, 1.0f - l / reach);
        V3 side = l > 0.01f ? o * (1.0f / l) : dir;
        return ((dir * 0.75f + side * 0.25f) * k + V3{0, 0, k * 0.4f}) * f;
    };
    if (d.done) {
        for (auto& b : d.bones) { b.v = push(b.wp, 1.1f * d.scale); b.w = V3{P::Rand(-2, 2), P::Rand(-2, 2), P::Rand(-1, 1)} * Len(b.v) * 0.5f; }
        for (auto& x : d.drops) { x.v = push(x.wp, 0.8f) * 1.2f; x.w = V3{P::Rand(-6, 6), P::Rand(-6, 6), P::Rand(-6, 6)} * Len(x.v) * 0.3f; }
        d.muscle = 0;
        BuildWorld(d, area, false);
    } else {
        for (auto& b : d.bones) b3Body_SetLinearVelocity(b.body, BV(VB(b3Body_GetLinearVelocity(b.body)) + push(b.wp, 1.1f * d.scale)));
        for (auto& x : d.drops) b3Body_SetLinearVelocity(x.body, BV(VB(b3Body_GetLinearVelocity(x.body)) + push(x.wp, 0.8f) * 1.2f));
    }
    if (C.debug) Log("ragdoll: %08x kicked at %.1f m/s", d.id, speed);
}

// ---- corpses stay: the server's "delete this creature" is not carried out for the dead ----
typedef int (*Msg_t)(void* msg);
Msg_t o_CreatureDelete = nullptr, fReadBool = nullptr, fReadOverflow = nullptr;
std::vector<uint32_t> g_kept;               // kept corpses of this area, oldest first

bool KeepCorpse(uint32_t id) {
    void* o = GetObj(id);
    void* pl = Player();
    if (!o || !pl || Type(o) != OT_CREATURE || o == pl) { if (C.debug) Log("ragdoll: %08x obj %p", id, o); return false; }
    if (F<void*>(o, OFS::Obj_Area) != PlayerArea()) { if (C.debug) Log("ragdoll: %08x not in this area", id); return false; }
    auto it = g_dead.find(id);
    bool dead = it != g_dead.end() ? it->second : ClientDead(o);
    if (!dead) { if (C.debug) Log("ragdoll: %08x not dead", id); return false; }
    if (P::ServerApp() && P::ServerCreature(id)) {                     // still there: only out of sight
        if (C.debug) Log("ragdoll: %08x still on the server", id);
        return false;
    }
    return true;
}

// what the delete message does, for a kept corpse that has to go after all
void FadeAway(void* o) {
    typedef void (*i_t)(void*, int); typedef void (*c_t)(void*, char); typedef void (*d_t)(void*, char, int, uint32_t);
    ((i_t)A(0x140764260))(o, 0);                         // CNWCObject::SetCutsceneInvisible
    ((c_t)A(0x140764780))(o, 1);                         // SetFadeState
    ((d_t)A(0x140764420))(o, 0, 1, 3000);                // SetDesiredFadeState
    VCall<void (*)(void*)>(o, 0x188)(o);
    F<int>(o, 0x26c) = 0;
    F<int>(o, 0x104) = 1;                                // deleted once faded
}

int hk_CreatureDelete(void* msg) {
    if (C.keepCorpses && C.enabled && fReadBool && fReadOverflow && !Loading()) {
        uint32_t pos = F<uint32_t>(msg, 0x44), size = F<uint32_t>(msg, 0x40);
        const char* buf = F<const char*>(msg, 0x38);
        if (buf && (uint64_t)pos + 4 <= size) {
            uint32_t id = *(const uint32_t*)(buf + pos);
            if (C.debug) Log("ragdoll: server deletes %08x", id);
            if (KeepCorpse(id)) {
                F<uint32_t>(msg, 0x44) = pos + 4;        // ReadOBJECTIDClient
                fReadBool(msg);                          // instant or faded
                if (fReadOverflow(msg)) return 0;
                void* o = GetObj(id);
                F<int>(o, OFS::Obj_Selectable) = 0;
                g_kept.push_back(id);
                Log("ragdoll: %08x corpse kept (%d in the area)", id, (int)g_kept.size());
                while ((int)g_kept.size() > C.maxCorpses) {
                    void* old = GetObj(g_kept.front());
                    if (old) FadeAway(old);
                    g_kept.erase(g_kept.begin());
                }
                return 1;
            }
        }
    }
    return o_CreatureDelete(msg);
}

struct DevWalk { V3 a, b; double t0 = 0, secs = 0; } g_walk;
void DevWalkStep() {
    if (g_walk.secs <= 0) return;
    void* pl = Player();
    if (!pl) { g_walk.secs = 0; return; }
    float u = (float)((P::Now() - g_walk.t0) / g_walk.secs);
    if (u > 1) { u = 1; g_walk.secs = 0; }
    V3 p = g_walk.a + (g_walk.b - g_walk.a) * u;
    P::SetServerPosition(Id(pl), p);
    F<Vec3>(pl, OFS::Obj_Pos) = Vec3{p.x, p.y, p.z};
}

void DevCommands() {
    DevWalkStep();
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
    if (strncmp(line, "walk ", 5) == 0) {     // walk <x0> <y0> <x1> <y1> <seconds>: move the player in a straight line
        float s = 1;
        if (sscanf(line + 5, "%f %f %f %f %f", &g_walk.a.x, &g_walk.a.y, &g_walk.b.x, &g_walk.b.y, &s) == 5) {
            void* pl = Player();
            g_walk.a.z = g_walk.b.z = pl ? Pos(pl).z : 0;
            g_walk.t0 = P::Now(); g_walk.secs = s;
            Log("dev: walk %.1f %.1f -> %.1f %.1f in %.1f s", g_walk.a.x, g_walk.a.y, g_walk.b.x, g_walk.b.y, s);
        }
        return;
    }
    if (strncmp(line, "objs", 4) == 0) {
        void* area = PlayerArea();
        int n = area ? F<int>(area, OFS::Area_ObjCount) : 0;
        uint32_t* ids = area ? F<uint32_t*>(area, OFS::Area_ObjIds) : nullptr;
        for (int i = 0; ids && i < n; i++) {
            void* o = GetObj(ids[i]);
            if (!o) { Log("dev: %08x (no object)", ids[i]); continue; }
            Vec3 p = Pos(o);
            Log("dev: %08x type %d %s at %.1f %.1f %.1f gob %p", ids[i], Type(o), ClassName(o), p.x, p.y, p.z, P::ObjectGob(o));
        }
        return;
    }
    if (strncmp(line, "blast ", 6) == 0) {    // blast <x> <y> <z>: a fireball-sized push
        V3 p;
        if (sscanf(line + 6, "%f %f %f", &p.x, &p.y, &p.z) == 3) {
            P::Blast b{p, 6.5f, 5.0f, false, P::Now(), 0x80000000u + (uint32_t)GetTickCount()};
            for (auto& kv : g_dolls) ApplyBlast(*kv.second, b, PlayerArea());
            Props_Blast(b);
            Log("dev: blast at %.1f %.1f %.1f", p.x, p.y, p.z);
        }
        return;
    }
    if (strncmp(line, "hostile ", 8) == 0) {  // put a creature in the hostile faction (single player)
        uint32_t id = (uint32_t)strtoul(line + 8, nullptr, 16);
        void* srv = P::ServerApp();
        void* in = srv ? F<void*>(srv, 8) : nullptr;
        void* mgr = in ? F<void*>(in, 0x100b0) : nullptr;
        typedef void* (*gf_t)(void*, int);
        typedef void (*am_t)(void*, uint32_t, int);
        void* fac = mgr ? ((gf_t)A(0x140355c80))(mgr, 1) : nullptr;
        Log("dev: hostile %08x mgr %p faction %p", id, mgr, fac);
        if (fac) ((am_t)A(0x1404ab900))(fac, id & 0x7fffffffu, 1);
        return;
    }
    if (strncmp(line, "destroy ", 8) == 0) {  // destroy <id>: the server removes the object (as DestroyObject)
        uint32_t id = (uint32_t)strtoul(line + 8, nullptr, 16) & 0x7fffffffu;
        HMODULE exe = GetModuleHandleA(nullptr);
        typedef void* (*gm_t)(void*);
        typedef int (*ev_t)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, void*);
        gm_t gm = (gm_t)GetProcAddress(exe, "?GetServerAIMaster@CServerExoApp@@QEAAPEAVCServerAIMaster@@XZ");
        ev_t ev = (ev_t)GetProcAddress(exe, "?AddEventDeltaTime@CServerAIMaster@@QEAAHIIIIIPEAX@Z");
        void* srv = P::ServerApp();
        void* ai = srv && gm ? gm(srv) : nullptr;
        Log("dev: destroy %08x", id);
        if (ai && ev) ev(ai, 0, 0, id, id, F<uint32_t>((void*)A(0x140df9db4), 0), nullptr);
        return;
    }
    if (strncmp(line, "kill ", 5) == 0) {
        uint32_t id = (uint32_t)strtoul(line + 5, nullptr, 16);
        void* sc = P::ServerCreature(id);
        typedef void (*dmg_t)(void*, int);
        static dmg_t fDmg = (dmg_t)GetProcAddress(GetModuleHandleA(nullptr), "?DoDamage@CNWSCreature@@UEAAXH@Z");
        Log("dev: kill %08x -> %p", id, sc);
        if (sc) { void* so = VCall<void* (*)(void*)>(sc, 0x20)(sc); if (so) F<uint32_t>(so, 0x198) = 0x7fffffff; }
        if (sc && fDmg) fDmg(sc, 1000);
        // what the death effect does: the "dead" activity starts the death animation
        typedef void (*uaq_t)(void*);
        if (sc) ((uaq_t)A(0x1403a4550))(sc);         // CNWSCreature::UpdateActionQueue
        return;
    }
    Props_DevCommand(line);
}

// once per rendered frame, before the scene animates: watch deaths, step the simulations
void Update() {
    double now = P::Now();
    double dt = g_lastUpdate > 0 ? now - g_lastUpdate : 0;
    if (g_lastUpdate > 0 && dt < 0.002) return;            // other scenes of the same frame
    g_lastUpdate = now;
    G::NewFrame();
    Log_Flush();
    if (dt > 0.25) dt = 0.25;
    DevCommands();
    P::PollBlasts();
    void* pl = Player();
    void* area = PlayerArea();
    static void* lastArea = nullptr;
    static std::map<uint32_t, V3> lastPos;
    if (area != lastArea) { lastArea = area; g_dead.clear(); g_pending.clear(); g_kept.clear(); lastPos.clear(); }
    struct Mover { V3 p0, p, v; };
    std::vector<Mover> movers;
    for (auto it = g_dolls.begin(); it != g_dolls.end();) {
        Doll* d = it->second; ++it;
        void* c = GetObj(d->id);
        if (!c || F<void*>(c, OFS::Obj_Area) != area) { FreeDoll(d); continue; }
        void* g = CreatureGob(c);
        if (g != d->gob) {
            if (g && d->done && Rebind(*d, g)) continue;
            g_byGob.erase(d->gob); d->gob = nullptr;
            FreeDoll(d);
        }
    }
    // pending deaths whose model went away
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        void* c = GetObj(it->second.id);
        if (!c || CreatureGob(c) != it->first || now - it->second.t0 > 5.0) it = g_pending.erase(it); else ++it;
    }
    if (!pl || !area) { g_dead.clear(); return; }
    int n = F<int>(area, OFS::Area_ObjCount);
    uint32_t* ids = F<uint32_t*>(area, OFS::Area_ObjIds);
    Vec3 pp = Pos(pl);
    int active = (int)g_pending.size();
    for (auto& kv : g_dolls) if (!kv.second->done) active++;
    for (int i = 0; ids && i < n && i < 100000; i++) {
        void* o = GetObj(ids[i]);
        if (!o || Type(o) != OT_CREATURE) continue;
        uint32_t id = ids[i];
        void* sc = P::ServerCreature(id);
        bool dead = sc && fSrvGetDead ? fSrvGetDead(sc) != 0 : ClientDead(o);
        auto it = g_dead.find(id);
        bool known = it != g_dead.end();
        bool was = known && it->second;
        g_dead[id] = dead;
        if (!dead && C.kick && !g_dolls.empty()) {
            Vec3 p = Pos(o);
            V3 cur{p.x, p.y, p.z};
            auto lp = lastPos.find(id);
            if (lp != lastPos.end() && dt > 0.001) {
                V3 v = (cur - lp->second) * (float)(1.0 / dt);
                float s = sqrtf(v.x * v.x + v.y * v.y);
                if (s > 0.6f && s < 15.0f) movers.push_back({lp->second, cur, v});
            }
            lastPos[id] = cur;
        }
        if (dead && known && !was && C.enabled) {
            Vec3 p = Pos(o);
            float dx = p.x - pp.x, dy = p.y - pp.y;
            void* gob = CreatureGob(o);
            if (gob && dx * dx + dy * dy < C.range * C.range && active < C.maxActive && !g_dolls.count(id) && !g_pending.count(gob)) {
                if (C.animLead > 0.01f) { g_pending[gob] = Pending{id, gob, now}; active++; }
                else if (StartDoll(o, gob, nullptr, 0)) active++;
            }
        }
        if (!dead && was) {
            auto d = g_dolls.find(id);
            if (d != g_dolls.end()) { Log("ragdoll: %08x alive again", id); FreeDoll(d->second, true); }
            for (auto pit = g_pending.begin(); pit != g_pending.end(); ++pit) if (pit->second.id == id) { g_pending.erase(pit); break; }
        }
    }
    // walking through bodies
    for (auto& m : movers)
        for (auto& kv : g_dolls) Kick(*kv.second, m.p0, m.p, m.v, area);
    // blasts
    for (auto& bl : P::Blasts()) {
        if (now - bl.t > 0.5) continue;
        for (auto& kv : g_dolls) ApplyBlast(*kv.second, bl, area);
        Props_Blast(bl);
    }
    // fixed 60 Hz steps
    g_acc += dt;
    int steps = 0;
    while (g_acc >= 1.0 / 60 && steps < 2) {     // after a slow frame: no catching up (it would make the next slow too)
        for (auto& kv : g_dolls) Step(*kv.second, 1.0f / 60);
        Props_Step(1.0f / 60);
        g_acc -= 1.0 / 60; steps++;
    }
    if (steps == 2) g_acc = 0;
    Props_Update(area);
    Blood_Update(area);
}

void hk_SceneAnimate(void* scene, float dt) {
    Update();                                    // (once per frame; it starts the frame's memory-check cache)
    o_SceneAnimate(scene, dt);
}

void hk_GobAnimate(void* gob, float dt) {
    o_GobAnimate(gob, dt);
    auto pit = g_pending.find(gob);
    if (pit != g_pending.end()) {
        // death animation still playing: remember this pose, start the ragdoll when the lead time is over
        Pending& pd = pit->second;
        double now = P::Now();
        // the server says dead a moment before the death animation starts: wait until the body moves,
        // let the animation carry it for the lead time, then take over with its momentum
        bool go = now - pd.t0 > (C.debug > 1 ? 4.0 : 1.6) || (pd.moveT > 0 && now - pd.moveT >= C.animLead);
        if (go && !pd.prev.empty()) {
            void* c = GetObj(pd.id);
            std::vector<WT> prev = pd.prev; float pdt = (float)(now - pd.lastT);
            g_pending.erase(pit);
            if (c) StartDoll(c, gob, &prev, pdt);
        } else {
            TreeWorld(F<void*>(gob, 0x90), pd.prev, P::GobScale(gob));
            pd.lastT = now;
            if (pd.first.empty()) pd.first = pd.prev;
            else if (pd.moveT <= 0 && pd.first.size() == pd.prev.size()) {
                float sum = 0;
                for (size_t i = 0; i < pd.prev.size(); i++) sum += Len(pd.prev[i].p - pd.first[i].p);
                if (C.debug > 1) Log("ragdoll: %08x pending %.2f s, moved %.3f", pd.id, now - pd.t0, sum / pd.prev.size());
                if (sum / pd.prev.size() > 0.05f * P::GobScale(gob)) { pd.moveT = now; if (C.debug) Log("ragdoll: %08x death animation moving after %.2f s", pd.id, now - pd.t0); }
            }
        }
    } else {
        auto it = g_byGob.find(gob);
        if (it != g_byGob.end()) {
            Doll& d = *it->second;
            for (auto& r : d.parts) { F<V3>(r.part, 0x10) = r.lp; F<Q4>(r.part, 0x1c) = r.lq; }
            V3 rp; Q4 rq;
            if (d.done && d.pose.valid) P::GetWorld(d.parts[0].part, rp, rq);
            if (d.done && d.pose.valid && Len(rp - d.pose.rp) < 1e-4f && fabsf(Dot4(rq, d.pose.rq)) > 0.999999f) {
                // settled and not moved: the same locals as last frame
                for (size_t i = 0; i < d.parts.size(); i++) { F<V3>(d.parts[i].part, 0x10) = d.pose.lp[i]; F<Q4>(d.parts[i].part, 0x1c) = d.pose.lq[i]; }
            } else {
                static std::vector<WT> w;
                ComputeWorld(d, w, true);
                if (d.done) {
                    d.pose.valid = true;
                    P::GetWorld(d.parts[0].part, d.pose.rp, d.pose.rq);
                    d.pose.lp.resize(d.parts.size()); d.pose.lq.resize(d.parts.size());
                    for (size_t i = 0; i < d.parts.size(); i++) { d.pose.lp[i] = F<V3>(d.parts[i].part, 0x10); d.pose.lq[i] = F<Q4>(d.parts[i].part, 0x1c); }
                } else d.pose.valid = false;
            }
        }
    }
    Cloth_AfterAnimate(gob);
}

// weapons that left the hand: the attachment keeps putting them in the hand, we put them back where they lie
int hk_AttachControl(void* beh, float dt) {
    int r = o_AttachControl(beh, dt);
    if (!g_dropByGob.empty()) {
        void* child = F<void*>(beh, 0x18);
        auto it = g_dropByGob.find(child);
        if (it != g_dropByGob.end()) P::SetGobTransform(child, it->second->wp, it->second->wq);
    }
    return r;
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
    C.pinRadius = IniFloat("Ragdoll", "MultiplayerPinRadius", 0, ini);
    C.push = IniFloat("Ragdoll", "DeathPush", 2.0f, ini);
    C.animLead = IniFloat("Ragdoll", "DeathAnimationSeconds", 0.24f, ini);
    C.muscle = IniFloat("Ragdoll", "MuscleTone", 1.0f, ini);
    C.settleTime = IniFloat("Ragdoll", "MaxSimulationSeconds", 6.0f, ini);
    C.maxActive = IniInt("Ragdoll", "MaxActive", 10, ini);
    C.range = IniFloat("Ragdoll", "Range", 45.0f, ini);
    C.dropWeapons = IniInt("Ragdoll", "DropWeapons", 1, ini);
    C.explosions = IniInt("Physics", "Explosions", 1, ini);
    C.kick = IniInt("Ragdoll", "KickBodies", 1, ini);
    C.kickSpeed = IniFloat("Ragdoll", "KickStrength", 2.0f, ini);
    C.keepCorpses = IniInt("Ragdoll", "KeepCorpses", 1, ini);
    C.maxCorpses = IniInt("Ragdoll", "MaxCorpses", 60, ini);
    C.debug = IniInt("Ragdoll", "Debug", 0, ini);
    GetPrivateProfileStringA("Developer", "CommandFile", "", C.devFile, sizeof C.devFile, ini);
    Log("config: enabled %d moveCorpse %d pin %.2f push %.2f lead %.2f muscle %.2f maxTime %.1f maxActive %d range %.0f drop %d blasts %d",
        C.enabled, C.reposition, C.pinRadius, C.push, C.animLead, C.muscle, C.settleTime, C.maxActive, C.range, C.dropWeapons, C.explosions);
    Log("config: kick %d (%.1f) keep corpses %d (max %d)", C.kick, C.kickSpeed, C.keepCorpses, C.maxCorpses);
    Props_LoadConfig(ini);
    Cloth_LoadConfig(ini);
    Blood_LoadConfig(ini);
}

void Ragdoll_Install() {
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!P::Init()) { Log("required exports missing - disabled"); return; }
    fSrvGetDead = (GetDead_t)GetProcAddress(exe, "?GetDead@CNWSObject@@QEAAHXZ");
    void* ga = (void*)GetProcAddress(exe, "?Animate@Gob@@UEAAXM@Z");
    void* sa = (void*)GetProcAddress(exe, "?Animate@Scene@@UEAAXM@Z");
    void* ac = (void*)GetProcAddress(exe, "?Control@CAurBehaviorAttach@@UEAAHM@Z");
    if (!ga || !sa) { Log("animate exports missing - disabled"); return; }
    if (MH_CreateHook(ga, (void*)hk_GobAnimate, (void**)&o_GobAnimate) != MH_OK || MH_EnableHook(ga) != MH_OK ||
        MH_CreateHook(sa, (void*)hk_SceneAnimate, (void**)&o_SceneAnimate) != MH_OK || MH_EnableHook(sa) != MH_OK) {
        Log("hooks failed - disabled"); return;
    }
    if (ac && MH_CreateHook(ac, (void*)hk_AttachControl, (void**)&o_AttachControl) == MH_OK && MH_EnableHook(ac) == MH_OK) Log("attach hook ready");
    else { Log("attach hook failed - weapons stay in the hands"); C.dropWeapons = 0; }
    fReadBool = (Msg_t)GetProcAddress(exe, "?ReadBOOL@CNWMessage@@QEAAHXZ");
    fReadOverflow = (Msg_t)GetProcAddress(exe, "?MessageReadOverflow@CNWMessage@@QEAAHXZ");
    void* cd = (void*)A(0x140780210);                    // CNWCMessage::HandleServerToPlayerCreatureUpdate_Delete
    if (fReadBool && fReadOverflow && MH_CreateHook(cd, (void*)hk_CreatureDelete, (void**)&o_CreatureDelete) == MH_OK && MH_EnableHook(cd) == MH_OK)
        Log("corpse hook ready");
    else { Log("corpse hook failed - corpses fade as usual"); C.keepCorpses = 0; }
    Blood_Install();
    P::InstallBlastHook();
    Props_Install();
    Cloth_Install();
    Log("ready");
}
