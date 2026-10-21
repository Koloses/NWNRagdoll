// NWNRagdoll - loose objects: dropped items tumble, bashed barrels and crates break into pieces
#pragma once
#include "phys.h"
void Props_LoadConfig(const char* ini);
void Props_Install();
void Props_Update(void* area);           // once per frame (watch items / placeables)
void Props_Step(float dt);               // fixed steps
void Props_AfterAnimate(void* gob);      // after Gob::Animate: put simulated gobs where their bodies are
void Props_Blast(const P::Blast& b);
void Props_DevCommand(const char* line);
