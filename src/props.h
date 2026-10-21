// NWNRagdoll - debris: bashed barrels and crates break into pieces
#pragma once
#include "phys.h"
void Props_LoadConfig(const char* ini);
void Props_Install();
void Props_Update(void* area);           // once per frame (watch placeables)
void Props_Step(float dt);               // fixed steps
void Props_Blast(const P::Blast& b);
void Props_DevCommand(const char* line);
