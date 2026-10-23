// NWNRagdoll - blood pools under the dead
#pragma once
#include "qmath.h"
#include <cstdint>

void Blood_LoadConfig(const char* ini);
void Blood_Install();
char Blood_Color(void* creature);                       // appearance.2da BLOODCOLR: 'R', 'G', 'Y', 'W', 'N' (0: unknown)
// a pool that spreads from 'p' to 'size' metres across over 'seconds'
void Blood_Spill(V3 p, float size, float seconds);
void Blood_Update(void* area);                          // once per frame
