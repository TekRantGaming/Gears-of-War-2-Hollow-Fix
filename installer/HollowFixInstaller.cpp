/*
 * HollowFixInstaller - installs/uninstalls the Gears of War 2: Hollow controller & HUD fix.
 *
 * The fix files (xinput1_3.dll, dinput8.dll, HollowInputFix.ini) are embedded as resources.
 * They go into <game>\Binaries next to GoW2Hollow.exe:
 *   controller fix -> xinput1_3.dll + dinput8.dll, [Fixes] ControllerFix=1
 *   HUD fix        -> xinput1_3.dll,                [Fixes] HUDFix=1
 * A DLL with the same name from another mod is renamed to *.hollowfix-backup and restored on
 * uninstall. An existing HollowInputFix.ini is kept so user tweaks survive reinstalls.
 *
 * The window is fully custom-painted with GDI+ (no stock controls), per-monitor DPI aware.
 * Command line (for scripting/testing; prints to the calling console, exit code 0 = success):
 *   HollowFixInstaller.exe /install [/controller] [/hud] [/path "<game folder>"] [/force]
 *   HollowFixInstaller.exe /uninstall [/path "<game folder>"]
 *   HollowFixInstaller.exe /path "<game folder>"   (opens the window with that folder chosen)
 * With /install and neither /controller nor /hud, both fixes are installed.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
using std::min;
using std::max;
#include <gdiplus.h>
#include "resource.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Gdiplus;

#define APP_VERSION L"1.0"
#define BACKUP_EXT  L".hollowfix-backup"

/* ======================================================================================= */
/*  Install logic (shared by the window and the command line)                               */
/* ======================================================================================= */

enum LogKind { LOG_INFO, LOG_OK, LOG_WARN, LOG_ERROR };
static void (*g_logSink)(LogKind, const std::wstring &);

static void Report(LogKind k, const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_logSink) g_logSink(k, buf);
}

static bool FileExists(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Accepts the game root, its Binaries folder, or the exe itself; outputs the Binaries dir. */
static bool ResolveBinaries(std::wstring p, std::wstring &bin)
{
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/' || p.back() == L' ' || p.back() == L'"')) p.pop_back();
    while (!p.empty() && (p.front() == L'"' || p.front() == L' ')) p.erase(0, 1);
    if (p.empty()) return false;
    const std::wstring exe = L"\\GoW2Hollow.exe";
    if (p.size() > exe.size() && !_wcsicmp(p.c_str() + p.size() - exe.size(), exe.c_str())) p.resize(p.size() - exe.size());
    if (FileExists(p + exe)) { bin = p; return true; }
    if (FileExists(p + L"\\Binaries" + exe)) { bin = p + L"\\Binaries"; return true; }
    return false;
}

static std::wstring DetectInstall()
{
    static const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    wchar_t v[MAX_PATH];
    std::wstring bin;
    for (HKEY root : roots) {
        DWORD sz = sizeof(v);
        if (RegGetValueW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GoW2Hollow",
                         L"InstallLocation", RRF_RT_REG_SZ, NULL, v, &sz) == ERROR_SUCCESS && ResolveBinaries(v, bin))
            return v;
    }
    /* Installer dropped into the game folder (or its Binaries folder)? */
    GetModuleFileNameW(NULL, v, MAX_PATH);
    *wcsrchr(v, L'\\') = 0;
    if (ResolveBinaries(v, bin)) return v;
    return L"";
}

/* ---- game build check: the same sites the DLL byte-checks before patching ------------- */
static bool ReadAt(HANDLE f, DWORD va, BYTE *buf, DWORD n)
{
    DWORD got;
    LARGE_INTEGER off;
    off.QuadPart = va - 0x00400000u; /* file offset == RVA in this exe */
    return SetFilePointerEx(f, off, NULL, FILE_BEGIN) && ReadFile(f, buf, n, &got, NULL) && got == n;
}

static bool IsCallTo(HANDLE f, DWORD site, DWORD target)
{
    BYTE b[5];
    return ReadAt(f, site, b, 5) && b[0] == 0xE8 && site + 5 + *(LONG *)(b + 1) == target;
}

static bool IsJmpThunk(HANDLE f, DWORD site, DWORD iat)
{
    BYTE b[6];
    return ReadAt(f, site, b, 6) && b[0] == 0xFF && b[1] == 0x25 && *(DWORD *)(b + 2) == iat;
}

/* 1 = known build, 0 = different build, -1 = couldn't read. */
static int CheckBuild(const std::wstring &bin, bool ctrl, bool hud)
{
    HANDLE f = CreateFileW((bin + L"\\GoW2Hollow.exe").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    bool ok = true;
    if (ctrl) ok = ok && IsJmpThunk(f, 0x01A428DCu, 0x01DE0858u) && IsJmpThunk(f, 0x01A428D6u, 0x01DE085Cu);
    if (hud)
        ok = ok && IsCallTo(f, 0x00AC30A6u, 0x00D07F90u) && IsCallTo(f, 0x00AC3D98u, 0x00D07F90u) &&
             IsCallTo(f, 0x00AC315Bu, 0x00D18A80u) && IsCallTo(f, 0x00AC3E4Fu, 0x00D18A80u) &&
             IsCallTo(f, 0x00AC408Cu, 0x00C0C280u);
    CloseHandle(f);
    return ok ? 1 : 0;
}

/* ---- files ------------------------------------------------------------------------------ */
static bool IsOurDll(const std::wstring &path)
{
    static const wchar_t marker[] = L"HollowInputFix";
    const size_t mlen = sizeof(marker) - sizeof(wchar_t);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    DWORD size = GetFileSize(f, NULL), got = 0;
    if (size > 0 && size < 16 * 1024 * 1024) {
        std::vector<BYTE> buf(size);
        if (ReadFile(f, buf.data(), size, &got, NULL))
            for (DWORD i = 0; i + mlen <= got && !found; i++) found = !memcmp(&buf[i], marker, mlen);
    }
    CloseHandle(f);
    return found;
}

static bool WriteResource(int id, const std::wstring &path)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g;
    if (!r || !(g = LoadResource(NULL, r))) return false;
    DWORD size = SizeofResource(NULL, r), wrote = 0;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = WriteFile(f, LockResource(g), size, &wrote, NULL) && wrote == size;
    CloseHandle(f);
    return ok;
}

static void ExplainError(const wchar_t *what)
{
    DWORD e = GetLastError();
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_USER_MAPPED_FILE)
        Report(LOG_ERROR, L"%s is in use. Close Gears of War 2 and try again.", what);
    else if (e == ERROR_ACCESS_DENIED)
        Report(LOG_ERROR, L"No permission to write %s. Try running the installer as administrator.", what);
    else
        Report(LOG_ERROR, L"Couldn't write %s (error %lu).", what, e);
}

/* Puts our DLL in place, moving a different mod's file of the same name to a backup first. */
static bool InstallDll(const std::wstring &bin, const wchar_t *name, int res)
{
    std::wstring dst = bin + L"\\" + name, bak = dst + BACKUP_EXT;
    if (FileExists(dst) && !IsOurDll(dst)) {
        if (!MoveFileExW(dst.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING)) { ExplainError(name); return false; }
        Report(LOG_WARN, L"Another mod's %s was backed up as %s%s", name, name, BACKUP_EXT);
    }
    if (!WriteResource(res, dst)) { ExplainError(name); return false; }
    Report(LOG_INFO, L"Installed %s", name);
    return true;
}

/* Removes our DLL (only if it is ours) and restores any backup. */
static bool RemoveDll(const std::wstring &bin, const wchar_t *name)
{
    std::wstring dst = bin + L"\\" + name, bak = dst + BACKUP_EXT;
    if (FileExists(dst) && IsOurDll(dst)) {
        if (!DeleteFileW(dst.c_str())) { ExplainError(name); return false; }
        Report(LOG_INFO, L"Removed %s", name);
    }
    if (FileExists(bak) && !FileExists(dst) && MoveFileW(bak.c_str(), dst.c_str()))
        Report(LOG_INFO, L"Restored the other mod's %s", name);
    return true;
}

static bool DoInstall(const std::wstring &bin, bool ctrl, bool hud)
{
    std::wstring ini = bin + L"\\HollowInputFix.ini";
    bool ok = InstallDll(bin, L"xinput1_3.dll", IDR_XINPUT);
    if (ok) ok = ctrl ? InstallDll(bin, L"dinput8.dll", IDR_DINPUT) : RemoveDll(bin, L"dinput8.dll");
    if (ok && !FileExists(ini) && !(ok = WriteResource(IDR_INI, ini))) ExplainError(L"HollowInputFix.ini");
    if (ok) {
        ok = WritePrivateProfileStringW(L"Fixes", L"ControllerFix", ctrl ? L"1" : L"0", ini.c_str()) &&
             WritePrivateProfileStringW(L"Fixes", L"HUDFix", hud ? L"1" : L"0", ini.c_str());
        if (!ok) ExplainError(L"HollowInputFix.ini");
    }
    if (!ok) { Report(LOG_ERROR, L"Install did not finish."); return false; }
    Report(LOG_OK, L"Installed. Controller fix %s, HUD fix %s. Launch the game as you normally do.",
           ctrl ? L"ON" : L"OFF", hud ? L"ON" : L"OFF");
    return true;
}

static bool DoUninstall(const std::wstring &bin)
{
    bool ok = RemoveDll(bin, L"xinput1_3.dll");
    ok = RemoveDll(bin, L"dinput8.dll") && ok;
    if (ok) {
        std::wstring p = bin + L"\\HollowInputFix.ini";
        if (FileExists(p) && DeleteFileW(p.c_str())) Report(LOG_INFO, L"Removed HollowInputFix.ini");
        p = bin + L"\\HollowInputFix.log";
        if (FileExists(p) && DeleteFileW(p.c_str())) Report(LOG_INFO, L"Removed HollowInputFix.log");
        Report(LOG_OK, L"Uninstalled. The game is back to normal.");
    } else Report(LOG_ERROR, L"Uninstall did not finish.");
    return ok;
}

/* Installed state of a Binaries folder: returns false if our files aren't there. */
static bool ReadInstalledState(const std::wstring &bin, bool &ctrl, bool &hud)
{
    std::wstring dll = bin + L"\\xinput1_3.dll", ini = bin + L"\\HollowInputFix.ini";
    if (!FileExists(dll) || !IsOurDll(dll)) return false;
    ctrl = GetPrivateProfileIntW(L"Fixes", L"ControllerFix", 1, ini.c_str()) != 0;
    hud  = GetPrivateProfileIntW(L"Fixes", L"HUDFix", 1, ini.c_str()) != 0;
    return true;
}

/* ======================================================================================= */
/*  Command line                                                                            */
/* ======================================================================================= */

static void ConsoleSink(LogKind k, const std::wstring &msg)
{
    static const wchar_t *tag[] = { L"  ", L"OK ", L"!! ", L"ERR" };
    wprintf(L"%s %s\n", tag[k], msg.c_str());
}

static int RunCommandLine(int argc, wchar_t **argv)
{
    bool install = false, uninstall = false, ctrl = false, hud = false, force = false;
    std::wstring path;
    for (int i = 1; i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!_wcsicmp(a, L"/install")) install = true;
        else if (!_wcsicmp(a, L"/uninstall")) uninstall = true;
        else if (!_wcsicmp(a, L"/controller")) ctrl = true;
        else if (!_wcsicmp(a, L"/hud")) hud = true;
        else if (!_wcsicmp(a, L"/force")) force = true;
        else if (!_wcsicmp(a, L"/path") && i + 1 < argc) path = argv[++i];
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *f;
        freopen_s(&f, "CONOUT$", "w", stdout);
    }
    g_logSink = ConsoleSink;
    if (!ctrl && !hud) ctrl = hud = true;
    if (path.empty()) path = DetectInstall();
    std::wstring bin;
    if (!ResolveBinaries(path, bin)) {
        Report(LOG_ERROR, L"Couldn't find Gears of War 2: Hollow. Pass the folder with /path \"...\"");
        return 2;
    }
    Report(LOG_INFO, L"Game: %s", bin.c_str());
    if (uninstall) return DoUninstall(bin) ? 0 : 1;
    if (CheckBuild(bin, ctrl, hud) == 0) {
        Report(LOG_WARN, L"GoW2Hollow.exe is a different version from the one these fixes were made for.");
        if (!force) { Report(LOG_ERROR, L"Not installed. Add /force to install anyway."); return 3; }
    }
    return DoInstall(bin, ctrl, hud) ? 0 : 1;
}

/* ======================================================================================= */
/*  Window                                                                                  */
/* ======================================================================================= */

/* Palette: gunmetal + COG crimson. */
static const Color C_BG(255, 16, 17, 19), C_PANEL(255, 27, 29, 32), C_PANEL_HOT(255, 34, 37, 41),
    C_BORDER(255, 50, 54, 59), C_TEXT(255, 236, 232, 226), C_DIM(255, 150, 154, 160),
    C_RED(255, 200, 16, 46), C_RED_DARK(255, 110, 8, 20), C_RED_HOT(255, 232, 40, 64),
    C_GREEN(255, 112, 196, 96), C_AMBER(255, 232, 170, 60);

enum HitId { HIT_NONE, HIT_BROWSE, HIT_PATH, HIT_CTRL, HIT_HUD, HIT_INSTALL, HIT_UNINSTALL };

struct LogLine { LogKind kind; std::wstring text; };

static HWND g_wnd;
static UINT g_dpi = 96;
static int g_hot = HIT_NONE, g_pressed = HIT_NONE;
static bool g_ctrl = true, g_hud = true, g_busy = false;
static std::wstring g_path, g_bin;
static bool g_found = false, g_installed = false;
static std::vector<LogLine> g_log;
static const wchar_t *g_family = L"Bahnschrift";
static std::wstring g_startPath; /* /path given without /install: pre-fills the folder */

static float S(float v) { return v * g_dpi / 96.0f; }
static const float LOGICAL_W = 680, LOGICAL_H = 610;

static void UiSink(LogKind k, const std::wstring &msg)
{
    g_log.push_back({ k, msg });
    if (g_log.size() > 3) g_log.erase(g_log.begin());
    if (g_wnd) InvalidateRect(g_wnd, NULL, FALSE);
}

static void RefreshGameState()
{
    g_found = ResolveBinaries(g_path, g_bin);
    g_installed = false;
    if (g_found) {
        bool c, h;
        if (ReadInstalledState(g_bin, c, h)) { g_installed = true; g_ctrl = c; g_hud = h; }
    }
    InvalidateRect(g_wnd, NULL, FALSE);
}

/* ---- layout (logical units, scaled at paint/hit time) ----------------------------------- */
static RectF L(float x, float y, float w, float h) { return RectF(S(x), S(y), S(w), S(h)); }
static RectF R_PATH()      { return L(28, 196, 500, 44); }
static RectF R_BROWSE()    { return L(538, 196, 114, 44); }
static RectF R_CTRL()      { return L(28, 296, 624, 86); }
static RectF R_HUD()       { return L(28, 392, 624, 86); }
static RectF R_INSTALL()   { return L(28, 498, 230, 52); }
static RectF R_UNINSTALL() { return L(270, 498, 160, 52); }

static int HitTest(int x, int y)
{
    PointF p((REAL)x, (REAL)y);
    if (R_BROWSE().Contains(p)) return HIT_BROWSE;
    if (R_PATH().Contains(p)) return HIT_PATH;
    if (R_CTRL().Contains(p)) return HIT_CTRL;
    if (R_HUD().Contains(p)) return HIT_HUD;
    if (R_INSTALL().Contains(p)) return HIT_INSTALL;
    if (R_UNINSTALL().Contains(p)) return HIT_UNINSTALL;
    return HIT_NONE;
}

/* ---- drawing helpers -------------------------------------------------------------------- */
static void RoundRect(GraphicsPath &gp, RectF r, float rad)
{
    float d = rad * 2;
    gp.AddArc(r.X, r.Y, d, d, 180, 90);
    gp.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    gp.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    gp.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    gp.CloseFigure();
}

static void FillRound(Graphics &g, RectF r, float rad, const Brush &b, const Color *border = nullptr, float bw = 1)
{
    GraphicsPath gp;
    RoundRect(gp, r, rad);
    g.FillPath(&b, &gp);
    if (border) { Pen p(*border, bw); g.DrawPath(&p, &gp); }
}

static void Text(Graphics &g, const wchar_t *s, float size, int style, const Color &c, RectF r,
                 StringAlignment h = StringAlignmentNear, StringAlignment v = StringAlignmentNear, bool ellipsis = false)
{
    FontFamily fam(g_family);
    Font f(fam.IsAvailable() ? &fam : FontFamily::GenericSansSerif(), S(size), style, UnitPixel);
    StringFormat fmt;
    fmt.SetAlignment(h);
    fmt.SetLineAlignment(v);
    if (ellipsis) { fmt.SetTrimming(StringTrimmingEllipsisPath); fmt.SetFormatFlags(StringFormatFlagsNoWrap); }
    SolidBrush b(c);
    g.DrawString(s, -1, &f, r, &fmt, &b);
}

/* A COG-style cog: toothed ring with a hollow centre and three spokes. */
static void DrawCog(Graphics &g, PointF c, float R)
{
    const int teeth = 12;
    const float rOut = R, rIn = R * 0.80f, rHole = R * 0.42f;
    GraphicsPath gp(FillModeAlternate);
    std::vector<PointF> pts;
    for (int i = 0; i < teeth; i++) {
        float a0 = (float)(i * 2 * 3.14159265 / teeth);
        float step = (float)(2 * 3.14159265 / teeth);
        float a[4] = { a0 - step * 0.22f, a0 - step * 0.14f, a0 + step * 0.14f, a0 + step * 0.22f };
        float rr[4] = { rIn, rOut, rOut, rIn };
        for (int k = 0; k < 4; k++) pts.push_back(PointF(c.X + rr[k] * cosf(a[k]), c.Y + rr[k] * sinf(a[k])));
        float mid = a0 + step * 0.5f;
        pts.push_back(PointF(c.X + rIn * cosf(mid), c.Y + rIn * sinf(mid)));
    }
    gp.AddPolygon(pts.data(), (INT)pts.size());
    gp.AddEllipse(c.X - rHole, c.Y - rHole, rHole * 2, rHole * 2);
    LinearGradientBrush br(PointF(c.X, c.Y - R), PointF(c.X, c.Y + R), C_RED_HOT, C_RED_DARK);
    g.FillPath(&br, &gp);
    Pen edge(Color(160, 0, 0, 0), S(1.5f));
    g.DrawPath(&edge, &gp);
    /* centre: a steel hex bolt in the hole (a generic cog, deliberately not the Crimson Omen) */
    std::vector<PointF> hex;
    float rh = rHole * 0.72f;
    for (int i = 0; i < 6; i++) {
        float a = (float)(i * 3.14159265 / 3 + 3.14159265 / 6);
        hex.push_back(PointF(c.X + rh * cosf(a), c.Y + rh * sinf(a)));
    }
    LinearGradientBrush steel(PointF(c.X, c.Y - rh), PointF(c.X, c.Y + rh), Color(255, 150, 154, 160), Color(255, 52, 55, 60));
    g.FillPolygon(&steel, hex.data(), 6);
    Pen hexEdge(Color(200, 20, 20, 22), S(1.2f));
    g.DrawPolygon(&hexEdge, hex.data(), 6);
    SolidBrush bore(Color(255, 20, 21, 23));
    float rb = rh * 0.42f;
    g.FillEllipse(&bore, c.X - rb, c.Y - rb, rb * 2, rb * 2);
}

static void DrawCheck(Graphics &g, RectF box, bool on, bool hot)
{
    if (on) {
        LinearGradientBrush br(box, C_RED_HOT, C_RED_DARK, LinearGradientModeVertical);
        FillRound(g, box, S(5), br);
        Pen p(Color(255, 255, 255, 255), S(3.2f));
        p.SetLineJoin(LineJoinRound);
        p.SetStartCap(LineCapRound);
        p.SetEndCap(LineCapRound);
        PointF pts[3] = { PointF(box.X + box.Width * 0.24f, box.Y + box.Height * 0.52f),
                          PointF(box.X + box.Width * 0.43f, box.Y + box.Height * 0.70f),
                          PointF(box.X + box.Width * 0.77f, box.Y + box.Height * 0.31f) };
        g.DrawLines(&p, pts, 3);
    } else {
        SolidBrush br(Color(255, 14, 15, 17));
        Color bc = hot ? C_RED : Color(255, 92, 97, 104);
        FillRound(g, box, S(5), br, &bc, S(2));
    }
}

static void DrawButton(Graphics &g, RectF r, const wchar_t *label, bool primary, bool hot, bool down, bool enabled)
{
    if (primary) {
        Color top = !enabled ? Color(255, 70, 30, 34) : hot ? C_RED_HOT : C_RED;
        Color bot = !enabled ? Color(255, 40, 18, 20) : C_RED_DARK;
        if (down) std::swap(top, bot);
        LinearGradientBrush br(r, top, bot, LinearGradientModeVertical);
        Color edge(255, 255, 90, 100);
        FillRound(g, r, S(4), br, enabled ? &edge : nullptr, S(1));
        /* hazard-stripe sheen on the left edge */
        GraphicsPath clip;
        RoundRect(clip, r, S(4));
        g.SetClip(&clip);
        SolidBrush stripe(Color(enabled ? 40 : 15, 0, 0, 0));
        for (float x = r.X - r.Height; x < r.X + S(46); x += S(14)) {
            PointF q[4] = { PointF(x, r.Y + r.Height), PointF(x + S(7), r.Y + r.Height),
                            PointF(x + S(7) + r.Height, r.Y), PointF(x + r.Height, r.Y) };
            g.FillPolygon(&stripe, q, 4);
        }
        g.ResetClip();
        Text(g, label, 19, FontStyleBold, enabled ? C_TEXT : C_DIM, r, StringAlignmentCenter, StringAlignmentCenter);
    } else {
        SolidBrush br(down ? Color(255, 22, 24, 27) : hot ? C_PANEL_HOT : C_PANEL);
        Color bc = hot && enabled ? Color(255, 120, 124, 130) : C_BORDER;
        FillRound(g, r, S(4), br, &bc, S(1.2f));
        Text(g, label, 16, FontStyleBold, enabled ? (hot ? C_TEXT : Color(255, 200, 202, 206)) : Color(255, 80, 84, 90), r,
             StringAlignmentCenter, StringAlignmentCenter);
    }
}

static void DrawCard(Graphics &g, RectF r, bool on, bool hot, const wchar_t *title, const wchar_t *desc)
{
    SolidBrush bg(hot ? C_PANEL_HOT : C_PANEL);
    Color bc = on ? Color(255, 140, 18, 34) : C_BORDER;
    FillRound(g, r, S(6), bg, &bc, S(on ? 1.6f : 1.0f));
    if (on) {
        SolidBrush acc(C_RED);
        GraphicsPath clip;
        RoundRect(clip, r, S(6));
        g.SetClip(&clip);
        g.FillRectangle(&acc, r.X, r.Y, S(5), r.Height);
        g.ResetClip();
    }
    RectF box(r.X + S(22), r.Y + (r.Height - S(28)) / 2, S(28), S(28));
    DrawCheck(g, box, on, hot);
    Text(g, title, 19, FontStyleBold, on ? C_TEXT : Color(255, 200, 202, 206), RectF(r.X + S(68), r.Y + S(12), r.Width - S(84), S(26)));
    Text(g, desc, 13.5f, FontStyleRegular, C_DIM, RectF(r.X + S(68), r.Y + S(38), r.Width - S(84), r.Height - S(42)));
}

static void Paint(HDC hdc)
{
    RECT rc;
    GetClientRect(g_wnd, &rc);
    Bitmap buf(rc.right, rc.bottom, PixelFormat32bppPARGB);
    Graphics g(&buf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    float W = (float)rc.right, H = (float)rc.bottom;

    /* background: gunmetal with faint diagonal tread plate */
    SolidBrush bg(C_BG);
    g.FillRectangle(&bg, 0.0f, 0.0f, W, H);
    Pen tread(Color(10, 255, 255, 255), S(1));
    for (float x = -H; x < W; x += S(9)) g.DrawLine(&tread, x, H, x + H, 0.0f);

    /* header: blood-red glow behind the cog */
    RectF head(0, 0, W, S(160));
    LinearGradientBrush hb(head, Color(255, 44, 10, 14), Color(255, 18, 12, 13), LinearGradientModeVertical);
    g.FillRectangle(&hb, head);
    GraphicsPath glow;
    glow.AddEllipse(S(-40), S(-60), S(260), S(260));
    PathGradientBrush gb(&glow);
    gb.SetCenterColor(Color(150, 200, 16, 46));
    Color edgeCol(0, 200, 16, 46);
    INT n = 1;
    gb.SetSurroundColors(&edgeCol, &n);
    g.FillPath(&gb, &glow);
    DrawCog(g, PointF(S(86), S(80)), S(52));
    Text(g, L"GEARS OF WAR 2: HOLLOW", 33, FontStyleBold, C_TEXT, RectF(S(160), S(36), W - S(170), S(44)));
    Text(g, L"CONTROLLER  &  HUD  FIX", 17, FontStyleBold, C_RED_HOT, RectF(S(162), S(82), W - S(170), S(26)));
    Text(g, L"Community patch  •  v" APP_VERSION, 13, FontStyleRegular, C_DIM, RectF(S(162), S(110), W - S(170), S(22)));
    LinearGradientBrush line(RectF(0, S(158), W, S(3)), C_RED, Color(0, 200, 16, 46), LinearGradientModeHorizontal);
    g.FillRectangle(&line, 0.0f, S(158), W, S(3));

    /* game folder */
    Text(g, L"GAME FOLDER", 13, FontStyleBold, C_DIM, L(30, 172, 300, 20));
    RectF pr = R_PATH();
    SolidBrush pb(Color(255, 11, 12, 14));
    Color pbc = g_hot == HIT_PATH ? Color(255, 110, 114, 120) : C_BORDER;
    FillRound(g, pr, S(4), pb, &pbc, S(1.2f));
    Text(g, g_path.empty() ? L"Click Browse to find your game folder" : g_path.c_str(), 14.5f, FontStyleRegular,
         g_path.empty() ? C_DIM : C_TEXT, RectF(pr.X + S(14), pr.Y, pr.Width - S(24), pr.Height),
         StringAlignmentNear, StringAlignmentCenter, true);
    DrawButton(g, R_BROWSE(), L"BROWSE", false, g_hot == HIT_BROWSE, g_pressed == HIT_BROWSE, true);

    const wchar_t *st;
    Color sc;
    if (!g_found) { st = g_path.empty() ? L"Game not found automatically" : L"This folder isn't a Gears of War 2: Hollow install"; sc = C_AMBER; }
    else if (g_installed) { st = L"Game found  •  fixes installed (install again to update or change them)"; sc = C_GREEN; }
    else { st = L"Game found  •  fixes not installed yet"; sc = C_GREEN; }
    SolidBrush dot(sc);
    g.FillEllipse(&dot, S(31), S(252.5f), S(9), S(9));
    Text(g, st, 13.5f, FontStyleRegular, sc, L(48, 246, 600, 22), StringAlignmentNear, StringAlignmentCenter);

    /* fixes */
    Text(g, L"CHOOSE YOUR FIXES", 13, FontStyleBold, C_DIM, L(30, 274, 300, 20));
    DrawCard(g, R_CTRL(), g_ctrl, g_hot == HIT_CTRL, L"CONTROLLER FIX",
             L"Stops one controller showing up as two players when the game is launched through Steam (needed for online co-op).");
    DrawCard(g, R_HUD(), g_hud, g_hot == HIT_HUD, L"HUD FIX",
             L"Makes the in-game HUD and subtitles readable at 1440p and 4K. The menus are unchanged.");

    /* actions */
    bool canInstall = g_found && (g_ctrl || g_hud) && !g_busy;
    DrawButton(g, R_INSTALL(), g_installed ? L"UPDATE" : L"INSTALL", true, g_hot == HIT_INSTALL && canInstall,
               g_pressed == HIT_INSTALL, canInstall);
    DrawButton(g, R_UNINSTALL(), L"UNINSTALL", false, g_hot == HIT_UNINSTALL && g_installed, g_pressed == HIT_UNINSTALL,
               g_found && g_installed && !g_busy);
    if (!g_ctrl && !g_hud)
        Text(g, L"Tick at least one fix.", 13.5f, FontStyleRegular, C_AMBER, L(446, 498, 210, 52), StringAlignmentNear,
             StringAlignmentCenter);

    /* status: the outcome of the last action (every step also lands in the log list, but the
       final line is the one that matters to the user) */
    if (!g_log.empty()) {
        const LogLine &ln = g_log.back();
        Color c = ln.kind == LOG_OK ? C_GREEN : ln.kind == LOG_WARN ? C_AMBER : ln.kind == LOG_ERROR ? C_RED_HOT : C_DIM;
        Text(g, ln.text.c_str(), 14, FontStyleBold, c, L(30, 564, 622, 40));
    }

    Graphics screen(hdc);
    screen.DrawImage(&buf, 0, 0);
}

/* ---- actions ---------------------------------------------------------------------------- */
static void Browse()
{
    IFileOpenDialog *dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Select your Gears of War 2: Hollow folder");
    IShellItem *item;
    if (SUCCEEDED(dlg->Show(g_wnd)) && SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            g_path = path;
            CoTaskMemFree(path);
            g_log.clear();
            RefreshGameState();
            if (!g_found) Report(LOG_WARN, L"Pick the folder that contains GoW2Hollow_Launcher.exe (or its Binaries folder).");
        }
        item->Release();
    }
    dlg->Release();
}

static void ClickInstall()
{
    if (!g_found || (!g_ctrl && !g_hud)) return;
    if (CheckBuild(g_bin, g_ctrl, g_hud) == 0 &&
        MessageBoxW(g_wnd, L"Your GoW2Hollow.exe is a different version from the one these fixes were made for "
                    L"(Hollow has probably been updated).\n\nThe fixes check the game before changing anything and "
                    L"switch themselves off where it doesn't match, so the game will still run, but some or all of "
                    L"the fixes may do nothing.\n\nInstall anyway?",
                    L"Gears of War 2: Hollow Fix", MB_ICONWARNING | MB_YESNO) != IDYES) return;
    g_busy = true;
    DoInstall(g_bin, g_ctrl, g_hud);
    g_busy = false;
    RefreshGameState();
}

static void ClickUninstall()
{
    if (!g_found || !g_installed) return;
    g_busy = true;
    DoUninstall(g_bin);
    g_busy = false;
    g_ctrl = g_hud = true;
    RefreshGameState();
}

static void ApplyDpiSize(HWND h, const RECT *suggested)
{
    if (suggested) {
        SetWindowPos(h, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    RECT r = { 0, 0, (LONG)S(LOGICAL_W), (LONG)S(LOGICAL_H) };
    AdjustWindowRectExForDpi(&r, GetWindowLongW(h, GWL_STYLE), FALSE, 0, g_dpi);
    SetWindowPos(h, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        g_wnd = h;
        g_dpi = GetDpiForWindow(h);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        COLORREF cap = RGB(30, 10, 13);
        DwmSetWindowAttribute(h, 35 /* DWMWA_CAPTION_COLOR (Win11) */, &cap, sizeof(cap));
        ApplyDpiSize(h, NULL);
        g_path = g_startPath.empty() ? DetectInstall() : g_startPath;
        RefreshGameState();
        if (!g_found) Report(LOG_WARN, L"Couldn't find the game automatically. Click Browse and pick its folder.");
        return 0;
    }
    case WM_DPICHANGED:
        g_dpi = HIWORD(w);
        ApplyDpiSize(h, (RECT *)l);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int hit = HitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hit != g_hot) {
            g_hot = hit;
            InvalidateRect(h, NULL, FALSE);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hot = HIT_NONE;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, g_hot != HIT_NONE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        g_pressed = HitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int hit = HitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        int was = g_pressed;
        g_pressed = HIT_NONE;
        ReleaseCapture();
        InvalidateRect(h, NULL, FALSE);
        if (hit != was) return 0;
        switch (hit) {
        case HIT_BROWSE: case HIT_PATH: Browse(); break;
        case HIT_CTRL: g_ctrl = !g_ctrl; break;
        case HIT_HUD: g_hud = !g_hud; break;
        case HIT_INSTALL: ClickInstall(); break;
        case HIT_UNINSTALL: ClickUninstall(); break;
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { DestroyWindow(h); return 0; }
        if (w == VK_RETURN) { ClickInstall(); return 0; }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++)
        if (!_wcsicmp(argv[i], L"/install") || !_wcsicmp(argv[i], L"/uninstall")) return RunCommandLine(argc, argv);
    for (int i = 1; i + 1 < argc; i++)
        if (!_wcsicmp(argv[i], L"/path")) g_startPath = argv[i + 1];

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gsi;
    ULONG_PTR token;
    GdiplusStartup(&token, &gsi, NULL);
    {
        FontFamily probe(L"Bahnschrift");
        if (!probe.IsAvailable()) g_family = L"Segoe UI";
    }
    g_logSink = UiSink;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = L"HollowFixInstaller";
    RegisterClassW(&wc);

    HWND wnd = CreateWindowW(wc.lpszClassName, L"Gears of War 2: Hollow — Controller & HUD Fix",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                             800, 700, NULL, NULL, inst, NULL);
    /* centre on the monitor it opened on */
    RECT wr;
    GetWindowRect(wnd, &wr);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(wnd, NULL, (mi.rcWork.left + mi.rcWork.right - (wr.right - wr.left)) / 2,
                 (mi.rcWork.top + mi.rcWork.bottom - (wr.bottom - wr.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(wnd, show);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    GdiplusShutdown(token);
    CoUninitialize();
    return 0;
}
