/* In-game settings menu: Dear ImGui drawn over the presented frame.
 * See overlay.h for what it does; the notes here are about how.
 *
 * Threads. The window procedure runs on the window's own thread, the
 * present on the GPU executor's. ImGui is not thread-safe, so the window
 * procedure only queues input events (under a lock) and flips the open
 * flag; everything ImGui does happens in ovl_present, on the executor.
 *
 * Pausing. The runtime asks g_present_pause_hook after each flip; while it
 * says yes, the executor keeps redrawing the last frame with the menu on
 * top instead of returning to the game, which then waits for its flip like
 * it would for a slow GPU. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "overlay.h"

/* ── Runtime hooks and live settings (src/kernel, src/video, src/apu) ── */
extern "C" {
extern void (*g_present_overlay_hook)(void *dev, void *ctx, void *rtv, unsigned w, unsigned h);
extern int  (*g_present_pause_hook)(void);
extern volatile long  g_present_vsync;        /* -1: settings.ini */
extern volatile float g_present_crop[2];      /* -1: settings.ini */
extern int  (*g_wndproc_hook)(HWND, UINT, WPARAM, LPARAM);
extern int  xbox_FramebufferIsFullscreen(void);
extern HWND xbox_FramebufferWindowHwnd(void);
extern volatile float g_apu_live_gain;        /* -1: settings.ini */
extern volatile long  g_apu_mute;
/* recomp_manual.c */
extern volatile float g_rumble_scale;         /* -1: settings.ini */
void orta_rumble_stop_now(void);
void orta_rumble_pulse(float strength);
}

#define FB_SET_FULLSCREEN (WM_APP + 7)        /* fb_present.c: wParam = on */

/* ── Settings ────────────────────────────────────────────────────────── */
typedef struct {
    int scale;          /* 1..6: render height = 480 * scale */
    int widescreen;
    int fullscreen;
    int winh;           /* window height when not fullscreen */
    int vsync;
    int fps_cap;
    int volume;         /* 0..100 */
    int sfx;            /* 0..100, effects relative to music */
    int cropx, cropy;   /* 0..20 percent per side */
    int rumble;
    int rumble_str;     /* 0..100 */
} Cfg;

static Cfg s_cfg, s_saved;

static int env_int(const char *name, int def)
{
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

static int env_pct(const char *name, int def)    /* "0.50" -> 50 */
{
    const char *e = getenv(name);
    return e && *e ? (int)floor(atof(e) * 100.0 + 0.5) : def;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void cfg_load(void)
{
    s_cfg.scale      = clampi(env_int("RECOMP_GPU_SCALE", 1), 1, 6);
    s_cfg.widescreen = env_int("RECOMP_WIDESCREEN", 1) != 0;
    s_cfg.fullscreen = xbox_FramebufferIsFullscreen();
    s_cfg.winh       = clampi(env_int("RECOMP_WINDOW_HEIGHT", 720), 240, 4320);
    s_cfg.vsync      = env_int("RECOMP_VSYNC", 0) != 0;
    s_cfg.fps_cap    = env_int("RECOMP_FPS_CAP", 60);
    s_cfg.volume     = clampi(env_pct("RECOMP_APU_GAIN", 50), 0, 100);
    s_cfg.sfx        = clampi(env_pct("RECOMP_APU_SFX", 70), 0, 100);
    s_cfg.cropx      = clampi(env_int("RECOMP_CROP_X", 0), 0, 20);
    s_cfg.cropy      = clampi(env_int("RECOMP_CROP_Y", 0), 0, 20);
    s_cfg.rumble     = env_int("RECOMP_RUMBLE", 1) != 0;
    s_cfg.rumble_str = clampi(env_int("RECOMP_RUMBLE_STRENGTH", 100), 0, 100);
    s_saved = s_cfg;
}

/* The settings that take effect at once. */
static void cfg_apply_live(void)
{
    g_present_vsync = s_cfg.vsync;
    g_present_crop[0] = (float)s_cfg.cropx / 100.0f;
    g_present_crop[1] = (float)s_cfg.cropy / 100.0f;
    g_apu_live_gain = (float)s_cfg.volume / 100.0f * (float)s_cfg.sfx / 100.0f;
    g_rumble_scale = s_cfg.rumble ? (float)s_cfg.rumble_str / 100.0f : 0.0f;
    if (s_cfg.fullscreen != xbox_FramebufferIsFullscreen()) {
        HWND h = xbox_FramebufferWindowHwnd();
        if (h)
            PostMessageA(h, FB_SET_FULLSCREEN, (WPARAM)s_cfg.fullscreen, 0);
    }
}

static void exe_dir(char *dir, size_t n)
{
    DWORD k = GetModuleFileNameA(NULL, dir, (DWORD)n);
    while (k > 0 && dir[k - 1] != '\\' && dir[k - 1] != '/')
        k--;
    dir[k] = 0;
}

/* Write settings.ini in the same layout main.c creates, keeping any
 * [Advanced] section the file already had. */
static void cfg_save(void)
{
    char dir[MAX_PATH], path[MAX_PATH + 32];
    char *adv = NULL;
    FILE *f;
    exe_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%ssettings.ini", dir);

    f = fopen(path, "rb");
    if (f) {                                   /* lift [Advanced] out as it is */
        long n;
        char *all;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        all = (char *)malloc((size_t)n + 1);
        if (all && fread(all, 1, (size_t)n, f) == (size_t)n) {
            char *a;
            all[n] = 0;
            a = strstr(all, "[Advanced]");
            if (a) {
                char *end = strstr(a + 10, "\n[");
                size_t len = end ? (size_t)(end + 1 - a) : strlen(a);
                adv = (char *)malloc(len + 1);
                if (adv) { memcpy(adv, a, len); adv[len] = 0; }
            }
        }
        free(all);
        fclose(f);
    }

    f = fopen(path, "wb");
    if (!f) {
        free(adv);
        return;
    }
    fprintf(f,
        "; Panzer Dragoon Orta - settings. Edit with Notepad, or press F1 in the game.\r\n"
        "\r\n"
        "[Display]\r\n"
        "; Height the game is drawn at, in steps of the original 480 lines:\r\n"
        ";   480, 960, 1440, 1920, 2400 or 2880 (1x-6x). Other values round up\r\n"
        ";   (1080 -> 1440, 2160 -> 2400). The picture is then scaled to fit the\r\n"
        ";   screen: 1440 for a 1080p/1440p monitor, 2160 for 4K.\r\n"
        "Resolution = %d\r\n"
        "; 1 = 16:9 widescreen, 0 = original 4:3.\r\n"
        "Widescreen = %d\r\n"
        "; Trim the black border Xbox games leave for old TVs, in percent per side (0-20).\r\n"
        "CropX = %d\r\n"
        "CropY = %d\r\n"
        "; 1 = fill the screen (borderless). Alt+Enter switches while playing.\r\n"
        "Fullscreen = %d\r\n"
        "; Window height when not fullscreen.\r\n"
        "WindowHeight = %d\r\n"
        "; Frame rate cap (the game is made for 60). 0 = no cap.\r\n"
        "FrameRateCap = %d\r\n"
        "; 1 = wait for the monitor's refresh when showing a frame (no tearing, more lag).\r\n"
        "VSync = %d\r\n"
        "\r\n"
        "[Audio]\r\n"
        "; 0 to 100.\r\n"
        "Volume = %d\r\n"
        "; Sound effects relative to music, 0 to 100.\r\n"
        "SfxVolume = %d\r\n"
        "\r\n"
        "[Controller]\r\n"
        "; 1 = the pad rumbles when the game asks it to.\r\n"
        "Rumble = %d\r\n"
        "; 0 to 100.\r\n"
        "RumbleStrength = %d\r\n",
        s_cfg.scale * 480, s_cfg.widescreen, s_cfg.cropx, s_cfg.cropy,
        s_cfg.fullscreen, s_cfg.winh, s_cfg.fps_cap, s_cfg.vsync,
        s_cfg.volume, s_cfg.sfx, s_cfg.rumble, s_cfg.rumble_str);
    if (adv)
        fprintf(f, "\r\n%s", adv);
    fclose(f);
    free(adv);
    s_saved = s_cfg;
}

/* ── State shared with the window thread ─────────────────────────────── */
static volatile LONG s_open;           /* the menu is up */
static volatile LONG s_req_toggle;     /* F1 pressed: toggle at the next present */
static volatile LONG s_block_release;  /* closed: hold the game's input until released */

typedef struct { int kind; int a; int b; } Ev;  /* kinds below */
enum { EV_MOVE, EV_BUTTON, EV_WHEEL, EV_KEY, EV_CHAR, EV_FOCUS };
#define EVQ 512
static CRITICAL_SECTION s_evlock;
static Ev  s_evq[EVQ];
static int s_evn;

static void ev_push(int kind, int a, int b)
{
    EnterCriticalSection(&s_evlock);
    if (s_evn < EVQ) {
        s_evq[s_evn].kind = kind;
        s_evq[s_evn].a = a;
        s_evq[s_evn].b = b;
        s_evn++;
    }
    LeaveCriticalSection(&s_evlock);
}

static ImGuiKey vk_to_key(WPARAM vk)
{
    switch (vk) {
    case VK_TAB:    return ImGuiKey_Tab;
    case VK_LEFT:   return ImGuiKey_LeftArrow;
    case VK_RIGHT:  return ImGuiKey_RightArrow;
    case VK_UP:     return ImGuiKey_UpArrow;
    case VK_DOWN:   return ImGuiKey_DownArrow;
    case VK_PRIOR:  return ImGuiKey_PageUp;
    case VK_NEXT:   return ImGuiKey_PageDown;
    case VK_HOME:   return ImGuiKey_Home;
    case VK_END:    return ImGuiKey_End;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SPACE:  return ImGuiKey_Space;
    case VK_BACK:   return ImGuiKey_Backspace;
    case VK_DELETE: return ImGuiKey_Delete;
    case 'Q':       return ImGuiKey_Q;
    case 'E':       return ImGuiKey_E;
    case 'R':       return ImGuiKey_R;
    case VK_SHIFT:  return ImGuiMod_Shift;
    case VK_CONTROL: return ImGuiMod_Ctrl;
    default:        return ImGuiKey_None;
    }
}

/* Window thread. Nonzero: the message was the menu's. */
static int ovl_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)h;
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_F1) {
            if (!(l & (1 << 30)))
                InterlockedExchange(&s_req_toggle, 1);
            return 1;
        }
        if (!s_open)
            return 0;
        if (!(l & (1 << 30)) || w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN)
            ev_push(EV_KEY, (int)vk_to_key(w), 1);
        return 1;
    case WM_KEYUP:
        if (!s_open)
            return w == VK_F1;
        ev_push(EV_KEY, (int)vk_to_key(w), 0);
        return 1;
    case WM_CHAR:
        if (!s_open)
            return 0;
        ev_push(EV_CHAR, (int)w, 0);
        return 1;
    case WM_MOUSEMOVE:
        if (!s_open)
            return 0;
        ev_push(EV_MOVE, (short)LOWORD(l), (short)HIWORD(l));
        return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        if (!s_open)
            return 0;
        ev_push(EV_MOVE, (short)LOWORD(l), (short)HIWORD(l));
        ev_push(EV_BUTTON, (m == WM_RBUTTONDOWN || m == WM_RBUTTONUP) ? 1 : 0,
                m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN);
        return 1;
    case WM_MOUSEWHEEL:
        if (!s_open)
            return 0;
        ev_push(EV_WHEEL, GET_WHEEL_DELTA_WPARAM(w), 0);
        return 1;
    case WM_KILLFOCUS:
        if (s_open)
            ev_push(EV_FOCUS, 0, 0);
        return 0;                  /* fb_present.c still clears its keys */
    }
    return 0;
}

/* ── Opening and closing ─────────────────────────────────────────────── */
static int  s_focus_first;          /* put the pad/keyboard cursor on the first row */
static int  s_cat;                  /* category shown */
static ULONGLONG s_pulse_end;       /* "test rumble" running until this tick */

static void ovl_set_open(int on)
{
    if (on == (int)s_open)
        return;
    if (on) {
        cfg_load();
        s_focus_first = 1;
        g_apu_mute = 1;
        orta_rumble_stop_now();
        InterlockedExchange(&s_open, 1);
    } else {
        if (memcmp(&s_cfg, &s_saved, sizeof s_cfg) != 0)
            cfg_save();
        if (s_pulse_end) {
            s_pulse_end = 0;
            orta_rumble_stop_now();
        }
        g_apu_mute = 0;
        InterlockedExchange(&s_block_release, 1);
        InterlockedExchange(&s_open, 0);
    }
}

extern "C" int ovl_is_open(void) { return s_open != 0; }

extern "C" int ovl_block_input(int held)
{
    if (s_open)
        return 1;
    if (s_block_release) {
        if (held)
            return 1;
        InterlockedExchange(&s_block_release, 0);
    }
    return 0;
}

static int ovl_pause(void) { return s_open != 0; }

/* ── Pad ─────────────────────────────────────────────────────────────── */
static XINPUT_STATE s_pad;
static int s_pad_ok;

/* XInputGetState on an empty slot can take milliseconds; with no pad,
 * look again only every two seconds. */
static void pad_poll(void)
{
    static ULONGLONG retry;
    ULONGLONG now = GetTickCount64();
    if (!s_pad_ok && now < retry) {
        memset(&s_pad, 0, sizeof s_pad);
        return;
    }
    s_pad_ok = XInputGetState(0, &s_pad) == ERROR_SUCCESS;
    if (!s_pad_ok) {
        memset(&s_pad, 0, sizeof s_pad);
        retry = now + 2000;
    }
}

/* Back + Start together opens or closes the menu. */
static void pad_combo(void)
{
    static int was;
    const WORD both = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START;
    int now = (s_pad.Gamepad.wButtons & both) == both;
    if (now && !was)
        InterlockedExchange(&s_req_toggle, 1);
    was = now;
}

static void pad_to_imgui(ImGuiIO &io)
{
    static const struct { WORD bit; ImGuiKey key; } map[] = {
        { XINPUT_GAMEPAD_DPAD_UP,    ImGuiKey_GamepadDpadUp },
        { XINPUT_GAMEPAD_DPAD_DOWN,  ImGuiKey_GamepadDpadDown },
        { XINPUT_GAMEPAD_DPAD_LEFT,  ImGuiKey_GamepadDpadLeft },
        { XINPUT_GAMEPAD_DPAD_RIGHT, ImGuiKey_GamepadDpadRight },
        { XINPUT_GAMEPAD_A,          ImGuiKey_GamepadFaceDown },
        { XINPUT_GAMEPAD_B,          ImGuiKey_GamepadFaceRight },
        { XINPUT_GAMEPAD_X,          ImGuiKey_GamepadFaceLeft },
        { XINPUT_GAMEPAD_Y,          ImGuiKey_GamepadFaceUp },
        { XINPUT_GAMEPAD_LEFT_SHOULDER,  ImGuiKey_GamepadL1 },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, ImGuiKey_GamepadR1 },
        { XINPUT_GAMEPAD_START,      ImGuiKey_GamepadStart },
        { XINPUT_GAMEPAD_BACK,       ImGuiKey_GamepadBack },
    };
    const XINPUT_GAMEPAD &g = s_pad.Gamepad;
    size_t i;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        io.AddKeyEvent(map[i].key, (g.wButtons & map[i].bit) != 0);
    {
        const float dz = 9000.0f, span = 32767.0f - 9000.0f;
        float lx = (float)g.sThumbLX, ly = (float)g.sThumbLY;
#define AX(key, v) do { float a = ((v) - dz) / span; a = a < 0 ? 0 : a > 1 ? 1 : a; \
                        io.AddKeyAnalogEvent(key, a > 0.1f, a); } while (0)
        AX(ImGuiKey_GamepadLStickLeft,  -lx);
        AX(ImGuiKey_GamepadLStickRight,  lx);
        AX(ImGuiKey_GamepadLStickUp,     ly);
        AX(ImGuiKey_GamepadLStickDown,  -ly);
#undef AX
    }
}

/* ── Look ────────────────────────────────────────────────────────────── */
static ImFont *s_font_body, *s_font_head, *s_font_small;
static float   s_font_h;            /* display height the fonts were made for */
static ImGuiStyle s_style_base;

static const ImVec4 C_ACCENT = ImVec4(0.96f, 0.76f, 0.36f, 1.00f);
static const ImVec4 C_TEXT   = ImVec4(0.93f, 0.91f, 0.87f, 1.00f);
static const ImVec4 C_DIM    = ImVec4(0.62f, 0.60f, 0.56f, 1.00f);

static void style_base(void)
{
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0; s.ChildRounding = 4; s.FrameRounding = 4;
    s.PopupRounding = 4; s.GrabRounding = 4; s.ScrollbarRounding = 4;
    s.WindowBorderSize = 0; s.ChildBorderSize = 0; s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(16, 16); s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(12, 10); s.ScrollbarSize = 12; s.GrabMinSize = 14;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text]            = C_TEXT;
    c[ImGuiCol_TextDisabled]    = C_DIM;
    c[ImGuiCol_WindowBg]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg]         = ImVec4(0.06f, 0.05f, 0.05f, 0.55f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.09f, 0.08f, 0.07f, 0.98f);
    c[ImGuiCol_FrameBg]         = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(1, 1, 1, 0.16f);
    c[ImGuiCol_SliderGrab]      = C_ACCENT;
    c[ImGuiCol_SliderGrabActive]= ImVec4(1.0f, 0.86f, 0.55f, 1.0f);
    c[ImGuiCol_Button]          = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.96f, 0.76f, 0.36f, 0.30f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.96f, 0.76f, 0.36f, 0.45f);
    c[ImGuiCol_Header]          = ImVec4(0.96f, 0.76f, 0.36f, 0.18f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.96f, 0.76f, 0.36f, 0.26f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.96f, 0.76f, 0.36f, 0.36f);
    c[ImGuiCol_Separator]       = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_NavHighlight]    = C_ACCENT;
    c[ImGuiCol_CheckMark]       = C_ACCENT;
    c[ImGuiCol_ModalWindowDimBg]= ImVec4(0, 0, 0, 0.5f);
    s_style_base = s;
}

/* Fonts sized for the display: Segoe UI when Windows has it, ImGui's own
 * otherwise. Remade when the window height changes a lot. */
static void fonts_build(float h)
{
    ImGuiIO &io = ImGui::GetIO();
    char win[MAX_PATH], body[MAX_PATH + 32], bold[MAX_PATH + 32];
    float k = h / 1080.0f, px;
    if (k < 0.5f) k = 0.5f;
    px = floorf(26.0f * k);
    io.Fonts->Clear();
    GetWindowsDirectoryA(win, MAX_PATH);
    snprintf(body, sizeof body, "%s\\Fonts\\segoeui.ttf", win);
    snprintf(bold, sizeof bold, "%s\\Fonts\\segoeuib.ttf", win);
    if (GetFileAttributesA(body) != INVALID_FILE_ATTRIBUTES) {
        s_font_body  = io.Fonts->AddFontFromFileTTF(body, px);
        s_font_small = io.Fonts->AddFontFromFileTTF(body, floorf(px * 0.8f));
        s_font_head  = io.Fonts->AddFontFromFileTTF(
            GetFileAttributesA(bold) != INVALID_FILE_ATTRIBUTES ? bold : body, floorf(px * 1.5f));
    } else {
        ImFontConfig fc;
        fc.SizePixels = px;
        s_font_body = io.Fonts->AddFontDefault(&fc);
        fc.SizePixels = floorf(px * 0.8f);
        s_font_small = io.Fonts->AddFontDefault(&fc);
        fc.SizePixels = floorf(px * 1.5f);
        s_font_head = io.Fonts->AddFontDefault(&fc);
    }
    io.FontDefault = s_font_body;
    ImGui_ImplDX11_InvalidateDeviceObjects();   /* new atlas texture on the next frame */
    ImGui::GetStyle() = s_style_base;
    ImGui::GetStyle().ScaleAllSizes(k);
    s_font_h = h;
}

/* ── Widgets ─────────────────────────────────────────────────────────── */
static const char *s_help_title, *s_help_text, *s_help_when;

/* An on/off switch: a pill and its "On"/"Off", one item (so help follows
 * the pad cursor onto it). */
static bool toggle(const char *id, int *v)
{
    ImGuiStyle &st = ImGui::GetStyle();
    float h = ImGui::GetFrameHeight(), w = h * 1.9f;
    float tw = ImGui::CalcTextSize("Off").x + st.ItemInnerSpacing.x * 2;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool changed = ImGui::InvisibleButton(id, ImVec2(w + tw, h));
    if (changed)
        *v = !*v;
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float r = h * 0.5f;
        ImU32 bg = *v ? ImGui::GetColorU32(C_ACCENT) : ImGui::GetColorU32(ImVec4(1, 1, 1, 0.16f));
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, r);
        dl->AddCircleFilled(ImVec2(*v ? p.x + w - r : p.x + r, p.y + r), r - 3.0f,
                            ImGui::GetColorU32(ImVec4(0.08f, 0.07f, 0.06f, 1)));
        dl->AddText(ImVec2(p.x + w + st.ItemInnerSpacing.x * 2, p.y + st.FramePadding.y),
                    ImGui::GetColorU32(ImGuiCol_Text), *v ? "On" : "Off");
    }
    return changed;
}

/* One labelled row of a settings table; the control goes in column 2. */
static void row(const char *label)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

/* After a row's control: show its help when it is under the mouse or the
 * pad/keyboard cursor. */
static void help(const char *title, const char *text, const char *when)
{
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
        s_help_title = title;
        s_help_text = text;
        s_help_when = when;
    }
}

static const char *k_now  = "Takes effect immediately.";
static const char *k_next = "Takes effect the next time you start the game.";

/* ── Pages ───────────────────────────────────────────────────────────── */
static void page_display(void)
{
    static const char *res[] = { "480 (1x)", "960 (2x)", "1440 (3x)", "1920 (4x)",
                                 "2400 (5x)", "2880 (6x)" };
    static const char *winh[] = { "540", "720", "900", "1080", "1440" };
    static const int winh_v[] = { 540, 720, 900, 1080, 1440 };
    int i, wi = 1;

    row("Render resolution");
    if (s_focus_first) { ImGui::SetKeyboardFocusHere(); s_focus_first = 0; }
    i = s_cfg.scale - 1;
    if (ImGui::Combo("##res", &i, res, 6))
        s_cfg.scale = i + 1;
    help("Render resolution",
         "How many lines the game is drawn at, in steps of the original 480. "
         "Higher is sharper but slower. 1440 suits a 1080p or 1440p screen; "
         "2400 suits 4K. Lower it if the game runs slowly.", k_next);

    row("Fullscreen");
    toggle("##fs", &s_cfg.fullscreen);
    help("Fullscreen", "Fill the screen with a borderless window. Alt+Enter also "
         "switches while playing.", k_now);

    row("Window height");
    for (i = 0; i < 5; i++)
        if (winh_v[i] == s_cfg.winh) wi = i;
    if (ImGui::Combo("##wh", &wi, winh, 5))
        s_cfg.winh = winh_v[wi];
    help("Window height", "The size of the window when not fullscreen.", k_next);

    row("Widescreen (16:9)");
    toggle("##ws", &s_cfg.widescreen);
    help("Widescreen", "Show the game at 16:9. Off shows the original 4:3 picture.",
         k_next);

    row("VSync");
    toggle("##vs", &s_cfg.vsync);
    help("VSync", "Wait for the monitor's refresh before showing each frame. "
         "Removes tearing but adds a little input lag.", k_now);

    row("Frame rate cap");
    {
        int cap = s_cfg.fps_cap == 0 ? 1 : 0;
        static const char *caps[] = { "60 fps (recommended)", "Off" };
        if (ImGui::Combo("##cap", &cap, caps, 2))
            s_cfg.fps_cap = cap ? 0 : 60;
    }
    help("Frame rate cap", "The game is made for 60 frames a second. Leave this at "
         "60 unless you are testing.", k_next);

    row("Crop border (sides)");
    ImGui::SliderInt("##cx", &s_cfg.cropx, 0, 20, "%d%%");
    help("Crop border", "Xbox games leave a dark border for old TVs. Cropping zooms "
         "in past it. 0 shows the whole picture.", k_now);

    row("Crop border (top/bottom)");
    ImGui::SliderInt("##cy", &s_cfg.cropy, 0, 20, "%d%%");
    help("Crop border", "Xbox games leave a dark border for old TVs. Cropping zooms "
         "in past it. 0 shows the whole picture.", k_now);
}

static void page_audio(void)
{
    row("Master volume");
    if (s_focus_first) { ImGui::SetKeyboardFocusHere(); s_focus_first = 0; }
    ImGui::SliderInt("##vol", &s_cfg.volume, 0, 100, "%d");
    help("Master volume", "Overall volume of the game.", k_now);

    row("Sound effects");
    ImGui::SliderInt("##sfx", &s_cfg.sfx, 0, 100, "%d");
    help("Sound effects", "Sound effects relative to the music. 70 matches the "
         "original mix closely.", k_now);
}

static void page_controller(void)
{
    row("Rumble");
    if (s_focus_first) { ImGui::SetKeyboardFocusHere(); s_focus_first = 0; }
    toggle("##rum", &s_cfg.rumble);
    help("Rumble", "Let the pad vibrate when the game asks it to. The game's own "
         "vibration option must also be on.", k_now);

    row("Rumble strength");
    ImGui::BeginDisabled(!s_cfg.rumble);
    ImGui::SliderInt("##rs", &s_cfg.rumble_str, 0, 100, "%d");
    help("Rumble strength", "How strongly the pad vibrates.", k_now);
    ImGui::EndDisabled();

    row("");
    ImGui::BeginDisabled(!s_cfg.rumble || !s_pad_ok);
    if (ImGui::Button("Test rumble")) {
        orta_rumble_pulse((float)s_cfg.rumble_str / 100.0f);
        s_pulse_end = GetTickCount64() + 600;
    }
    ImGui::EndDisabled();
    help("Test rumble", s_pad_ok ? "A short pulse at the chosen strength."
                                 : "No controller found.", k_now);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Dummy(ImVec2(1, ImGui::GetFrameHeight() * 0.5f));
    ImGui::PushFont(s_font_small);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(C_DIM, "KEYBOARD");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(C_DIM, "Arrows: D-pad   Enter: Start   Esc: Back\n"
                              "Z: A   X: B   A: X   S: Y\n"
                              "Q / W: left / right trigger   C / V: White / Black");
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(C_DIM, "THIS MENU");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(C_DIM, "F1, or Back + Start on the pad");
    ImGui::PopFont();
}

static void page_about(void)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushFont(s_font_head);
    ImGui::TextUnformatted("Panzer Dragoon Orta - PC");
    ImGui::PopFont();
    ImGui::TextColored(C_DIM, "Build " __DATE__);
    ImGui::Spacing();
    ImGui::TextUnformatted(
        "The original Xbox game, recompiled to run natively on Windows. "
        "You need your own copy of the game.");
    ImGui::Spacing();
    ImGui::TextColored(C_ACCENT, "SAVING");
    ImGui::TextUnformatted(
        "The game saves only at the end of each episode. Always accept the save "
        "screen after clearing an episode.");
    ImGui::Spacing();
    ImGui::TextColored(C_ACCENT, "REPORTING PROBLEMS");
    ImGui::TextUnformatted(
        "Note where it happened and what you were doing, and take a screenshot "
        "if you can. stderr.txt next to the game helps too.");
    ImGui::Spacing();
    ImGui::TextColored(C_ACCENT, "CREDITS");
    ImGui::TextUnformatted(
        "Built on xboxrecomp by sp00nznet. Graphics behaviour checked against xemu. "
        "Menu by Dear ImGui. Panzer Dragoon Orta is (c) SEGA.");
    ImGui::PopTextWrapPos();
}

/* ── The menu ────────────────────────────────────────────────────────── */
static void draw_menu(float W, float H)
{
    static const char *cats[] = { "DISPLAY", "AUDIO", "CONTROLLER", "ABOUT" };
    static int popup_was_open;
    ImGuiStyle &st = ImGui::GetStyle();
    float pad = floorf(H * 0.05f), top = floorf(H * 0.13f), foot = floorf(H * 0.09f);
    float left_w = floorf(W * 0.20f), help_w = floorf(W * 0.27f);
    float mid_w = W - 2 * pad - left_w - help_w - 2 * st.ItemSpacing.x;
    float body_h = H - top - foot;
    bool quit_open;

    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H),
                                                  IM_COL32(8, 6, 5, 200));
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("##orta_menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                 | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
                 | ImGuiWindowFlags_NoBringToFrontOnFocus);

    /* Header */
    ImGui::SetCursorPos(ImVec2(pad, pad * 0.8f));
    ImGui::PushFont(s_font_small);
    ImGui::TextColored(C_DIM, "PANZER DRAGOON ORTA  /  PC");
    ImGui::PopFont();
    ImGui::SetCursorPosX(pad);
    ImGui::PushFont(s_font_head);
    ImGui::TextUnformatted("Settings");
    ImGui::PopFont();
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float y = top - st.ItemSpacing.y;
        dl->AddLine(ImVec2(pad, y), ImVec2(W - pad, y), IM_COL32(255, 255, 255, 28));
    }

    /* Category list. LB / RB, or Q / E, step through it. */
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_Q, false))
        { s_cat = (s_cat + 3) % 4; s_focus_first = 1; }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_E, false))
        { s_cat = (s_cat + 1) % 4; s_focus_first = 1; }
    ImGui::SetCursorPos(ImVec2(pad, top));
    ImGui::BeginChild("##cats", ImVec2(left_w, body_h), 0, ImGuiWindowFlags_NoBackground);
    for (int i = 0; i < 4; i++) {
        ImGui::PushStyleColor(ImGuiCol_Text, i == s_cat ? C_ACCENT : C_TEXT);
        if (ImGui::Selectable(cats[i], i == s_cat, 0, ImVec2(0, ImGui::GetFrameHeight() * 1.2f))) {
            s_cat = i;
            s_focus_first = 0;
        }
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::PushFont(s_font_small);
    ImGui::TextColored(C_DIM, "LB / RB  or  Q / E");
    ImGui::PopFont();
    ImGui::EndChild();

    /* Settings */
    s_help_title = s_help_text = s_help_when = NULL;
    ImGui::SetCursorPos(ImVec2(pad + left_w + st.ItemSpacing.x, top));
    ImGui::BeginChild("##page", ImVec2(mid_w, body_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(s_font_small);
    ImGui::TextColored(C_ACCENT, "%s", cats[s_cat]);
    ImGui::PopFont();
    ImGui::Separator();
    if (s_cat == 3) {
        page_about();
    } else if (ImGui::BeginTable("##rows", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        if (s_cat == 0) page_display();
        else if (s_cat == 1) page_audio();
        else page_controller();
        ImGui::EndTable();
    }
    ImGui::EndChild();

    /* Help */
    ImGui::SetCursorPos(ImVec2(W - pad - help_w, top));
    ImGui::BeginChild("##help", ImVec2(help_w, body_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (s_help_title) {
        ImGui::PushFont(s_font_head);
        ImGui::TextUnformatted(s_help_title);
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(s_help_text);
        ImGui::Spacing();
        ImGui::TextColored(C_ACCENT, "%s", s_help_when);
        ImGui::PopTextWrapPos();
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(C_DIM, "Choose a setting to see what it does.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    /* Footer */
    ImGui::SetCursorPos(ImVec2(pad, H - foot + st.ItemSpacing.y));
    if (ImGui::Button("Resume"))
        ovl_set_open(0);
    ImGui::SameLine();
    if (ImGui::Button("Quit game"))
        ImGui::OpenPopup("Quit?");
    ImGui::SameLine(0, st.ItemSpacing.x * 3);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(s_font_small);
    ImGui::TextColored(C_DIM, "Changes are saved when you close this menu.   "
                              "F1 / B / Esc: resume");
    ImGui::PopFont();

    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    quit_open = ImGui::BeginPopupModal("Quit?", NULL, ImGuiWindowFlags_AlwaysAutoResize
                                       | ImGuiWindowFlags_NoTitleBar);
    if (quit_open) {
        ImGui::TextUnformatted("Quit the game?");
        ImGui::TextColored(C_DIM, "Anything since your last save will be lost.");
        ImGui::Spacing();
        if (ImGui::Button("Quit")) {
            HWND h = xbox_FramebufferWindowHwnd();
            if (memcmp(&s_cfg, &s_saved, sizeof s_cfg) != 0)
                cfg_save();
            orta_rumble_stop_now();
            if (h)
                PostMessageA(h, WM_CLOSE, 0, 0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }

    /* B or Esc closes the menu, unless it just closed a list or popup. */
    {
        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        if (!any_popup && !popup_was_open && !ImGui::IsAnyItemActive()
            && (ImGui::IsKeyPressed(ImGuiKey_Escape, false)
                || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
            ovl_set_open(0);
        popup_was_open = any_popup;
    }
    ImGui::End();
}

/* ── Present hook (GPU executor thread) ──────────────────────────────── */
static int s_ready;

static void ovl_present(void *dev, void *ctx, void *rtv, unsigned w, unsigned h)
{
    static LARGE_INTEGER qf, last;
    LARGE_INTEGER now;
    (void)rtv;

    pad_poll();
    if (!s_open)
        pad_combo();
    if (InterlockedExchange(&s_req_toggle, 0))
        ovl_set_open(!s_open);
    if (s_pulse_end && GetTickCount64() >= s_pulse_end) {
        s_pulse_end = 0;
        orta_rumble_stop_now();
    }
    if (!s_open) {
        EnterCriticalSection(&s_evlock);
        s_evn = 0;
        LeaveCriticalSection(&s_evlock);
        return;
    }
    if (s_open)
        pad_combo();                   /* Back + Start closes it too */

    if (!s_ready) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = NULL;
        io.LogFilename = NULL;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendPlatformName = "orta_overlay";
        style_base();
        if (!ImGui_ImplDX11_Init((ID3D11Device *)dev, (ID3D11DeviceContext *)ctx)) {
            ImGui::DestroyContext();
            ovl_set_open(0);
            return;
        }
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&last);
        s_ready = 1;
    }
    if (s_font_h <= 0.0f || fabsf((float)h - s_font_h) > s_font_h * 0.15f)
        fonts_build((float)h);

    ImGuiIO &io = ImGui::GetIO();
    QueryPerformanceCounter(&now);
    io.DeltaTime = (float)((double)(now.QuadPart - last.QuadPart) / (double)qf.QuadPart);
    if (io.DeltaTime <= 0.0f || io.DeltaTime > 0.5f)
        io.DeltaTime = 1.0f / 60.0f;
    last = now;
    io.DisplaySize = ImVec2((float)w, (float)h);

    EnterCriticalSection(&s_evlock);
    for (int i = 0; i < s_evn; i++) {
        const Ev &e = s_evq[i];
        switch (e.kind) {
        case EV_MOVE:   io.AddMousePosEvent((float)e.a, (float)e.b); break;
        case EV_BUTTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL:  io.AddMouseWheelEvent(0.0f, (float)e.a / (float)WHEEL_DELTA); break;
        case EV_KEY:    if (e.a != ImGuiKey_None) io.AddKeyEvent((ImGuiKey)e.a, e.b != 0); break;
        case EV_CHAR:   io.AddInputCharacter((unsigned)e.a); break;
        case EV_FOCUS:  io.AddFocusEvent(false); io.AddFocusEvent(true); break;
        }
    }
    s_evn = 0;
    LeaveCriticalSection(&s_evlock);
    pad_to_imgui(io);

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    draw_menu((float)w, (float)h);
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    if (memcmp(&s_cfg, &s_saved, sizeof s_cfg) != 0 || s_open)
        cfg_apply_live();
}

/* ── Install ─────────────────────────────────────────────────────────── */
extern "C" void ovl_install(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    InitializeCriticalSection(&s_evlock);
    cfg_load();
    g_wndproc_hook = ovl_wndproc;
    g_present_overlay_hook = ovl_present;
    g_present_pause_hook = ovl_pause;
}
