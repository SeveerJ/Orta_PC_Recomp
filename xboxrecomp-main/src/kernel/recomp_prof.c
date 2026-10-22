/**
 * RECOMP_PROFILE=1: a sampling profiler for the whole process.
 *
 * Every millisecond a background thread suspends each other thread, reads
 * its instruction pointer and resumes it. Samples inside the .exe are
 * bucketed by 256-byte offset; everything else (system DLLs: waits, the
 * driver) counts as "outside". Once a minute it prints, per busy thread, the
 * share of time spent in the exe and its hottest offsets -- resolved against
 * the linker map afterwards. That answers both "where does the frame time
 * go" and, during a freeze, "which thread is spinning, and where".
 */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PROF_THREADS 48
#define PROF_SLOTS   4096

typedef struct {
    DWORD tid;
    uint32_t total, outside;
    uint32_t key[PROF_SLOTS], cnt[PROF_SLOTS];
    uint64_t okey[256];            /* outside the exe: 4 KB page of rip */
    uint32_t ocnt[256];
    uint32_t ckey[256], ccnt[256]; /* outside: the exe code that called out */
    uintptr_t stack_hi;
} ProfThread;

/* Set by the kernel bridge: prints what each guest thread is blocked on. */
void (*g_recomp_dump_waits)(void) = 0;

static ProfThread *s_pt;
static int s_npt;
static uintptr_t s_base, s_size;

static ProfThread *prof_thread(DWORD tid)
{
    int i;
    for (i = 0; i < s_npt; i++)
        if (s_pt[i].tid == tid)
            return &s_pt[i];
    if (s_npt == PROF_THREADS)
        return NULL;
    memset(&s_pt[s_npt], 0, sizeof s_pt[s_npt]);
    s_pt[s_npt].tid = tid;
    return &s_pt[s_npt++];
}

static void prof_add_caller(ProfThread *t, uintptr_t rsp)
{
    uintptr_t p, end;
    MEMORY_BASIC_INFORMATION mbi;
    if (!t->stack_hi) {
        if (!VirtualQuery((void *)rsp, &mbi, sizeof mbi))
            return;
        t->stack_hi = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    if (rsp >= t->stack_hi)
        return;
    end = rsp + 16384 < t->stack_hi ? rsp + 16384 : t->stack_hi;
    for (p = rsp; p + 8 <= end; p += 8) {
        uintptr_t v = *(const uintptr_t *)p;
        if (v >= s_base && v < s_base + s_size) {
            uint32_t k = (uint32_t)((v - s_base) >> 4) + 1, h = (k * 2654435761u) & 255u, n = 0;
            while (t->ckey[h] && t->ckey[h] != k && n++ < 256)
                h = (h + 1) & 255u;
            if (n < 256) {
                t->ckey[h] = k;
                t->ccnt[h]++;
            }
            return;
        }
    }
}

static void prof_add(ProfThread *t, uintptr_t rip, uintptr_t rsp)
{
    uint32_t k, h;
    t->total++;
    if (rip < s_base || rip >= s_base + s_size) {
        prof_add_caller(t, rsp);
        uint64_t pk = ((uint64_t)rip >> 12) + 1;
        uint32_t oh = (uint32_t)(pk * 2654435761u) & 255u, probes = 0;
        t->outside++;
        while (t->okey[oh] && t->okey[oh] != pk && probes++ < 256)
            oh = (oh + 1) & 255u;
        if (probes < 256) {
            t->okey[oh] = pk;
            t->ocnt[oh]++;
        }
        return;
    }
    k = (uint32_t)((rip - s_base) >> 8) + 1;
    h = (k * 2654435761u) & (PROF_SLOTS - 1);
    while (t->key[h] && t->key[h] != k)
        h = (h + 1) & (PROF_SLOTS - 1);
    t->key[h] = k;
    t->cnt[h]++;
}

static void prof_report(void)
{
    int i, j, n;
    uint32_t all = 0;
    char line[2048];
    for (i = 0; i < s_npt; i++)
        all += s_pt[i].total;
    fprintf(stderr, "[PROF] --- %u samples over %d threads ---\n", all, s_npt);
    for (i = 0; i < s_npt; i++) {
        ProfThread *t = &s_pt[i];
        uint32_t in = t->total - t->outside, otop = 0;
        int len;
        for (j = 0; j < 256; j++)
            if (t->ocnt[j] > otop)
                otop = t->ocnt[j];
        /* Skip threads that sit in one place outside the exe: a wait. */
        if (!t->total || (in * 50u < t->total && otop * 10u >= t->outside * 9u))
            continue;
        len = snprintf(line, sizeof line, "[PROF] tid %lu: %u samples, %u%% in exe; hottest (exe offset/256, per mille):",
                       (unsigned long)t->tid, t->total, in * 100u / t->total);
        for (n = 0; n < 25 && len < (int)sizeof line - 200; n++) {
            int best = -1;
            for (j = 0; j < PROF_SLOTS; j++)
                if (t->key[j] && t->cnt[j] && (best < 0 || t->cnt[j] > t->cnt[best]))
                    best = j;
            if (best < 0 || t->cnt[best] * 200u < t->total)
                break;
            len += snprintf(line + len, sizeof line - len, " %X:%u", (t->key[best] - 1) << 8,
                            t->cnt[best] * 1000u / t->total);
            t->cnt[best] = 0;                       /* consumed */
        }
        len += snprintf(line + len, sizeof line - len, " | outside:");
        for (n = 0; n < 8 && len < (int)sizeof line - 200; n++) {
            int best = -1;
            HMODULE hm = NULL;
            char mod[MAX_PATH] = "?";
            const char *mn;
            uintptr_t page;
            for (j = 0; j < 256; j++)
                if (t->ocnt[j] && (best < 0 || t->ocnt[j] > t->ocnt[best]))
                    best = j;
            if (best < 0 || t->ocnt[best] * 100u < t->total)
                break;
            page = (uintptr_t)((t->okey[best] - 1) << 12);
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)page, &hm))
                GetModuleFileNameA(hm, mod, sizeof mod);
            mn = strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod;
            len += snprintf(line + len, sizeof line - len, " %s+%llX:%u", mn,
                            (unsigned long long)(page - (uintptr_t)hm),
                            t->ocnt[best] * 1000u / t->total);
            t->ocnt[best] = 0;
        }
        len += snprintf(line + len, sizeof line - len, " | called out from (exe offset/16):");
        for (n = 0; n < 6 && len < (int)sizeof line - 60; n++) {
            int best = -1;
            for (j = 0; j < 256; j++)
                if (t->ccnt[j] && (best < 0 || t->ccnt[j] > t->ccnt[best]))
                    best = j;
            if (best < 0 || t->ccnt[best] * 100u < t->total)
                break;
            len += snprintf(line + len, sizeof line - len, " %X:%u", (t->ckey[best] - 1) << 4,
                            t->ccnt[best] * 1000u / t->total);
            t->ccnt[best] = 0;
        }
        if (len > (int)sizeof line - 2)
            len = (int)sizeof line - 2;
        line[len++] = '\n';
        line[len] = 0;
        fputs(line, stderr);
    }
    fflush(stderr);
    s_npt = 0;
}

extern uint32_t nv2a_flip_count(void);

/* RECOMP_WATCH=<guest VA, hex>: a hardware write watchpoint on one dword
 * (and on its tiled alias at 0xF0000000 when the VA is in the contiguous
 * window). Every thread gets the debug registers as the profiler lists it;
 * a vectored handler records who wrote, what, and the exe return addresses
 * on its stack. Hits writing 0 print at once; the last 16 of any kind print
 * on recomp_watch_dump() (called when the pushbuffer walk gives up). */
extern ptrdiff_t xbox_GetMemoryOffset(void);
typedef struct { DWORD tid; uint32_t val; uint64_t rip; uint32_t stk[8]; int n; } WatchHit;
static uintptr_t s_watch[2];
static uint32_t s_watch_va;
static WatchHit s_hits[16];
static volatile LONG s_nhits;
static DWORD s_armed[256];
static volatile LONG s_traps, s_sanity;
static int s_watch_all;
static int s_narmed;
/* RECOMP_HWWATCH=*<ptr VA>+<off>: watch [ptr]+off, resolved once the
 * pointer is set. RECOMP_HWWATCH_BIT=<hex>: print every write that flips
 * one of these bits (both directions), whatever the value. */
static uint32_t s_watch_ptr, s_watch_ptroff, s_watch_bit, s_watch_prev;
static void watch_resolve(void);

static void watch_format(const WatchHit *h, char *line, size_t cap)
{
    int len, i;
    len = snprintf(line, cap, "[WATCH] %08X <- %08X by tid %lu at exe+%llX:", s_watch_va,
                   h->val, (unsigned long)h->tid, (unsigned long long)h->rip);
    for (i = 0; i < h->n && len > 0 && len < (int)cap - 12; i++)
        len += snprintf(line + len, cap - len, " %X", h->stk[i]);
    if (len > 0 && len < (int)cap - 2) {
        line[len++] = '\n';
        line[len] = 0;
    }
}

static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    WatchHit h;
    uintptr_t p;
    LONG k;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !s_watch[0])
        return EXCEPTION_CONTINUE_SEARCH;
    /* Only DR6's breakpoint bits make it ours. The runtime single-steps
     * too (the AC'97 write trap re-protects its page from a trace trap), and
     * swallowing one of those leaves that page writable for good. */
    if (!(c->Dr6 & 7))
        return EXCEPTION_CONTINUE_SEARCH;
    s_traps++;
    if ((c->Dr6 & 4) && !(c->Dr6 & 3)) {             /* the sanity watch */
        s_sanity++;
        c->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    memset(&h, 0, sizeof h);
    h.tid = GetCurrentThreadId();
    h.val = *(volatile uint32_t *)s_watch[0];
    h.rip = (c->Rip >= s_base && c->Rip < s_base + s_size) ? c->Rip - s_base : c->Rip;
    {
        uintptr_t top = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
        uintptr_t lim = (uintptr_t)c->Rsp + 4096;
        if (top && top < lim)
            lim = top;
    for (p = (uintptr_t)c->Rsp; h.n < 8 && p + 8 <= lim; p += 8) {
        uintptr_t v = *(const uintptr_t *)p;
        if (v >= s_base && v < s_base + s_size)
            h.stk[h.n++] = (uint32_t)(v - s_base);
    }
    }
    k = InterlockedIncrement(&s_nhits) - 1;
    s_hits[k & 15] = h;
    if (s_watch_bit) {
        uint32_t prev = s_watch_prev;
        s_watch_prev = h.val;
        if ((prev ^ h.val) & s_watch_bit) {
            static volatile LONG bprinted;
            if (InterlockedIncrement(&bprinted) <= 300) {
                char line[256];
                watch_format(&h, line, sizeof line);
                fputs(line, stderr);
            }
        }
    } else if (h.val == 0 || s_watch_all) {
        static volatile LONG printed;
        if (InterlockedIncrement(&printed) <= (s_watch_all ? 400 : 24)) {
            char line[256];
            watch_format(&h, line, sizeof line);
            fputs(line, stderr);
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void recomp_watch_dump(void)
{
    LONG n = s_nhits, i;
    char line[256];
    if (!s_watch[0])
        return;
    fprintf(stderr, "[WATCH] %ld writes to %08X so far (%d threads armed, %ld traps, %ld on the"
            " sanity watch 002BDEB0); the last ones:\n", (long)n, s_watch_va, s_narmed,
            (long)s_traps, (long)s_sanity);
    for (i = n > 16 ? n - 16 : 0; i < n; i++) {
        watch_format(&s_hits[i & 15], line, sizeof line);
        fputs(line, stderr);
    }
}

static void watch_arm(HANDLE h, DWORD tid)
{
    CONTEXT c;
    int i;
    if (!s_watch[0] && s_watch_ptr)
        watch_resolve();
    if (!s_watch[0])
        return;
    for (i = 0; i < s_narmed; i++)
        if (s_armed[i] == tid)
            return;
    if (s_narmed == 256 || SuspendThread(h) == (DWORD)-1)
        return;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(h, &c)) {
        c.Dr0 = s_watch[0];
        c.Dr7 = (c.Dr7 & ~0x000F0003ull) | 1ull | (1ull << 16) | (3ull << 18);
        if (s_watch[1]) {
            c.Dr1 = s_watch[1];
            c.Dr7 = (c.Dr7 & ~0x00F0000Cull) | 4ull | (1ull << 20) | (3ull << 22);
        }
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (SetThreadContext(h, &c))
            s_armed[s_narmed++] = tid;
    }
    ResumeThread(h);
}

static void watch_resolve(void)
{
    uint32_t p = *(volatile uint32_t *)(xbox_GetMemoryOffset() + s_watch_ptr), va;
    if (!p)
        return;
    va = (p + s_watch_ptroff) & ~3u;
    s_watch_va = va;
    s_watch_prev = *(volatile uint32_t *)(xbox_GetMemoryOffset() + va);
    if (va >= 0x80000000u && va < 0x84000000u)
        s_watch[1] = (uintptr_t)(xbox_GetMemoryOffset() + (va - 0x80000000u + 0xF0000000u));
    s_watch[0] = (uintptr_t)(xbox_GetMemoryOffset() + va);
    fprintf(stderr, "[WATCH] [%08X]=%08X: write watchpoint on %08X (now %08X)\n",
            s_watch_ptr, p, va, s_watch_prev);
}

static void watch_init(void)
{
    const char *e = getenv("RECOMP_HWWATCH");
    uint32_t va;
    if (!e || !*e)
        return;
    s_watch_all = getenv("RECOMP_HWWATCH_ALL") != NULL;   /* every write, not just zeros */
    if (getenv("RECOMP_HWWATCH_BIT"))
        s_watch_bit = (uint32_t)strtoul(getenv("RECOMP_HWWATCH_BIT"), NULL, 16);
    if (*e == '*') {
        char *end;
        s_watch_ptr = (uint32_t)strtoul(e + 1, &end, 16);
        if (*end == '+')
            s_watch_ptroff = (uint32_t)strtoul(end + 1, NULL, 16);
        AddVectoredExceptionHandler(1, watch_veh);
        fprintf(stderr, "[WATCH] will watch [%08X]+%X once set (bit mask %X)\n",
                s_watch_ptr, s_watch_ptroff, s_watch_bit);
        return;
    }
    va = (uint32_t)strtoul(e, NULL, 16) & ~3u;
    s_watch_va = va;
    s_watch[0] = (uintptr_t)(xbox_GetMemoryOffset() + va);
    if (va >= 0x80000000u && va < 0x84000000u)
        s_watch[1] = (uintptr_t)(xbox_GetMemoryOffset() + (va - 0x80000000u + 0xF0000000u));
    AddVectoredExceptionHandler(1, watch_veh);
    fprintf(stderr, "[WATCH] write watchpoint on %08X (host %p, tiled alias %p)\n",
            va, (void *)s_watch[0], (void *)s_watch[1]);
}

/* Where is a stalled thread? Its instruction pointer (and the module that
 * holds it), then every value on its stack that points into the .exe --
 * return addresses, mostly -- which the map turns into a call chain. */
static void dump_thread(HANDLE h, DWORD tid, const char *tag)
{
    /* Nothing that takes a lock while the thread is stopped: it may hold
     * the C runtime's stderr lock (or the heap's), and then this thread
     * would wait for it forever. Gather into a local buffer, resume, print. */
    CONTEXT c;
    HMODULE hm = NULL;
    uintptr_t sp, end, p, vals[24];
    MEMORY_BASIC_INFORMATION mbi;
    char mod[MAX_PATH] = "?", line[512];
    int n = 0, len, i, got;

    if (SuspendThread(h) == (DWORD)-1)
        return;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    got = GetThreadContext(h, &c);
    if (got) {
        sp = (uintptr_t)c.Rsp;
        if (VirtualQuery((void *)sp, &mbi, sizeof mbi)) {
            end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (end - sp > 65536)
                end = sp + 65536;
            for (p = sp; p + 8 <= end && n < 24; p += 8) {
                uintptr_t v = *(const uintptr_t *)p;
                if (v >= s_base && v < s_base + s_size)
                    vals[n++] = v - s_base;
            }
        }
    }
    ResumeThread(h);
    if (!got)
        return;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)c.Rip, &hm))
        GetModuleFileNameA(hm, mod, sizeof mod);
    len = snprintf(line, sizeof line, "[%s] tid %lu rip %s+%llX:", tag, (unsigned long)tid,
                   strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod,
                   (unsigned long long)(c.Rip - (uintptr_t)hm));
    for (i = 0; i < n && len > 0 && len < (int)sizeof line - 12; i++)
        len += snprintf(line + len, sizeof line - len, " %llX", (unsigned long long)vals[i]);
    if (len > 0 && len < (int)sizeof line - 2) {
        line[len++] = '\n';
        line[len] = 0;
    }
    fputs(line, stderr);
}

/* The executor thread's real call stack, every 5 s (unwound with the
 * modules' own unwind data, so frames inside d3d11 and the driver show up
 * as module+offset). Answers "what is the executor waiting in" exactly,
 * where the stack scan above only finds the nearest exe frame. */
volatile DWORD g_nv2a_exec_tid;

static void walk_thread(HANDLE h, DWORD tid)
{
    CONTEXT c;
    DWORD64 rips[28];
    int n = 0, i, len;
    char line[1600];
    if (SuspendThread(h) == (DWORD)-1)
        return;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(h, &c)) {
        while (n < 28 && c.Rip) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f;
            PVOID hd = NULL;
            DWORD64 est = 0;
            rips[n++] = c.Rip;
            f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
            if (!f) {                                  /* leaf: return address at rsp */
                c.Rip = *(DWORD64 *)c.Rsp;
                c.Rsp += 8;
            } else {
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &est, NULL);
            }
        }
    }
    ResumeThread(h);
    len = snprintf(line, sizeof line, "[EXECSTACK] tid %lu:", (unsigned long)tid);
    /* (also used for every thread during a stall) */
    for (i = 0; i < n && len < (int)sizeof line - 80; i++) {
        HMODULE hm = NULL;
        char mod[MAX_PATH] = "?";
        const char *mn;
        if (rips[i] >= s_base && rips[i] < s_base + s_size) {
            len += snprintf(line + len, sizeof line - len, " exe+%llX",
                            (unsigned long long)(rips[i] - s_base));
            continue;
        }
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)rips[i], &hm))
            GetModuleFileNameA(hm, mod, sizeof mod);
        mn = strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod;
        len += snprintf(line + len, sizeof line - len, " %s+%llX", mn,
                        (unsigned long long)(rips[i] - (DWORD64)(uintptr_t)hm));
    }
    if (len < (int)sizeof line - 2) { line[len++] = '\n'; line[len] = 0; }
    fputs(line, stderr);
}

static DWORD WINAPI prof_main(LPVOID arg)
{
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId(), last;
    DWORD flip_tick;
    uint32_t flips = 0;
    int dumped = 0;
    (void)arg;
    last = flip_tick = GetTickCount();
    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        int rounds;
        if (snap == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }
        /* Re-list threads every 100 samples; sample the list meanwhile. */
        {
            DWORD tids[PROF_THREADS];
            HANDLE hs[PROF_THREADS];
            int nt = 0, k;
            te.dwSize = sizeof te;
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == pid && te.th32ThreadID != self
                        && nt < PROF_THREADS) {
                        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                                              | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                              FALSE, te.th32ThreadID);
                        if (h) {
                            tids[nt] = te.th32ThreadID;
                            hs[nt++] = h;
                            watch_arm(h, te.th32ThreadID);
                        }
                    }
                    te.dwSize = sizeof te;
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
            for (rounds = 0; rounds < 100; rounds++) {
                for (k = 0; k < nt; k++) {
                    CONTEXT c;
                    ProfThread *t;
                    if (SuspendThread(hs[k]) == (DWORD)-1)
                        continue;
                    memset(&c, 0, sizeof c);
                    c.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(hs[k], &c) && (t = prof_thread(tids[k])) != NULL)
                        prof_add(t, (uintptr_t)c.Rip, (uintptr_t)c.Rsp);
                    ResumeThread(hs[k]);
                }
                Sleep(1);
            }
            /* No new frame for 20 s after the game had started drawing: say
             * where every thread is, once per stall. */
            if (nv2a_flip_count() != flips) {
                flips = nv2a_flip_count();
                flip_tick = GetTickCount();
                dumped = 0;
                {
                    /* RECOMP_EXIT_AT_FLIP: end the run at this frame. */
                    static long exit_at = -1;
                    if (exit_at < 0) {
                        const char *e = getenv("RECOMP_EXIT_AT_FLIP");
                        exit_at = e ? atol(e) : 0;
                    }
                    if (exit_at > 0 && flips >= (uint32_t)exit_at) {
                        fprintf(stderr, "[EXIT] reached frame %u -- ending the run\n", flips);
                        if (s_watch[0])
                            recomp_watch_dump();
                        fflush(stderr);
                        ExitProcess(0);
                    }
                }
            } else if (flips > 0 && !dumped && GetTickCount() - flip_tick > 20000) {
                dumped = 1;
                fprintf(stderr, "[STALL] no frame for 20 s at flip %u -- thread stacks"
                                " (exe offsets):\n", flips);
                for (k = 0; k < nt; k++)
                    dump_thread(hs[k], tids[k], "STALL");
                if (g_recomp_dump_waits)
                    g_recomp_dump_waits();
                fflush(stderr);
            }
            /* RECOMP_STALL_EXIT_SECS: a run that has stopped drawing for
             * this long is over -- end it, so an unattended test loop moves
             * on to the next build instead of sitting out the time limit. */
            {
                static long exit_secs = -1;
                if (exit_secs < 0) {
                    const char *e = getenv("RECOMP_STALL_EXIT_SECS");
                    exit_secs = e ? atol(e) : 0;
                }
                /* Before frame 100 the title may legitimately sit loading
                 * for a while, so a stall there has to last three minutes. */
                if (exit_secs > 0 && dumped
                    && GetTickCount() - flip_tick
                       > (DWORD)(flips > 100 ? exit_secs : (exit_secs > 180 ? exit_secs : 180)) * 1000u) {
                    fprintf(stderr, "[STALL] no frame for %lu s at flip %u -- ending the run\n",
                            (unsigned long)((GetTickCount() - flip_tick) / 1000u), flips);
                    fflush(stderr);
                    ExitProcess(4);
                }
            }
            {
                /* RECOMP_WAITDUMP=<seconds>: the wait census on a timer, not
                 * just in a stall -- what every guest thread is blocked on
                 * while something is slow rather than stopped. */
                static long every = -1;
                static DWORD last_wd;
                if (every < 0) {
                    const char *e = getenv("RECOMP_WAITDUMP");
                    every = e ? atol(e) : 0;
                }
                if (every > 0 && g_recomp_dump_waits
                    && GetTickCount() - last_wd >= (DWORD)every * 1000u) {
                    last_wd = GetTickCount();
                    fprintf(stderr, "[WAITDUMP] at flip %u:\n", flips);
                    g_recomp_dump_waits();
                }
            }
            /* Once a minute, with the report: where every thread is right
             * now. The report says where time goes; this says what each
             * waiting thread is waiting in. */
            if (GetTickCount() - last >= 60000 && flips > 100) {
                fprintf(stderr, "[WHERE] thread stacks at flip %u (exe offsets):\n", flips);
                for (k = 0; k < nt; k++)
                    dump_thread(hs[k], tids[k], "WHERE");
            }
            {
                /* During a stall, every thread's real stack every 2 s: what
                 * the game is waiting on is in one of them. */
                static DWORD last_all;
                if (flips > 100 && GetTickCount() - flip_tick > 10000
                    && GetTickCount() - last_all >= 2000) {
                    last_all = GetTickCount();
                    for (k = 0; k < nt; k++)
                        walk_thread(hs[k], tids[k]);
                }
            }
            {
                static DWORD last_walk;
                if (g_nv2a_exec_tid && GetTickCount() - last_walk >= 5000 && flips > 100) {
                    last_walk = GetTickCount();
                    for (k = 0; k < nt; k++)
                        if (tids[k] == g_nv2a_exec_tid)
                            walk_thread(hs[k], tids[k]);
                }
            }
            for (k = 0; k < nt; k++)
                CloseHandle(hs[k]);
        }
        if (GetTickCount() - last >= 60000) {
            last = GetTickCount();
            prof_report();
        }
    }
    return 0;
}

/* stderr is unbuffered by default, so every fprintf is its own WriteFile to
 * the log -- and the executor thread spent close to a third of its time in
 * those writes. RECOMP_STDERR_BUFFER (default on; 0 = off) buffers it and a
 * small thread flushes twice a second, so a log never lags by much. */
static DWORD WINAPI stderr_flusher(LPVOID a)
{
    (void)a;
    for (;;) {
        Sleep(500);
        fflush(stderr);
    }
    return 0;
}

static void stderr_buffer_init(void)
{
    static int done;
    const char *e = getenv("RECOMP_STDERR_BUFFER");
    if (done || (e && !strcmp(e, "0")))
        return;
    done = 1;
    fflush(stderr);
    if (setvbuf(stderr, NULL, _IOFBF, 1u << 20) == 0)
        CloseHandle(CreateThread(NULL, 0, stderr_flusher, NULL, 0, NULL));
}

void recomp_prof_start(void)
{
    static int started;
    const char *e = getenv("RECOMP_PROFILE");
    HMODULE m;
    IMAGE_NT_HEADERS *nt;
    stderr_buffer_init();
    if (started || !e || !*e || !strcmp(e, "0"))
        return;
    started = 1;
    m = GetModuleHandleA(NULL);
    nt = (IMAGE_NT_HEADERS *)((uint8_t *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    s_base = (uintptr_t)m;
    s_size = nt->OptionalHeader.SizeOfImage;
    s_pt = (ProfThread *)calloc(PROF_THREADS, sizeof *s_pt);
    if (!s_pt)
        return;
    watch_init();
    CloseHandle(CreateThread(NULL, 0, prof_main, NULL, 0, NULL));
    fprintf(stderr, "[PROF] sampling profiler on (exe base %p)\n", (void *)m);
}
#else
void recomp_prof_start(void) {}
void recomp_watch_dump(void) {}
#endif
