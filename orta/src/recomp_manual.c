/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;
/* Thread-local in the runtime, so it has to be declared that way here too. */
#ifdef _MSC_VER
extern __declspec(thread) uint32_t g_esp;
extern __declspec(thread) uint32_t g_eax;
extern __declspec(thread) uint32_t g_ecx;
#else
extern __thread uint32_t g_esp;
extern __thread uint32_t g_eax;
extern __thread uint32_t g_ecx;
#endif
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;

/* ── Manual function overrides ─────────────────────────────── */

/* ── Orta: functions replaced by hand ───────────────────────────
 *
 * The recompiler skips generating any sub_XXXXXXXX defined here (the loop
 * script passes --exclude-manual), so these replace the game's own code
 * for every caller, direct or indirect.
 */

/* XInitDevices(DWORD dwPreallocTypeCount, PXDEVICE_PREALLOC_TYPE pTypes)
 *
 * stdcall, 8 bytes of arguments. The real one brings up the console's USB
 * host controller, which does not exist here. When the recompiler could not
 * find this function it left a stub that popped only the return address, so
 * the 8 bytes of arguments stayed on the stack and the DirectInput set-up that
 * calls it read its own locals 4 bytes out of place: it handed the game a null
 * input object, input init failed, and the game shut down.
 *
 * Skipped for now. Controllers will need bridging to Windows separately. */
void sub_003068D4(void)
{
    g_esp += 4 + 8;    /* return address + both arguments, as `ret 8` would */
}

/* ── Controller: Xbox XInput answered from the PC ──────────────────────────
 *
 * With the USB stack skipped, the game's controller calls (statically linked
 * XAPI, in the XPP section) are replaced here. One controller is reported in
 * port 1; its state comes from the first Windows XInput pad, or from the
 * keyboard when the game window has focus:
 *
 *   Arrows = D-pad   Enter = Start   Esc/Backspace = Back
 *   Z = A   X = B   A = X   S = Y   Q = L trigger   W = R trigger
 *   C = White   V = Black
 *
 * Addresses identified from the XBE (all stdcall):
 *   0x3075B4 XGetDevices(type)                     ret 4
 *   0x30C55D XInputOpen(type, port, slot, params)  ret 0x10
 *   0x30C5B3 XInputClose(h)                        ret 4
 *   0x30C5BF XInputGetCapabilities(h, caps)        ret 8
 *   0x30C79D XInputGetState(h, state)              ret 8
 *   0x30C809 XInputSetState(h, feedback)           ret 8
 *   0x30C83C XInputPoll(h)                         ret 4
 */
#define ORTA_GAMEPAD_TYPE  0x003067CCu
#define ORTA_PAD_HANDLE    0x0C0FFEE0u

#define GMEM8(a)  (*(volatile uint8_t  *)(g_xbox_mem_offset + (uintptr_t)(uint32_t)(a)))
#define GMEM16(a) (*(volatile uint16_t *)(g_xbox_mem_offset + (uintptr_t)(uint32_t)(a)))
#define GMEM32(a) (*(volatile uint32_t *)(g_xbox_mem_offset + (uintptr_t)(uint32_t)(a)))

#ifdef _WIN32
__declspec(dllimport) void * __stdcall LoadLibraryA(const char *);
__declspec(dllimport) void * __stdcall GetProcAddress(void *, const char *);
__declspec(dllimport) short  __stdcall GetAsyncKeyState(int);
__declspec(dllimport) void * __stdcall GetForegroundWindow(void);
__declspec(dllimport) unsigned long __stdcall GetWindowThreadProcessId(void *, unsigned long *);
__declspec(dllimport) unsigned long __stdcall GetCurrentProcessId(void);
#pragma comment(lib, "user32.lib")

typedef struct {
    uint32_t packet;
    uint16_t buttons;
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
} WinPadState;
typedef unsigned long (__stdcall *WinXInputGetState)(unsigned long, WinPadState *);

static WinXInputGetState orta_win_xinput(void)
{
    static int tried;
    static WinXInputGetState fn;
    if (!tried) {
        const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        int i;
        tried = 1;
        for (i = 0; i < 3 && !fn; i++) {
            void *m = LoadLibraryA(dlls[i]);
            if (m)
                fn = (WinXInputGetState)GetProcAddress(m, "XInputGetState");
        }
        fprintf(stderr, "[PAD] Windows XInput %s\n", fn ? "found" : "not found (keyboard only)");
    }
    return fn;
}

static int orta_has_focus(void)
{
    unsigned long pid = 0;
    void *w = GetForegroundWindow();
    if (!w)
        return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
#endif

#include <stdlib.h>
#include <string.h>
#include "overlay.h"

/* ── Rumble ──────────────────────────────────────────────────────────────
 * XInputSetState's XINPUT_FEEDBACK is a 66-byte header (dwStatus, hEvent,
 * 58 reserved bytes) and then XINPUT_RUMBLE: left (heavy) and right (light)
 * motor speeds, 0..65535 -- the same pair Windows takes. The game's last
 * request is kept and sent on, scaled by settings.ini Rumble /
 * RumbleStrength (or the menu), and held at zero while the window is in
 * the background or the menu is open. */
volatile float g_rumble_scale = -1.0f;      /* -1: from settings.ini */
static uint16_t s_rum_want[2];              /* what the game asked for */
static int      s_rum_sent[2] = { -1, -1 }; /* what the pad was last told */

#ifdef _WIN32
typedef struct { uint16_t l, r; } WinVib;
typedef unsigned long (__stdcall *WinXInputSetState)(unsigned long, WinVib *);

static WinXInputSetState orta_win_xinput_set(void)
{
    static int tried;
    static WinXInputSetState fn;
    if (!tried) {
        const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        int i;
        tried = 1;
        for (i = 0; i < 3 && !fn; i++) {
            void *m = LoadLibraryA(dlls[i]);
            if (m)
                fn = (WinXInputSetState)GetProcAddress(m, "XInputSetState");
        }
    }
    return fn;
}
#endif

static float orta_rumble_k(void)
{
    static float k = -1.0f;
    if (g_rumble_scale >= 0.0f)
        return g_rumble_scale;
    if (k < 0.0f) {
        const char *on = getenv("RECOMP_RUMBLE"), *st = getenv("RECOMP_RUMBLE_STRENGTH");
        int s = st ? atoi(st) : 100;
        if (s < 0) s = 0;
        if (s > 100) s = 100;
        k = (on && *on == '0') ? 0.0f : (float)s / 100.0f;
    }
    return k;
}

static void orta_rumble_send(int l, int r)
{
#ifdef _WIN32
    WinXInputSetState xs = orta_win_xinput_set();
    if (xs && (l != s_rum_sent[0] || r != s_rum_sent[1])) {
        WinVib v;
        v.l = (uint16_t)l;
        v.r = (uint16_t)r;
        xs(0, &v);
        s_rum_sent[0] = l;
        s_rum_sent[1] = r;
    }
#else
    (void)l; (void)r;
#endif
}

static void orta_rumble_exit(void)
{
    s_rum_sent[0] = s_rum_sent[1] = -1;
    orta_rumble_send(0, 0);
}

/* Bring the pad in line with the game's request; every game frame. */
static void orta_rumble_update(void)
{
    static int hooked;
    float k = orta_rumble_k();
    int quiet = ovl_is_open();
#ifdef _WIN32
    quiet = quiet || !orta_has_focus();
#endif
    if (!hooked) {
        hooked = 1;
        atexit(orta_rumble_exit);
    }
    if (quiet || k <= 0.0f)
        orta_rumble_send(0, 0);
    else
        orta_rumble_send((int)(s_rum_want[0] * k), (int)(s_rum_want[1] * k));
}

/* For the menu: stop now (and resend on the next game frame), or a test pulse. */
void orta_rumble_stop_now(void)
{
    s_rum_sent[0] = s_rum_sent[1] = -1;
    orta_rumble_send(0, 0);
}

void orta_rumble_pulse(float strength)
{
    int v = (int)(65535.0f * (strength < 0 ? 0 : strength > 1 ? 1 : strength));
    s_rum_sent[0] = s_rum_sent[1] = -1;
    orta_rumble_send(v, v);
}

/* Fill an Xbox XINPUT_GAMEPAD (18 bytes) at guest address `g`. */
static void orta_read_pad(uint32_t g)
{
    uint16_t buttons = 0;
    uint8_t an[8] = {0};
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    int i;
#ifdef _WIN32
    WinXInputGetState xi = orta_win_xinput();
    WinPadState ws;
    if (xi && xi(0, &ws) == 0) {
        buttons = ws.buttons & 0x00FF;          /* d-pad, start, back, sticks */
        an[0] = (ws.buttons & 0x1000) ? 0xFF : 0; /* A */
        an[1] = (ws.buttons & 0x2000) ? 0xFF : 0; /* B */
        an[2] = (ws.buttons & 0x4000) ? 0xFF : 0; /* X */
        an[3] = (ws.buttons & 0x8000) ? 0xFF : 0; /* Y */
        an[4] = (ws.buttons & 0x0200) ? 0xFF : 0; /* Black = RB */
        an[5] = (ws.buttons & 0x0100) ? 0xFF : 0; /* White = LB */
        an[6] = ws.lt;
        an[7] = ws.rt;
        lx = ws.lx; ly = ws.ly; rx = ws.rx; ry = ws.ry;
    }
    if (orta_has_focus()) {
        #define KEY(vk) (GetAsyncKeyState(vk) & 0x8000)
        if (KEY(0x26)) buttons |= 0x01;             /* Up    */
        if (KEY(0x28)) buttons |= 0x02;             /* Down  */
        if (KEY(0x25)) buttons |= 0x04;             /* Left  */
        if (KEY(0x27)) buttons |= 0x08;             /* Right */
        if (KEY(0x0D)) buttons |= 0x10;             /* Enter = Start */
        if (KEY(0x1B) || KEY(0x08)) buttons |= 0x20;/* Esc/Backspace = Back */
        if (KEY('Z')) an[0] = 0xFF;
        if (KEY('X')) an[1] = 0xFF;
        if (KEY('A')) an[2] = 0xFF;
        if (KEY('S')) an[3] = 0xFF;
        if (KEY('V')) an[4] = 0xFF;
        if (KEY('C')) an[5] = 0xFF;
        if (KEY('Q')) an[6] = 0xFF;
        if (KEY('W')) an[7] = 0xFF;
        #undef KEY
    }
#endif
    {   /* The settings menu takes the controls while it is up, and until
         * everything is let go after it closes. */
        int held = buttons != 0 || lx > 8000 || lx < -8000 || ly > 8000 || ly < -8000
                || rx > 8000 || rx < -8000 || ry > 8000 || ry < -8000;
        for (i = 0; i < 8; i++)
            held |= an[i] > 0x40;
        if (ovl_block_input(held)) {
            buttons = 0;
            memset(an, 0, sizeof an);
            lx = ly = rx = ry = 0;
        }
    }
    orta_rumble_update();
    GMEM16(g) = buttons;
    for (i = 0; i < 8; i++)
        GMEM8(g + 2 + i) = an[i];
    GMEM16(g + 10) = (uint16_t)lx;
    GMEM16(g + 12) = (uint16_t)ly;
    GMEM16(g + 14) = (uint16_t)rx;
    GMEM16(g + 16) = (uint16_t)ry;
}

/* XGetDevices(PXPP_DEVICE_TYPE type): bitmask of connected ports. */
void sub_003075B4(void)
{
    uint32_t type = GMEM32(g_esp + 4);
    g_eax = (type == ORTA_GAMEPAD_TYPE) ? 1u : 0u;
    g_esp += 4 + 4;
}

/* XInputOpen(type, port, slot, params) -> handle */
void sub_0030C55D(void)
{
    uint32_t type = GMEM32(g_esp + 4), port = GMEM32(g_esp + 8);
    static int logged;
    g_eax = (type == ORTA_GAMEPAD_TYPE && port == 0) ? ORTA_PAD_HANDLE : 0u;
    if (logged++ < 4)
        fprintf(stderr, "[PAD] XInputOpen type %08X port %u -> %08X\n", type, port, g_eax);
    g_esp += 4 + 16;
}

/* XInputClose(h) */
void sub_0030C5B3(void)
{
    g_esp += 4 + 4;
}

/* XInputGetCapabilities(h, caps): a standard controller that has everything. */
void sub_0030C5BF(void)
{
    uint32_t caps = GMEM32(g_esp + 8);
    int i;
    GMEM8(caps) = 1;                          /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
    GMEM8(caps + 1) = 0;
    GMEM8(caps + 2) = 0;
    GMEM16(caps + 3) = 0x00FF;                /* every digital button */
    for (i = 0; i < 8; i++)
        GMEM8(caps + 5 + i) = 0xFF;           /* every analog button  */
    for (i = 0; i < 4; i++)
        GMEM16(caps + 13 + 2 * i) = 0xFFFF;   /* full-range sticks    */
    GMEM16(caps + 21) = 0xFFFF;               /* rumble motors        */
    GMEM16(caps + 23) = 0xFFFF;
    g_eax = 0;
    g_esp += 4 + 8;
}

/* XInputGetState(h, state) */
/* ── Scripted controller (RECOMP_AUTOPAD) ──────────────────────────────────
 *
 *   RECOMP_AUTOPAD=start@1100,a@1260,a@1400~A,start@1900
 *   ("~A": repeat every 40 frames while the game mode at 0x342718 is still 0xA;
 *    "=64": do not press before the game mode is 0x64;
 *    "*60": hold for 60 frames instead of 3)
 * Also: up/down/left/right (d-pad), sl/sr/su/sd (left stick), black, white,
 * lt, rt.
 *
 * Presses a button when the game has shown that many frames. Frames rather
 * than seconds, because the game counts frames: the menus come up at the same
 * frame whatever the frame rate. Each press is held for 3 frames and at least
 * a quarter of a second, then released. Buttons: start, back, a, b, x, y.
 * Real keyboard / controller input still works alongside it. */
#include <time.h>
#include <stdlib.h>
#include <string.h>
extern uint32_t nv2a_flip_count(void);

static void orta_autopad(uint32_t pad)
{
    static int n = -1, k, pressing;
    static uint32_t at[24], until[24], waitfor[24], hold[24];
    static char btn[24][8];
    static uint32_t press_flip;
    static clock_t press_clock;
    uint32_t f;

    if (n < 0) {
        const char *e = getenv("RECOMP_AUTOPAD");
        n = 0;
        while (e && *e && n < 24) {
            int len = 0;
            while (e[len] && e[len] != '@' && len < 7) { btn[n][len] = e[len]; len++; }
            btn[n][len] = 0;
            e += len;
            if (*e != '@') break;
            at[n] = (uint32_t)strtoul(e + 1, (char **)&e, 10);
            until[n] = 0xFFFFFFFFu;
            waitfor[n] = 0xFFFFFFFFu;
            hold[n] = 3;
            if (*e == '*')                       /* hold for N frames */
                hold[n] = (uint32_t)strtoul(e + 1, (char **)&e, 10);
            if (*e == '~')                       /* repeat while mode == value */
                until[n] = (uint32_t)strtoul(e + 1, (char **)&e, 16);
            else if (*e == '=')                  /* hold off until mode == value */
                waitfor[n] = (uint32_t)strtoul(e + 1, (char **)&e, 16);
            n++;
            if (*e == ',') e++;
        }
        if (n)
            fprintf(stderr, "[PAD] scripted input: %d press(es)\n", n);
    }
    if (k >= n)
        return;
    f = nv2a_flip_count();
    if (!pressing) {
        if (f < at[k])
            return;
        if (waitfor[k] != 0xFFFFFFFFu && GMEM32(0x00342718u) != waitfor[k]) {
            if (f > at[k] + 1500) {              /* never came: skip this step */
                fprintf(stderr, "[PAD] scripted press %d skipped (mode never became %X)\n",
                        k + 1, waitfor[k]);
                k++;
            }
            return;
        }
        pressing = 1;
        press_flip = f;
        press_clock = clock();
        fprintf(stderr, "[PAD] scripted press %d: %s at frame %u\n", k + 1, btn[k], f);
    } else if (f >= press_flip + hold[k] &&
               (clock() - press_clock) * 1000 / CLOCKS_PER_SEC >= 250) {
        pressing = 0;
        /* "a@1400~A": keep pressing every 40 frames while the game's mode
         * (0x342718) is still 0xA -- the menus open at different frames
         * depending on how fast the title screen draws. */
        if (until[k] != 0xFFFFFFFFu && GMEM32(0x00342718u) == until[k])
            at[k] = f + 40;
        else
            k++;
        return;
    }
    if (!strcmp(btn[k], "start"))      GMEM16(pad) |= 0x0010;
    else if (!strcmp(btn[k], "back"))  GMEM16(pad) |= 0x0020;
    else if (!strcmp(btn[k], "a"))     GMEM8(pad + 2) = 0xFF;
    else if (!strcmp(btn[k], "b"))     GMEM8(pad + 3) = 0xFF;
    else if (!strcmp(btn[k], "x"))     GMEM8(pad + 4) = 0xFF;
    else if (!strcmp(btn[k], "y"))     GMEM8(pad + 5) = 0xFF;
    else if (!strcmp(btn[k], "up"))    GMEM16(pad) |= 0x0001;
    else if (!strcmp(btn[k], "down"))  GMEM16(pad) |= 0x0002;
    else if (!strcmp(btn[k], "left"))  GMEM16(pad) |= 0x0004;
    else if (!strcmp(btn[k], "right")) GMEM16(pad) |= 0x0008;
    else if (!strcmp(btn[k], "black")) GMEM8(pad + 6) = 0xFF;
    else if (!strcmp(btn[k], "white")) GMEM8(pad + 7) = 0xFF;
    else if (!strcmp(btn[k], "lt"))    GMEM8(pad + 8) = 0xFF;
    else if (!strcmp(btn[k], "rt"))    GMEM8(pad + 9) = 0xFF;
    /* left stick: sl/sr/su/sd = full left/right/up/down */
    else if (!strcmp(btn[k], "sl"))    GMEM16(pad + 10) = (uint16_t)(int16_t)-32767;
    else if (!strcmp(btn[k], "sr"))    GMEM16(pad + 10) = 32767;
    else if (!strcmp(btn[k], "su"))    GMEM16(pad + 12) = 32767;
    else if (!strcmp(btn[k], "sd"))    GMEM16(pad + 12) = (uint16_t)(int16_t)-32767;
}

void sub_0030C79D(void)
{
    uint32_t state = GMEM32(g_esp + 8);
    static uint32_t packet;
    static uint8_t last[18];
    uint8_t now[18];
    int i;
    orta_read_pad(state + 4);
    orta_autopad(state + 4);
    for (i = 0; i < 18; i++)
        now[i] = GMEM8(state + 4 + i);
    for (i = 0; i < 18; i++)
        if (now[i] != last[i]) {
            static int shown;
            packet++;
            if (shown++ < 40)
                fprintf(stderr, "[PAD] input changed: buttons %02X%02X A%02X B%02X X%02X Y%02X\n",
                        now[1], now[0], now[2], now[3], now[4], now[5]);
            break;
        }
    {
        static uint32_t calls;
        if (++calls == 1 || calls == 100 || calls == 1000)
            fprintf(stderr, "[PAD] XInputGetState called %u times\n", calls);
    }
    for (i = 0; i < 18; i++)
        last[i] = now[i];
    GMEM32(state) = packet;
    g_eax = 0;
    g_esp += 4 + 8;
}

/* XInputSetState(h, feedback): rumble to the PC pad; report it done. */
void sub_0030C809(void)
{
    uint32_t fb = GMEM32(g_esp + 8);
    static unsigned logged;
    s_rum_want[0] = GMEM16(fb + 66);
    s_rum_want[1] = GMEM16(fb + 68);
    if (logged < 12 && (s_rum_want[0] || s_rum_want[1] || logged < 2)) {
        logged++;
        fprintf(stderr, "[RUMBLE] left %u right %u (event %08X)\n",
                s_rum_want[0], s_rum_want[1], GMEM32(fb + 4));
    }
    orta_rumble_update();
    GMEM32(fb) = 0;                           /* header.dwStatus = success */
    g_eax = 0;
    g_esp += 4 + 8;
}

/* ── Media-object graph update (0x220520) with a guard ─────────────────────
 *
 *   for (p = list.begin; p != list.end; p++) if (*p) (*p)->vtbl[8](*p);
 *
 * The original as C, except that an entry which is not a plausible object
 * (outside the title's heap, or whose vtable is not code) is reported and
 * skipped instead of called. Orta crashed here on an entry of 0x80000000
 * whose "vtable" was the 0xDEADBEEF D3D writes to physical page 0. */
extern recomp_func_t recomp_lookup(uint32_t xbox_va);

static void orta_report_list(const char *who, uint32_t self, uint32_t b, uint32_t e,
                             uint32_t cap, uint32_t bad_at);

static int orta_plausible_obj(uint32_t obj)
{
    uint32_t vt;
    if (obj < 0x00010000u || obj >= 0x80000000u)
        return 0;
    vt = GMEM32(obj);
    if (vt < g_xbox_code_lo || vt >= 0x00400000u)
        return 0;
    return 1;
}

void sub_00220520(void)
{
    uint32_t self = GMEM32(g_esp + 4);
    uint32_t p, result = 0;
    static int reported;

    if (GMEM32(self + 0xC) == 0) {
        g_eax = 0x800401F0u;
        g_esp += 4 + 4;
        return;
    }
    for (p = GMEM32(self + 0x14); p != GMEM32(self + 0x18); p += 4) {
        uint32_t obj = GMEM32(p), fnva;
        recomp_func_t fn;
        if (!obj)
            continue;
        if (!orta_plausible_obj(obj)) {
            orta_report_list("graph", self, GMEM32(self + 0x14), GMEM32(self + 0x18),
                             GMEM32(self + 0x1C), p);
            continue;
        }
        fnva = GMEM32(GMEM32(obj) + 0x20);
        fn = recomp_lookup(fnva);
        if (!fn) {
            if (reported++ < 8)
                fprintf(stderr, "[GRAPH] method %08X of %08X not translated -- skipped\n", fnva, obj);
            continue;
        }
        g_ecx = obj;
        g_esp -= 4; GMEM32(g_esp) = obj;          /* argument */
        g_esp -= 4; GMEM32(g_esp) = 0x0022054Cu;  /* return address */
        fn();                                      /* stdcall: pops both */
        result = g_eax;
    }
    g_eax = result;
    g_esp += 4 + 4;
}

/* ── Sound engine work loop (0x21E570) with the same guard ────────────────
 *
 *   if (!list.begin || list.empty()) return 0x800401F0;
 *   for (p in list) if (*p) r = (*p)->vtbl[4](*p);
 *   DSOUND_0x2C379E(self->dsound);  DSOUND_0x2C26A5();   return r;
 */
static void orta_call_guest(uint32_t va, uint32_t ret_va)
{
    recomp_func_t fn = recomp_lookup(va);
    g_esp -= 4; GMEM32(g_esp) = ret_va;
    if (fn)
        fn();
    else {
        fprintf(stderr, "[GRAPH] %08X not translated\n", va);
        g_esp += 4;
    }
}

static void orta_report_list(const char *who, uint32_t self, uint32_t b, uint32_t e,
                             uint32_t cap, uint32_t bad_at)
{
    static int reported;
    uint32_t q;
    if (reported++ >= 12)
        return;
    fprintf(stderr, "[GRAPH] %s owner %08X: entry %u of %u (cap %u) is %08X -- skipped. list:",
            who, self, (bad_at - b) / 4, (e - b) / 4, (cap - b) / 4, GMEM32(bad_at));
    for (q = b; q != e && q < b + 64; q += 4)
        fprintf(stderr, " %08X", GMEM32(q));
    fprintf(stderr, "\n");
}

void sub_0021E570(void)
{
    uint32_t self = GMEM32(g_esp + 4);
    uint32_t b = GMEM32(self + 0x20), p, r = 0;

    if (!b || GMEM32(self + 0x24) == b) {
        g_eax = 0x800401F0u;
        g_esp += 4 + 4;
        return;
    }
    for (p = b; p != GMEM32(self + 0x24); p += 4) {
        uint32_t obj = GMEM32(p);
        if (!obj)
            continue;
        if (!orta_plausible_obj(obj)) {
            orta_report_list("work", self, GMEM32(self + 0x20), GMEM32(self + 0x24),
                             GMEM32(self + 0x28), p);
            continue;
        }
        g_ecx = GMEM32(obj);
        g_esp -= 4; GMEM32(g_esp) = obj;
        orta_call_guest(GMEM32(GMEM32(obj) + 0x10), 0x0021E5A1u);
        r = g_eax;
    }
    g_esp -= 4; GMEM32(g_esp) = GMEM32(self + 0x14);
    orta_call_guest(0x002C379Eu, 0x0021E5B6u);
    orta_call_guest(0x002C26A5u, 0x0021E5BBu);
    g_eax = r;
    g_esp += 4 + 4;
}

/* ── DirectSound effects-image download wrapper (0x2C1221) ───────────────
 *
 *   thiscall(obj, a1, a2, a3), ret 0xC:
 *     hr = DSP_MANAGER_0x2C5AA3(obj->mgr (at +0x14), a1, a2, a3);
 *     obj->flags_0x320 = (hr >= 0) ? flags | 1 : flags & ~1;
 *
 * 0x2C5AA3 ends in 0x2C5836, which posts command 3 to the GP DSP at
 * scratch+0x810 and spins until the DSP clears it. The DSP here is a stub, so
 * the runtime's DSP-ack clears it instead -- but only for addresses it knows,
 * and the scratch block is new every time DirectSound re-initialises (the
 * Options menu does that), so the fixed RECOMP_DSP_ACK list went stale and
 * Options hung at 0 fps. Register the live address before every download. */
extern void xbox_DspAckAdd(uint32_t va);

void sub_002C1221(void)
{
    uint32_t obj = g_ecx;
    uint32_t a1 = GMEM32(g_esp + 4), a2 = GMEM32(g_esp + 8), a3 = GMEM32(g_esp + 12);
    uint32_t mgr = GMEM32(obj + 0x14);
    uint32_t hr;

    if (mgr) {
        uint32_t p = GMEM32(mgr + 8);
        if (p) p = GMEM32(p + 0x10);
        if (p) p = GMEM32(p);
        if (p) xbox_DspAckAdd(p + 0x810);
    }
    g_esp -= 4; GMEM32(g_esp) = a3;
    g_esp -= 4; GMEM32(g_esp) = a2;
    g_esp -= 4; GMEM32(g_esp) = a1;
    g_ecx = mgr;
    orta_call_guest(0x002C5AA3u, 0x002C1238u);   /* callee pops ret + 12 */
    hr = g_eax;
    if ((int32_t)hr >= 0)
        GMEM32(obj + 0x320) |= 1u;
    else
        GMEM32(obj + 0x320) &= ~1u;
    g_eax = hr;
    g_esp += 4 + 12;
}

/* ── Background loader thread (0x14D50) ───────────────────────────────────
 *
 *   while (WaitForSingleObjectEx(quit_event, 0, TRUE) != WAIT_OBJECT_0) {
 *       do {
 *           if (stream_service_0x3E870() >= 2) break;
 *           if (pending_list_0x3410A0) {
 *               for (p = &pending_list; *p; p++)
 *                   if (**p && load_one_0x14CC0(*p)) goto next;
 *               pending_list_0x3410A0 = 0;
 *           }
 *       next: ;
 *       } while (WaitForSingleObjectEx(quit_event, 0, TRUE) != 0);
 *   }
 *   ExitThread(0);
 *
 * The translator split this loop across four functions (0x14D50, 0x14D64,
 * 0x14DA0, 0x14DAA) that jump into each other. A jump between translated
 * functions is a C call that never returns, so each pass of the loop added a
 * host stack frame and the thread died of a stack overflow about a second
 * after the cache partition started working. Written out here as one loop. */
static uint32_t orta_wait_quit_event(void)
{
    g_esp -= 4; GMEM32(g_esp) = 1;                       /* bAlertable */
    g_esp -= 4; GMEM32(g_esp) = 0;                       /* dwMilliseconds */
    g_esp -= 4; GMEM32(g_esp) = GMEM32(0x003410B0u);     /* hHandle */
    orta_call_guest(0x0022D1E2u, 0x00014DBAu);           /* stdcall, ret 12 */
    return g_eax;
}

void sub_00014D50(void)
{
    if (orta_wait_quit_event() != 0) {
        do {
            uint32_t p;
            orta_call_guest(0x0003E870u, 0x00014D69u);
            if (g_eax >= 2)
                continue;
            if (GMEM32(0x003410A0u)) {
                int stop = 0;
                for (p = 0x003410A0u; GMEM32(p); p += 4) {
                    uint32_t item = GMEM32(p);
                    if (GMEM32(item) == 0)
                        continue;
                    g_esp -= 4; GMEM32(g_esp) = item;
                    orta_call_guest(0x00014CC0u, 0x00014D8Du);  /* cdecl */
                    g_esp += 4;
                    if (g_eax) { stop = 1; break; }
                }
                if (stop)
                    continue;
                GMEM32(0x003410A0u) = 0;
            }
        } while (orta_wait_quit_event() != 0);
    }
    g_esp -= 4; GMEM32(g_esp) = 0;
    orta_call_guest(0x0022D4F1u, 0x00014DC6u);           /* ExitThread: no return */
}

/* ── Reached a placeholder for code the translator never produced ─────────
 * Called by every stub in recomp_stubs_unresolved.c. Each one is a branch of
 * the game whose code is being skipped, so the first hit of each is logged
 * with where it came from. */
void recomp_stub_hit(uint32_t va)
{
    enum { N = 512 };
    static uint32_t seen[N];
    static int count;
    int i;
    for (i = 0; i < count; i++)
        if (seen[i] == va)
            return;
    if (count < N)
        seen[count++] = va;
    fprintf(stderr, "[STUB] code at 0x%08X skipped (never translated); return to %08X, esp %08X\n",
            va, GMEM32(g_esp), g_esp);
}

/* XInputPoll(h) */
void sub_0030C83C(void)
{
    g_eax = 0;
    g_esp += 4 + 4;
}

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /* Rate-limited per address (first 3, then every power of two): a missing
     * function inside a per-frame loop logged ~1000 times a second with a
     * flush each, which alone cost Orta's stage 2 a third of its frame rate.
     * The first line per address is all the run loop needs to seed it. */
    enum { SLOTS = 64 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int nseen;
    uint64_t n = 0;
    int i;
    for (i = 0; i < nseen; i++)
        if (seen[i] == va) { n = ++hits[i]; break; }
    if (i == nseen && nseen < SLOTS) { seen[nseen] = va; hits[nseen] = n = 1; nseen++; }
    if (n > 3 && (n & (n - 1)))
        return;
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu, this one %llu times)\n",
            va, (unsigned long long)g_icall_count, (unsigned long long)n);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    /* First sighting: say WHERE. The caller pushed its guest return address
     * just before this call, so the guest stack names the call site and the
     * chain above it. */
    if (hits[i] == 1 && g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int k, shown = 0;
        fprintf(stderr, "  called from (guest return addresses, innermost first):");
        for (k = 0; k < 64 && shown < 8; k++) {
            uint32_t v = sp[k];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, " %08X", v);
                shown++;
            }
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── DirectSound voice-service DPC body (0x2C5F25) ────────────────────────
 *
 *   thiscall(dsound), plain ret:
 *     sub_2C5EC2(this);
 *     for each of the three voice lists at this+0x728/+0x730/+0x738:
 *         for (e = head->Flink; e != head; ) {
 *             voice(e - 0x4C)->vtbl[5]();          // service
 *             next = e->Flink;
 *             if (next == e) next = head->Flink;   // e removed itself: restart
 *             e = next;
 *         }
 *     while ((e = RemoveEntry(this+0x740 ->Flink)) != this+0x740)
 *         obj(e - 0x54)->vtbl[5]();
 *
 * At the end of the first stage (frame ~18,708, every run) one voice's list
 * entry links to itself while the list head still points at it, so the
 * restart rule picks the same entry forever and the DPC never returns.
 * Same walk, but an entry that is both self-linked and still the head's
 * first entry ends that list's pass, and is reported. A cap per list stops
 * any other cycle the same way. */
void sub_002C5F25(void)
{
    uint32_t self = g_ecx, k;
    static int reported;

    g_ecx = self;
    orta_call_guest(0x002C5EC2u, 0x002C5F33u);
    {
        /* Check the three lists before walking them: a zero link is how the
         * stage-end crash starts (it faults on entry 0 - 0x4C). Report the
         * entry whose Flink was cleared, once per distinct entry. */
        static uint32_t said[16];
        static int nsaid;
        uint32_t kk;
        for (kk = 0; kk < 3; kk++) {
            uint32_t head = self + 0x728u + 8u * kk, prev = head, q = GMEM32(head), n = 0;
            while (q != head && n++ < 512) {
                if (!q || !GMEM32(q)) {
                    int j;
                    uint32_t bad = q ? q : prev;
                    for (j = 0; j < nsaid && said[j] != bad; j++)
                        ;
                    if (j == nsaid && nsaid < 16) {
                        said[nsaid++] = bad;
                        fprintf(stderr, "[DSOUND] list %u of %08X: entry %08X has Flink %08X (Blink %08X,"
                                " object vtbl %08X), reached from %08X\n", kk, self, bad,
                                GMEM32(bad), GMEM32(bad + 4), q ? GMEM32(q - 0x4Cu) : 0, prev);
                    }
                    break;
                }
                prev = q;
                q = GMEM32(q);
            }
        }
    }
    for (k = 0; k < 3; k++) {
        uint32_t head = self + 0x728u + 8u * k, node = GMEM32(head), steps = 0;
        while (node != head) {
            uint32_t obj = node - 0x4Cu, next;
            g_ecx = obj;
            orta_call_guest(GMEM32(GMEM32(obj) + 0x14u), 0x002C5F4Cu);
            GMEM32(self + 0x324u) += 1;
            next = GMEM32(node);
            if (next == node) {
                next = GMEM32(head);
                if (next == node) {
                    if (reported++ < 8)
                        fprintf(stderr, "[DSOUND] voice list %u of %08X: entry %08X (object %08X,"
                                " vtbl %08X) links to itself but is still first -- pass ended\n",
                                k, self, node, obj, GMEM32(obj));
                    next = head;
                }
            }
            GMEM32(self + 0x324u) -= 1;
            node = next;
            /* A pass legitimately takes thousands of steps when many voices
             * remove themselves (each removal restarts the walk), so only a
             * walk that is clearly endless is cut -- and described: the
             * last entries visited, with their links and classes. */
            if (++steps > 2000000u) {
                if (reported++ < 8) {
                    uint32_t q = GMEM32(head), i;
                    fprintf(stderr, "[DSOUND] voice list %u of %08X: endless walk, cut. head %08X"
                            " Flink %08X Blink %08X; entries:", k, self, head,
                            GMEM32(head), GMEM32(head + 4));
                    for (i = 0; i < 12 && q != head && q; i++, q = GMEM32(q))
                        fprintf(stderr, " [%08X F%08X B%08X vt%08X st%04X]", q, GMEM32(q),
                                GMEM32(q + 4), GMEM32(q - 0x4Cu),
                                (unsigned)(GMEM32(q - 0x4Cu + 0x12u) & 0xFFFF));
                    fprintf(stderr, "\n");
                }
                break;
            }
        }
    }
    {
        uint32_t lh = self + 0x740u, steps = 0;
        for (;;) {
            uint32_t e;
            g_esp -= 4; GMEM32(g_esp) = GMEM32(lh);
            GMEM32(self + 0x324u) += 1;
            orta_call_guest(0x002C1140u, 0x002C5F84u);   /* stdcall(1): pops its argument */
            GMEM32(self + 0x324u) -= 1;
            e = g_eax;
            if (e == lh || ++steps > 2000000u)
                break;
            g_ecx = e - 0x54u;
            orta_call_guest(GMEM32(GMEM32(e - 0x54u) + 0x14u), 0x002C5F97u);
        }
    }
}

/* ── DirectSound idle-voice trap handler (0x2C6028) ───────────────────────
 *
 *   thiscall(dsound, handle), ret 4:
 *     if (handle < 0x100 && !this->walking (0x324)
 *         && !(voice_mem[handle].0x04 & 0x800000)) {
 *         obj = this->voice_by_handle[handle] (0x328);
 *         if (obj->handles[obj->cur (0x64)] (0x0A) == handle
 *             && obj->hw_link (0x4C) is not self-linked)
 *             obj->UnlinkHardwareVoice(1);           // 0x2C7141
 *     }
 *
 * The unlink faults when the voice's list entry was never initialised
 * (Flink and Blink both 0): the APU reported a voice idle that DirectSound
 * had not linked into its lists yet. Same checks, plus that one, which is
 * reported and skipped instead of crashing. */
extern void xbox_ApuVoiceOrphaned(uint32_t v);

void sub_002C6028(void)
{
    uint32_t self = g_ecx, h = GMEM32(g_esp + 4);
    static int reported;
    static int deferred_rep;

    /* Tell the APU which idle voices have no live owner (so it may evict
     * them) -- and, by omission, which ones it must keep trapping. */
    if (h < 0x100 && (GMEM32(self + 0x324u) != 0
                      || (GMEM32(GMEM32(0x002DFF48u) + (h << 7) + 4u) & 0x800000u))) {
        if (deferred_rep++ < 12)
            fprintf(stderr, "[DSOUND] idle trap for voice %u deferred (busy %08X, flags %08X)\n",
                    h, GMEM32(self + 0x324u), GMEM32(GMEM32(0x002DFF48u) + (h << 7) + 4u));
    } else if (h < 0x100) {
        uint32_t obj = GMEM32(self + 0x328u + h * 4u);
        if (!obj) {
            xbox_ApuVoiceOrphaned(h);
        } else {
            uint32_t idx = GMEM32(obj + 0x64u) & 0xFFu;
            uint32_t hh = GMEM32(obj + 0x0Au + idx * 2u) & 0xFFFFu;
            uint32_t e = obj + 0x4Cu;
            if (hh != h || GMEM32(e) == e || !GMEM32(e) || !GMEM32(e + 4u))
                xbox_ApuVoiceOrphaned(h);
        }
    }

    if (h < 0x100 && GMEM32(self + 0x324u) == 0
        && !(GMEM32(GMEM32(0x002DFF48u) + (h << 7) + 4u) & 0x800000u)) {
        uint32_t obj = GMEM32(self + 0x328u + h * 4u);
        if (obj) {
            uint32_t idx = GMEM32(obj + 0x64u) & 0xFFu;
            uint32_t hh = GMEM32(obj + 0x0Au + idx * 2u) & 0xFFFFu;
            uint32_t e = obj + 0x4Cu;
            if (hh == h && GMEM32(e) != e) {
                if (!GMEM32(e) || !GMEM32(e + 4u)) {
                    if (reported++ < 12)
                        fprintf(stderr, "[DSOUND] idle trap for voice %u: object %08X has an"
                                " unlinked list entry (F %08X B %08X) -- skipped\n",
                                h, obj, GMEM32(e), GMEM32(e + 4u));
                } else {
                    g_ecx = obj;
                    g_esp -= 4; GMEM32(g_esp) = 1;
                    orta_call_guest(0x002C7141u, 0x002C6075u);   /* pops ret + 4 */
                }
            }
        } else if (reported++ < 12) {
            fprintf(stderr, "[DSOUND] idle trap for voice %u: no object -- skipped\n", h);
        }
    }
    g_esp += 4 + 4;
}

/* ── Sound-engine voice GetStatus (0x221F10), with a poll census ─────────
 *
 *   stdcall(voice, out):
 *     if (!voice->buffer (0x10)) return 0x800401F0;
 *     if (!out) return 0x80070057;
 *     st = 0; hr = DSBUFFER_GetStatus(voice->buffer, &st);       // 0x2C25FA
 *     if (hr >= 0) *out = st | flags from voice->0x1AC / 0x160;
 *     return hr;
 *
 * Same behaviour; it also counts polls per (caller, voice) and every 5 s
 * prints the busiest ones with the last raw status, so a loop that waits on
 * a voice which never stops shows up by name. */
void sub_00221F10(void)
{
    static struct { uint32_t ret, self, buf, n, last, st; } tab[32];
    static clock_t next;
    uint32_t sp = g_esp, ret = GMEM32(sp), self = GMEM32(sp + 4), out = GMEM32(sp + 8);
    uint32_t buf = GMEM32(self + 0x10), st, i;

    if (!buf) { g_eax = 0x800401F0u; g_esp = sp + 12; return; }
    if (!out) { g_eax = 0x80070057u; g_esp = sp + 12; return; }
    GMEM32(sp + 4) = 0;                                  /* local, as the original */
    g_esp -= 4; GMEM32(g_esp) = sp + 4;
    g_esp -= 4; GMEM32(g_esp) = buf;
    orta_call_guest(0x002C25FAu, 0x00221F38u);           /* stdcall(2) */
    g_esp = sp;
    st = GMEM32(sp + 4);
    if ((int32_t)g_eax >= 0) {
        uint32_t f = GMEM32(self + 0x1ACu), v = st;
        if (f & 0x100u) v |= 0x100u;
        if (f & 0x200u) v |= 0x200u;
        if (f & 0x400u) v |= 0x400u;
        if (GMEM32(self + 0x160u) & 0x100u) v |= 0x1000u;
        GMEM32(out) = v;
    }
    for (i = 0; i < 32; i++)
        if ((tab[i].ret == ret && tab[i].self == self) || !tab[i].n) break;
    if (i < 32) {
        tab[i].ret = ret; tab[i].self = self; tab[i].buf = buf;
        tab[i].n++; tab[i].st = st;
    }
    if (clock() >= next) {
        if (next) {
            fprintf(stderr, "[SNDSTAT] GetStatus polls in the last 5 s (caller voice buffer: count status):");
            for (i = 0; i < 32 && tab[i].n; i++)
                if (tab[i].n != tab[i].last)
                    fprintf(stderr, " %08X %08X %08X:%u st=%X", tab[i].ret, tab[i].self,
                            tab[i].buf, tab[i].n - tab[i].last, tab[i].st);
            fprintf(stderr, "\n");
            for (i = 0; i < 32; i++) tab[i].last = tab[i].n;
        }
        next = clock() + 5 * CLOCKS_PER_SEC;
    }
    g_esp = sp + 12;
}
