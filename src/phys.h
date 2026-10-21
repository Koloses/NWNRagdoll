// NWNRagdoll - shared physics helpers: engine access, walkmesh ground, explosions
#pragma once
#include "qmath.h"
#include <box3d/box3d.h>
#include <cstdint>
#include <vector>

namespace P {
inline b3Vec3 BV(V3 v) { return b3Vec3{v.x, v.y, v.z}; }
inline V3 VB(b3Vec3 v) { return V3{v.x, v.y, v.z}; }
inline b3Quat BQ(Q4 q) { b3Quat r; r.v = b3Vec3{q.x, q.y, q.z}; r.s = q.w; return r; }
inline Q4 QB(b3Quat q) { return Q4{q.s, q.v.x, q.v.y, q.v.z}; }

double Now();                                    // seconds
float Rand(float a, float b);

// ---- engine ----
bool Init();                                     // resolve engine functions (false: missing exports)
void* ServerApp();                               // in-process server (single player), else null
void* ServerCreature(uint32_t clientId);
void* ServerObject(uint32_t clientId);           // CNWSObject of any game object
bool SetServerPosition(uint32_t clientId, V3 p); // single player: move the server object
void GetWorld(void* part, V3& p, Q4& q);         // Part::GetWorld
const char* PartName(void* part);
void* ObjectGob(void* obj);                      // the Gob of a creature / item / placeable
float GobScale(void* gob);
void SetGobTransform(void* gob, V3 p, Q4 q);     // Gob::SetPosition / SetOrientation
// mesh extent of a gob (and the gobs attached to it) in the gob's own frame
bool GobExtent(void* gob, V3& mn, V3& mx);

// ---- ground: the area's walkmesh ----
bool GroundAt(void* area, float x, float y, float& h);
// static ground body around 'center': walkable cells at their height, the rest raised as walls;
// returns the mesh to free with b3DestroyMesh (null when a flat box was used)
b3MeshData* AddGround(b3WorldId world, void* area, V3 center, float refZ, int n, float cell);

// ---- explosions (fire-and-forget visual effects with a blast) ----
struct Blast { V3 p; float radius; float speed; bool implode; double t; uint32_t serial; };
void InstallBlastHook();
void PollBlasts();                               // once per frame: turn new effects into blasts
const std::vector<Blast>& Blasts();              // the last few seconds
// velocity a blast gives to something at 'at' (zero outside its radius)
V3 BlastVelocity(const Blast& b, V3 at);
}  // namespace P
