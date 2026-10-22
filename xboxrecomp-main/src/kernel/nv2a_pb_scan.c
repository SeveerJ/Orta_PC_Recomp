/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
typedef unsigned long DWORD;
static DWORD GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#endif

/* Wall time spent executing pushbuffers (drawing), for the fps report. */
double g_pb_draw_seconds;

#ifndef XBOX_CONTIG_BASE
#define XBOX_CONTIG_BASE 0x80000000u   /* physical P is at this + P */
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;

/* Where executor time goes, by method (RECOMP_PB_METHOD_TIME=1). Each
 * method call is timed with the cycle counter; once a minute the dozen most
 * expensive print with their share of time spent inside nv2a_pb_run. */
#if defined(_WIN32)
#include <intrin.h>
static int s_mt_on = -1;
static uint64_t s_mt_cyc[2048], s_mt_cnt[2048], s_mt_run_cyc;
static void exec_timed(uint32_t subch, uint32_t m, uint32_t param)
{
    uint64_t t0;
    static uint32_t tick;
    /* Timed one call in 16 (and counted 16 times): two rdtsc around each of
     * the ~2.5 million methods a second cost more than many methods do. */
    if (!s_mt_on || (++tick & 15) != 0) {
        nv2a_pb_exec_method(subch, m, param);
        return;
    }
    t0 = __rdtsc();
    nv2a_pb_exec_method(subch, m, param);
    s_mt_cyc[(m >> 2) & 2047] += (__rdtsc() - t0) * 16;
    s_mt_cnt[(m >> 2) & 2047] += 16;
}
static void method_time_report(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    int n, j;
    char line[1400];
    int len;
    if (!s_mt_on || now - last < 60000)
        return;
    last = now;
    if (!s_mt_run_cyc)
        return;
    len = snprintf(line, sizeof line, "[PBTIME] share of executor time by method:");
    for (n = 0; n < 12 && len < (int)sizeof line - 40; n++) {
        int best = -1;
        for (j = 0; j < 2048; j++)
            if (s_mt_cyc[j] && (best < 0 || s_mt_cyc[j] > s_mt_cyc[best]))
                best = j;
        if (best < 0)
            break;
        len += snprintf(line + len, sizeof line - len, " %04X:%.1f%%(x%llu)", best << 2,
                        100.0 * (double)s_mt_cyc[best] / (double)s_mt_run_cyc,
                        (unsigned long long)s_mt_cnt[best]);
        s_mt_cyc[best] = 0;
    }
    {
        extern uint64_t g_pbt[8];
        double r = (double)s_mt_run_cyc;
        len += snprintf(line + len, sizeof line - len,
                        " | program batches: setup %.1f%% fetch %.1f%% state %.1f%% d3d %.1f%%"
                        " | raster_batch %.1f%% (program %.1f%%, other %.1f%%)",
                        100.0 * (double)g_pbt[0] / r, 100.0 * (double)g_pbt[1] / r,
                        100.0 * (double)g_pbt[2] / r, 100.0 * (double)g_pbt[3] / r,
                        100.0 * (double)g_pbt[4] / r, 100.0 * (double)g_pbt[5] / r,
                        100.0 * (double)g_pbt[6] / r);
        memset(g_pbt, 0, sizeof g_pbt);
    }
    if (len < (int)sizeof line - 2) { line[len++] = '\n'; line[len] = 0; }
    fputs(line, stderr);
    memset(s_mt_cyc, 0, sizeof s_mt_cyc);
    memset(s_mt_cnt, 0, sizeof s_mt_cnt);
    s_mt_run_cyc = 0;
}
#else
#define exec_timed nv2a_pb_exec_method
static void method_time_report(void) {}
#endif

/* Direct index into s_seen: every executed method lands here, and a linear
 * search over the table was 15% of the executor thread. 0 = not seen yet. */
static uint16_t s_seen_ix[8][2048];

static void note(uint32_t subch, uint32_t method)
{
    uint32_t mi = (method >> 2) & 2047u, si = subch & 7u;
    int i = (int)s_seen_ix[si][mi] - 1;
    if (i >= 0 && i < s_seen_count && s_seen[i].method == method && s_seen[i].subch == subch) {
        s_seen[i].count++;
        return;
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        s_seen_ix[si][mi] = (uint16_t)s_seen_count;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0) {
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk commands from va to end_va. Returns the words consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. This used to skip CALLs, which
 * was invisible until a title used them for real: RenderWare on the Xbox
 * compiles static geometry into pushbuffers and draws it with a CALL, so the
 * whole race track went missing from Burnout 3's main view while the
 * geometry it submits inline (environment maps, the HUD) drew fine.
 * The NV2A has one level of subroutine, so a CALL inside a call is not
 * followed. Addresses are physical, like PUT's, and reach guest memory the
 * same way: through the contiguous window. */
static uint32_t pb_walk(uint32_t va, uint32_t end_va, int in_call,
                        uint32_t *jumps, uint32_t *unknown)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;

    while (va < end_va && words < 0x100000u) {
        uint32_t w;
        /* A jump or call into physical memory past the 64 MB the console has
         * is a corrupt or stale command word, not a place to read -- Orta hit
         * one at physical 0x0EB0F0B0 and took the process down from here. */
        if (va >= XBOX_CONTIG_BASE && va - XBOX_CONTIG_BASE >= 0x04000000u) {
            static int warned;
            if (warned++ < 4)
                fprintf(stderr, "[PB] walk left RAM at 0x%08X -- stopping this segment\n", va);
            break;
        }
        w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            (*jumps)++;
            if (!in_call)
                break;                        /* the ring: a jump ends it */
            va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            end_va = va + 0x400000u;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {  /* RETURN */
            if (in_call)
                break;
            continue;
        }
        if ((w & 3u) == 2u) {                    /* CALL */
            if (!in_call) {
                uint32_t sub = XBOX_CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
                s_tot_calls++;
                words += pb_walk(sub, sub + 0x400000u, 1, jumps, unknown);
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        (*unknown)++;
    }
    return words;
}

/* Run the ring the way the GPU does: from where it got to (GET) until it
 * reaches PUT, following every JUMP.
 *
 * nv2a_pb_scan walks [start, end) and stops at the first JUMP, which is right
 * for the jump at the ring's end only if the caller has split the segment at
 * the wrap by itself -- and it had to guess where the ring ends. Any guess
 * that falls short leaves commands unexecuted, and among them D3D's fence
 * releases: the title then waits forever for GPU time that never arrives.
 * Here there is nothing to guess: the GPU's own rule is "execute until GET
 * equals PUT", and a JUMP just moves GET. Returns the new GET (a VA in the
 * contiguous window). */
static uint32_t s_run_left, s_run_subch, s_run_method;
static int s_run_noninc;
/* The last command words the run decoded (address, word), and the last PUTs
 * it was handed: when GET ends up somewhere impossible, this says how. */
static uint32_t s_hist_va[64], s_hist_w[64], s_hist_n;
static uint32_t s_put_hist[16], s_put_n, s_backoff_gaveup;
uint32_t g_pb_ring_lo, g_pb_ring_hi;   /* ring bounds (VA), from the caller */
static uint32_t s_jmp_from[16], s_jmp_to[16], s_jmp_n;   /* jumps taken */
static uint32_t s_seg_get[16], s_seg_put[16], s_seg_n;   /* runs: start, PUT */

void nv2a_pb_dump_history(void)
{
                extern void recomp_watch_dump(void);
                uint32_t k;
                recomp_watch_dump();
                fprintf(stderr, "[PB]   recent PUTs:");
                for (k = 0; k < 16 && k < s_put_n; k++)
                    fprintf(stderr, " %08X", s_put_hist[(s_put_n - 16 + k) & 15]);
                fprintf(stderr, "\n[PB]   recent runs (GET->PUT):");
                for (k = 0; k < 16 && k < s_seg_n; k++) {
                    uint32_t j = (s_seg_n - (s_seg_n < 16 ? s_seg_n : 16) + k) & 15;
                    fprintf(stderr, " %08X->%08X", s_seg_get[j], s_seg_put[j]);
                }
                fprintf(stderr, "\n[PB]   recent jumps (from->to):");
                for (k = 0; k < 16 && k < s_jmp_n; k++) {
                    uint32_t j = (s_jmp_n - (s_jmp_n < 16 ? s_jmp_n : 16) + k) & 15;
                    fprintf(stderr, " %08X->%08X", s_jmp_from[j], s_jmp_to[j]);
                }
                fprintf(stderr, "\n[PB]   last decoded words (va:word):");
                for (k = 0; k < 64 && k < s_hist_n; k++) {
                    uint32_t j = (s_hist_n - (s_hist_n < 64 ? s_hist_n : 64) + k) & 63;
                    fprintf(stderr, "%s%08X:%08X", k % 6 ? " " : "\n[PB]    ",
                            s_hist_va[j], s_hist_w[j]);
                }
                fprintf(stderr, "\n");
    fflush(stderr);
}

void nv2a_pb_run_reset(void)
{
    s_run_left = 0;
}

uint32_t nv2a_pb_run(uint32_t get_va, uint32_t put_va)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = get_va, words = 0, jumps = 0, unknown = 0, hops = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (!s_exec_enabled)
        return put_va;
    s_put_hist[s_put_n++ & 15] = put_va;
    s_seg_get[s_seg_n & 15] = get_va;
    s_seg_put[s_seg_n & 15] = put_va;
    s_seg_n++;
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t0, t1;
    uint64_t run_c0;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    if (s_mt_on < 0) {
        const char *e = getenv("RECOMP_PB_METHOD_TIME");
        s_mt_on = e && *e && strcmp(e, "0") != 0;
    }
    run_c0 = __rdtsc();
#endif
    /* A method's data may continue past PUT: D3D kicks whenever it likes,
     * and the GPU keeps its place inside the method across kicks. Forget
     * that and the next pass starts on a data word as if it were a header --
     * vertex data read as commands, jumps to nowhere, and fences written to
     * garbage addresses (the cause of the freezes). */
    while (s_run_left && va != put_va) {
        if (va < XBOX_CONTIG_BASE || va - XBOX_CONTIG_BASE >= 0x04000000u)
            break;
        note(s_run_subch, s_run_method);
        exec_timed(s_run_subch, s_run_method, *(const uint32_t *)(mem + va));
        if (!s_run_noninc)
            s_run_method += 4;
        s_run_left--;
        va += 4;
        words++;
    }
    while (va != put_va && words < 0x400000u) {
        uint32_t w;
        if (va < XBOX_CONTIG_BASE || va - XBOX_CONTIG_BASE >= 0x04000000u) {
            static int warned;
            if (warned++ < 4) {
                fprintf(stderr, "[PB] GET left RAM at 0x%08X -- resyncing to PUT\n", va);
                nv2a_pb_dump_history();
            }
            va = put_va;
            s_run_left = 0;
            break;
        }
        w = *(const uint32_t *)(mem + va);
        /* Read ahead of the writer. The flight recorder caught it: a header
         * slot still 0 while the data after it was already there, so the
         * next data word (a float) decoded as a JUMP to nowhere. D3D had
         * moved PUT past words it had not finished writing yet. A zero word
         * where a header belongs, or a jump out of RAM, is therefore taken as
         * "not written yet": stop here, keep GET, and look again next tick.
         * Only if it is still that way 200 ms later is it believed. */
        {
            int suspect = (w == 0);
            if (!suspect && ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u)) {
                uint32_t t = (w & 3u) == 1u ? (w & 0xFFFFFFFCu) : (w & 0x1FFFFFFCu);
                uint32_t tva = XBOX_CONTIG_BASE | (t & 0x0FFFFFFFu);
                suspect = (t & 0x0FFFFFFFu) >= 0x04000000u;
                /* D3D only ever jumps within its ring. */
                if (!suspect && g_pb_ring_hi && (tva < g_pb_ring_lo || tva >= g_pb_ring_hi)) {
                    static int said;
                    suspect = 1;
                    if (said++ < 3) {
                        fprintf(stderr, "[PB] jump at %08X to %08X is outside the ring"
                                " %08X..%08X\n", va, tva, g_pb_ring_lo, g_pb_ring_hi);
                        nv2a_pb_dump_history();
                    }
                }
            }
            if (suspect) {
                static uint32_t wait_va;
                static DWORD wait_since;
                DWORD now = GetTickCount();
                if (wait_va != va) {
                    wait_va = va;
                    wait_since = now;
                }
                if (now - wait_since < 200)
                    break;                        /* retry from here */
                s_backoff_gaveup++;
                if (s_backoff_gaveup <= 3) {
                    fprintf(stderr, "[PB] gave up waiting on %08X at %08X (PUT %08X)\n",
                            w, va, put_va);
                    nv2a_pb_dump_history();
                }
                if (w != 0) {                     /* a bad jump: skip to PUT */
                    va = put_va;
                    s_run_left = 0;
                    break;
                }
            }
        }
        s_hist_va[s_hist_n & 63] = va;
        s_hist_w[s_hist_n & 63] = w;
        s_hist_n++;
        va += 4;
        words++;
        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {     /* JUMP */
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu) : (w & 0x1FFFFFFCu);
            jumps++;
            if (++hops > 256) {                      /* a jump loop: give up */
                va = put_va;
                break;
            }
            s_jmp_from[s_jmp_n & 15] = va - 4;
            s_jmp_to[s_jmp_n & 15] = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            s_jmp_n++;
            va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u)        /* RETURN outside a call */
            continue;
        if ((w & 3u) == 2u) {                        /* CALL */
            uint32_t sub = XBOX_CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
            s_tot_calls++;
            words += pb_walk(sub, sub + 0x400000u, 1, &jumps, &unknown);
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count = (w >> 18) & 0x7FFu, subch = (w >> 13) & 7u;
            uint32_t method = w & 0x1FFCu, i;
            int noninc = (w & 0xE0000000u) == 0x40000000u;
            for (i = 0; i < count && va != put_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                exec_timed(subch, m, *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            if (i < count) {                 /* PUT fell inside this method */
                s_run_left = count - i;
                s_run_subch = subch;
                s_run_method = noninc ? method : method + i * 4;
                s_run_noninc = noninc;
            }
            continue;
        }
        unknown++;
    }
#ifdef _WIN32
    QueryPerformanceCounter(&t1);
    g_pb_draw_seconds += (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    s_mt_run_cyc += __rdtsc() - run_c0;
    method_time_report();
#endif
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
    return va;
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    uint32_t words, jumps = 0, unknown = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    static int scan = -1;
    if (scan < 0)
        scan = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(scan || s_exec_enabled) || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

#ifdef _WIN32
    {
        static LARGE_INTEGER freq;
        LARGE_INTEGER t0, t1;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        words = pb_walk(start_va, end_va, 0, &jumps, &unknown);
        QueryPerformanceCounter(&t1);
        g_pb_draw_seconds += (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    }
#else
    words = pb_walk(start_va, end_va, 0, &jumps, &unknown);
#endif

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}
