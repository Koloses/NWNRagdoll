# NWNRagdoll

Physics for **Neverwinter Nights: Enhanced Edition** (Windows, build 8193.37):

* **Ragdolls.** When a creature dies, its death animation starts and then real physics takes over
  with the animation's own motion: the body is knocked over away from whoever dealt the killing blow,
  goes limp over a second and comes to rest on the ground the way it landed. Knees and elbows only
  bend the right way and limbs do not pass through each other.
* **Weapons and shields** fall from the hands and land next to the body.
* **Bodies stay.** The dead no longer fade away when the server removes them; they lie where they
  fell until you leave the area (`MaxCorpses` per area, the oldest fade beyond that).
* **Kicking bodies.** Walking through a body (you or anyone else) kicks it along.
* **Blood.** A pool of blood spreads under bleeding bodies as they hit the ground, with a few
  drops around the head, and stays until you leave the area: red, green (zombies, ...) or yellow,
  by the creature's blood colour.
* **Dismemberment.** A killing blow with a slashing weapon (swords, axes, claws ...) that is a
  critical hit, or one that takes half the victim's hit points at once, can cut off the head, an arm or
  a leg (humanoids, single player): the piece flies off spinning, the wound spurts with the game's own
  combat blood and gore effects, and blood pools under both. The game's rules decide the blow; the mod
  only shows it.
* **Cloth.** Cloaks, tabards, belt flaps and loincloths swing with the body's motion, on everyone
  near you, alive or dead.
* **Explosions** (fireball, meteor swarm, ice storm, gas / electric / mystical explosions, implosion
  pulls inwards) throw bodies, dropped weapons and items lying around.
* **Items** that land on the ground (dropped, from corpses) fall and tumble before they come to rest.
* **Debris.** Bashed barrels, crates and other placeables break into wood / stone / shard pieces
  that fly, lie around for a while and sink away (single player). Physics by [Box3D](https://github.com/erincatto/box3d) (Erin Catto, MIT).

It is purely visual on your machine, nothing is sent to a server, and it works next to NWNPad.

* **Single player** (and games you host): when the body has come to rest the corpse is moved under
  it, so selecting and looting it happens where you see it.
* **On someone else's server** everything runs on your side only, the same as in single player; the
  corpse object itself stays where the server has it (selecting it there), so `MultiplayerPinRadius`
  can keep the body near it (0 = free). Items stay close to their real spot, and debris needs the
  local server.

The ground is the area's walkmesh around the body (slopes and steps), and walls and furniture
are solid, so limbs do not end up in them. Humanoids (players, NPCs, goblins, orcs ...) get a ragdoll
with proper joint limits, other creatures one built from their skeleton.

## Install

Copy `dbghelp.dll` and `NWNRagdoll.ini` into `Neverwinter Nights - Enhanced Edition\bin\win32`
(next to `nwmain.exe`), and the files in `ovr` (green and yellow blood pools, `dag_floorbldgr*` /
`dag_floorbldye*`) into the game's user override folder, `Documents\Neverwinter Nights\override`. A log is written to
`bin\win32\NWNRagdoll.log`. Delete those files to uninstall. Other game builds are detected and the mod stays off.

## Settings (`NWNRagdoll.ini`)

| key | default | |
|---|---|---|
| **[Ragdoll]** Enabled | 1 | |
| MoveCorpseSinglePlayer | 1 | move the corpse under the body when the server runs on this PC |
| MultiplayerPinRadius | 0 | otherwise, how far the body may end up from the corpse (m, 0 = no limit) |
| DeathPush | 2.0 | how hard the body is knocked over, away from the killer (m/s at head height) |
| DeathAnimationSeconds | 0.24 | death animation before the physics takes over (0 = at once) |
| MuscleTone | 1.0 | fading muscle tone (0 = limp at once) |
| DropWeapons | 1 | weapons and shields fall from the hands |
| MaxSimulationSeconds | 6 | simulation time limit per body |
| MaxActive | 10 | bodies simulated at the same time |
| Range | 45 | only deaths this close to you (m) |
| KickBodies | 1 | walking through a body kicks it |
| KickStrength | 2.0 | how hard (m/s) |
| KeepCorpses | 1 | the dead stay instead of fading away |
| MaxCorpses | 60 | kept bodies per area |
| **[Blood]** Enabled | 1 | blood pools under the dead |
| PoolSize | 1.0 | size of a full pool |
| MaxPools | 48 | pools per area (the oldest are reused) |
| **[Gore]** Dismemberment | 1 | slashing killing blows can cut off a head or limb |
| CriticalChance | 1.0 | chance on a critical hit |
| HeavyBlowChance | 0.5 | chance on a blow of half the victim's hit points or more |
| Heads / Arms | 0.35 / 0.45 | share of heads and arms among cuts (the rest legs) |
| Spurts | 1 | the wound spurts blood for a moment |
| **[Physics]** Explosions | 1 | blasts throw bodies and items |
| ItemsTumble | 1 | items landing on the ground tumble |
| Debris | 1 | bashed placeables break into pieces (single player) |
| DebrisSeconds | 20 | how long pieces stay |
| Cloth | 1 | cloaks, tabards, flaps swing |
| ClothStiffness | 1.0 | lower = looser cloth |

## How it works

`version.dll`-style proxy (`dbghelp.dll`, forwards every export to the system DLL). Hooks the
exported `Scene::Animate` (once per frame: watch deaths, step the simulations at 60 Hz) and
`Gob::Animate` (right after the engine animates a creature model: freeze the pose from the moment of
death and set the bone rotations from the rigid bodies; cloth chains are simulated there too),
`CAurBehaviorAttach::Control` (weapons that left the hand stay where they lie) and
`CNWCVisualEffect::LoadEffect` (fire-and-forget effects with a blast) and
`CNWCMessage::HandleServerToPlayerCreatureUpdate_Delete` (a dead creature's delete is read and
dropped, so the client keeps the body; it is cleared with the area). Blood pools are the game's
`dag_floorblood1..6` models, scaled up over a few seconds. Death is read from the in-process server in
single player (`CNWSObject::GetDead`) and from the client's damage level otherwise. The killer is the
server's last damager (CNWSObject+0x198) in single player; online the closest creature of the other side. Dismemberment notes each blow as the server
resolves it (`CNWSCreature::ResolvePostMeleeDamage` / `ResolvePostRangedDamage`: the attack's result,
damage and weapon from the combat round), frees the cut bone from its joint and plays the
`visualeffects.2da` blood spurts / chunks at the wound (`CNWCArea::ApplyFireAndForgetVisualEffect`). See
`docs/RE_NOTES.md`.

## Building

```
make            # mingw-w64; Box3D and MinHook are compiled in from third_party/
make release
```
