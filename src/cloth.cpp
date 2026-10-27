// NWNRagdoll - cloaks, tabards, belt flaps and loincloths swing.
//
// The standard humanoid skeleton carries bone chains for the cloak (CL*, CM*, CR*: left, middle,
// right, four bones each), the front flap (TF1_g ..) and the back flap (FB1_g01 ..). The game only
// animates them with the body. After each animation update the chains are simulated as verlet rope
// points hanging from their first bone (gravity, inertia from the body's own motion, a gentle pull
// back to the animated shape), kept out of the torso and legs, and the bones are turned to follow
// the points. Works the same on living creatures and on ragdolls.
#include "cloth.h"
#include "phys.h"
#include "game.h"
#include "qmath.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace G;

namespace {
struct Cfg { int enabled = 1; float range = 25.0f; int maxActive = 24; float stiffness = 1.0f; } C;

struct WT { V3 p; Q4 q; float s; };
struct Chain {
    std::vector<void*> bones;        // first .. last bone
    std::vector<V3> pt, prev;        // bone origins + tip
    std::vector<float> len;          // rest lengths (pt[i-1] -> pt[i])
    float pull = 0.08f;              // pull towards the animated shape per step
    float radius = 0.03f;
};
struct Capsule { V3 a, b; float r; };
struct Cloth {
    void* root = nullptr;            // root part when built (model rebuilt: rebuild)
    std::vector<Chain> chains;
    struct Col { void* a; void* b; float r; };
    std::vector<Col> cols;
    double last = 0;
    bool init = false;
    V3 lastAnchor;
};
std::map<void*, Cloth> g_cloth;

const char* PN(void* p) { return P::PartName(p); }
void* FindPartRec(void* p, const char* name, int depth = 0) {
    if (!p || depth > 48 || !Readable(p, 0x58)) return nullptr;
    if (_stricmp(PN(p), name) == 0) return p;
    void** ch = F<void**>(p, 0x38); int n = F<int>(p, 0x40);
    for (int i = 0; ch && i < n && i < 128; i++) if (void* r = FindPartRec(ch[i], name, depth + 1)) return r;
    return nullptr;
}
void* FirstChild(void* p) {
    void** ch = F<void**>(p, 0x38); int n = F<int>(p, 0x40);
    return ch && n > 0 ? ch[0] : nullptr;
}
void* ChildNamed(void* p, const char* prefix) {
    void** ch = F<void**>(p, 0x38); int n = F<int>(p, 0x40);
    size_t l = strlen(prefix);
    for (int i = 0; ch && i < n && i < 64; i++) if (_strnicmp(PN(ch[i]), prefix, l) == 0) return ch[i];
    return nullptr;
}

// world transform of a part from its current locals (walks up to the model root)
WT WorldOf(void* part, float gs) {
    void* chain[64]; int n = 0;
    for (void* p = part; p && n < 64; p = F<void*>(p, 0x48)) chain[n++] = p;
    WT w;
    V3 rp; Q4 rq; P::GetWorld(chain[n - 1], rp, rq);
    w = {rp, rq, gs * F<float>(chain[n - 1], 0x2c)};
    for (int i = n - 2; i >= 0; i--) {
        void* p = chain[i];
        w = {w.p + Rot(w.q, F<V3>(p, 0x10) * w.s), w.q * F<Q4>(p, 0x1c), w.s * F<float>(p, 0x2c)};
    }
    return w;
}

bool Build(Cloth& c, void* gob) {
    void* root = F<void*>(gob, 0x90);
    c = Cloth(); c.root = root;
    // chains: each named first bone, then the first child bone of the same family down the line
    struct Def { const char* first; const char* family; float pull; };
    const Def defs[] = {{"CL1_g", "CL", 0.06f}, {"CM1_g", "CM", 0.06f}, {"CR1_g", "CR", 0.06f},
                        {"TF1_g", "TF", 0.15f}, {"FB1_g01", "FB", 0.15f}};
    for (const Def& d : defs) {
        void* b = FindPartRec(root, d.first);
        if (!b) continue;
        Chain ch; ch.pull = d.pull * C.stiffness;
        for (void* p = b; p && ch.bones.size() < 6; p = ChildNamed(p, d.family)) {
            if (_stricmp(PN(p), "TF1L_g") == 0 || _stricmp(PN(p), "TF1R_g") == 0) break;
            ch.bones.push_back(p);
        }
        if (ch.bones.empty()) continue;
        c.chains.push_back(ch);
    }
    if (c.chains.empty()) return false;
    // what the cloth must stay out of
    auto col = [&](const char* a, const char* b, float r) {
        void* pa = FindPartRec(root, a); void* pb = b ? FindPartRec(root, b) : nullptr;
        if (pa) c.cols.push_back({pa, pb, r});
    };
    col("torso_g", "neck_g", 0.15f);
    col("rootdummy", nullptr, 0.15f);
    col("lthigh_g", "lshin_g", 0.09f); col("rthigh_g", "rshin_g", 0.09f);
    col("lshin_g", "lfoot_g", 0.07f); col("rshin_g", "rfoot_g", 0.07f);
    return true;
}

void Simulate(Cloth& c, void* gob, float dt) {
    float gs = P::GobScale(gob);
    // colliders from the current pose
    std::vector<Capsule> caps;
    for (auto& k : c.cols) {
        WT a = WorldOf(k.a, gs);
        V3 b = k.b ? WorldOf(k.b, gs).p : a.p;
        caps.push_back({a.p, b, k.r * gs});
    }
    V3 anchor = WorldOf(c.chains[0].bones[0], gs).p;
    bool reset = !c.init || Len(anchor - c.lastAnchor) > 2.0f;
    c.lastAnchor = anchor;
    c.init = true;
    void* area = PlayerArea();
    for (auto& ch : c.chains) {
        size_t n = ch.bones.size();
        // animated pose of the chain
        std::vector<WT> wb(n);
        wb[0] = WorldOf(ch.bones[0], gs);
        for (size_t i = 1; i < n; i++) {
            void* p = ch.bones[i];
            const WT& pw = wb[i - 1];
            wb[i] = {pw.p + Rot(pw.q, F<V3>(p, 0x10) * pw.s), pw.q * F<Q4>(p, 0x1c), pw.s * F<float>(p, 0x2c)};
        }
        std::vector<V3> A(n + 1);
        for (size_t i = 0; i < n; i++) A[i] = wb[i].p;
        // the tip: past the last bone, as long as the bone before it (or straight down)
        V3 lastDir = n > 1 ? A[n - 1] - A[n - 2] : Rot(wb[0].q, V3{0, 0, -0.25f * gs});
        A[n] = A[n - 1] + lastDir;
        if (reset || ch.pt.size() != n + 1) {
            ch.pt = A; ch.prev = A;
            ch.len.assign(n + 1, 0);
            for (size_t i = 1; i <= n; i++) ch.len[i] = Len(A[i] - A[i - 1]);
        }
        float ground = -1e9f;
        if (area) { float h; if (P::GroundAt(area, A[0].x, A[0].y, h)) ground = h; }
        // verlet
        int sub = dt > 1.0f / 45 ? 2 : 1;
        float h = dt / sub;
        for (int s = 0; s < sub; s++) {
            ch.pt[0] = A[0]; ch.prev[0] = A[0];
            for (size_t i = 1; i <= n; i++) {
                V3 v = (ch.pt[i] - ch.prev[i]) * 0.97f;
                ch.prev[i] = ch.pt[i];
                ch.pt[i] = ch.pt[i] + v + V3{0, 0, -9.81f * h * h};
                ch.pt[i] = ch.pt[i] + (A[i] - ch.pt[i]) * ch.pull;
            }
            for (int it = 0; it < 3; it++) {
                for (size_t i = 1; i <= n; i++) {
                    V3 d = ch.pt[i] - ch.pt[i - 1];
                    float l = Len(d);
                    if (l > 1e-5f) ch.pt[i] = ch.pt[i - 1] + d * (ch.len[i] / l);
                    for (auto& cp : caps) {
                        V3 ab = cp.b - cp.a;
                        float t = Dot(ab, ab) > 1e-6f ? fminf(1.0f, fmaxf(0.0f, Dot(ch.pt[i] - cp.a, ab) / Dot(ab, ab))) : 0.0f;
                        V3 q = cp.a + ab * t;
                        V3 e = ch.pt[i] - q;
                        float el = Len(e), r = cp.r + ch.radius;
                        if (el < r && el > 1e-5f) ch.pt[i] = q + e * (r / el);
                    }
                    if (ch.pt[i].z < ground + 0.02f) ch.pt[i].z = ground + 0.02f;
                }
            }
        }
        // turn the bones to follow the points, from the first bone down
        V3 tipLocal = Rot(Conj(wb[n - 1].q), A[n] - A[n - 1]);
        Q4 parentQ = Norm(wb[0].q * Conj(F<Q4>(ch.bones[0], 0x1c)));
        float sc = wb[0].s;
        for (size_t i = 0; i < n; i++) {
            Q4 wq = Norm(parentQ * F<Q4>(ch.bones[i], 0x1c));
            V3 childDir = i + 1 < n ? Rot(wq, F<V3>(ch.bones[i + 1], 0x10) * sc) : Rot(wq, tipLocal);
            V3 target = ch.pt[i + 1] - ch.pt[i];
            Q4 wn = Norm(FromTo(childDir, target) * wq);
            F<Q4>(ch.bones[i], 0x1c) = Norm(Conj(parentQ) * wn);
            parentQ = wn;
        }
    }
}
}  // namespace

void Cloth_LoadConfig(const char* ini) {
    C.enabled = GetPrivateProfileIntA("Physics", "Cloth", 1, ini);
    char b[64]; GetPrivateProfileStringA("Physics", "ClothStiffness", "1.0", b, sizeof b, ini);
    C.stiffness = (float)atof(b);
    Log("config: cloth %d stiffness %.2f", C.enabled, C.stiffness);
}

void Cloth_Install() {}

void Cloth_AfterAnimate(void* gob) {
    if (!C.enabled || !gob) return;
    double now = P::Now();
    auto it = g_cloth.find(gob);
    if (it == g_cloth.end()) {
        // only creature models near the player
        void* pl = Player();
        if (!pl) return;
        V3 gp = F<V3>(gob, 0xa8);
        Vec3 pp = Pos(pl);
        if ((gp.x - pp.x) * (gp.x - pp.x) + (gp.y - pp.y) * (gp.y - pp.y) > C.range * C.range) return;
        // models without cloth bones: not looked at again until the model is rebuilt (another root part)
        static std::map<void*, void*> rejected;
        void* root = F<void*>(gob, 0x90);
        auto rj = rejected.find(gob);
        if (rj != rejected.end() && rj->second == root) return;
        // a few new models per frame (entering an area shows hundreds at once)
        static double tryFrame = -1; static int tries = 0;
        if (now != tryFrame) { tryFrame = now; tries = 0; }
        if (++tries > 4) return;
        if ((int)g_cloth.size() >= C.maxActive) {
            // drop the one not seen for the longest
            auto old = g_cloth.begin();
            for (auto i = g_cloth.begin(); i != g_cloth.end(); ++i) if (i->second.last < old->second.last) old = i;
            if (now - old->second.last < 1.0) return;
            g_cloth.erase(old);
        }
        Cloth c;
        if (!Build(c, gob)) { if (rejected.size() > 20000) rejected.clear(); rejected[gob] = root; return; }
        it = g_cloth.emplace(gob, c).first;
    }
    Cloth& c = it->second;
    if (F<void*>(gob, 0x90) != c.root) { if (!Build(c, gob)) { g_cloth.erase(it); return; } }
    float dt = c.last > 0 ? (float)(now - c.last) : 1.0f / 60;
    c.last = now;
    if (dt <= 0.0005f) return;
    if (dt > 0.1f) { dt = 0.1f; c.init = false; }
    Simulate(c, gob, dt);
}
