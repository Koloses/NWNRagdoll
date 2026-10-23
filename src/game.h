// NWNPad - game interface for NWN:EE 8193.37 (Win64, nwmain.exe)
#pragma once
#include <cstdint>
#include <cstddef>

struct Vec3 { float x, y, z; };

namespace G {
extern uintptr_t base;
bool Init();                       // resolve + verify addresses
inline uintptr_t A(uintptr_t va) { return base + (va - 0x140000000ULL); }
template <class T> inline T& F(const void* p, size_t off) { return *(T*)((const char*)p + off); }
template <class T> inline T VCall(void* obj, size_t off) { return (T)((*(void***)obj)[off / 8]); }

// ---- addresses (VA in the 0x140000000-based image) ----
namespace VA {
constexpr uintptr_t g_pAppManager            = 0;   // resolved from export
constexpr uintptr_t ProcessInput             = 0x1406eeb50;
constexpr uintptr_t HandleInputEvent         = 0x1406eada0;
constexpr uintptr_t UpdateDriveMode          = 0x1406ef4b0;
constexpr uintptr_t UpdateCamera             = 0x1406e38a0;
constexpr uintptr_t UpdateMouseCursorAppearance = 0x1406efea0;
constexpr uintptr_t Internal_GetPlayerCreature = 0x1406d6140;
constexpr uintptr_t Internal_GetModule       = 0x1406d5970;
constexpr uintptr_t App_GetGameObject        = 0x1406a91d0;
constexpr uintptr_t App_GetPlayerCreature    = 0x1406a9810;
constexpr uintptr_t PerformDefaultActionOnObject = 0x1406ed540;
constexpr uintptr_t FeedbackHitCheckHostile  = 0x1406e84a0;
constexpr uintptr_t FeedbackHitCheckNonHostile = 0x1406e9370;
constexpr uintptr_t PerformLButtonDownAction = 0x1406ed940;
constexpr uintptr_t PerformRButtonDownAction = 0x1406ee750;
constexpr uintptr_t GuiMan_GuiObjectHitCheck = 0x1401f31f0;
constexpr uintptr_t GuiMan_HandleKeyEvent    = 0x1401f3630;
constexpr uintptr_t GuiMan_SetMousePosition  = 0x1401f4c30;
constexpr uintptr_t Camera_GetYaw            = 0x140059020;
constexpr uintptr_t Camera_MapToScreen       = 0x140059a90;
constexpr uintptr_t QuickBar_ClickButton     = 0x14085ae10;
constexpr uintptr_t QuickBar_ActivateButtonSet = 0x1408584c0;
constexpr uintptr_t GuiInGame_ShowDialogEntry   = 0x1406f8bd0;
constexpr uintptr_t GuiInGame_ShowDialogReplies = 0x1406f8f50;
constexpr uintptr_t GuiInGame_HandleDialogSelection = 0x1406f5850;
constexpr uintptr_t ActionTarget_HandleLMouseDown = 0x14083a860;
}

// ---- offsets ----
namespace OFS {
// CClientExoAppInternal
constexpr size_t In_Module = 0x30, In_GuiInGame = 0x90, In_ActionTarget = 0x108;
constexpr size_t In_MouseX = 0x120, In_MouseY = 0x124, In_Mode = 0x140;
constexpr size_t In_CamZoom = 0x148, In_CamYaw = 0x14c, In_CamPitch = 0x150;
constexpr size_t In_DriveDirty = 0x1b0, In_DriveFwd = 0x1b4, In_DriveFwdRun = 0x1b8, In_DriveBack = 0x1bc;
constexpr size_t In_Message = 0x208;
// CNWCObject (common)
constexpr size_t Obj_Id = 0x8, Obj_Type = 0xc, Obj_Area = 0x30, Obj_Pos = 0x38, Obj_Orient = 0x44;
constexpr size_t Obj_Visible = 0xa8, Obj_Selectable = 0x11c;
// CNWCCreature (Win)
constexpr size_t Cre_Stats = 0x3b0, Cre_Reaction = 0x388, Cre_DriveFlags = 0x37c, Cre_DriveOk = 0x41c;
constexpr size_t Cre_WalkMode = 0x290;
// CNWCCreatureStats
constexpr size_t Stats_TempHP = 0x86, Stats_CurHP = 0x88, Stats_MaxHP = 0x8a;
// CNWCArea
constexpr size_t Area_ObjIds = 0x318, Area_ObjCount = 0x320, Area_Id = 0x118;
// CNWCModule
constexpr size_t Mod_Camera = 0xe8, Mod_Area = 0xf8;
// vtable offsets (Win)
constexpr size_t V_AsNWCObject = 0x18, V_AsCreature = 0x58, V_AsItem = 0x68, V_AsDoor = 0x28;
constexpr size_t V_SetOrientation = 0x120, V_Hilite = 0x158, V_UnHilite = 0x160;
constexpr size_t V_MouseOver = 0x108, V_MouseOff = 0x110;
}

enum ObjType : uint8_t { OT_CREATURE = 5, OT_ITEM = 6, OT_TRIGGER = 7, OT_PLACEABLE = 9, OT_DOOR = 10 };

// ---- accessors ----
void* App();            // CClientExoApp*
void* Internal();       // CClientExoAppInternal*
bool Loading();        // a game / module is loading: object tables are not usable
void InstallLoadGuard();
void* Player();         // CNWCCreature* or null
void* Module();
void* Camera();
void* GetObj(uint32_t id);   // CNWCObject* or null
void* PlayerArea();
inline Vec3 Pos(void* o) { return F<Vec3>(o, OFS::Obj_Pos); }
inline uint32_t Id(void* o) { return F<uint32_t>(o, OFS::Obj_Id); }
inline uint8_t Type(void* o) { return F<uint8_t>(o, OFS::Obj_Type); }

// ---- original function pointers / wrappers ----
int  HandleInputEvent(int id, int value);
const char* ClassName(void* obj);
bool Readable(const void* p, size_t n);
int  DefaultAction(void* obj);
float CameraYaw();
bool WorldToScreen(const Vec3& w, float& sx, float& sy); // normalized 0..1, y down
void SetOrientation(void* creature, const Vec3& dir);
}
