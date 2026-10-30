// NWNRagdoll - dismemberment: which blow cuts a limb off, the game's blood effects, giblets
#pragma once
#include "qmath.h"
#include <cstdint>

void Gore_LoadConfig(const char* ini);
void Gore_Install();
bool Gore_Enabled();

// the bone a killing blow cuts off ("head_g", "rforearm_g" ...), or null: single player, a critical hit or
// a heavy blow of a slashing weapon (the server's combat round of the killer)
const char* Gore_Cut(void* creature);
// the game's blood effects (visualeffects.2da, fire and forget at a point): spurt sizes 0 small, 1 large,
// 2 critical; a burst of flesh (chunks)
void Gore_Spurt(V3 p, int size, char blood);
void Gore_Chunks(V3 p, char blood);
// dev: the next death of this creature cuts this bone
void Gore_Force(uint32_t id, const char* bone);
// dev: every weapon counts as slashing (testing with whatever the hero holds)
void Gore_AnyWeapon(bool on);
