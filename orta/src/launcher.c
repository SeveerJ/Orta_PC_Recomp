/* launcher.c -- the settings window shown before the game starts.
 *
 * Shown the first time the game is started (settings.ini has no
 * [Launcher] ShowOnStart yet), every time if the player ticks "Show this
 * every time", and whenever Shift is held while the game starts. Scripted
 * runs (stderr captured, see recomp_diag_quiet) never see it.
 *
 * Plain Win32 so it is up before Direct3D: mouse, touch, keyboard, or a
 * controller (D-pad/stick moves, left/right changes, A selects, Start plays).
 * Values are written to settings.ini with WritePrivateProfileString, which
 * keeps the file's comments and the [Advanced] section as they are. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "launcher.h"

extern int recomp_diag_quiet(void);

enum {
    ID_RES = 100, ID_MODE, ID_WINH, ID_WIDE, ID_VSYNC, ID_VOL, ID_VOLTXT,
    ID_SHOW, ID_PLAY, ID_QUIT, ID_TIMER = 1
};

static const int k_res[]  = { 480, 960, 1440, 1920, 2400, 2880 };
static const int k_winh[] = { 540, 720, 900, 1080, 1440 };

static const char *s_ini;
static HWND s_wnd, s_res, s_mode, s_winh, s_wide, s_vsync, s_vol, s_voltxt, s_show;
static HFONT s_font, s_title_font;
static int s_dpi = 96, s_result;
static int s_screen_h;

static int S(int v) { return MulDiv(v, s_dpi, 96); }

static int ini_int(const char *sec, const char *key, int def)
{
    return (int)GetPrivateProfileIntA(sec, key, def, s_ini);
}

static void ini_put(const char *sec, const char *key, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    WritePrivateProfileStringA(sec, key, b, s_ini);
}

static int nearest(const int *list, int n, int v)
{
    int i, best = 0;
    for (i = 0; i < n; i++)
        if (abs(list[i] - v) < abs(list[best] - v)) best = i;
    return best;
}

/* ── controls ────────────────────────────────────────────────────────── */
static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y,
               int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), s_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

static void label(const wchar_t *text, int y)
{
    mk(L"STATIC", text, SS_CENTERIMAGE, 24, y, 180, 30, 0);
}

static void update_volume_text(void)
{
    wchar_t b[16];
    swprintf(b, 16, L"%d", (int)SendMessageW(s_vol, TBM_GETPOS, 0, 0));
    SetWindowTextW(s_voltxt, b);
}

static void update_enabled(void)
{
    EnableWindow(s_winh, SendMessageW(s_mode, CB_GETCURSEL, 0, 0) == 1);
}

static void build(void)
{
    wchar_t b[96];
    int i, y = 76;
    HWND t = mk(L"STATIC", L"Panzer Dragoon Orta", 0, 24, 16, 460, 36, 0);
    SendMessageW(t, WM_SETFONT, (WPARAM)s_title_font, TRUE);
    swprintf(b, 96, L"Settings before you play. Your screen is %d pixels tall.", s_screen_h);
    mk(L"STATIC", b, 0, 24, 50, 460, 22, 0);

    label(L"Render resolution", y);
    s_res = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 210, y + 2, 270, 300, ID_RES);
    for (i = 0; i < 6; i++) {
        swprintf(b, 96, i == 0 ? L"%d (original, fastest)" : L"%d (%dx)", k_res[i], i + 1);
        SendMessageW(s_res, CB_ADDSTRING, 0, (LPARAM)b);
    }
    SendMessageW(s_res, CB_SETCURSEL, nearest(k_res, 6, ini_int("Display", "Resolution", 480)), 0);
    y += 40;

    label(L"Display", y);
    s_mode = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 210, y + 2, 270, 200, ID_MODE);
    SendMessageW(s_mode, CB_ADDSTRING, 0, (LPARAM)L"Fullscreen");
    SendMessageW(s_mode, CB_ADDSTRING, 0, (LPARAM)L"Window");
    SendMessageW(s_mode, CB_SETCURSEL, ini_int("Display", "Fullscreen", 1) ? 0 : 1, 0);
    y += 40;

    label(L"Window height", y);
    s_winh = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 210, y + 2, 270, 200, ID_WINH);
    for (i = 0; i < 5; i++) {
        swprintf(b, 96, L"%d", k_winh[i]);
        SendMessageW(s_winh, CB_ADDSTRING, 0, (LPARAM)b);
    }
    SendMessageW(s_winh, CB_SETCURSEL, nearest(k_winh, 5, ini_int("Display", "WindowHeight", 720)), 0);
    y += 44;

    s_wide = mk(WC_BUTTONW, L"Widescreen (16:9)", BS_AUTOCHECKBOX | WS_TABSTOP, 210, y, 270, 28, ID_WIDE);
    SendMessageW(s_wide, BM_SETCHECK, ini_int("Display", "Widescreen", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 34;
    s_vsync = mk(WC_BUTTONW, L"VSync (no tearing, a little more lag)", BS_AUTOCHECKBOX | WS_TABSTOP,
                 210, y, 280, 28, ID_VSYNC);
    SendMessageW(s_vsync, BM_SETCHECK, ini_int("Display", "VSync", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 42;

    label(L"Volume", y);
    s_vol = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 204, y, 236, 30, ID_VOL);
    SendMessageW(s_vol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(s_vol, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(s_vol, TBM_SETPOS, TRUE, ini_int("Audio", "Volume", 50));
    s_voltxt = mk(L"STATIC", L"", SS_CENTERIMAGE, 446, y, 40, 30, ID_VOLTXT);
    update_volume_text();
    y += 48;

    s_show = mk(WC_BUTTONW, L"Show this window every time the game starts",
                BS_AUTOCHECKBOX | WS_TABSTOP, 24, y, 460, 28, ID_SHOW);
    SendMessageW(s_show, BM_SETCHECK, ini_int("Launcher", "ShowOnStart", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 30;
    mk(L"STATIC", L"Hold Shift while starting the game to see it again. "
                  L"Press F1 (or Back + Start) in the game for more settings.",
       0, 24, y, 460, 40, 0);
    y += 50;

    mk(WC_BUTTONW, L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, 264, y, 104, 36, ID_PLAY);
    mk(WC_BUTTONW, L"Quit", WS_TABSTOP, 380, y, 104, 36, ID_QUIT);
    update_enabled();
}

static void save(void)
{
    ini_put("Display", "Resolution", k_res[SendMessageW(s_res, CB_GETCURSEL, 0, 0)]);
    ini_put("Display", "Fullscreen", SendMessageW(s_mode, CB_GETCURSEL, 0, 0) == 0);
    ini_put("Display", "WindowHeight", k_winh[SendMessageW(s_winh, CB_GETCURSEL, 0, 0)]);
    ini_put("Display", "Widescreen", SendMessageW(s_wide, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ini_put("Display", "VSync", SendMessageW(s_vsync, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ini_put("Audio", "Volume", (int)SendMessageW(s_vol, TBM_GETPOS, 0, 0));
    ini_put("Launcher", "ShowOnStart", SendMessageW(s_show, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

/* ── controller ──────────────────────────────────────────────────────── */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } PadState;
typedef struct { DWORD packet; PadState g; } PadInput;
typedef DWORD (WINAPI *PfnGetState)(DWORD, PadInput *);
static PfnGetState s_get_state;
static WORD s_prev;
static DWORD s_repeat_at;

#define PAD_UP 0x0001
#define PAD_DOWN 0x0002
#define PAD_LEFT 0x0004
#define PAD_RIGHT 0x0008
#define PAD_START 0x0010
#define PAD_A 0x1000

static void nudge(HWND c, int dir)
{
    wchar_t cls[32];
    GetClassNameW(c, cls, 32);
    if (!lstrcmpiW(cls, WC_COMBOBOXW)) {
        int n = (int)SendMessageW(c, CB_GETCOUNT, 0, 0);
        int i = (int)SendMessageW(c, CB_GETCURSEL, 0, 0) + dir;
        if (i >= 0 && i < n) {
            SendMessageW(c, CB_SETCURSEL, i, 0);
            SendMessageW(s_wnd, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(c), CBN_SELCHANGE), (LPARAM)c);
        }
    } else if (!lstrcmpiW(cls, TRACKBAR_CLASSW)) {
        int p = (int)SendMessageW(c, TBM_GETPOS, 0, 0) + dir * 5;
        SendMessageW(c, TBM_SETPOS, TRUE, p < 0 ? 0 : p > 100 ? 100 : p);
        update_volume_text();
    } else if (!lstrcmpiW(cls, WC_BUTTONW) && (GetWindowLongW(c, GWL_STYLE) & 0xF) == BS_AUTOCHECKBOX) {
        SendMessageW(c, BM_CLICK, 0, 0);
    }
}

static void poll_pad(void)
{
    PadInput in;
    WORD b = 0, press;
    DWORD now = GetTickCount();
    int i;
    if (!s_get_state) return;
    for (i = 0; i < 4; i++) {
        if (s_get_state((DWORD)i, &in) == 0) {
            b |= in.g.buttons;
            if (in.g.ly > 16000) b |= PAD_UP;
            if (in.g.ly < -16000) b |= PAD_DOWN;
            if (in.g.lx < -16000) b |= PAD_LEFT;
            if (in.g.lx > 16000) b |= PAD_RIGHT;
        }
    }
    press = (WORD)(b & ~s_prev);
    if (b & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) {
        if (press) s_repeat_at = now + 400;
        else if ((LONG)(now - s_repeat_at) >= 0) { press |= (WORD)(b & 0xF); s_repeat_at = now + 120; }
    }
    s_prev = b;
    if (!press) return;
    if (GetForegroundWindow() != s_wnd) return;
    SendMessageW(s_wnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    if (press & PAD_UP)    SendMessageW(s_wnd, WM_NEXTDLGCTL, 1, FALSE);
    if (press & PAD_DOWN)  SendMessageW(s_wnd, WM_NEXTDLGCTL, 0, FALSE);
    if (press & PAD_LEFT)  nudge(GetFocus(), -1);
    if (press & PAD_RIGHT) nudge(GetFocus(), 1);
    if (press & PAD_A) {
        HWND f = GetFocus();
        wchar_t cls[32];
        GetClassNameW(f, cls, 32);
        if (!lstrcmpiW(cls, WC_BUTTONW)) SendMessageW(f, BM_CLICK, 0, 0);
        else if (!lstrcmpiW(cls, WC_COMBOBOXW)) nudge(f, 1);
    }
    if (press & PAD_START) PostMessageW(s_wnd, WM_COMMAND, ID_PLAY, 0);
}

/* ── window ──────────────────────────────────────────────────────────── */
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_PLAY: case IDOK: save(); s_result = 1; DestroyWindow(h); return 0;
        case ID_QUIT: case IDCANCEL: s_result = 0; DestroyWindow(h); return 0;
        case ID_MODE: if (HIWORD(w) == CBN_SELCHANGE) update_enabled(); return 0;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)l == s_vol) update_volume_text();
        return 0;
    case WM_TIMER:
        poll_pad();
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        s_result = 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Common controls 6 (themed controls) without a manifest in the exe: borrow
 * the one shell32.dll carries. */
static HANDLE themed_controls(ULONG_PTR *cookie)
{
    ACTCTXW ac;
    HANDLE ctx;
    memset(&ac, 0, sizeof ac);
    ac.cbSize = sizeof ac;
    ac.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
    ac.lpSource = L"shell32.dll";
    ac.lpResourceName = MAKEINTRESOURCEW(124);
    ctx = CreateActCtxW(&ac);
    if (ctx == INVALID_HANDLE_VALUE) return NULL;
    if (!ActivateActCtx(ctx, cookie)) { ReleaseActCtx(ctx); return NULL; }
    return ctx;
}

int launcher_maybe_run(const char *ini_path)
{
    typedef void *(WINAPI *PfnSetCtx)(void *);
    typedef UINT (WINAPI *PfnDpiWnd)(HWND);
    HMODULE user = GetModuleHandleW(L"user32.dll"), xi;
    PfnSetCtx set_ctx = (PfnSetCtx)(void *)GetProcAddress(user, "SetThreadDpiAwarenessContext");
    PfnDpiWnd dpi_wnd = (PfnDpiWnd)(void *)GetProcAddress(user, "GetDpiForWindow");
    void *old_ctx = NULL;
    ULONG_PTR cookie = 0;
    HANDLE actx;
    WNDCLASSW wc;
    RECT r;
    MSG msg;
    NONCLIENTMETRICSW ncm;
    INITCOMMONCONTROLSEX icc;
    int show, shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const char *force = getenv("RECOMP_LAUNCHER");
    DEVMODEW dm;

    s_ini = ini_path;
    show = ini_int("Launcher", "ShowOnStart", -1);       /* -1: never chosen = first start */
    if (force && *force == '0') return 1;
    if (!(force && *force == '1')) {
        if (!recomp_diag_quiet()) return 1;               /* a script is running the game */
        if (show == 0 && !shift) return 1;
    }

    /* Crisp on high-DPI screens, for this window only (the game's own
     * window keeps the process's setting). */
    if (set_ctx) old_ctx = set_ctx((void *)(INT_PTR)-4);  /* PER_MONITOR_AWARE_V2 */
    actx = themed_controls(&cookie);
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    s_screen_h = EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmPelsHeight
                                                                       : GetSystemMetrics(SM_CYSCREEN);
    xi = LoadLibraryW(L"xinput1_4.dll");
    if (!xi) xi = LoadLibraryW(L"xinput1_3.dll");
    s_get_state = xi ? (PfnGetState)(void *)GetProcAddress(xi, "XInputGetState") : NULL;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"OrtaLauncher";
    RegisterClassW(&wc);
    s_wnd = CreateWindowExW(0, L"OrtaLauncher", L"Panzer Dragoon Orta",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, wc.hInstance, NULL);
    if (!s_wnd) {
        if (set_ctx) set_ctx(old_ctx);
        return 1;
    }
    s_dpi = dpi_wnd ? (int)dpi_wnd(s_wnd) : 96;
    if (s_dpi < 96) s_dpi = 96;

    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    ncm.lfMessageFont.lfHeight = -S(15);
    s_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -S(24);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    s_title_font = CreateFontIndirectW(&ncm.lfMessageFont);

    build();
    r.left = 0; r.top = 0; r.right = S(508); r.bottom = S(500);
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    {   /* centre on the screen */
        int w = r.right - r.left, hgt = r.bottom - r.top;
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        SetWindowPos(s_wnd, NULL, wa.left + (wa.right - wa.left - w) / 2,
                     wa.top + (wa.bottom - wa.top - hgt) / 2, w, hgt, SWP_NOZORDER);
    }
    ShowWindow(s_wnd, SW_SHOW);
    SetForegroundWindow(s_wnd);
    SetFocus(GetDlgItem(s_wnd, ID_PLAY));
    SetTimer(s_wnd, ID_TIMER, 16, NULL);

    s_result = 0;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(s_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(s_font);
    DeleteObject(s_title_font);
    UnregisterClassW(L"OrtaLauncher", wc.hInstance);
    if (actx) { DeactivateActCtx(0, cookie); ReleaseActCtx(actx); }
    if (set_ctx) set_ctx(old_ctx);
    /* Wait for the buttons to be let go, so the A or Start that pressed Play
     * does not reach the game's first screen. */
    if (s_get_state) {
        int i;
        for (i = 0; i < 50; i++) {
            PadInput in;
            WORD b = 0;
            int k;
            for (k = 0; k < 4; k++)
                if (s_get_state((DWORD)k, &in) == 0) b |= in.g.buttons;
            if (!b) break;
            Sleep(20);
        }
    }
    return s_result;
}
