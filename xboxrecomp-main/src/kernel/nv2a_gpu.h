/**
 * Opt-in D3D11 back end for the pushbuffer executor (nv2a_pb_exec.c).
 *
 * The executor decodes the NV2A command stream and, by default, rasterises
 * every triangle on the CPU straight into guest memory. With RECOMP_GPU=1 the
 * same triangles -- already transformed by the executor's vertex-program
 * interpreter -- are drawn by the host GPU instead: one render target per
 * guest colour surface, a depth buffer per zeta surface, and an uber pixel
 * shader that evaluates the NV2A register combiners exactly as
 * nv2a_combiner.c does.
 *
 * Guest memory stays the source of truth for everything outside the GPU:
 * a surface is uploaded when it is first drawn to (or when the CPU changed
 * it since), and written back at every flip and whenever something is about
 * to read it as a texture. So the framebuffer window, frame dumps and
 * render-to-texture all keep working unchanged.
 *
 * Any failure to initialise (no D3D11, no shader compiler, a shader that does
 * not compile) logs why and leaves the software rasteriser in charge.
 */
#ifndef XBOXRECOMP_NV2A_GPU_H
#define XBOXRECOMP_NV2A_GPU_H

#include <stdint.h>
#include "nv2a_combiner.h"

/* One texture stage, as the executor decoded it. */
typedef struct {
    uint32_t offset;          /* resolved guest address of texel (0,0) */
    uint32_t width, height, pitch, color;
    uint32_t addr_u, addr_v;
    uint32_t palette;
    uint32_t levels;
    uint32_t filter;
    int      cube;
    int      valid;
} Nv2aGpuTexDesc;

/* One vertex: screen-space x, y, z (in depth-buffer units) and clip w, then
 * everything the pixel shader interpolates. Same layout as Nv2aVshOutput,
 * except fog[0] carries the fog *factor* (fog_factor() already applied). */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog[4];
    float tex[4][4];
} Nv2aGpuVert;

/* Pixel pipeline modes. */
#define NV2A_GPU_MODE_2D     0   /* screen-space batch: flat colour or texel   */
#define NV2A_GPU_MODE_FF     1   /* program batch, no combiners: texel*diffuse */
#define NV2A_GPU_MODE_RC     2   /* program batch through the combiners        */

/* Everything about a batch that decides its pixels. Filled by the executor,
 * compared whole: a batch with the same state joins the previous draw. */
typedef struct {
    uint32_t surf_base;       /* resolved guest address of the colour surface */
    uint32_t pitch, bpp;
    uint32_t clip_x, clip_y, clip_w, clip_h;
    uint32_t zeta_offset, zeta_format;     /* SURFACE_FORMAT bits 4-7 */
    uint32_t mode;
    uint32_t blend_enable, blend_sfactor, blend_dfactor;
    uint32_t blend_equation, blend_color, color_mask;
    uint32_t depth_test, depth_func, depth_mask;
    uint32_t alpha_test, alpha_func, alpha_ref;
    uint32_t clip_plane_mode;
    uint32_t fog_enable, fog_mode;        /* for vertex programs on the GPU */
    float    fog_param[2];
    Nv2aCombiner rc;
    float    fog_color[4];    /* r g b a */
    uint32_t tex_norm[4];     /* stage coordinates are normalised (scale by size) */
    Nv2aGpuTexDesc tex[4];    /* .valid = 0 for a stage not sampled */
    /* Bump-env stages (texture modes 6/7): per stage the 2x2 matrix
     * m00 m01 m10 m11, then luminance scale and offset (RC mode only). */
    float    bump[4][6];
    uint32_t other_stage_input; /* SET_SHADER_OTHER_STAGE_INPUT: source stage of 2, 3 */
    uint32_t z_float;           /* SET_CONTROL0 Z_FORMAT: the depth buffer holds floats */
    float    vpy[2];            /* c58.y, c59.y: the vertex program viewport's y scale/offset */
    float    zclip[2];          /* SET_CLIP_MIN/MAX: the depth range, in the title's z units */
    uint32_t zcull;             /* SET_ZMIN_MAX_CONTROL: 1 = pixels outside it are dropped */
    float    zoff[2];           /* polygon offset when enabled: bias, slope factor (z units) */
    uint32_t cull;              /* 0 off, else SET_CULL_FACE: 0x404 front, 0x405 back */
    uint32_t front_ccw;         /* SET_FRONT_FACE: counter-clockwise triangles face front */
    uint32_t shadow_func;       /* SET_SHADOW_DEPTH_FUNC: 0 never .. 7 always */
    uint32_t prim_kind;         /* 0 triangles, 1 points, 2 lines (expanded by a geometry shader) */
    uint32_t pt_params_on;      /* SET_POINT_PARAMS_ENABLE: size by distance */
    uint32_t pt_sprite;         /* SET_POINT_SMOOTH_ENABLE: point sprites, stage 3 coords */
    float    pt_size;           /* SET_POINT_SIZE / 8, in guest pixels */
    float    pt_params[8];      /* SET_POINT_PARAMS */
} Nv2aGpuState;

/* Is the GPU back end on? Initialises on first call when RECOMP_GPU=1. */
int  nv2a_gpu_enabled(void);

/* Start a batch with this state (flushes the previous one if it differs). */
void nv2a_gpu_begin(const Nv2aGpuState *st);
/* One triangle of the current batch. */
void nv2a_gpu_tri(const Nv2aGpuVert *a, const Nv2aGpuVert *b, const Nv2aGpuVert *c);

/* CLEAR_SURFACE: colour inside the clip rectangle, depth over the whole zeta. */
void nv2a_gpu_clear(const Nv2aGpuState *st, int color, uint32_t argb,
                    int depth, uint32_t zstencil_clear);

/* End of frame: draw what is pending and write every touched surface back. */
void nv2a_gpu_flip(uint32_t present_base);
/* Write back any surface overlapping [base, base+bytes) / every surface. */
void nv2a_gpu_sync_range(uint32_t base, uint32_t bytes);
void nv2a_gpu_sync_all(void);

/* Vertex programs on the GPU (RECOMP_GPU_VSH=1): draw a whole program batch.
 * `in` is nverts x 16 attributes x float4; `idx` a triangle list into it;
 * program/constants as nv2a_vsh_program_info hands them out. */
int  nv2a_gpu_vsh_enabled(void);
/* Map room for nmax program-batch vertices (256 bytes each) in the GPU's
 * vertex ring and return it, or NULL. Draw them with
 * nv2a_gpu_draw_program(in = NULL), or give the room back with
 * nv2a_gpu_vtx_cancel. */
float *nv2a_gpu_vtx_begin(uint32_t nmax);
/* Cumulative texture-check time and texture uploads, for the hitch report. */
void nv2a_gpu_hitch_counters(double *tex_ms, uint32_t *uploads);
void nv2a_gpu_vtx_cancel(void);
void nv2a_gpu_draw_program(const Nv2aGpuState *st, const float *in, uint32_t nverts,
                           const uint32_t *idx, uint32_t nidx, const uint32_t *prog,
                           uint32_t start, const float *consts);

/* Visibility tests (SET_ZPASS_PIXEL_COUNT_ENABLE / GET_REPORT). */
void     nv2a_gpu_zpass_enable(uint32_t on);
uint32_t nv2a_gpu_zpass_collect(void);

/* Provided by nv2a_pb_exec.c: decode a whole texture through the software
 * sampler (so both paths agree on every format), and its raw footprint. */
int      nv2a_exec_tex_decode(const Nv2aGpuTexDesc *d, uint32_t face,
                              uint32_t *argb_out);
uint32_t nv2a_exec_tex_bytes(const Nv2aGpuTexDesc *d);
uint32_t nv2a_exec_tex_face_stride(const Nv2aGpuTexDesc *d);
int      nv2a_exec_tex_level(const Nv2aGpuTexDesc *d, uint32_t level,
                             Nv2aGpuTexDesc *out);

/* Compile the start-up shaders into the disk cache and return (Setup runs
 * the game with --prewarm). 1 on success. */
int nv2a_gpu_prewarm_shaders(void);

#endif
