/*
 * HollowInputFix - controller de-duplication for Gears of War 2: Hollow.
 *
 * Built twice from this one source:
 *   xinput1_3.dll  (HIF_XINPUT)  - the game only ever sees ONE XInput controller, in slot 0,
 *                                  fed from whichever physical slot is actually connected.
 *   dinput8.dll    (HIF_DINPUT)  - hides DirectInput game controllers from the engine's
 *                                  joystick enumeration (keyboard/mouse untouched).
 *
 * Why: the engine (UWindowsClient) creates 4 XInput joystick slots and then lets any
 * DirectInput pad it doesn't recognise as XInput take over a slot. Every slot that reports
 * "connected" becomes a separate controller in game. Under Steam, Steam Input's virtual pad
 * shows up next to the real one (or is mis-detected via DirectInput), so one physical pad
 * appears as two. Online co-op is one player per PC, so exposing a single pad is correct.
 *
 * Settings: HollowInputFix.ini next to the DLLs. Diagnostics: HollowInputFix.log.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

static WCHAR g_dir[MAX_PATH];
static int   g_logEnabled = 1;

static void InitDir(HMODULE self)
{
    WCHAR *slash;
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
}

static int CfgInt(const WCHAR *section, const WCHAR *key, int def)
{
    WCHAR path[MAX_PATH];
    wsprintfW(path, L"%sHollowInputFix.ini", g_dir);
    return (int)GetPrivateProfileIntW(section, key, def, path);
}

static void Log(const char *fmt, ...)
{
    WCHAR path[MAX_PATH];
    char buf[512];
    SYSTEMTIME t;
    FILE *f;
    va_list ap;
    if (!g_logEnabled) return;
    wsprintfW(path, L"%sHollowInputFix.log", g_dir);
    if (_wfopen_s(&f, path, L"a") != 0 || !f) return;
    GetLocalTime(&t);
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    fprintf(f, "%02d:%02d:%02d.%03d [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
#ifdef HIF_XINPUT
            "xinput",
#else
            "dinput",
#endif
            buf);
    fclose(f);
}

/* [Fixes] master switches, so one set of files can be installed as controller-only,
   HUD-only or both. */
static int ControllerFixOn(void) { return CfgInt(L"Fixes", L"ControllerFix", 1); }

static HMODULE LoadSystemDll(const WCHAR *name)
{
    WCHAR path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n > MAX_PATH - 32) return NULL;
    wsprintfW(path + n, L"\\%s", name);
    return LoadLibraryW(path);
}

/* ------------------------------------------------------------------------------------------ */
#ifdef HIF_XINPUT

#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167
#endif
#define XUSER_MAX_COUNT 4

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XINPUT_VIBRATION;
typedef struct { BYTE Type, SubType; WORD Flags; XINPUT_GAMEPAD Gamepad; XINPUT_VIBRATION Vibration; } XINPUT_CAPABILITIES;
typedef struct { BYTE BatteryType, BatteryLevel; } XINPUT_BATTERY_INFORMATION;
typedef struct { WORD VirtualKey; WCHAR Unicode; WORD Flags; BYTE UserIndex, HidCode; } XINPUT_KEYSTROKE;

typedef DWORD (WINAPI *PFN_GetState)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *PFN_SetState)(DWORD, XINPUT_VIBRATION *);
typedef DWORD (WINAPI *PFN_GetCaps)(DWORD, DWORD, XINPUT_CAPABILITIES *);
typedef void  (WINAPI *PFN_Enable)(BOOL);
typedef DWORD (WINAPI *PFN_GetDSound)(DWORD, GUID *, GUID *);
typedef DWORD (WINAPI *PFN_GetBattery)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION *);
typedef DWORD (WINAPI *PFN_GetKeystroke)(DWORD, DWORD, XINPUT_KEYSTROKE *);

static INIT_ONCE        g_once = INIT_ONCE_STATIC_INIT;
static PFN_GetState     pGetState, pGetStateEx;
static PFN_SetState     pSetState;
static PFN_GetCaps      pGetCaps;
static PFN_Enable       pEnable;
static PFN_GetDSound    pGetDSound;
static PFN_GetBattery   pGetBattery;
static PFN_GetKeystroke pGetKeystroke;

static int  g_singleMode = 1;   /* 1 = expose one controller in slot 0, 0 = passthrough */
static int  g_pinnedSlot = -1;  /* -1 = auto (first connected physical slot) */
static volatile LONG g_source = -1;
static volatile LONG g_lastScan = 0;
static DWORD g_lastMask = 0xFFFFFFFF;

static BOOL CALLBACK InitXInput(PINIT_ONCE o, PVOID p, PVOID *c)
{
    HMODULE real;
    const WCHAR *used = L"xinput1_3.dll";
    (void)o; (void)p; (void)c;
    g_logEnabled = CfgInt(L"General", L"Log", 1);
    g_singleMode = ControllerFixOn() && CfgInt(L"XInput", L"SingleController", 1);
    g_pinnedSlot = CfgInt(L"XInput", L"SourceSlot", -1);
    if (g_pinnedSlot > 3) g_pinnedSlot = -1;

    /* Prefer the same DLL the game was built against, from the real system directory. */
    real = LoadSystemDll(L"xinput1_3.dll");
    if (!real) { used = L"xinput1_4.dll"; real = LoadSystemDll(used); }
    if (!real) { used = L"xinput9_1_0.dll"; real = LoadSystemDll(used); }
    if (!real) { Log("ERROR: no system XInput DLL found"); return TRUE; }

    pGetState     = (PFN_GetState)GetProcAddress(real, "XInputGetState");
    pGetStateEx   = (PFN_GetState)GetProcAddress(real, (LPCSTR)100);
    pSetState     = (PFN_SetState)GetProcAddress(real, "XInputSetState");
    pGetCaps      = (PFN_GetCaps)GetProcAddress(real, "XInputGetCapabilities");
    pEnable       = (PFN_Enable)GetProcAddress(real, "XInputEnable");
    pGetDSound    = (PFN_GetDSound)GetProcAddress(real, "XInputGetDSoundAudioDeviceGuids");
    pGetBattery   = (PFN_GetBattery)GetProcAddress(real, "XInputGetBatteryInformation");
    pGetKeystroke = (PFN_GetKeystroke)GetProcAddress(real, "XInputGetKeystroke");

    Log("loaded system %ls; SingleController=%d SourceSlot=%d; Steam=%s", used, g_singleMode,
        g_pinnedSlot, GetModuleHandleW(L"gameoverlayrenderer.dll") ? "overlay injected" : "no overlay");
    return TRUE;
}

static void EnsureInit(void) { InitOnceExecuteOnce(&g_once, InitXInput, NULL, NULL); }

/* Picks the physical slot that feeds virtual slot 0. Rescans at most every 500 ms. */
static int ResolveSource(void)
{
    XINPUT_STATE s;
    DWORD now, mask = 0;
    int i, pick = -1;

    if (g_pinnedSlot >= 0) return g_pinnedSlot;

    now = GetTickCount();
    if (g_source >= 0) return g_source;
    if (now - (DWORD)g_lastScan < 500) return -1;
    g_lastScan = (LONG)now;

    for (i = 0; i < XUSER_MAX_COUNT; i++)
        if (pGetState(i, &s) == ERROR_SUCCESS) { mask |= 1u << i; if (pick < 0) pick = i; }

    if (mask != g_lastMask) {
        Log("physical XInput slots connected: [%c%c%c%c] -> game slot 0 uses physical slot %d%s",
            mask & 1 ? '0' : '-', mask & 2 ? '1' : '-', mask & 4 ? '2' : '-', mask & 8 ? '3' : '-',
            pick, (mask & (mask - 1)) ? "  (more than one slot connected: duplicates hidden)" : "");
        g_lastMask = mask;
    }
    g_source = pick;
    return pick;
}

/* Maps a game-side slot to a physical slot, or -1 if the game should see it as empty. */
static int MapSlot(DWORD idx)
{
    if (!g_singleMode) return (int)idx;
    if (idx != 0) return -1;
    return ResolveSource();
}

static DWORD CallState(PFN_GetState fn, DWORD idx, XINPUT_STATE *st)
{
    int src;
    DWORD r;
    EnsureInit();
    if (!fn) return ERROR_DEVICE_NOT_CONNECTED;
    src = MapSlot(idx);
    if (src < 0) return ERROR_DEVICE_NOT_CONNECTED;
    r = fn((DWORD)src, st);
    if (r != ERROR_SUCCESS && g_singleMode && g_pinnedSlot < 0) g_source = -1; /* lost it: rescan */
    return r;
}

/* The real logic lives in these non-exported functions. Steam's overlay inline-hooks the
   exports of any module named xinput1_3.dll (including this proxy) and answers from Steam
   Input directly, so the game's own XInput thunks are pointed here instead (see
   InstallXInputThunks); the exports below are only for anything else that asks. */
static __declspec(noinline) DWORD WINAPI Internal_GetState(DWORD i, XINPUT_STATE *s)
{
    EnsureInit();
    return CallState(pGetState, i, s);
}

static __declspec(noinline) DWORD WINAPI Internal_SetState(DWORD i, XINPUT_VIBRATION *v)
{
    int src;
    EnsureInit();
    if (!pSetState) return ERROR_DEVICE_NOT_CONNECTED;
    src = MapSlot(i);
    return src < 0 ? ERROR_DEVICE_NOT_CONNECTED : pSetState((DWORD)src, v);
}

DWORD WINAPI HIF_XInputGetState(DWORD i, XINPUT_STATE *s)   { return Internal_GetState(i, s); }
DWORD WINAPI HIF_XInputGetStateEx(DWORD i, XINPUT_STATE *s) { EnsureInit(); return CallState(pGetStateEx ? pGetStateEx : pGetState, i, s); }
DWORD WINAPI HIF_XInputSetState(DWORD i, XINPUT_VIBRATION *v) { return Internal_SetState(i, v); }

/* GoW2Hollow.exe (CL 297571) reaches XInput through two import thunks:
     01A428D6: jmp dword ptr [01DE085Ch]   ; XInputSetState
     01A428DC: jmp dword ptr [01DE0858h]   ; XInputGetState
   Rewrite each as "jmp Internal_X; nop" so Steam's hooks on DLL exports are bypassed. */
static int PatchThunk(DWORD site, DWORD iatSlot, void *target)
{
    BYTE *p = (BYTE *)site;
    DWORD old;
    if (IsBadReadPtr(p, 6) || p[0] != 0xFF || p[1] != 0x25 || *(DWORD *)(p + 2) != iatSlot) return 0;
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[0] = 0xE9;
    *(LONG *)(p + 1) = (LONG)((DWORD)target - (site + 5));
    p[5] = 0x90;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    return 1;
}

static void InstallXInputThunks(void)
{
    int a, b;
    if (!GetModuleHandleW(L"GoW2Hollow.exe")) return;
    a = PatchThunk(0x01A428DCu, 0x01DE0858u, (void *)Internal_GetState);
    b = PatchThunk(0x01A428D6u, 0x01DE085Cu, (void *)Internal_SetState);
    Log("game XInput thunks redirected past Steam's hooks: GetState=%s SetState=%s",
        a ? "ok" : "NOT FOUND", b ? "ok" : "NOT FOUND");
}

DWORD WINAPI HIF_XInputGetCapabilities(DWORD i, DWORD f, XINPUT_CAPABILITIES *c)
{
    int src;
    EnsureInit();
    if (!pGetCaps) return ERROR_DEVICE_NOT_CONNECTED;
    src = MapSlot(i);
    return src < 0 ? ERROR_DEVICE_NOT_CONNECTED : pGetCaps((DWORD)src, f, c);
}

void WINAPI HIF_XInputEnable(BOOL e) { EnsureInit(); if (pEnable) pEnable(e); }

DWORD WINAPI HIF_XInputGetDSoundAudioDeviceGuids(DWORD i, GUID *r, GUID *c)
{
    int src;
    EnsureInit();
    if (!pGetDSound) return ERROR_DEVICE_NOT_CONNECTED;
    src = MapSlot(i);
    return src < 0 ? ERROR_DEVICE_NOT_CONNECTED : pGetDSound((DWORD)src, r, c);
}

DWORD WINAPI HIF_XInputGetBatteryInformation(DWORD i, BYTE t, XINPUT_BATTERY_INFORMATION *b)
{
    int src;
    EnsureInit();
    if (!pGetBattery) return ERROR_DEVICE_NOT_CONNECTED;
    src = MapSlot(i);
    return src < 0 ? ERROR_DEVICE_NOT_CONNECTED : pGetBattery((DWORD)src, t, b);
}

DWORD WINAPI HIF_XInputGetKeystroke(DWORD i, DWORD r, XINPUT_KEYSTROKE *k)
{
    int src;
    DWORD res;
    EnsureInit();
    if (!pGetKeystroke) return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_singleMode) return pGetKeystroke(i, r, k);
    if (i != 0 && i != 0xFF) return ERROR_DEVICE_NOT_CONNECTED;
    src = ResolveSource();
    if (src < 0) return ERROR_DEVICE_NOT_CONNECTED;
    res = pGetKeystroke((DWORD)src, r, k);
    if (res == ERROR_SUCCESS) k->UserIndex = 0;
    return res;
}

/* ------------------------------------------------------------------------------------------
 * HUD scaling. The in-game HUD (subtitles, ammo, health, objectives...) is drawn through a
 * UCanvas sized to the full view in pixels, with art/fonts authored for 720p, so at 1440p/4K
 * it shrinks. In UGameViewportClient::Draw (per player) the engine does:
 *     CanvasObject->SizeX/SizeY = View size;  CanvasObject->Update();   // ClipX/Y = SizeX/Y
 *     Canvas->PushRelativeTransform(Translation(View->X, View->Y));
 *     PlayerController -> HUD PostRender;  Canvas->PopTransform();
 * We redirect two calls there: Update() shrinks SizeX/Y by the scale first, and the pushed
 * matrix gets the same scale folded in. The HUD then lays out in a "virtual" 720p-ish space
 * that the GPU scales up. Menus (UIScenes) are drawn elsewhere and already scale correctly.
 * Addresses are for GoW2Hollow.exe changelist 297571 (fixed base, no ASLR); every site is
 * byte-checked first, so a different build just leaves the HUD unscaled.
 * ---------------------------------------------------------------------------------------- */
#define HUD_UPDATE_CALL  0x00AC30A6u  /* call UCanvas::Update                 */
#define HUD_UPDATE_FN    0x00D07F90u
#define HUD_PUSH_CALL    0x00AC315Bu  /* call FCanvas::PushRelativeTransform  */
#define HUD_PUSH_FN      0x00D18A80u
#define HUD_POP_FN       0x00D2BC90u  /* FCanvas::PopTransform                */
#define SUB_DISPLAY_CALL 0x00AC408Cu  /* call FSubtitleManager::DisplaySubtitles(Canvas, Region) */
#define SUB_DISPLAY_FN   0x00C0C280u

typedef void (__fastcall *PFN_PopTransform)(void *canvas, void *edx);
typedef void (__fastcall *PFN_DisplaySubs)(void *mgr, void *edx, void *canvas, int *region);

typedef void (__fastcall *PFN_CanvasUpdate)(void *canvasObj, void *edx);
typedef void (__fastcall *PFN_PushTransform)(void *canvas, void *edx, const float *matrix);

static float g_hudScaleCfg;      /* 0 = auto */
static float g_hudRefHeight;     /* auto: view height / this */
static float g_hudMaxScale;
static float g_frameScale = 1.0f;

static float CfgFloat(const WCHAR *section, const WCHAR *key, float def)
{
    WCHAR path[MAX_PATH], buf[64];
    wsprintfW(path, L"%sHollowInputFix.ini", g_dir);
    if (!GetPrivateProfileStringW(section, key, L"", buf, 64, path) || !buf[0]) return def;
    if (!lstrcmpiW(buf, L"auto")) return 0.0f;
    return (float)_wtof(buf);
}

static void __fastcall HookCanvasUpdate(void *canvasObj, void *edx)
{
    int *sizeX = (int *)((BYTE *)canvasObj + 0x64), *sizeY = (int *)((BYTE *)canvasObj + 0x68);
    float s = g_hudScaleCfg;
    if (s <= 0.0f) s = (float)*sizeY / g_hudRefHeight;
    if (s > g_hudMaxScale) s = g_hudMaxScale;
    if (s < 1.0f) s = 1.0f;
    if (s != g_frameScale)
        Log("HUD: view %dx%d -> scale %.2f (HUD lays out at %dx%d)", *sizeX, *sizeY, s,
            (int)((float)*sizeX / s + 0.5f), (int)((float)*sizeY / s + 0.5f));
    g_frameScale = s;
    if (s > 1.0f) {
        *sizeX = (int)((float)*sizeX / s + 0.5f);
        *sizeY = (int)((float)*sizeY / s + 0.5f);
    }
    ((PFN_CanvasUpdate)HUD_UPDATE_FN)(canvasObj, edx);
}

static void __fastcall HookPushTransform(void *canvas, void *edx, const float *m)
{
    __declspec(align(16)) float scaled[16]; /* engine's FMatrix math uses aligned SSE loads */
    int i;
    memcpy(scaled, m, sizeof(scaled));
    for (i = 0; i < 3; i++) { scaled[i] *= g_frameScale; scaled[4 + i] *= g_frameScale; }
    ((PFN_PushTransform)HUD_PUSH_FN)(canvas, edx, scaled);
}

/* Subtitles are drawn by the subtitle manager after the HUD, outside its transform. Draw them
   in the same scaled space: scale the canvas, shrink the region to match, then restore. */
static void __fastcall HookDisplaySubtitles(void *mgr, void *edx, void *canvas, int *region)
{
    __declspec(align(16)) float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    int r[4], i;
    float s = g_frameScale;
    static int logged;
    if (!logged++)
        Log("subtitles: region (%d,%d)-(%d,%d), scale %.2f", region[0], region[1], region[2], region[3], s);
    if (s <= 1.0f) { ((PFN_DisplaySubs)SUB_DISPLAY_FN)(mgr, edx, canvas, region); return; }
    for (i = 0; i < 4; i++) r[i] = (int)((float)region[i] / s + 0.5f);
    m[0] = m[5] = s;
    ((PFN_PushTransform)HUD_PUSH_FN)(canvas, NULL, m);
    ((PFN_DisplaySubs)SUB_DISPLAY_FN)(mgr, edx, canvas, r);
    ((PFN_PopTransform)HUD_POP_FN)(canvas, NULL);
}

static int RedirectCall(DWORD site, DWORD expectTarget, void *hook)
{
    BYTE *p = (BYTE *)site;
    DWORD old;
    if (IsBadReadPtr(p, 5) || p[0] != 0xE8 || site + 5 + *(LONG *)(p + 1) != expectTarget) return 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(LONG *)(p + 1) = (LONG)((DWORD)hook - (site + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return 1;
}

static int IsCallTo(DWORD site, DWORD target)
{
    return !IsBadReadPtr((void *)site, 5) && *(BYTE *)site == 0xE8 &&
           site + 5 + *(LONG *)(site + 1) == target;
}

static void InstallHudScale(void)
{
    /* Two per-player canvas passes in UGameViewportClient::Draw: an earlier PlayerController
       pass, and the main one that calls HUD.PostRender (PlayerController.myHUD at +0x3B0). */
    static const DWORD updateSites[2] = { 0x00AC30A6u, 0x00AC3D98u };
    static const DWORD pushSites[2]   = { 0x00AC315Bu, 0x00AC3E4Fu };
    WCHAR exe[MAX_PATH], *name;
    int i;
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    name = wcsrchr(exe, L'\\');
    if (lstrcmpiW(name ? name + 1 : exe, L"GoW2Hollow.exe")) return;

    g_logEnabled  = CfgInt(L"General", L"Log", 1);
    if (!CfgInt(L"Fixes", L"HUDFix", 1) || !CfgInt(L"HUD", L"Enable", 1)) { Log("HUD fix turned off in ini"); return; }
    g_hudScaleCfg  = CfgFloat(L"HUD", L"Scale", 0.0f);
    g_hudRefHeight = CfgFloat(L"HUD", L"ReferenceHeight", 720.0f);
    g_hudMaxScale  = CfgFloat(L"HUD", L"MaxScale", 4.0f);
    if (g_hudRefHeight < 240.0f) g_hudRefHeight = 720.0f;

    /* Check every site before touching any, so we never apply half the patch. */
    for (i = 0; i < 2; i++)
        if (!IsCallTo(updateSites[i], HUD_UPDATE_FN) || !IsCallTo(pushSites[i], HUD_PUSH_FN)) {
            Log("HUD scaling NOT applied: GoW2Hollow.exe doesn't match the expected build");
            return;
        }
    for (i = 0; i < 2; i++) {
        RedirectCall(updateSites[i], HUD_UPDATE_FN, (void *)HookCanvasUpdate);
        RedirectCall(pushSites[i], HUD_PUSH_FN, (void *)HookPushTransform);
    }
    if (CfgInt(L"HUD", L"ScaleSubtitles", 1) &&
        !RedirectCall(SUB_DISPLAY_CALL, SUB_DISPLAY_FN, (void *)HookDisplaySubtitles))
        Log("subtitle scaling NOT applied (call site mismatch)");
    if (g_hudScaleCfg > 0.0f) Log("HUD scaling active: fixed scale %.2f", g_hudScaleCfg);
    else Log("HUD scaling active: auto (view height / %.0f, max %.2f)", g_hudRefHeight, g_hudMaxScale);
}

#endif /* HIF_XINPUT */

/* ------------------------------------------------------------------------------------------ */
#ifdef HIF_DINPUT

#define DI8DEVCLASS_ALL      0
#define DI8DEVCLASS_GAMECTRL 4
#define DI8DEVTYPE_JOYSTICK  0x14
#define DI8DEVTYPE_GAMEPAD   0x15
#define DI8DEVTYPE_DRIVING   0x16
#define DI8DEVTYPE_FLIGHT    0x17
#define DI8DEVTYPE_1STPERSON 0x18
#define DIENUM_CONTINUE      1

typedef HRESULT (WINAPI *PFN_DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPVOID);
/* IDirectInput8 vtable slot 4. A and W variants share the layout; we only inspect dwDevType
   (first DWORD of the instance struct after dwSize) so the callback type doesn't matter. */
typedef BOOL    (CALLBACK *PFN_EnumCb)(const void *inst, LPVOID ref);
typedef HRESULT (WINAPI *PFN_EnumDevices)(void *self, DWORD devType, PFN_EnumCb cb, LPVOID ref, DWORD flags);

static PFN_DI8Create   pRealCreate;
static PFN_EnumDevices pRealEnum;
static int             g_hidePads = 1;

typedef struct { PFN_EnumCb cb; LPVOID ref; } FilterCtx;

static int IsGameController(const void *inst)
{
    DWORD type = ((const DWORD *)inst)[1 + 4 + 4] & 0xFF; /* dwSize, guidInstance, guidProduct, dwDevType */
    return type >= DI8DEVTYPE_JOYSTICK && type <= DI8DEVTYPE_1STPERSON;
}

static BOOL CALLBACK FilterCb(const void *inst, LPVOID ref)
{
    FilterCtx *ctx = (FilterCtx *)ref;
    if (IsGameController(inst)) return DIENUM_CONTINUE;
    return ctx->cb(inst, ctx->ref);
}

static HRESULT WINAPI HookEnumDevices(void *self, DWORD devType, PFN_EnumCb cb, LPVOID ref, DWORD flags)
{
    FilterCtx ctx;
    if (devType == DI8DEVCLASS_GAMECTRL || (devType >= DI8DEVTYPE_JOYSTICK && devType <= DI8DEVTYPE_1STPERSON)) {
        static int logged;
        if (!logged++) Log("hid DirectInput game-controller enumeration (XInput handles the pad)");
        return S_OK;
    }
    if (devType == DI8DEVCLASS_ALL) {
        ctx.cb = cb; ctx.ref = ref;
        return pRealEnum(self, devType, FilterCb, &ctx, flags);
    }
    return pRealEnum(self, devType, cb, ref, flags);
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, LPVOID outer)
{
    HRESULT hr;
    if (!pRealCreate) {
        HMODULE real = LoadSystemDll(L"dinput8.dll");
        g_logEnabled = CfgInt(L"General", L"Log", 1);
        g_hidePads = ControllerFixOn() && CfgInt(L"DirectInput", L"HideGameControllers", 1);
        if (real) pRealCreate = (PFN_DI8Create)GetProcAddress(real, "DirectInput8Create");
        if (!pRealCreate) { Log("ERROR: system dinput8.dll not found"); return E_FAIL; }
        Log("loaded system dinput8.dll; HideGameControllers=%d", g_hidePads);
    }
    hr = pRealCreate(inst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && g_hidePads && out && *out) {
        /* Patch the EnumDevices slot of DirectInput's own vtable in place. Swapping the
           object's vtable pointer is not an option: DirectInput validates it on every call. */
        void **vtbl = *(void ***)*out;
        DWORD old;
        if (vtbl[4] != (void *)HookEnumDevices && !pRealEnum &&
            VirtualProtect(&vtbl[4], sizeof(void *), PAGE_READWRITE, &old)) {
            pRealEnum = (PFN_EnumDevices)vtbl[4];
            vtbl[4] = (void *)HookEnumDevices;
            VirtualProtect(&vtbl[4], sizeof(void *), old, &old);
        }
    }
    return hr;
}

#endif /* HIF_DINPUT */

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitDir(h);
#ifdef HIF_XINPUT
        g_logEnabled = CfgInt(L"General", L"Log", 1);
        if (ControllerFixOn()) InstallXInputThunks(); else Log("controller fix turned off in ini (XInput passes through unchanged)");
        InstallHudScale();
#endif
    }
    return TRUE;
}
