# NWNRagdoll - reverse-engineering notes (NWN:EE 8193.37-17, Win64)

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
* CNWCArea::GetTile(Vector*) 0x1407db200 (area, pos) -> CNWCTile (0x60): +0x10 height, +0x1c / +0x20 grid,
  +0x24 orientation, +0x40 tile data (+8 CNWTileSurfaceMesh*).
* ?RotateRealToCanonicalTile@CNWTile@@QEAAXMMPEAM0@Z, ?FindTriangle@CNWTileSurfaceMesh@@QEAAHHHMMH@Z
  (mesh, 0 walkable, 0, x, y, -1) -> triangle or -1, ?ComputeHeight@CNWTileSurfaceMesh@@QEAAMVVector@@@Z.
* Last damager: AsNWSObject (vfunc 0x20) +0x198 (server id, 0x7f000000 none) = NWScript GetLastDamager;
  ?GetGameObject@CServerExoApp@@QEAAPEAVCGameObject@@I@Z, CNWSObject position +0xa4.

## Physics additions (Win 8193.37-17)

* Attachments: Gob+0x4a8 list / +0x4b0 count; attached Gob+0x450 = CAurBehaviorAttach (+0x18 child gob,
  +0x20 parent, +0x30 part). `?Control@CAurBehaviorAttach@@UEAAHM@Z` places the child each frame, so a
  dropped weapon is re-placed after it (Gob+0xa8 position, +0xb4 orientation w,x,y,z).
* Mesh extent: MdlNode (Part+8) of TriMesh parts: bbox min +0xc0, max +0xcc, sphere r +0xd8 c +0xdc.
* CNWCVisualEffect::LoadEffect(id) = 0x140872770 (string xref "OrientWithGround"); the position is set
  right after it in CNWCArea::ApplyFireAndForgetVisualEffect, read the next frame at obj+0x38.
* `?NewCAurObject@@YAPEAVCAurObject@@PEBD0PEAU_iobuf@@@Z(model, "body", 0)` + `?SetScene@Gob@@...`
  (scene = player gob +0x88) put free models (debris) in the world; SetScene(null) removes them.
* Death animation: the server marks dead before the client starts the animation; the ragdoll waits
  until the part tree moves (or 1.6 s), plays DeathAnimationSeconds of it, then starts with the
  velocities of the last two animated poses.
* Server: CNWSFaction::AddMember 0x1404ad6b0, CFactionManager::GetFaction 0x140357900 (manager at
  server internal +0x100b0), CNWSCreature::UpdateActionQueue 0x1403a6270 (dev commands).
* Humanoid cloth bones: Cloak_g > CL1_g..CL4_g, CM1_g..CM4_g, CR1_g..CR4_g (+ CL1_fg, CR1_fg,
  cloak_shL/R helpers); TF1_g > TF2_g > TF3_g01 (+ TF1L_g, TF1R_g); FB1_g01 > FB2_g01 > FB4_g01.

## Gore (Win 8193.37-17)

- `CNWCArea::ApplyFireAndForgetVisualEffect` 0x1407da460 (area, uint16 vfx, Vector*, holder*); holder =
  {head, size} with head from 0x140368530(&holder) (a fresh map head, taken over by the call).
- `CNWSCreature+0x880` CNWSCombatRound*; attack data at round+0, 50 x 0xf0 (`CNWSCombatAttackData`):
  +4 target id, +0x16 damage shorts (-1 = none; [12] base physical), +0x56 weapon attack type,
  +0x68 killing blow, +0x78 attack result (1 hit, 3 critical, 10 devastating). The round is reset
  right after the attack, so the blow is read in a hook on `ResolvePostMeleeDamage` /
  `ResolvePostRangedDamage` (killing blow already set there).
- `CNWSItem+0x40` base item id; `g_pRules+0x20` CNWBaseItemArray; `CNWBaseItem+9` WeaponType
  (1 piercing, 3 slashing, 4 piercing-slashing). Verified: light crossbow = 7, type 1.
- visualeffects.2da: spurts 109-111 small, 112-114 large, 115-117 critical (red/green/yellow);
  chunks 122/124/126.
