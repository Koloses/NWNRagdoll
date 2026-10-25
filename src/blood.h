// NWNRagdoll - blood pools under the dead
#pragma once
#include "qmath.h"
#include <cstdint>

void Blood_LoadConfig(const char* ini);
void Blood_Install();
char Blood_Color(void* creature);                       // appearance.2da BLOODCOLR: 'R', 'G', 'Y', 'W', 'N' (0: unknown)
bool Blood_Bleeds(char color);                          // a pool model exists for this blood colour (R, G, Y)
// a pool of 'color' blood that spreads from 'p' to 'size' metres across over 'seconds'
// 'ground': height of the floor the body lies on, used where the walkmesh under 'p' can't be found
void Blood_Spill(V3 p, float size, float seconds, char color, float ground);
void Blood_Update(void* area);                          // once per frame
