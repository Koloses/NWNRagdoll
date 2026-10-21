# NWNRagdoll - reverse-engineering notes (NWN:EE 8193.37, Win64)

## Models
* CNWCObject+0x78 -> CNWCAnimBase* (CNWCAnimBaseParts for part-based humanoids, CNWCAnimBaseWield ...);
  anim base +0xc8 -> Gob (the creature model; RTTI class "Gob").
* Gob+0x90 root Part. Gob vfuncs: +0x2d0 scale (float), +0x2d8 position, +0x2e0 orientation.
* Part: +0x08 MdlNode* (name at node+0x38), +0x10 local position (Vector), +0x1c local rotation
  (Quaternion w, x, y, z), +0x2c scale, +0x38 children (array) / +0x40 count, +0x48 parent, +0x50 Gob.
  World: parent world * (translate pos * rotate q * scale); Part::GetWorld export.
* Exports: ?Animate@Gob@@UEAAXM@Z (writes the animated locals), ?Animate@Scene@@UEAAXM@Z,
  ?GetWorld@Part@@UEAAXAEAVVector@@AEAVQuaternion@@@Z, ?GetChildren@Part, ?Name@Part, ?GetPart@Gob.
* Humanoid bones: rootdummy (hips pivot), torso_g, neck_g, head_g, L/Rbicep_g, L/Rforearm_g,
  L/Rhand_g, pelvis_g, L/Rthigh_g, L/Rshin_g, L/Rfoot_g (case varies); limbs point along local -z.
  Heads of part-based models are separate Gobs attached to the "head" part.

## Server (in-process in single player)
* g_pAppManager+8 CServerExoApp. Server object id = client id & 0x7fffffff (player: client ffffffff).
* ?GetCreatureByGameObjectID@CServerExoApp@@QEAAPEAVCNWSCreature@@I@Z,
  ?SetPosition@CNWSObject@@QEAAXVVector@@H@Z (Vector passed by pointer), ?GetDead@CNWSObject@@QEAAHXZ,
  ?DoDamage@CNWSCreature@@UEAAXH@Z (test kills).
* Client damage level: CNWCCreatureStats+0x8e (5 = dead).

## Walkmesh
* CNWCArea::GetTile(Vector*) 0x1407d9d00 (area, pos) -> CNWCTile (0x60): +0x10 height, +0x1c / +0x20 grid,
  +0x24 orientation, +0x40 tile data (+8 CNWTileSurfaceMesh*).
* ?RotateRealToCanonicalTile@CNWTile@@QEAAXMMPEAM0@Z, ?FindTriangle@CNWTileSurfaceMesh@@QEAAHHHMMH@Z
  (mesh, 0 walkable, 0, x, y, -1) -> triangle or -1, ?ComputeHeight@CNWTileSurfaceMesh@@QEAAMVVector@@@Z.
* Last damager: AsNWSObject (vfunc 0x20) +0x198 (server id, 0x7f000000 none) = NWScript GetLastDamager;
  ?GetGameObject@CServerExoApp@@QEAAPEAVCGameObject@@I@Z, CNWSObject position +0xa4.
