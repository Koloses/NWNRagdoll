# NWNRagdoll

Ragdoll physics for dying creatures in **Neverwinter Nights: Enhanced Edition** (Windows, build
8193.37). When a creature dies its body falls with real physics instead of playing the canned
death animation: it crumples, tips over, slides off steps and comes to rest on the ground the way it
landed. Physics by [Box3D](https://github.com/erincatto/box3d) (Erin Catto, MIT).

It is purely visual on your machine, nothing is sent to a server, and it works next to NWNPad.

* **Single player** (and games you host): when the body has come to rest the corpse is moved under
  it, so selecting and looting it happens where you see it.
* **On someone else's server** the corpse cannot be moved, so the body is kept within
  `MultiplayerPinRadius` (0.6 m) of where the server has the corpse.

The ground is the area's walkmesh around the body (slopes and steps), and walls and furniture
are solid, so limbs do not end up in them. Humanoids (players, NPCs, goblins, orcs ...) get a ragdoll
with proper joint limits, other creatures one built from their skeleton.

## Install

Copy `dbghelp.dll` and `NWNRagdoll.ini` into `Neverwinter Nights - Enhanced Edition\bin\win32`
(next to `nwmain.exe`). A log is written to `bin\win32\NWNRagdoll.log`. Delete the two files to
uninstall. Other game builds are detected and the mod stays off.

## Settings (`NWNRagdoll.ini`)

| key | default | |
|---|---|---|
| Enabled | 1 | |
| MoveCorpseSinglePlayer | 1 | move the corpse under the body when the server runs on this PC |
| MultiplayerPinRadius | 0.6 | otherwise, how far the body may end up from the corpse (m) |
| DeathPush | 1.2 | backwards speed of the upper body at the moment of death (m/s) |
| MaxSimulationSeconds | 6 | simulation time limit per body |
| MaxActive | 10 | bodies simulated at the same time |
| Range | 45 | only deaths this close to you (m) |

## How it works

`version.dll`-style proxy (`dbghelp.dll`, forwards every export to the system DLL). Hooks the
exported `Scene::Animate` (once per frame: watch deaths, step the simulations at 60 Hz) and
`Gob::Animate` (right after the engine animates a creature model: freeze the pose from the moment of
death and set the bone rotations from the rigid bodies). Death is read from the in-process server in
single player (`CNWSObject::GetDead`) and from the client's damage level otherwise. See
`docs/RE_NOTES.md`.

## Building

```
make            # mingw-w64; Box3D and MinHook are compiled in from third_party/
make release
```
