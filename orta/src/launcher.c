/* launcher.c -- the settings window shown before the game starts.
 *
 * Shown the first time the game is started (settings.ini has no
 * [Launcher] ShowOnStart yet), every time if the player ticks "Show this
 * every time", and whenever Shift is held while the game starts. Scripted
 * runs (stderr captured, see recomp_diag_quiet) never see it.
 *
 * Plain Win32 and GDI so it is up before Direct3D, drawn in the style of the
 * game's own option menus. Mouse, keyboard, or a controller: D-pad/stick
 * moves, left/right changes, LB/RB switch pages, A selects, Start plays.
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
    ID_RES = 100, ID_MODE, ID_WINH, ID_WIDE, ID_VSYNC, ID_AA, ID_VOL, ID_VOLTXT,
    ID_MUS, ID_MUSTXT, ID_SFX, ID_SFXTXT, ID_SPK,
    ID_SHOW, ID_PLAY, ID_QUIT, ID_TIMER = 1
};

static const int k_res_h[] = { 480, 720, 1080, 1440, 2160, 0 };   /* 0: Native */
static const char *k_res_v[] = { "480", "720", "1080", "1440", "2160", "Native" };
static const char *k_res_n[] = { "original, fastest", "720p", "1080p", "1440p", "4K", "Native" };
static const int k_winh[] = { 540, 720, 900, 1080, 1440 };

/* The primary screen in real pixels. Asked once: the F1 menu labels ask
 * every frame, and EnumDisplaySettings is slow. */
void orta_screen_size(int *w, int *h)
{
    static int sw, sh;
    if (!sh) {
        DEVMODEW dm;
        memset(&dm, 0, sizeof dm);
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsHeight) {
            sw = (int)dm.dmPelsWidth;
            sh = (int)dm.dmPelsHeight;
        } else {
            sw = GetSystemMetrics(SM_CXSCREEN);
            sh = GetSystemMetrics(SM_CYSCREEN);
        }
    }
    *w = sw;
    *h = sh;
}

int orta_res_index(const char *v)
{
    int i, h, best = 0;
    while (v && (*v == ' ' || *v == '\t')) v++;
    if (!v || !*v) return 0;
    if (!_strnicmp(v, "native", 6)) return 5;
    h = atoi(v);
    for (i = 0; i < 5; i++)
        if (abs(k_res_h[i] - h) < abs(k_res_h[best] - h)) best = i;
    return best;
}

const char *orta_res_value(int i)
{
    return k_res_v[i < 0 || i >= ORTA_RES_COUNT ? 0 : i];
}

int orta_res_height(int i)
{
    int w, h;
    if (i < 0 || i >= ORTA_RES_COUNT) i = 0;
    if (k_res_h[i]) return k_res_h[i];
    orta_screen_size(&w, &h);
    return h > 0 ? h : 1080;
}

int orta_res_scale(const char *v)
{
    int h, k;
    while (v && (*v == ' ' || *v == '\t')) v++;
    if (v && !_strnicmp(v, "native", 6)) h = orta_res_height(5);
    else h = v ? atoi(v) : 480;          /* old values (960, 2400...) keep working */
    if (h <= 0) h = 480;
    k = (h + 479) / 480;
    return k < 1 ? 1 : k > 6 ? 6 : k;
}

void orta_res_label(int i, int wide, char *out, int cap)
{
    int w, h;
    if (i < 0 || i >= ORTA_RES_COUNT) i = 0;
    if (!k_res_h[i]) {
        orta_screen_size(&w, &h);
        snprintf(out, cap, "Native (%d x %d)", w, h);
        return;
    }
    h = k_res_h[i];
    w = wide ? (h * 16 + 8) / 9 : h * 4 / 3;
    snprintf(out, cap, "%d x %d (%s)", w, h, k_res_n[i]);
}


/* ══════════════════════════════════════════════════════════════════════
 * The window. Drawn by hand in the style of the game's own option menus:
 * a silver panel of rows (name on the left, value in a box on the right),
 * the chosen row lit cyan with arrows either side of its value, a green
 * mark beside anything changed from the default, and a black box below
 * that explains the chosen row. Pages are switched with LB/RB, Q/E or the
 * tabs; Up/Down choose, Left/Right change, Enter/A plays.
 *
 * Shapes are drawn at twice the size and averaged down (GDI has no
 * anti-aliasing of its own); text is drawn after, at full sharpness.
 * Fonts: OrtaDisplay.ttf (from Orbitron) and OrtaText.ttf (from Exo 2), both
 * SIL Open Font License (see FONTS_LICENSE.txt), shipped next to the
 * exe; without them the system's fonts are used.
 * ══════════════════════════════════════════════════════════════════════ */

static const char *s_ini;
static HWND s_wnd;
static int s_dpi = 96, s_result;
static int s_screen_w, s_screen_h;
static DWORD s_t0;

/* ── settings ─────────────────────────────────────────────────────────── */
enum { V_RES, V_MODE, V_WINH, V_WIDE, V_VSYNC, V_AA, V_VOL, V_MUS, V_SFX, V_SPK, V_SHOW, V_COUNT };
static int s_v[V_COUNT];
static const int k_def[V_COUNT] = { 0, 0, 1, 1, 0, 0, 50, 70, 70, 0, 0 };

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

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void load(void)
{
    char v[32];
    GetPrivateProfileStringA("Display", "Resolution", "480", v, sizeof v, s_ini);
    s_v[V_RES] = orta_res_index(v);
    s_v[V_MODE] = ini_int("Display", "Fullscreen", 1) ? 0 : 1;
    s_v[V_WINH] = nearest(k_winh, 5, ini_int("Display", "WindowHeight", 720));
    s_v[V_WIDE] = ini_int("Display", "Widescreen", 1) != 0;
    s_v[V_VSYNC] = ini_int("Display", "VSync", 0) != 0;
    s_v[V_AA] = clampi(ini_int("Display", "AntiAliasing", 0), 0, 2);
    s_v[V_VOL] = clampi(ini_int("Audio", "Volume", 50), 0, 100);
    s_v[V_MUS] = clampi(ini_int("Audio", "MusicVolume", 70), 0, 200);
    s_v[V_SFX] = clampi(ini_int("Audio", "SfxVolume", 70), 0, 200);
    GetPrivateProfileStringA("Audio", "Speakers", "Stereo", v, sizeof v, s_ini);
    s_v[V_SPK] = (v[0] == '5' || v[0] == '6' || !_strnicmp(v, "surround", 8)) ? 1 : 0;
    s_v[V_SHOW] = ini_int("Launcher", "ShowOnStart", 0) > 0;
}

static void save(void)
{
    WritePrivateProfileStringA("Display", "Resolution", orta_res_value(s_v[V_RES]), s_ini);
    ini_put("Display", "Fullscreen", s_v[V_MODE] == 0);
    ini_put("Display", "WindowHeight", k_winh[s_v[V_WINH]]);
    ini_put("Display", "Widescreen", s_v[V_WIDE]);
    ini_put("Display", "VSync", s_v[V_VSYNC]);
    ini_put("Display", "AntiAliasing", s_v[V_AA]);
    ini_put("Audio", "Volume", s_v[V_VOL]);
    ini_put("Audio", "MusicVolume", s_v[V_MUS]);
    ini_put("Audio", "SfxVolume", s_v[V_SFX]);
    WritePrivateProfileStringA("Audio", "Speakers", s_v[V_SPK] ? "5.1" : "Stereo", s_ini);
    ini_put("Launcher", "ShowOnStart", s_v[V_SHOW]);
}

/* ── rows and pages ───────────────────────────────────────────────────── */
enum { R_SET, R_PLAY, R_EXIT };
typedef struct {
    int kind, var, min, max, step;
    const char *name, *help;
} Row;

static const Row k_video[] = {
    { R_SET, V_RES, 0, ORTA_RES_COUNT - 1, 1, "RESOLUTION",
      "The size the 3D scene is drawn at. 480 is the Xbox's own and the fastest; higher sizes "
      "are sharper but need a stronger graphics card. Native draws at your screen's own size." },
    { R_SET, V_MODE, 0, 1, 1, "DISPLAY",
      "Fullscreen fills the screen. Window opens a window you can move and resize. "
      "Alt+Enter switches between them in the game." },
    { R_SET, V_WINH, 0, 4, 1, "WINDOW SIZE",
      "How big the window opens, when Display is Window." },
    { R_SET, V_WIDE, 0, 1, 1, "WIDESCREEN",
      "On: the game's own 16:9 mode, as on an Xbox set to widescreen. Off: the original 4:3 picture." },
    { R_SET, V_VSYNC, 0, 1, 1, "VSYNC",
      "On: no tearing, with a little more input lag. Off: the lowest lag." },
    { R_SET, V_AA, 0, 2, 1, "ANTI-ALIASING",
      "Smooths jagged edges. Fast costs almost nothing; High quality smooths more and costs a "
      "little more on slower graphics." },
    { R_PLAY, -1, 0, 0, 0, "PLAY", "Save these settings and start the game." },
    { R_EXIT, -1, 0, 0, 0, "EXIT", "Close without starting the game. Nothing is saved." },
};
static const Row k_sound[] = {
    { R_SET, V_VOL, 0, 100, 5, "VOLUME", "The overall volume." },
    { R_SET, V_MUS, 0, 200, 5, "MUSIC",
      "The music's volume. 70 is the original mix; above 100 boosts it." },
    { R_SET, V_SFX, 0, 200, 5, "SOUND EFFECTS",
      "Everything that is not music: shots, explosions, voices. 70 is the original mix; above 100 boosts it." },
    { R_SET, V_SPK, 0, 1, 1, "SPEAKERS",
      "Stereo for headphones or two speakers. 5.1 places sounds around the room, as an Xbox set "
      "to Dolby Digital does (needs Windows set to 5.1)." },
    { R_PLAY, -1, 0, 0, 0, "PLAY", "Save these settings and start the game." },
    { R_EXIT, -1, 0, 0, 0, "EXIT", "Close without starting the game. Nothing is saved." },
};
static const Row k_start[] = {
    { R_SET, V_SHOW, 0, 1, 1, "SHOW AT START",
      "On: this screen opens every time the game starts. Off: it opens only when Shift is held "
      "while the game starts. In the game, F1 (or Back + Start) opens the settings menu." },
    { R_PLAY, -1, 0, 0, 0, "PLAY", "Save these settings and start the game." },
    { R_EXIT, -1, 0, 0, 0, "EXIT", "Close without starting the game. Nothing is saved." },
};
typedef struct { const char *name; const Row *rows; int n; } Page;
static const Page k_pages[] = {
    { "VIDEO", k_video, (int)(sizeof k_video / sizeof k_video[0]) },
    { "SOUND", k_sound, (int)(sizeof k_sound / sizeof k_sound[0]) },
    { "LAUNCHER", k_start, (int)(sizeof k_start / sizeof k_start[0]) },
};
enum { N_PAGES = (int)(sizeof k_pages / sizeof k_pages[0]), MAX_ROWS = 8 };
static int s_page, s_sel;

static int row_enabled(const Row *r)
{
    return !(r->kind == R_SET && r->var == V_WINH && s_v[V_MODE] == 0);
}

static void value_text(int var, int v, char *out, int cap)
{
    static const char *const onoff[2] = { "OFF", "ON" };
    switch (var) {
    case V_RES:
        if (v == ORTA_RES_COUNT - 1) snprintf(out, cap, "NATIVE  %d x %d", s_screen_w, s_screen_h);
        else {
            int h = k_res_h[v], w = s_v[V_WIDE] ? (h * 16 + 8) / 9 : h * 4 / 3;
            snprintf(out, cap, "%d x %d%s", w, h, v == 0 ? "  ORIGINAL" : "");
        }
        break;
    case V_MODE: snprintf(out, cap, "%s", v ? "WINDOW" : "FULLSCREEN"); break;
    case V_WINH: {
        int h = k_winh[v];
        snprintf(out, cap, "%d x %d", s_v[V_WIDE] ? (h * 16 + 8) / 9 : h * 4 / 3, h);
        break;
    }
    case V_AA: snprintf(out, cap, "%s", v == 0 ? "OFF" : v == 1 ? "FAST" : "HIGH QUALITY"); break;
    case V_SPK: snprintf(out, cap, "%s", v ? "5.1 SURROUND" : "STEREO"); break;
    case V_VOL: case V_MUS: case V_SFX: snprintf(out, cap, "%d", v); break;
    default: snprintf(out, cap, "%s", onoff[v != 0]); break;
    }
}

static int is_level(int var) { return var == V_VOL || var == V_MUS || var == V_SFX; }

/* ── drawing helpers ──────────────────────────────────────────────────── */
/* Layout is in 96-dpi units; g_k turns them into pixels of the surface
 * being drawn (twice as many for the shape pass). */
static float g_k = 1.0f;
static int P(float v) { return (int)(v * g_k + 0.5f); }

enum { W_LOG = 900, H_LOG = 600 };
/* Panel and rows */
#define PX 28.0f
#define PY 96.0f
#define PW 612.0f
#define ROW_Y0 (PY + 16.0f)
#define ROW_H 34.0f
#define LBL_X (PX + 30.0f)
#define BOX_X (PX + 316.0f)
#define BOX_W 262.0f
#define BOX_H 26.0f
#define PANEL_H (16.0f + MAX_ROWS * ROW_H + 14.0f)
#define INFO_Y (PY + PANEL_H + 18.0f)
#define BOX2_Y (INFO_Y + 38.0f)
#define BOX2_H (H_LOG - BOX2_Y - 22.0f)

static HFONT s_f_label, s_f_value, s_f_tab, s_f_title, s_f_help, s_f_small;

static void fill_rect(HDC dc, float x, float y, float w, float h, COLORREF c)
{
    RECT r = { P(x), P(y), P(x + w), P(y + h) };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static COLORREF mix(COLORREF a, COLORREF b, float t)
{
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

/* A vertical gradient inside a polygon (the clip). */
static void grad_poly(HDC dc, const POINT *pt, int n, COLORREF top, COLORREF bot)
{
    HRGN rg = CreatePolygonRgn(pt, n, WINDING);
    RECT bb;
    int y;
    GetRgnBox(rg, &bb);
    SelectClipRgn(dc, rg);
    for (y = bb.top; y < bb.bottom; y++) {
        RECT r = { bb.left, y, bb.right, y + 1 };
        HBRUSH b = CreateSolidBrush(mix(top, bot, (float)(y - bb.top) / (float)(bb.bottom - bb.top)));
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(rg);
}

static void outline_poly(HDC dc, const POINT *pt, int n, COLORREF c, float width)
{
    HPEN pen = CreatePen(PS_SOLID, P(width) < 1 ? 1 : P(width), c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Polygon(dc, pt, n);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

/* A box with its corners cut at 45 degrees. */
static int chamfer(POINT *pt, float x, float y, float w, float h, float c)
{
    pt[0].x = P(x + c);     pt[0].y = P(y);
    pt[1].x = P(x + w - c); pt[1].y = P(y);
    pt[2].x = P(x + w);     pt[2].y = P(y + c);
    pt[3].x = P(x + w);     pt[3].y = P(y + h - c);
    pt[4].x = P(x + w - c); pt[4].y = P(y + h);
    pt[5].x = P(x + c);     pt[5].y = P(y + h);
    pt[6].x = P(x);         pt[6].y = P(y + h - c);
    pt[7].x = P(x);         pt[7].y = P(y + c);
    return 8;
}

static void disc(HDC dc, float cx, float cy, float r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, P(cx - r), P(cy - r), P(cx + r) + 1, P(cy + r) + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

static void ring(HDC dc, float cx, float cy, float r, COLORREF c, float width)
{
    HPEN pen = CreatePen(PS_SOLID, P(width) < 1 ? 1 : P(width), c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, P(cx - r), P(cy - r), P(cx + r), P(cy + r));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

static void tri(HDC dc, float x0, float y0, float x1, float y1, float x2, float y2, COLORREF c)
{
    POINT pt[3] = { { P(x0), P(y0) }, { P(x1), P(y1) }, { P(x2), P(y2) } };
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, pt, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

static void curve(HDC dc, const float *xy, COLORREF c, float width)
{
    POINT pt[4];
    HPEN pen = CreatePen(PS_SOLID, P(width) < 1 ? 1 : P(width), c);
    HGDIOBJ op = SelectObject(dc, pen);
    int i;
    for (i = 0; i < 4; i++) { pt[i].x = P(xy[i * 2]); pt[i].y = P(xy[i * 2 + 1]); }
    PolyBezier(dc, pt, 4);
    SelectObject(dc, op);
    DeleteObject(pen);
}

/* ── colours ──────────────────────────────────────────────────────────── */
#define COL_BG        RGB(16, 18, 20)
#define COL_LINE      RGB(44, 48, 52)
#define COL_LINE2     RGB(30, 33, 36)
#define COL_CYAN      RGB(64, 208, 220)
#define COL_CYAN_HI   RGB(150, 236, 240)
#define COL_CYAN_LO   RGB(36, 160, 172)
#define COL_PANEL_TOP RGB(236, 238, 240)
#define COL_PANEL_BOT RGB(150, 155, 160)
#define COL_BOX_TOP   RGB(250, 251, 252)
#define COL_BOX_BOT   RGB(196, 200, 205)
#define COL_BOX_EDGE  RGB(128, 134, 140)
#define COL_TEXT      RGB(104, 110, 116)
#define COL_TEXT_DIS  RGB(170, 174, 178)
#define COL_GREEN     RGB(70, 255, 100)
#define COL_GREEN_LO  RGB(24, 120, 48)

/* Rows' places, for the mouse. */
static RECT s_row_rc[MAX_ROWS], s_box_rc[MAX_ROWS], s_tab_rc[N_PAGES], s_next_rc;

/* ── the shape pass (drawn at twice the size) ─────────────────────────── */
static void draw_shapes(HDC dc, int w, int h)
{
    const Page *pg = &k_pages[s_page];
    POINT pt[8];
    RECT all = { 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(COL_BG);
    int i, n;

    FillRect(dc, &all, bg);
    DeleteObject(bg);

    {   /* line work on the right */
        static const float c[][8] = {
            { 900, 40, 760, 90, 820, 230, 700, 330 },
            { 900, 120, 800, 170, 860, 300, 740, 420 },
            { 900, 210, 830, 260, 900, 380, 780, 520 },
            { 690, 0, 720, 70, 650, 120, 700, 200 },
            { 760, 600, 720, 520, 820, 470, 760, 380 },
            { 860, 600, 800, 540, 900, 470, 840, 360 },
        };
        for (i = 0; i < (int)(sizeof c / sizeof c[0]); i++) {
            curve(dc, c[i], COL_LINE, 4.0f);
            curve(dc, c[i], COL_LINE2, 1.0f);
        }
        fill_rect(dc, 812, 140, 3, 10, COL_CYAN_LO);
        fill_rect(dc, 840, 300, 3, 12, COL_CYAN_LO);
        fill_rect(dc, 772, 470, 3, 9, COL_CYAN_LO);
        fill_rect(dc, 878, 230, 3, 8, COL_CYAN_LO);
    }

    /* page tabs */
    for (i = 0; i < N_PAGES; i++) {
        float x = PX + i * 150.0f, y = 52.0f;
        POINT tp[4] = { { P(x), P(y + 30) }, { P(x + 10), P(y) },
                        { P(x + 140), P(y) }, { P(x + 130), P(y + 30) } };
        grad_poly(dc, tp, 4, i == s_page ? RGB(46, 52, 56) : RGB(28, 31, 34),
                  i == s_page ? RGB(26, 30, 33) : RGB(22, 24, 27));
        if (i == s_page) fill_rect(dc, x + 6, y + 28, 128, 2, COL_CYAN);
    }

    /* the panel: silver, bottom-right corner cut away */
    {
        float x = PX, y = PY, pw = PW, ph = PANEL_H, c = 26.0f;
        POINT pp[6] = { { P(x + 8), P(y) }, { P(x + pw), P(y) }, { P(x + pw), P(y + ph - c) },
                        { P(x + pw - c), P(y + ph) }, { P(x), P(y + ph) }, { P(x), P(y + 8) } };
        HRGN rg;
        grad_poly(dc, pp, 6, COL_PANEL_TOP, COL_PANEL_BOT);
        rg = CreatePolygonRgn(pp, 6, WINDING);
        SelectClipRgn(dc, rg);
        for (i = 0; i < 6; i++)
            ring(dc, x + 120, y + ph * 0.55f, 60.0f + i * 34.0f, RGB(222, 225, 228), 1.5f);
        SelectClipRgn(dc, NULL);
        DeleteObject(rg);
        outline_poly(dc, pp, 6, RGB(250, 251, 252), 1.0f);
        /* the tab on the right edge: next page */
        {
            float tx = x + pw, ty = y + 44;
            POINT rp[5] = { { P(tx), P(ty) }, { P(tx + 18), P(ty + 12) }, { P(tx + 18), P(ty + 40) },
                            { P(tx), P(ty + 52) }, { P(tx), P(ty) } };
            grad_poly(dc, rp, 5, RGB(210, 214, 218), RGB(150, 155, 160));
            tri(dc, tx + 3, ty + 14, tx + 13, ty + 26, tx + 3, ty + 38, RGB(255, 255, 255));
            s_next_rc.left = (LONG)(tx * g_k * 0.5f); s_next_rc.top = (LONG)(ty * g_k * 0.5f);
            s_next_rc.right = (LONG)((tx + 18) * g_k * 0.5f); s_next_rc.bottom = (LONG)((ty + 52) * g_k * 0.5f);
        }
    }

    /* rows */
    n = pg->n;
    for (i = 0; i < n; i++) {
        const Row *r = &pg->rows[i];
        float y = ROW_Y0 + i * ROW_H;
        int sel = i == s_sel, en = row_enabled(r);
        if (r->kind == R_SET) {
            int np = chamfer(pt, BOX_X, y + (ROW_H - BOX_H) / 2, BOX_W, BOX_H, 3.0f);
            if (sel) {
                grad_poly(dc, pt, np, COL_CYAN_HI, COL_CYAN_LO);
                outline_poly(dc, pt, np, RGB(20, 110, 120), 1.0f);
                tri(dc, BOX_X - 6, y + ROW_H / 2, BOX_X + 4, y + ROW_H / 2 - 9, BOX_X + 4, y + ROW_H / 2 + 9, COL_CYAN);
                tri(dc, BOX_X + BOX_W + 6, y + ROW_H / 2, BOX_X + BOX_W - 4, y + ROW_H / 2 - 9,
                    BOX_X + BOX_W - 4, y + ROW_H / 2 + 9, COL_CYAN);
            } else {
                grad_poly(dc, pt, np, COL_BOX_TOP, COL_BOX_BOT);
                outline_poly(dc, pt, np, COL_BOX_EDGE, 1.0f);
            }
            if (is_level(r->var)) {   /* how full, as a thin bar along the box's foot */
                float f = (float)s_v[r->var] / (float)r->max;
                fill_rect(dc, BOX_X + 8, y + (ROW_H + BOX_H) / 2 - 5, (BOX_W - 16) * f, 2,
                          sel ? RGB(10, 70, 80) : COL_CYAN_LO);
            }
        } else if (sel) {
            fill_rect(dc, LBL_X - 10, y + 4, BOX_X + BOX_W - LBL_X + 10, ROW_H - 8, RGB(36, 40, 44));
            fill_rect(dc, LBL_X - 10, y + 4, 4, ROW_H - 8, COL_CYAN);
        }
        if (sel && r->kind == R_SET) {   /* the cyan trail from the name to its value */
            int k;
            for (k = 0; k < 7; k++)
                fill_rect(dc, BOX_X - 70 + k * 8.5f, y + ROW_H / 2 - (k % 3 == 1 ? 4 : 1), 3, 3, COL_CYAN);
            fill_rect(dc, LBL_X - 18, y + ROW_H - 3, BOX_X - LBL_X - 60, 1.5f, COL_CYAN);
        }
        if (r->kind == R_SET && en && s_v[r->var] != k_def[r->var]) {   /* changed: green mark */
            disc(dc, PX + 14, y + ROW_H / 2, 7.0f, COL_GREEN_LO);
            disc(dc, PX + 14, y + ROW_H / 2, 4.0f, COL_GREEN);
        }
    }

    /* the name tab and the black box below */
    {
        POINT tp[4] = { { P(PX), P(INFO_Y) }, { P(PX + 330), P(INFO_Y) },
                        { P(PX + 346), P(INFO_Y + 30) }, { P(PX), P(INFO_Y + 30) } };
        grad_poly(dc, tp, 4, RGB(40, 44, 48), RGB(24, 27, 30));
        n = chamfer(pt, PX, BOX2_Y, W_LOG - 2 * PX, BOX2_H, 12.0f);
        grad_poly(dc, pt, n, RGB(6, 7, 8), RGB(2, 3, 4));
        outline_poly(dc, pt, n, RGB(96, 102, 108), 2.0f);
        n = chamfer(pt, PX + 4, BOX2_Y + 4, W_LOG - 2 * PX - 8, BOX2_H - 8, 10.0f);
        outline_poly(dc, pt, n, RGB(34, 38, 42), 1.0f);
    }
}

/* ── the text pass (full size) ────────────────────────────────────────── */
static void text_at(HDC dc, HFONT f, const char *s, float x, float y, float w, float h,
                    UINT fmt, COLORREF c)
{
    RECT r = { P(x), P(y), P(x + w), P(y + h) };
    wchar_t ws[512];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, ws, 512);
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, ws, -1, &r, fmt | DT_NOPREFIX);
}

/* Engraved: a light edge under dark letters, as on the game's panels. */
static void text_engraved(HDC dc, HFONT f, const char *s, float x, float y, float w, float h,
                          UINT fmt, COLORREF c)
{
    text_at(dc, f, s, x + 1.0f / g_k, y + 1.0f / g_k, w, h, fmt, RGB(250, 251, 252));
    text_at(dc, f, s, x, y, w, h, fmt, c);
}

static void draw_text(HDC dc)
{
    const Page *pg = &k_pages[s_page];
    const Row *cur = &pg->rows[s_sel];
    char b[128];
    int i;
    SetBkMode(dc, TRANSPARENT);

    text_at(dc, s_f_title, "PANZER DRAGOON ORTA", PX + 2, 16, 400, 18, DT_LEFT | DT_SINGLELINE, COL_CYAN);
    snprintf(b, sizeof b, "PC SETTINGS     YOUR SCREEN %d x %d", s_screen_w, s_screen_h);
    text_at(dc, s_f_small, b, PX + 2, 34, 500, 16, DT_LEFT | DT_SINGLELINE, RGB(120, 126, 132));

    for (i = 0; i < N_PAGES; i++) {
        float x = PX + i * 150.0f;
        text_at(dc, s_f_tab, k_pages[i].name, x + 8, 52, 124, 30, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
                i == s_page ? COL_CYAN : RGB(110, 116, 122));
        s_tab_rc[i].left = P(x); s_tab_rc[i].top = P(52);
        s_tab_rc[i].right = P(x + 140); s_tab_rc[i].bottom = P(82);
    }
    text_at(dc, s_f_small, "LB / Q          RB / E", PX + N_PAGES * 150.0f + 6, 52, 220, 30,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, RGB(90, 96, 102));

    for (i = 0; i < pg->n; i++) {
        const Row *r = &pg->rows[i];
        float y = ROW_Y0 + i * ROW_H;
        int sel = i == s_sel, en = row_enabled(r);
        s_row_rc[i].left = P(PX); s_row_rc[i].top = P(y);
        s_row_rc[i].right = P(PX + PW); s_row_rc[i].bottom = P(y + ROW_H);
        if (r->kind != R_SET) {
            text_at(dc, s_f_label, r->name, LBL_X, y, 300, ROW_H, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
                    sel ? RGB(255, 255, 255) : COL_TEXT);
            memset(&s_box_rc[i], 0, sizeof s_box_rc[i]);
            continue;
        }
        if (sel) {
            text_at(dc, s_f_label, r->name, LBL_X + 1.0f / g_k, y + 1.0f / g_k, 300, ROW_H,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE, RGB(70, 76, 82));
            text_at(dc, s_f_label, r->name, LBL_X, y, 300, ROW_H, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
                    RGB(255, 255, 255));
        } else
            text_engraved(dc, s_f_label, r->name, LBL_X, y, 300, ROW_H, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
                          en ? COL_TEXT : COL_TEXT_DIS);
        value_text(r->var, s_v[r->var], b, sizeof b);
        if (sel)
            text_at(dc, s_f_value, b, BOX_X, y, BOX_W, ROW_H, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
                    RGB(12, 40, 46));
        else
            text_engraved(dc, s_f_value, b, BOX_X, y, BOX_W, ROW_H, DT_CENTER | DT_VCENTER | DT_SINGLELINE,
                          en ? COL_TEXT : COL_TEXT_DIS);
        s_box_rc[i].left = P(BOX_X - 10); s_box_rc[i].top = P(y);
        s_box_rc[i].right = P(BOX_X + BOX_W + 10); s_box_rc[i].bottom = P(y + ROW_H);
    }

    /* "press start" in the dark, slowly pulsing */
    {
        float t = (float)((GetTickCount() - s_t0) % 2400) / 2400.0f;
        float a = t < 0.5f ? t * 2 : 2 - t * 2;
        text_at(dc, s_f_tab, "PRESS START TO PLAY", PX + 350, INFO_Y, W_LOG - PX * 2 - 350, 30,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE, mix(RGB(40, 44, 48), RGB(96, 104, 110), a));
    }

    text_at(dc, s_f_tab, cur->kind == R_SET ? cur->name : k_pages[s_page].name, PX + 14, INFO_Y, 310, 30,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE, COL_CYAN);
    text_at(dc, s_f_help, cur->help, PX + 26, BOX2_Y + 16, W_LOG - 2 * PX - 52, BOX2_H - 40,
            DT_LEFT | DT_WORDBREAK, RGB(236, 238, 240));
    if (cur->kind == R_SET) {
        char d[96];
        value_text(cur->var, k_def[cur->var], d, sizeof d);
        snprintf(b, sizeof b, "Default: %s", d);
        text_at(dc, s_f_small, b, PX + 26, BOX2_Y + BOX2_H - 30, 400, 18, DT_LEFT | DT_SINGLELINE,
                s_v[cur->var] != k_def[cur->var] ? COL_GREEN : RGB(120, 126, 132));
    }
    text_at(dc, s_f_small, "Up/Down choose   Left/Right change   Enter/A play   Esc quit",
            PX + 300, BOX2_Y + BOX2_H - 30, W_LOG - 2 * PX - 326, 18, DT_RIGHT | DT_SINGLELINE,
            RGB(90, 96, 102));
}

/* ── render: shapes at 2x, averaged down, then the text ───────────────── */
static HBITMAP dib(HDC ref, int w, int h, unsigned **bits)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    return CreateDIBSection(ref, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

static void render(HDC out, int w, int h)
{
    unsigned *hi = NULL, *lo = NULL;
    HDC hd = CreateCompatibleDC(out), ld = CreateCompatibleDC(out);
    HBITMAP hb = dib(out, w * 2, h * 2, &hi), lb = dib(out, w, h, &lo);
    HGDIOBJ oh, ol;
    int x, y;
    if (!hb || !lb) {
        if (hb) DeleteObject(hb);
        if (lb) DeleteObject(lb);
        DeleteDC(hd);
        DeleteDC(ld);
        return;
    }
    oh = SelectObject(hd, hb);
    ol = SelectObject(ld, lb);
    g_k = 2.0f * (float)s_dpi / 96.0f;
    draw_shapes(hd, w * 2, h * 2);
    GdiFlush();
    for (y = 0; y < h; y++) {
        const unsigned *a = hi + (size_t)(y * 2) * (size_t)(w * 2), *b = a + w * 2;
        unsigned *d = lo + (size_t)y * (size_t)w;
        for (x = 0; x < w; x++) {
            unsigned p0 = a[x * 2], p1 = a[x * 2 + 1], p2 = b[x * 2], p3 = b[x * 2 + 1];
            unsigned r = ((p0 >> 16 & 255) + (p1 >> 16 & 255) + (p2 >> 16 & 255) + (p3 >> 16 & 255) + 2) >> 2;
            unsigned g = ((p0 >> 8 & 255) + (p1 >> 8 & 255) + (p2 >> 8 & 255) + (p3 >> 8 & 255) + 2) >> 2;
            unsigned bl = ((p0 & 255) + (p1 & 255) + (p2 & 255) + (p3 & 255) + 2) >> 2;
            d[x] = r << 16 | g << 8 | bl;
        }
    }
    g_k = (float)s_dpi / 96.0f;
    draw_text(ld);
    BitBlt(out, 0, 0, w, h, ld, 0, 0, SRCCOPY);
    SelectObject(hd, oh);
    SelectObject(ld, ol);
    DeleteObject(hb);
    DeleteObject(lb);
    DeleteDC(hd);
    DeleteDC(ld);
}

/* ── behaviour ────────────────────────────────────────────────────────── */
static void redraw(void) { InvalidateRect(s_wnd, NULL, FALSE); }

static void select_row(int i)
{
    const Page *pg = &k_pages[s_page];
    if (i < 0 || i >= pg->n || !row_enabled(&pg->rows[i])) return;
    if (i != s_sel) { s_sel = i; redraw(); }
}

static void move_sel(int dir)
{
    const Page *pg = &k_pages[s_page];
    int i = s_sel;
    do i = (i + dir + pg->n) % pg->n; while (!row_enabled(&pg->rows[i]) && i != s_sel);
    s_sel = i;
    redraw();
}

static void set_page(int p)
{
    s_page = (p + N_PAGES) % N_PAGES;
    s_sel = 0;
    redraw();
}

static void finish(int play)
{
    if (play) save();
    s_result = play;
    DestroyWindow(s_wnd);
}

/* Change the chosen row's value; lists wrap, levels stop at their ends. */
static void change(int dir)
{
    const Row *r = &k_pages[s_page].rows[s_sel];
    int *v;
    if (r->kind != R_SET || !row_enabled(r)) return;
    v = &s_v[r->var];
    if (is_level(r->var)) *v = clampi(*v + dir * r->step, r->min, r->max);
    else *v = *v + dir > r->max ? r->min : *v + dir < r->min ? r->max : *v + dir;
    redraw();
}

static void activate(void)
{
    const Row *r = &k_pages[s_page].rows[s_sel];
    if (r->kind == R_PLAY) finish(1);
    else if (r->kind == R_EXIT) finish(0);
    else if (!is_level(r->var)) change(1);
}

/* ── controller ───────────────────────────────────────────────────────── */
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
#define PAD_LB 0x0100
#define PAD_RB 0x0200
#define PAD_A 0x1000

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
        else if ((LONG)(now - s_repeat_at) >= 0) { press |= (WORD)(b & 0xF); s_repeat_at = now + 90; }
    }
    s_prev = b;
    if (!press || GetForegroundWindow() != s_wnd) return;
    if (press & PAD_UP) move_sel(-1);
    if (press & PAD_DOWN) move_sel(1);
    if (press & PAD_LEFT) change(-1);
    if (press & PAD_RIGHT) change(1);
    if (press & PAD_LB) set_page(s_page - 1);
    if (press & PAD_RB) set_page(s_page + 1);
    if (press & PAD_A) activate();
    else if (press & PAD_START) finish(1);
}

/* ── window ───────────────────────────────────────────────────────────── */
static int hit_row(int x, int y, int *in_box)
{
    POINT p = { x, y };
    int i;
    for (i = 0; i < k_pages[s_page].n; i++)
        if (PtInRect(&s_row_rc[i], p)) {
            *in_box = PtInRect(&s_box_rc[i], p);
            return i;
        }
    return -1;
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        render(dc, rc.right, rc.bottom);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        switch (w) {
        case VK_UP: move_sel(-1); break;
        case VK_DOWN: move_sel(1); break;
        case VK_LEFT: change(-1); break;
        case VK_RIGHT: change(1); break;
        case VK_RETURN: case VK_SPACE: activate(); break;
        case VK_ESCAPE: finish(0); break;
        case VK_PRIOR: case 'Q': set_page(s_page - 1); break;
        case VK_NEXT: case 'E': case VK_TAB:
            set_page(s_page + ((GetKeyState(VK_SHIFT) & 0x8000) && w == VK_TAB ? -1 : 1));
            break;
        }
        return 0;
    case WM_MOUSEMOVE: {
        int box, i = hit_row((short)LOWORD(l), (short)HIWORD(l), &box);
        if (i >= 0) select_row(i);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        int box, i;
        for (i = 0; i < N_PAGES; i++)
            if (PtInRect(&s_tab_rc[i], p)) { set_page(i); return 0; }
        if (PtInRect(&s_next_rc, p)) { set_page(s_page + 1); return 0; }
        i = hit_row(p.x, p.y, &box);
        if (i < 0) return 0;
        select_row(i);
        if (s_sel != i) return 0;
        if (k_pages[s_page].rows[i].kind != R_SET) activate();
        else if (box) change(p.x < (s_box_rc[i].left + s_box_rc[i].right) / 2 ? -1 : 1);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        int box, i;
        ScreenToClient(h, &p);
        i = hit_row(p.x, p.y, &box);
        if (i >= 0 && box && i == s_sel) change((short)HIWORD(w) > 0 ? 1 : -1);
        else move_sel((short)HIWORD(w) > 0 ? -1 : 1);
        return 0;
    }
    case WM_TIMER:
        poll_pad();
        if (IsWindow(s_wnd)) {   /* the pulsing "press start" */
            RECT r = { 0, (LONG)(INFO_Y * s_dpi / 96), (LONG)(W_LOG * s_dpi / 96),
                       (LONG)((INFO_Y + 30) * s_dpi / 96) };
            static DWORD last;
            DWORD now = GetTickCount();
            if (now - last > 60) { last = now; InvalidateRect(h, &r, FALSE); }
        }
        return 0;
    case WM_CLOSE:
        finish(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* The two fonts from next to the exe, for this process only. */
static int s_fonts_added;
static wchar_t s_font_path[2][MAX_PATH + 32];
static void add_fonts(void)
{
    static const wchar_t *const names[2] = { L"OrtaDisplay.ttf", L"OrtaText.ttf" };
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    int i;
    wchar_t *slash;
    if (!n || n >= MAX_PATH) return;
    slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    slash[1] = 0;
    for (i = 0; i < 2; i++) {
        swprintf(s_font_path[i], MAX_PATH + 32, L"%ls%ls", dir, names[i]);
        if (AddFontResourceExW(s_font_path[i], FR_PRIVATE, NULL) > 0) s_fonts_added |= 1 << i;
    }
}

static HFONT font(const wchar_t *face, const wchar_t *fallback, int have, int px, int weight)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -MulDiv(px, s_dpi, 96);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy(lf.lfFaceName, have ? face : fallback);
    return CreateFontIndirectW(&lf);
}

int launcher_maybe_run(const char *ini_path)
{
    typedef void *(WINAPI *PfnSetCtx)(void *);
    typedef UINT (WINAPI *PfnDpiWnd)(HWND);
    typedef HRESULT (WINAPI *PfnDwmSet)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE user = GetModuleHandleW(L"user32.dll"), xi, dwm;
    PfnSetCtx set_ctx = (PfnSetCtx)(void *)GetProcAddress(user, "SetThreadDpiAwarenessContext");
    PfnDpiWnd dpi_wnd = (PfnDpiWnd)(void *)GetProcAddress(user, "GetDpiForWindow");
    void *old_ctx = NULL;
    WNDCLASSW wc;
    RECT r;
    MSG msg;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int show, shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const char *force = getenv("RECOMP_LAUNCHER");
    int orb, exo;

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

    orta_screen_size(&s_screen_w, &s_screen_h);
    load();
    xi = LoadLibraryW(L"xinput1_4.dll");
    if (!xi) xi = LoadLibraryW(L"xinput1_3.dll");
    s_get_state = xi ? (PfnGetState)(void *)GetProcAddress(xi, "XInputGetState") : NULL;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"OrtaLauncher";
    RegisterClassW(&wc);
    s_wnd = CreateWindowExW(0, L"OrtaLauncher", L"Panzer Dragoon Orta", style,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, NULL, NULL, wc.hInstance, NULL);
    if (!s_wnd) {
        if (set_ctx) set_ctx(old_ctx);
        return 1;
    }
    s_dpi = dpi_wnd ? (int)dpi_wnd(s_wnd) : 96;
    if (s_dpi < 96) s_dpi = 96;
    dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {   /* a dark title bar to match */
        PfnDwmSet set = (PfnDwmSet)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
        BOOL on = TRUE;
        if (set) set(s_wnd, 20, &on, sizeof on);          /* DWMWA_USE_IMMERSIVE_DARK_MODE */
    }

    add_fonts();
    orb = s_fonts_added & 1;
    exo = (s_fonts_added & 2) != 0;
    s_f_label = font(L"Orta Display", L"Arial", orb, 16, orb ? FW_NORMAL : FW_BOLD);
    s_f_value = font(L"Orta Display", L"Arial", orb, 15, orb ? FW_NORMAL : FW_BOLD);
    s_f_tab = font(L"Orta Display", L"Arial", orb, 14, orb ? FW_NORMAL : FW_BOLD);
    s_f_title = font(L"Orta Display", L"Arial", orb, 13, orb ? FW_NORMAL : FW_BOLD);
    s_f_help = font(L"Orta Text", L"Segoe UI", exo, 17, FW_NORMAL);
    s_f_small = font(L"Orta Text", L"Segoe UI", exo, 13, FW_NORMAL);

    r.left = 0; r.top = 0; r.right = MulDiv(W_LOG, s_dpi, 96); r.bottom = MulDiv(H_LOG, s_dpi, 96);
    AdjustWindowRectEx(&r, style, FALSE, 0);
    {   /* centre on the screen */
        int w = r.right - r.left, hgt = r.bottom - r.top;
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        SetWindowPos(s_wnd, NULL, wa.left + (wa.right - wa.left - w) / 2,
                     wa.top + (wa.bottom - wa.top - hgt) / 2, w, hgt, SWP_NOZORDER);
    }
    /* Start on Play, so Enter/A straight away starts the game. */
    s_page = 0;
    s_sel = k_pages[0].n - 2;
    s_t0 = GetTickCount();
    ShowWindow(s_wnd, SW_SHOW);
    SetForegroundWindow(s_wnd);
    SetFocus(s_wnd);
    SetTimer(s_wnd, 1, 16, NULL);

    s_result = 0;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(s_f_label);
    DeleteObject(s_f_value);
    DeleteObject(s_f_tab);
    DeleteObject(s_f_title);
    DeleteObject(s_f_help);
    DeleteObject(s_f_small);
    if (s_fonts_added & 1) RemoveFontResourceExW(s_font_path[0], FR_PRIVATE, NULL);
    if (s_fonts_added & 2) RemoveFontResourceExW(s_font_path[1], FR_PRIVATE, NULL);
    UnregisterClassW(L"OrtaLauncher", wc.hInstance);
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
