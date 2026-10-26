/**
 * D3D11 back end for the pushbuffer executor. See nv2a_gpu.h.
 *
 * Stage 1: rasterisation, combiners, blending, depth and alpha test on the
 * GPU; vertex programs still run in the executor's interpreter. Everything
 * is loaded at run time (d3d11.dll, d3dcompiler_47.dll), so nothing new is
 * linked and a machine without them simply keeps the software path.
 */
#include "nv2a_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* Blits for the scaled renderer: box-filter a high-resolution surface down
 * to the guest's size (write-back), copy guest pixels up (upload), and draw
 * the shown frame into the window's swap chain. */
static const char s_hlsl_blit[] =
"Texture2DArray<float4> S : register(t0);\n"
"SamplerState L : register(s0);\n"
"cbuffer BC : register(b0) { int4 g_b; float4 g_f; float4 g_g; float4 g_o; float4 g_k; };\n"
"Texture2D<float4> O : register(t1);\n"
"float4 vs_blit(uint id : SV_VertexID) : SV_Position {\n"
"  float2 p = float2((id << 1) & 2, id & 2);\n"
"  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"}\n"
"float4 ps_down(float4 p : SV_Position) : SV_Target {\n"
"  int k = g_b.y;\n"
"  int2 b = int2(p.xy) * k;\n"
"  float4 s = 0;\n"
"  for (int y = 0; y < k; y++)\n"
"    for (int x = 0; x < k; x++)\n"
"      s += S.Load(int4(b.x + x, b.y + y, 0, 0));\n"
"  return s / (float)(k * k);\n"
"}\n"
"float4 ps_up(float4 p : SV_Position) : SV_Target {\n"
"  return S.Load(int4(int2(p.xy) / g_b.y, 0, 0));\n"
"}\n"
"float4 ps_present(float4 p : SV_Position) : SV_Target {\n"
"  float2 uv = (g_g.zw + (p.xy - g_f.xy) * g_f.zw * (1 - 2 * g_g.zw)) * g_g.xy;\n"
"  return float4(S.SampleLevel(L, float3(uv, 0), 0).rgb, 1);\n"
"}\n"
/* The NV2A video overlay (PVIDEO): movies are scaled onto the display by
 * the video unit, not drawn into the framebuffer. g_o is its rectangle on
 * the 640x480 display, g_k the colour key (w = enabled): where the key is
 * on, the movie shows only through pixels of that colour, so subtitles
 * drawn over it stay on top. */
"float4 ps_overlay(float4 p : SV_Position) : SV_Target {\n"
"  float2 uv = (g_g.zw + (p.xy - g_f.xy) * g_f.zw * (1 - 2 * g_g.zw)) * g_g.xy;\n"
"  float3 fb = S.SampleLevel(L, float3(uv, 0), 0).rgb;\n"
"  float2 q = ((g_g.zw + (p.xy - g_f.xy) * g_f.zw * (1 - 2 * g_g.zw)) * float2(640, 480) - g_o.xy) / g_o.zw;\n"
"  if (all(q >= 0) && all(q < 1) && (g_k.w < 0.5 || all(abs(fb - g_k.rgb) < 0.05)))\n"
"    return float4(O.SampleLevel(L, q, 0).rgb, 1);\n"
"  return float4(fb, 1);\n"
"}\n";

/* ── Shaders ─────────────────────────────────────────────────────────────
 * The pixel shader is nv2a_combiner.c and the executor's texture fetch,
 * line for line, so the two paths can be compared pixel for pixel. */
static const char s_hlsl[] =
"cbuffer CB : register(b0) {\n"
"  uint4 g_mode;\n"      /* x mode, y alpha test, z alpha func, w alpha ref */
"  uint4 g_rc;\n"        /* x control, y stage program, z final0, w final1 */
"  uint4 g_rc2;\n"       /* x final c0, y final c1, z clip plane mode */
"  uint4 g_cw[8];\n"     /* x colour icw, y colour ocw, z alpha icw, w alpha ocw */
"  uint4 g_fac[8];\n"    /* x factor0, y factor1 */
"  uint4 g_tex[4];\n"    /* x flags (1 usable 2 normalised 4 bilinear 8 cube 16 alpha one 32 mipmapped), y addr u, z addr v, w mip levels */
"  float4 g_tsize[4];\n"
"  float4 g_fogc;\n"
"  float4 g_clear;\n"
"  float4 g_bump[4];\n"   /* stage bump-env matrix m00 m01 m10 m11 */
"  float4 g_bumpl[4];\n"  /* x lum scale, y lum offset, z source stage */
"  float4 g_zc;\n"       /* x,y the depth range in D3D depth units, z 1 = drop pixels outside it */
"  float4 g_zo;\n"       /* polygon offset: x bias in D3D depth units, y slope factor per host pixel */
"};\n"
"#ifndef SPEC\n"
"#define RCX g_rc.x\n"
"#define RCY g_rc.y\n"
"#define RCZ g_rc.z\n"
"#define RCW g_rc.w\n"
"#define CW(s) g_cw[s]\n"
"#define RC_LOOP [loop]\n"
"#define MODEX g_mode.x\n"
"#endif\n"
"Texture2DArray<float4> T0 : register(t0);\n"
"Texture2DArray<float4> T1 : register(t1);\n"
"Texture2DArray<float4> T2 : register(t2);\n"
"Texture2DArray<float4> T3 : register(t3);\n"
"struct VSIN {\n"
"  float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
"  float4 fog : TEXCOORD4; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
"};\n"
"struct VSIO {\n"
"  float4 pos : SV_Position;\n"
"  noperspective float4 d0 : COLOR0;\n"
"  noperspective float4 d1 : COLOR1;\n"
"  noperspective float4 fog : TEXCOORD4;\n"
"  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
"  noperspective float4 n1 : TEXCOORD5; noperspective float4 n2 : TEXCOORD6;\n"
"  noperspective float4 n3 : TEXCOORD7;\n"
"  nointerpolation float4 sc : TEXCOORD8;\n"
"};\n"
"VSIO vs_main(VSIN i) {\n"
"  VSIO o;\n"
"  o.pos = i.pos; o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3;\n"
"  o.n1 = o.t1; o.n2 = o.t2; o.n3 = o.t3;\n"
"  o.sc = float4(0, 0, 0, 0);\n"
"  return o;\n"
"}\n"
"static float g_lod[4];\n"
"static float3 g_pix;\n"   /* this pixel in guest pixels, z: stage 1 coords per guest pixel (0: none) */
"float4 ld(uint s, int x, int y, uint f, uint m) {\n"
"  int4 c = int4(x, y, (int)f, (int)m);\n"
"  float4 r;\n"
"  [branch] if (s == 0) r = T0.Load(c);\n"
"  else if (s == 1) r = T1.Load(c);\n"
"  else if (s == 2) r = T2.Load(c);\n"
"  else r = T3.Load(c);\n"
"  return r;\n"
"}\n"
"int wrapc(int c, int n, uint mode) {\n"
"  if (n <= 0) return 0;\n"
"  if (mode == 1) return ((c % n) + n) % n;\n"
"  if (mode == 2) { int p = ((c % (2 * n)) + 2 * n) % (2 * n); return p < n ? p : 2 * n - 1 - p; }\n"
"  return clamp(c, 0, n - 1);\n"
"}\n"
"float4 texel(uint s, int x, int y, uint f, uint m) {\n"
"  int w = max((int)g_tsize[s].x >> m, 1), h = max((int)g_tsize[s].y >> m, 1);\n"
"  float4 r = ld(s, wrapc(x, w, g_tex[s].y), wrapc(y, h, g_tex[s].z), f, m);\n"
"  if (g_tex[s].x & 16) r.a = 1.0;\n"
"  return r;\n"
"}\n"
"float4 rc_level(uint s, uint f, float u, float v, uint m, uint fl) {\n"
"  float k = 1.0 / (float)(1u << m);\n"
"  u *= k; v *= k;\n"
"  if (fl & 4) {\n"
"    float fu = u - 0.5, fv = v - 0.5;\n"
"    int iu = (int)floor(fu), iv = (int)floor(fv);\n"
"    float wu = fu - (float)iu, wv = fv - (float)iv;\n"
"    float4 c0 = texel(s, iu, iv, f, m), c1 = texel(s, iu + 1, iv, f, m);\n"
"    float4 c2 = texel(s, iu, iv + 1, f, m), c3 = texel(s, iu + 1, iv + 1, f, m);\n"
"    return lerp(lerp(c0, c1, wu), lerp(c2, c3, wu), wv);\n"
"  }\n"
"  return texel(s, (int)floor(u), (int)floor(v), f, m);\n"
"}\n"
/* Mip selection as the min filter asks: nearest level (BOX/TENT_NEARESTLOD),
 * or the two nearest blended (BOX_TENT_LOD, TENT_TENT_LOD -- trilinear, as
 * xemu gets from GL). Nearest-only made alpha-tested cut-outs pop in and
 * out as their distance changed: the holes in Orta's Episode 2 boss shell
 * flickered (19 Oct). */
"float4 rc_texel(uint s, uint f, float u, float v) {\n"
"  uint fl = g_tex[s].x;\n"
"  if ((fl & 1) == 0) return float4(1, 1, 1, 1);\n"
"  if (g_rc2.w & 1) fl &= ~32u;\n"
"  if (fl & 2) { u *= g_tsize[s].x; v *= g_tsize[s].y; }\n"
"  else if (fl & 64) { u *= g_tsize[s].z; v *= g_tsize[s].z; }\n"
"  if ((fl & 32) && g_tex[s].w > 1) {\n"
"    int top = (int)g_tex[s].w - 1;\n"
"    float l = clamp(g_lod[s], 0.0, (float)top);\n"
"    if (fl & 128) {\n"
"      uint m0 = (uint)floor(l);\n"
"      uint m1 = min(m0 + 1u, (uint)top);\n"
"      float t = l - (float)m0;\n"
"      float4 a = rc_level(s, f, u, v, m0, fl);\n"
"      if (t <= 0.0 || m1 == m0) return a;\n"
"      return lerp(a, rc_level(s, f, u, v, m1, fl), t);\n"
"    }\n"
"    return rc_level(s, f, u, v, (uint)clamp((int)floor(g_lod[s] + 0.5), 0, top), fl);\n"
"  }\n"
"  return rc_level(s, f, u, v, 0u, fl);\n"
"}\n"
"bool stage_fetch(uint st, float4 c, out float4 o) {\n"
"  uint mode = (RCY >> (st * 5)) & 0x1F;\n"
"  float q = c.w != 0 ? c.w : 1.0;\n"
"  o = float4(0, 0, 0, 0);\n"
"  if (mode == 0) { o = float4(0, 0, 0, 1); return true; }\n"
"  if ((mode == 1 || mode == 2) && (g_tex[st].x & 256)) {\n"   /* shadow map (xemu psh.c) */
"    float mx = g_tsize[st].w;\n"
"    float d = rc_texel(st, 0, c.x / q, c.y / q).x * mx;\n"
"    float ref = mode == 2 ? clamp(c.z / q, 0.0, mx) : 0.0;\n"
"    uint fn = (uint)g_zo.z;\n"
"    bool r = fn == 7 || (fn == 1 && d < ref) || (fn == 2 && d == ref) || (fn == 3 && d <= ref)\n"
"          || (fn == 4 && d > ref) || (fn == 5 && d != ref) || (fn == 6 && d >= ref);\n"
"    o = r ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);\n"
"    return true;\n"
"  }\n"
"  if (mode == 1 || mode == 2) { o = rc_texel(st, 0, c.x / q, c.y / q); return true; }\n"
"  if (mode == 3) {\n"
"    float ax = abs(c.x), ay = abs(c.y), az = abs(c.z), ma, sc, tc;\n"
"    uint face;\n"
"    if ((g_tex[st].x & 8) == 0) { o = rc_texel(st, 0, c.x, c.y); return true; }\n"
"    if (ax >= ay && ax >= az) { face = c.x > 0 ? 0 : 1; ma = ax; sc = c.x > 0 ? -c.z : c.z; tc = -c.y; }\n"
"    else if (ay >= az) { face = c.y > 0 ? 2 : 3; ma = ay; sc = c.x; tc = c.y > 0 ? c.z : -c.z; }\n"
"    else { face = c.z > 0 ? 4 : 5; ma = az; sc = c.z > 0 ? c.x : -c.x; tc = -c.y; }\n"
"    if (ma == 0) ma = 1.0;\n"
"    o = rc_texel(st, face, (sc / ma + 1.0) * 0.5, (tc / ma + 1.0) * 0.5);\n"
"    return true;\n"
"  }\n"
"  if (mode == 4) { o = saturate(c); return true; }\n"
"  if (mode == 5) {\n"
"    [unroll] for (uint j = 0; j < 4; j++) {\n"
"      uint ge = (g_rc2.z >> (st * 4 + j)) & 1;\n"
"      if (ge ? c[j] >= 0 : c[j] < 0) return false;\n"
"    }\n"
"    return true;\n"
"  }\n"
"  return true;\n"
"}\n"
"float2 bump_signed(float2 v) {\n"   /* texels arrive offset by 128 */
"  return (v * 255.0 - 128.0) / 127.0;\n"
"}\n"
/* BUMPENVMAP (6) and _LUM (7): the source stage's two signed channels,
 * through this stage's 2x2 matrix, offset this stage's coordinates (xemu:
 * dsdt = M * src.bg, no projective divide). Water and heat haze. */
"bool stage_fetch_b(uint st, float4 c, float4 p0, float4 p1, float4 p2, out float4 o) {\n"
"  uint mode = (RCY >> (st * 5)) & 0x1F;\n"
"  if (mode == 6 || mode == 7) {\n"
"    uint src = (uint)g_bumpl[st].z;\n"
"    float4 tin = src == 0 ? p0 : (src == 1 ? p1 : p2);\n"
"    float fw = fmod(g_bumpl[st].w, 16.0);\n"
"    bool px = fw > 7.5; if (px) fw -= 8.0;\n"
"    if (px && st == 1 && g_pix.z > 0.0) c = float4(g_pix.xy * g_pix.z, c.z, 1.0);\n"
"    float2 ds = bump_signed(tin.bg);\n"
"    float2 d = float2(g_bump[st].x * ds.x + g_bump[st].z * ds.y,\n"
"                      g_bump[st].y * ds.x + g_bump[st].w * ds.y);\n"
"    float pm = fw > 3.5 ? fw - 4.0 : fw;\n"
"    float q = (pm != 0 && c.w != 0) ? c.w : 1.0;\n"
"    float2 uv = pm > 1.5 ? (c.xy + d) / q : c.xy / q + d;\n"
"    o = rc_texel(st, 0, uv.x, uv.y);\n"
"    if (mode == 7) o.rgb *= saturate(tin.r * g_bumpl[st].x + g_bumpl[st].y);\n"
"    return true;\n"
"  }\n"
"  return stage_fetch(st, c, o);\n"
"}\n"
"float map_in(float x, uint b) {\n"
"  float p = max(x, 0);\n"
"  uint m = (b >> 5) & 7;\n"
"  if (m == 0) return p;\n"
"  if (m == 1) return 1 - saturate(x);\n"
"  if (m == 2) return 2 * p - 1;\n"
"  if (m == 3) return -2 * p + 1;\n"
"  if (m == 4) return p - 0.5;\n"
"  if (m == 5) return -p + 0.5;\n"
"  if (m == 6) return x;\n"
"  return -x;\n"
"}\n"
"float map_out(float x, uint m) {\n"
"  if (m == 0x08) return x - 0.5;\n"
"  if (m == 0x10) return x * 2;\n"
"  if (m == 0x18) return (x - 0.5) * 2;\n"
"  if (m == 0x20) return x * 4;\n"
"  if (m == 0x30) return x * 0.5;\n"
"  return x;\n"
"}\n"
"float3 map_out3(float3 v, uint m) {\n"
"  return clamp(float3(map_out(v.x, m), map_out(v.y, m), map_out(v.z, m)), -1.0, 1.0);\n"
"}\n"
"float4 unpackc(uint c) {\n"
"  return float4((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24) / 255.0;\n"
"}\n"
"static float4 R[16];\n"
"float3 read_rgb(uint b) {\n"
"  float4 r = R[b & 15];\n"
"  float3 v = (b & 0x10) ? r.aaa : r.rgb;\n"
"  return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));\n"
"}\n"
"float read_a(uint b) {\n"
"  float4 r = R[b & 15];\n"
"  return map_in((b & 0x10) ? r.a : r.b, b);\n"
"}\n"
"float4 rc_eval(float4 v0, float4 v1, float4 fog, float4 t0, float4 t1, float4 t2, float4 t3) {\n"
"  uint n = min(RCX & 0xFFu, 8u), flags = RCX >> 8, s, d;\n"
"  [unroll] for (s = 0; s < 16; s++) R[s] = float4(0, 0, 0, 0);\n"
"  R[3] = fog; R[4] = v0; R[5] = v1;\n"
"  R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
"  R[12].a = (RCY & 0x1F) ? t0.a : 1.0;\n"
"  RC_LOOP for (s = 0; s < n; s++) {\n"
"    uint icw = CW(s).x, ocw = CW(s).y, aicw = CW(s).z, aocw = CW(s).w;\n"
"    uint fl = ocw >> 12, afl = aocw >> 12;\n"
"    R[1] = unpackc(g_fac[(flags & 0x010) ? s : 0u].x);\n"
"    R[2] = unpackc(g_fac[(flags & 0x100) ? s : 0u].y);\n"
"    bool mux = (flags & 1) ? (R[12].a >= 0.5) : ((((int)(R[12].a * 255.0)) & 1) != 0);\n"
"    float3 A = read_rgb(icw >> 24), B = read_rgb(icw >> 16);\n"
"    float3 C = read_rgb(icw >> 8), D = read_rgb(icw);\n"
"    float aA = read_a(aicw >> 24), aB = read_a(aicw >> 16);\n"
"    float aC = read_a(aicw >> 8), aD = read_a(aicw);\n"
"    float dab = dot(A, B), dcd = dot(C, D);\n"
"    float3 pab = (fl & 2) ? float3(dab, dab, dab) : A * B;\n"
"    float3 pcd = (fl & 1) ? float3(dcd, dcd, dcd) : C * D;\n"
"    float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"    float3 ab = map_out3(pab, fl & 0x38), cd = map_out3(pcd, fl & 0x38), ms = map_out3(sum, fl & 0x38);\n"
"    float apab = aA * aB, apcd = aC * aD;\n"
"    float asum = (afl & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"    float aab = clamp(map_out(apab, afl & 0x38), -1.0, 1.0);\n"
"    float acd = clamp(map_out(apcd, afl & 0x38), -1.0, 1.0);\n"
"    float ams = clamp(map_out(asum, afl & 0x38), -1.0, 1.0);\n"
"    d = (ocw >> 4) & 15; if (d != 0) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.z; }\n"
"    d = ocw & 15;        if (d != 0) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.z; }\n"
"    d = (ocw >> 8) & 15; if (d != 0) R[d].rgb = ms;\n"
"    d = (aocw >> 4) & 15; if (d != 0) R[d].a = aab;\n"
"    d = aocw & 15;        if (d != 0) R[d].a = acd;\n"
"    d = (aocw >> 8) & 15; if (d != 0) R[d].a = ams;\n"
"    R[0] = float4(0, 0, 0, 0);\n"
"  }\n"
"  float4 outc;\n"
"  if (RCZ != 0 || RCW != 0) {\n"
"    uint f0 = RCZ, f1 = RCW, ff = f1 & 0xFF;\n"
"    R[1] = unpackc(g_rc2.x); R[2] = unpackc(g_rc2.y);\n"
"    float3 va = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"    float3 vb = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"    R[14] = float4((ff & 0x80) ? saturate(va + vb) : va + vb, 0);\n"
"    float3 E = read_rgb(f1 >> 24), F = read_rgb(f1 >> 16);\n"
"    R[15] = float4(E * F, 0);\n"
"    float3 A = read_rgb(f0 >> 24), B = read_rgb(f0 >> 16);\n"
"    float3 C = read_rgb(f0 >> 8), D = read_rgb(f0);\n"
"    outc.rgb = D + C * (1.0 - A) + B * A;\n"
"    outc.a = read_a(f1 >> 8);\n"
"  } else {\n"
"    outc = R[12];\n"
"  }\n"
"  return saturate(outc);\n"
"}\n"
"bool atest(float a) {\n"
"  int v = (int)(a * 255.0 + 0.5), r = (int)(g_mode.w & 255);\n"
"  uint f = g_mode.z;\n"
"  if (f == 0x200) return false;\n"
"  if (f == 0x201) return v < r;\n"
"  if (f == 0x202) return v == r;\n"
"  if (f == 0x203) return v <= r;\n"
"  if (f == 0x204) return v > r;\n"
"  if (f == 0x205) return v != r;\n"
"  if (f == 0x206) return v >= r;\n"
"  return true;\n"
"}\n"
"float lod_of(float4 t, uint s, bool project) {\n"
"  float q = (project && t.w != 0) ? t.w : 1.0;\n"
"  float2 uv = t.xy / q;\n"
"  if (g_tex[s].x & 2) uv *= g_tsize[s].xy;\n"
"  float2 dx = ddx(uv), dy = ddy(uv);\n"
"  return 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-12));\n"
"}\n"
"float4 ps_main(VSIO i) : SV_Target {\n"
"  uint mode = MODEX;\n"
"  float4 col;\n"
"  g_lod[0] = lod_of(i.t0, 0, mode == 2);\n"
"  g_lod[1] = lod_of(i.t1, 1, true);\n"
"  g_lod[2] = lod_of(i.t2, 2, true);\n"
"  g_lod[3] = lod_of(i.t3, 3, true);\n"
"  g_pix = float3(i.pos.xy / max(g_zc.w, 1.0), i.sc.x);\n"
"  if (mode == 3) return g_clear;\n"
"  if (mode == 0) {\n"
"    col = saturate(i.d0);\n"
"    if (g_tex[0].x & 1)\n"
"      col = texel(0, (int)floor(max(i.t0.x, 0)), (int)floor(max(i.t0.y, 0)), 0, 0);\n"
"    return col;\n"
"  }\n"
"  if (mode == 1) {\n"
"    col = saturate(i.d0);\n"
"    if (g_tex[0].x & 1) {\n"

"      col = saturate(rc_texel(0, 0, i.t0.x, i.t0.y) * i.d0);\n"
"    }\n"
"    if (g_mode.y != 0 && !atest(col.a)) discard;\n"
"    return col;\n"
"  }\n"
"  if (g_rc2.w & 2) {\n"
"    float q = i.t0.w != 0 ? i.t0.w : 1.0;\n"
"    float2 uv = i.t0.xy / q;\n"
"    if ((g_tex[0].x & 2) == 0) uv /= max(g_tsize[0].xy, 1.0);\n"
"    return float4(frac(uv), (g_tex[0].x & 1) ? 0.0 : 1.0, 1);\n"
"  }\n"
"  if (g_zc.z > 0.5 && (i.pos.z < g_zc.x || i.pos.z > g_zc.y)) discard;\n"
"  float4 t0 = float4(0, 0, 0, 1), t1 = t0, t2 = t0, t3 = t0;\n"
"  if (!stage_fetch(0, i.t0, t0)) discard;\n"
"  if (!stage_fetch_b(1, fmod(g_bumpl[1].w, 8.0) > 3.5 ? i.n1 : i.t1, t0, t0, t0, t1)) discard;\n"
"  if (!stage_fetch_b(2, fmod(g_bumpl[2].w, 8.0) > 3.5 ? i.n2 : i.t2, t0, t1, t1, t2)) discard;\n"
"  if (!stage_fetch_b(3, fmod(g_bumpl[3].w, 8.0) > 3.5 ? i.n3 : i.t3, t0, t1, t2, t3)) discard;\n"
"  col = rc_eval(i.d0, i.d1, float4(g_fogc.rgb, i.fog.x), t0, t1, t2, t3);\n"
"  if (g_mode.y != 0 && !atest(col.a)) discard;\n"
"  return col;\n"
"}\n"
"struct PSZ { float4 c : SV_Target; float d : SV_Depth; };\n"
"PSZ ps_zoff(VSIO i) {\n"   /* ps_main with the polygon offset added to its depth */
"  PSZ o;\n"
"  float dz = max(abs(ddx(i.pos.z)), abs(ddy(i.pos.z)));\n"
"  o.c = ps_main(i);\n"
"  o.d = saturate(i.pos.z + g_zo.x + g_zo.y * dz);\n"
"  return o;\n"
"}\n";

/* The NV2A vertex program, interpreted per vertex on the GPU: the same
 * decode as nv2a_vsh_interp.c. Program words and constants arrive in
 * cbuffers, so one compiled shader serves every program a title loads. */
static const char s_hlsl_vsh[] =
"cbuffer VP : register(b1) {\n"
"  uint4 g_prog[136];\n"
"  uint4 g_vinfo;\n"     /* x start slot, y fog enable, z fog mode, w combiners on */
"  float4 g_vpar;\n"     /* 2/width, 2/height, depth scale */
"  float4 g_fogp;\n"
"  float4 g_lb;\n"      /* letterbox lift: x scale, y centre, z texture centre (texels), w on */
"  float4 g_pt;\n"      /* points: x size (px), y params on, z sprite on; w line width (px) */
"  float4 g_ptp[2];\n"  /* SET_POINT_PARAMS */
"};\n"
"cbuffer VC : register(b2) { float4 g_c[192]; };\n"
"struct VSIO {\n"
"  float4 pos : SV_Position;\n"
"  noperspective float4 d0 : COLOR0;\n"
"  noperspective float4 d1 : COLOR1;\n"
"  noperspective float4 fog : TEXCOORD4;\n"
"  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"  float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
"  noperspective float4 n1 : TEXCOORD5; noperspective float4 n2 : TEXCOORD6;\n"
"  noperspective float4 n3 : TEXCOORD7;\n"
"  nointerpolation float4 sc : TEXCOORD8;\n"
"};\n"
"struct VIN16 {\n"
"  float4 v0 : V0;   float4 v1 : V1;   float4 v2 : V2;   float4 v3 : V3;\n"
"  float4 v4 : V4;   float4 v5 : V5;   float4 v6 : V6;   float4 v7 : V7;\n"
"  float4 v8 : V8;   float4 v9 : V9;   float4 v10 : V10; float4 v11 : V11;\n"
"  float4 v12 : V12; float4 v13 : V13; float4 v14 : V14; float4 v15 : V15;\n"
"};\n"
"static float4 VT[13];\n"   /* R0..R11, and oPos as 12 */
"static float4 VI[16];\n"
"static float4 VO[13];\n"
"static int g_a0;\n"
"uint fld(uint word, uint lo, uint w) { return (word >> lo) & ((1u << w) - 1u); }\n"
"float4 vsrc(uint4 ins, uint which) {\n"
"  uint neg, swz, mux, reg;\n"
"  if (which == 0) { neg = fld(ins.y, 8, 1); swz = fld(ins.y, 0, 8); mux = fld(ins.z, 26, 2); reg = fld(ins.z, 28, 4); }\n"
"  else if (which == 1) { neg = fld(ins.z, 25, 1); swz = fld(ins.z, 17, 8); mux = fld(ins.z, 11, 2); reg = fld(ins.z, 13, 4); }\n"
"  else { neg = fld(ins.z, 10, 1); swz = fld(ins.z, 2, 8); mux = fld(ins.w, 28, 2); reg = (fld(ins.z, 0, 2) << 2) | fld(ins.w, 30, 2); }\n"
"  float4 s;\n"
"  if (mux == 1) s = VT[reg <= 12 ? reg : 0];\n"
"  else if (mux == 2) s = VI[fld(ins.y, 9, 4)];\n"
"  else {\n"
"    int ci = (int)fld(ins.y, 13, 8);\n"
"    if (fld(ins.w, 1, 1) != 0) ci += g_a0;\n"
"    if (ci < 0 || ci >= 192) ci = 0;\n"
"    s = g_c[ci];\n"
"  }\n"
"  float4 r = float4(s[(swz >> 6) & 3], s[(swz >> 4) & 3], s[(swz >> 2) & 3], s[swz & 3]);\n"
"  return neg != 0 ? -r : r;\n"
"}\n"
"float4 wmask(float4 d, float4 v, uint m) {\n"
"  if (m & 8) d.x = v.x;\n"
"  if (m & 4) d.y = v.y;\n"
"  if (m & 2) d.z = v.z;\n"
"  if (m & 1) d.w = v.w;\n"
"  return d;\n"
"}\n"
/* NV2A arithmetic rules, as xemu's vsh-prog.c has them: anything times 0
 * is 0, even infinity or NaN (an RCP of 0 fed into a multiply). Without
 * this Orta's octopus laser came out with NaN colours and drew nothing. */
"bool4 isnan4(float4 v) { return (asuint(v) & 0x7FFFFFFFu) > 0x7F800000u; }\n"
"float4 xmul(float4 a, float4 b) {\n"
"  float4 r = a * b;\n"
"  bool4 za = (asuint(a) & 0x7FFFFFFFu) == 0u, zb = (asuint(b) & 0x7FFFFFFFu) == 0u;\n"
"  r.x = (za.x || zb.x) ? 0.0 : r.x; r.y = (za.y || zb.y) ? 0.0 : r.y;\n"
"  r.z = (za.z || zb.z) ? 0.0 : r.z; r.w = (za.w || zb.w) ? 0.0 : r.w;\n"
"  return r;\n"
"}\n"
"float4 mac_op(uint op, float4 a, float4 b, float4 c) {\n"
"  float4 r = a;\n"
"  float d;\n"
"  if (op == 2) r = xmul(a, b);\n"
"  else if (op == 3) r = a + c;\n"
"  else if (op == 4) r = xmul(a, b) + c;\n"
"  else if (op == 5) { d = dot(a.xyz, b.xyz); r = float4(d, d, d, d); }\n"
"  else if (op == 6) { d = dot(a.xyz, b.xyz) + b.w; r = float4(d, d, d, d); }\n"
"  else if (op == 7) { d = dot(a, b); r = float4(d, d, d, d); }\n"
"  else if (op == 8) r = float4(1.0, a.y * b.y, a.z, b.w);\n"
"  else if (op == 9) r = (a < b) ? a : b;\n"
"  else if (op == 10) r = (a >= b) ? a : b;\n"
"  else if (op == 11) r = (a < b) ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);\n"
"  else if (op == 12) r = (a >= b) ? float4(1, 1, 1, 1) : float4(0, 0, 0, 0);\n"
"  else if (op == 13) g_a0 = (int)floor(a.x + 0.001);\n"
"  return r;\n"
"}\n"
"float4 ilu_op(uint op, float4 c) {\n"
"  float x = c.x, f;\n"
"  float4 r = c;\n"
"  if (op == 2) { f = (x == 0) ? asfloat(0x7F800000u) : 1.0 / x; r = float4(f, f, f, f); }\n"
"  else if (op == 3) {\n"
"    f = 1.0 / x;\n"
"    if (abs(f) < 5.42101e-20) f = f < 0 ? -5.42101e-20 : 5.42101e-20;\n"
"    if (abs(f) > 1.884467e19) f = f < 0 ? -1.884467e19 : 1.884467e19;\n"
"    r = float4(f, f, f, f);\n"
"  }\n"
"  else if (op == 4) {\n"   /* RSQ: 0 -> infinity, infinity -> 0 */
"    uint ax = asuint(x) & 0x7FFFFFFFu;\n"
"    f = ax == 0u ? asfloat(0x7F800000u) : (ax == 0x7F800000u ? 0.0 : rsqrt(abs(x)));\n"
"    r = float4(f, f, f, f);\n"
"  }\n"
"  else if (op == 5) { f = floor(x); r = float4(exp2(f), x - f, exp2(x), 1.0); }\n"
"  else if (op == 6) {\n"
"    float ax = abs(x);\n"
"    if (ax == 0) r = float4(asfloat(0xFF800000u), 1.0, asfloat(0xFF800000u), 1.0);\n"
"    else { f = floor(log2(ax)); r = float4(f, ax / exp2(f), log2(ax), 1.0); }\n"
"  }\n"
"  else if (op == 7) {\n"
"    float nl = max(c.x, 0), nh = max(c.y, 0), p = clamp(c.w, -127.9961, 127.9961);\n"
"    r = float4(1.0, nl, c.x > 0 ? pow(nh, p) : 0.0, 1.0);\n"
"  }\n"
"  return r;\n"
"}\n"
"float fogf(float d) {\n"
"  if (g_vinfo.y == 0) return 1.0;\n"
"  float px = g_fogp.x, py = g_fogp.y, f;\n"
"  uint m = g_vinfo.z;\n"
"  if (m == 0x800 || m == 0x802) f = px + exp2(d * py * 16.0) - 1.5;\n"
"  else if (m == 0x801 || m == 0x803) f = px + exp2(-d * d * py * py * 32.0) - 1.5;\n"
"  else f = px + d * py - 1.0;\n"
"  if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n"
"  if (isnan(f)) f = 1.0;\n"
"  return saturate(f);\n"
"}\n"
"float4 ffrow(uint b, float4 v) {\n"
"  return float4(dot(g_c[b], v), dot(g_c[b + 1], v), dot(g_c[b + 2], v), dot(g_c[b + 3], v));\n"
"}\n"
"float3 csrc(uint m, float3 mat, float4 d, float4 s) { return m == 1 ? d.rgb : (m == 2 ? s.rgb : mat); }\n"
"void ff_light(float4 vd, float4 vs, float4 epos, float3 n, out float4 d0, out float4 d1) {\n"
"  uint lmask = asuint(g_c[137].x), cm = asuint(g_c[137].y);\n"
"  uint em = cm & 3, am = (cm >> 2) & 3, dm = (cm >> 4) & 3, sm = (cm >> 6) & 3;\n"
"  bool spec = asuint(g_c[137].z) != 0;\n"
"  float3 P = epos.xyz / (abs(epos.w) > 1e-20 ? epos.w : 1.0);\n"
"  float3 c0 = am != 0 ? g_c[136].rgb * csrc(am, 1, vd, vs) + g_c[138].rgb : g_c[136].rgb;\n"
"  if (em != 0) c0 += csrc(em, 0, vd, vs);\n"
"  float3 c1 = 0;\n"
"  [loop] for (uint l = 0; l < 8; l++) {\n"
"    uint mode = (lmask >> (2 * l)) & 3;\n"
"    if (mode == 0) continue;\n"
"    uint b = 64 + 9 * l;\n"
"    float3 VP, H;\n"
"    float att = 1.0;\n"
"    if (mode == 1) {\n"
"      VP = normalize(g_c[b + 4].xyz + 1e-20);\n"
"      H = normalize(g_c[b + 3].xyz + 1e-20);\n"
"    } else {\n"
"      VP = g_c[b + 7].xyz - P;\n"
"      float d = length(VP);\n"
"      VP /= max(d, 1e-20);\n"
"      H = normalize(VP + float3(0, 0, 1));\n"
"      float3 k = g_c[b + 8].xyz;\n"
"      att = 1.0 / max(k.x + k.y * d + k.z * d * d, 1e-20);\n"
"      if (g_c[b].w > 0 && d > g_c[b].w) att = 0;\n"
"      if (mode == 3) {\n"
"        float4 sd = g_c[b + 6];\n"
"        float inv = 1.0 / max(length(sd.xyz), 1e-20);\n"
"        float cphi = -inv * sd.w, ctheta = inv + cphi;\n"
"        float sdv = dot(sd.xyz, VP), rho = inv * sdv;\n"
"        if (rho <= cphi) att = 0;\n"
"        else if (rho <= ctheta) att *= saturate(sdv + sd.w);\n"
"      }\n"
"    }\n"
"    float ndl = max(dot(n, VP), 0.0);\n"
"    float pf = (spec && ndl > 0) ? pow(max(dot(n, H), 0.0), g_c[139].x) : 0.0;\n"
"    c0 += g_c[b].rgb * att * csrc(am, 1, vd, vs);\n"
"    c0 += g_c[b + 1].rgb * att * ndl * csrc(dm, 1, vd, vs);\n"
"    c1 += g_c[b + 2].rgb * att * pf * csrc(sm, 1, vd, vs);\n"
"  }\n"
"  d0 = float4(saturate(c0), dm != 0 ? (dm == 1 ? vd.a : vs.a) : g_c[136].w);\n"
"  d1 = float4(saturate(c1), vs.a);\n"
"}\n"
/* With the letterbox lifted, the title's own black bands -- untextured
 * black quads spanning the full width of the frame's top and bottom 16
 * lines -- would sit over the recovered picture. Their edge corners in
 * those lines are moved 17 lines further out (g_lb.w == 2: untextured
 * draws onto the 640x480 frame): the fixed bands leave the screen, and the
 * taller animated bars of the episode titles keep their visible shape --
 * dropping the corners instead turned those into wedges. */
"float lb_band(float2 sp, float3 c) {\n"
"  if (g_lb.w < 1.5 || max(c.r, max(c.g, c.b)) >= 0.02 || (sp.x > 1.0 && sp.x < 639.0)) return 0;\n"
"  return sp.y <= 17.0 ? -17.0 : (sp.y >= 463.0 ? 17.0 : 0.0);\n"
"}\n"
"VSIO vs_ff(VIN16 i) {\n"
"  VSIO o;\n"
"  uint tmen = asuint(g_c[44].x), amask = asuint(g_c[44].y);\n"
"  float4 pos = float4(i.v0.xyz, 1.0);\n"
"  float3 n = (amask & 4) != 0 ? i.v2.xyz : float3(0, 0, 1);\n"
"  float4 t, epos;\n"
"  float3 en;\n"
"  uint skin = asuint(g_c[49].x);\n"
"  if (skin == 0) {\n"
"    t = ffrow(0, pos);\n"
"    epos = ffrow(4, pos);\n"
"    en = normalize(g_c[8].xyz * n.x + g_c[9].xyz * n.y + g_c[10].xyz * n.z + 1e-20);\n"
"  } else {\n"   /* vertex blending, xemu vsh-ff.c: modes 1,3,5 derive the last weight */
"    uint cnt = skin == 1 ? 2 : (skin <= 4 ? (skin == 2 ? 2 : 3) : 4);\n"
"    bool mix = (skin & 1) != 0;\n"
"    float4 wv = (amask & 2) != 0 ? i.v1 : float4(1, 0, 0, 0);\n"
"    float wn = 1.0;\n"
"    float4 tp = float4(0, 0, 0, 0);\n"
"    float3 tn = float3(0, 0, 0);\n"
"    [unroll] for (uint k = 0; k < 4; k++) {\n"
"      if (k < cnt) {\n"
"        float wk = (mix && k == cnt - 1) ? wn : wv[k];\n"
"        wn -= wv[k];\n"
"        uint mb = k == 0 ? 4u : 140u + 4u * (k - 1u), ib = k == 0 ? 8u : 152u + 4u * (k - 1u);\n"
"        tp += ffrow(mb, pos) * wk;\n"
"        tn += (g_c[ib].xyz * n.x + g_c[ib + 1].xyz * n.y + g_c[ib + 2].xyz * n.z) * wk;\n"
"      }\n"
"    }\n"
"    epos = tp;\n"
"    t = ffrow(0, tp);\n"
"    en = normalize(tn + 1e-20);\n"
"  }\n"
"  t.xy += g_c[50].xy * t.w;\n"   /* viewport offset, after the divide (xemu vsh-ff.c) */
"  o.pos = float4(t.x * g_vpar.x - t.w, t.w - t.y * g_vpar.y, t.z * g_vpar.z, t.w);\n"
"  o.d0 = (amask & 8) != 0 ? i.v3 : float4(1, 1, 1, 1);\n"
"  o.d1 = (amask & 16) != 0 ? i.v4 : float4(0, 0, 0, 0);\n"
"  if (asuint(g_c[44].z) != 0) { float4 l0, l1; ff_light(o.d0, o.d1, epos, en, l0, l1); o.d0 = l0; o.d1 = l1; }\n"
"  { float dy = lb_band(t.xy / t.w, o.d0.rgb); if (dy != 0) o.pos.y -= dy * t.w * g_vpar.y; }\n"
"  o.fog = float4(g_vinfo.w != 0 ? fogf(abs(epos.z)) : 1.0, 0, 0, 0);\n"
"  float3 ev = normalize(epos.xyz + 1e-20);\n"
"  float3 rf = reflect(ev, en);\n"
"  float4 T[4];\n"
"  T[0] = i.v9; T[1] = i.v10; T[2] = i.v11; T[3] = i.v12;\n"
"  [unroll] for (uint s = 0; s < 4; s++) {\n"
"    float4 tc = (amask & (512u << s)) != 0 ? T[s] : float4(0, 0, 0, 1);\n"
"    [unroll] for (uint j = 0; j < 4; j++) {\n"
"      uint m = asuint(g_c[45 + s][j]);\n"
"      float4 pl = g_c[28 + s * 4 + j];\n"
"      if (m == 0x2400) tc[j] = dot(pl, epos);\n"
"      else if (m == 0x2401) tc[j] = dot(pl, pos);\n"
"      else if (m == 0x2402) {\n"
"        float mm = 2.0 * sqrt(rf.x * rf.x + rf.y * rf.y + (rf.z + 1.0) * (rf.z + 1.0)) + 1e-20;\n"
"        tc[j] = float4(rf.x / mm + 0.5, rf.y / mm + 0.5, 0.0, 1.0)[j];\n"
"      }\n"
"      else if (m == 0x8511) tc[j] = float4(en, 1.0)[j];\n"
"      else if (m == 0x8512) tc[j] = float4(rf, 1.0)[j];\n"
"    }\n"
"    if ((tmen & (1u << s)) != 0) tc = ffrow(12 + s * 4, tc);\n"
"    T[s] = tc;\n"
"  }\n"
"  o.t0 = T[0]; o.t1 = T[1]; o.t2 = T[2]; o.t3 = T[3];\n"
"  o.n1 = o.t1; o.n2 = o.t2; o.n3 = o.t3;\n"
"  float ps = g_pt.x;\n"   /* point size, xemu vsh-ff.c */
"  if (g_pt.y != 0) {\n"
"    float de = length(epos.xyz);\n"
"    float mn = min(g_ptp[1].w, 63.875), mx = min(g_ptp[0].w + mn, 63.875);\n"
"    ps = 1.0 / sqrt(max(g_ptp[0].x + g_ptp[0].y * de + g_ptp[0].z * de * de, 1e-20)) + g_ptp[1].z;\n"
"    ps = clamp(ps * g_ptp[0].w + g_ptp[1].w, mn, mx);\n"
"  }\n"
"  o.sc = float4(0, ps, 0, 0);\n"
"  return o;\n"
"}\n"
"VSIO vs_prog(VIN16 i) {\n"
"  VI[0] = i.v0; VI[1] = i.v1; VI[2] = i.v2; VI[3] = i.v3;\n"
"  VI[4] = i.v4; VI[5] = i.v5; VI[6] = i.v6; VI[7] = i.v7;\n"
"  VI[8] = i.v8; VI[9] = i.v9; VI[10] = i.v10; VI[11] = i.v11;\n"
"  VI[12] = i.v12; VI[13] = i.v13; VI[14] = i.v14; VI[15] = i.v15;\n"
"  uint k;\n"
"  [unroll] for (k = 0; k < 13; k++) { VT[k] = float4(0, 0, 0, 0); VO[k] = float4(0, 0, 0, 0); }\n"
"  VO[3].w = 1.0; VO[4].w = 1.0;\n"
"  VO[9].w = 1.0; VO[10].w = 1.0; VO[11].w = 1.0; VO[12].w = 1.0;\n"
"  g_a0 = 0;\n"
"  if (g_vinfo.x == 0xFFFFu) return vs_ff(i);\n"
"  [loop] for (uint s = g_vinfo.x; s < 136; s++) {\n"
"    uint4 ins = g_prog[s];\n"
"    uint mac = fld(ins.y, 21, 4), ilu = fld(ins.y, 25, 3);\n"
"    uint mmask = fld(ins.w, 24, 4), imask = fld(ins.w, 16, 4), tdst = fld(ins.w, 20, 4);\n"
"    uint omask = fld(ins.w, 12, 4), oaddr = fld(ins.w, 3, 8);\n"
"    float4 a = vsrc(ins, 0), b = vsrc(ins, 1), c = vsrc(ins, 2);\n"
"    float4 mres = float4(0, 0, 0, 0), ires = float4(0, 0, 0, 0);\n"
"    if (mac != 0) mres = mac_op(mac, a, b, c);\n"
"    if (ilu != 0) ires = ilu_op(ilu, c);\n"
"    if (mac != 0 && mac != 13 && mmask != 0 && tdst <= 12) VT[tdst] = wmask(VT[tdst], mres, mmask);\n"
"    if (ilu != 0 && imask != 0) {\n"
"      uint it = mac != 0 ? 1u : tdst;\n"
"      if (it <= 12) VT[it] = wmask(VT[it], ires, imask);\n"
"    }\n"
"    if (omask != 0 && fld(ins.w, 11, 1) != 0) {\n"
"      float4 src = fld(ins.w, 2, 1) != 0 ? ires : mres;\n"
"      if (oaddr == 0) VT[12] = wmask(VT[12], src, omask);\n"
"      else if (oaddr < 13) VO[oaddr] = wmask(VO[oaddr], src, omask);\n"
"    }\n"
"    if (fld(ins.w, 0, 1) != 0) break;\n"
"  }\n"
"  VSIO o;\n"
"  float4 p = VT[12];\n"
"  float sc = 0.0;\n"   /* stage 1 coords are this vertex's screen position * sc */
"  if (abs(VO[10].w - 1.0) < 1e-4 && abs(p.x) > 1.0) {\n"
"    float cx = VO[10].x / p.x;\n"
"    if (cx > 0.0 && abs(VO[10].y - p.y * cx) <= 1e-3 * max(1.0, abs(VO[10].y))) sc = cx;\n"
"  }\n"
"  float w = p.w;\n"
"  if (g_vpar.w > 0.5) p.xy += float2(g_vpar.w < 2.5 ? 1.0 / g_vpar.x : 0.0,\n"   /* RECOMP_REFL_CENTRE */
"                                      (g_vpar.w < 1.5 || g_vpar.w > 2.5) ? 1.0 / g_vpar.y : 0.0);\n"
"  p.y += lb_band(p.xy, VO[3].rgb);\n"
"  if (g_lb.w > 0.5 && g_lb.w < 1.5) {\n"
"    p.y = g_lb.y + (p.y - g_lb.y) * g_lb.x;\n"
"    float tc = (abs(VO[9].y) <= 2.0 * abs(VO[9].w) ? 0.5 : g_lb.z) * VO[9].w;\n"
"    VO[9].y = tc + (VO[9].y - tc) * g_lb.x;\n"
"  }\n"
"  o.pos = float4((p.x * g_vpar.x - 1.0) * w, (1.0 - p.y * g_vpar.y) * w, p.z * g_vpar.z * w, w);\n"
"  o.d0 = saturate(isnan4(VO[3]) ? float4(1, 1, 1, 1) : VO[3]);\n"   /* xemu: NaN colours are 1 */
"  o.d1 = saturate(isnan4(VO[4]) ? float4(1, 1, 1, 1) : VO[4]);\n"
"  o.fog = float4(g_vinfo.w != 0 ? fogf(VO[5].x) : 1.0, 0, 0, 0);\n"
"  o.t0 = VO[9]; o.t1 = VO[10]; o.t2 = VO[11]; o.t3 = VO[12];\n"
"  o.n1 = o.t1; o.n2 = o.t2; o.n3 = o.t3;\n"
"  o.sc = float4(sc, g_pt.y != 0 ? VO[6].x : g_pt.x, 0, 0);\n"   /* oPts */
"  return o;\n"
"}\n"
/* Points and lines become quads here: a point a square of its size about
 * its centre (stage 3 coordinates across it for sprites, as xemu's
 * gl_PointCoord), a line a strip one guest pixel wide. */
"[maxvertexcount(4)]\n"
"void gs_pt(point VSIO v[1], inout TriangleStream<VSIO> s) {\n"
"  VSIO o = v[0];\n"
"  float2 h = 0.5 * max(v[0].sc.y, 1.0) * g_vpar.xy * v[0].pos.w;\n"
"  [unroll] for (uint k = 0; k < 4; k++) {\n"
"    float2 c = float2((k & 1) != 0 ? 1.0 : -1.0, (k & 2) != 0 ? -1.0 : 1.0);\n"
"    o.pos = v[0].pos + float4(c * h, 0, 0);\n"
"    if (g_pt.z != 0) { o.t3 = float4((k & 1) != 0 ? 1.0 : 0.0, (k & 2) != 0 ? 1.0 : 0.0, 1, 1); o.n3 = o.t3; }\n"
"    s.Append(o);\n"
"  }\n"
"}\n"
"[maxvertexcount(4)]\n"
"void gs_ln(line VSIO v[2], inout TriangleStream<VSIO> s) {\n"
"  float4 a = v[0].pos, b = v[1].pos;\n"
"  if (a.w <= 0 || b.w <= 0) return;\n"
"  float2 d = (b.xy / b.w - a.xy / a.w) / g_vpar.xy;\n"
"  float l = length(d);\n"
"  float2 n = l > 1e-6 ? float2(-d.y, d.x) / l : float2(0, 1);\n"
"  n *= 0.5 * max(g_pt.w, 1.0) * g_vpar.xy;\n"
"  VSIO o = v[0];\n"
"  o.pos = a + float4(n * a.w, 0, 0); s.Append(o);\n"
"  o.pos = a - float4(n * a.w, 0, 0); s.Append(o);\n"
"  o = v[1];\n"
"  o.pos = b + float4(n * b.w, 0, 0); s.Append(o);\n"
"  o.pos = b - float4(n * b.w, 0, 0); s.Append(o);\n"
"}\n";

typedef struct {
    uint32_t prog[136][4];
    uint32_t vinfo[4];
    float    vpar[4];
    float    fogp[4];
    float    lb[4];
    float    pt[4];
    float    ptp[8];
} GpuVP;

/* Must match the cbuffer above, 16-byte rows. */
typedef struct {
    uint32_t mode[4];
    uint32_t rc[4];
    uint32_t rc2[4];
    uint32_t cw[8][4];
    uint32_t fac[8][4];
    uint32_t tex[4][4];
    float    tsize[4][4];
    float    fogc[4];
    float    clear[4];
    float    bump[4][4];
    float    bumpl[4][4];
    float    zc[4];
    float    zo[4];
} GpuCB;

typedef HRESULT (WINAPI *PfnCompile)(LPCVOID, SIZE_T, LPCSTR,
                                     const D3D_SHADER_MACRO *, ID3DInclude *,
                                     LPCSTR, LPCSTR, UINT, UINT,
                                     ID3DBlob **, ID3DBlob **);
/* Running totals for the hitch report (nv2a_gpu_hitch_counters). */
static LONGLONG s_tex_total;
static uint32_t s_uploads_total;
static PfnCompile s_spec_comp;      /* for the specialised combiner shaders */

/* ── Surfaces, depth buffers, textures ─────────────────────────────────── */

#define GPU_MAX_SURF 24
typedef struct { int b[4]; float f[4]; float g[4]; float o[4]; float k[4]; } BlitCB;
#define GPU_MAX_ZBUF 8
#define GPU_MAX_TEX  512
#define GPU_VB_VERTS (3 * 21845)

typedef struct {
    uint32_t base, pitch, bpp, w, h;
    ID3D11Texture2D *tex, *staging;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;        /* sampled directly as a texture */
    int dirty;
    uint32_t dx0, dy0, dx1, dy1;          /* touched since last write-back */
    uint64_t hash;                        /* guest bytes as last synced */
    uint32_t checked, used;
    uint32_t drawn;                       /* last frame the GPU drew into it */
    /* Asynchronous flip write-back: copies queued, mapped a frame later. */
    ID3D11Texture2D *ast[2];
    uint32_t aq[2];                       /* 1 = a copy is in flight */
    uint32_t ar[2][4];                    /* its rectangle */
    int anext;
    /* Scaled rendering (G.k > 1): tex is k times the guest's size; lo is
     * the guest-sized copy that write-backs and uploads go through. */
    ID3D11Texture2D *lo;
    ID3D11RenderTargetView *lo_rtv;
    ID3D11ShaderResourceView *lo_srv;
} GSurf;

typedef struct {
    uint32_t zeta, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *srv;            /* read as a shadow map */
    uint32_t used;
} GZbuf;

typedef struct {
    Nv2aGpuTexDesc d;
    uint64_t hash;
    uint32_t checked, used, slices, mips;
    int supported;
    DXGI_FORMAT fmt;                       /* B8G8R8A8, or BC1-3 for DXT */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} GTex;

static struct {
    int state;                            /* 0 untried, 1 on, -1 off */
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11PixelShader *ps_zoff;                /* ps_main + polygon offset (SV_Depth) */
    ID3D11InputLayout *il;
    ID3D11Buffer *vb, *cb;
    ID3D11Query *query;
    /* Vertex programs on the GPU (RECOMP_GPU_VSH). */
    int vsh_on;
    ID3D11VertexShader *vs_prog;
    ID3D11GeometryShader *gs_pt, *gs_ln;
    ID3D11InputLayout *il_prog;
    ID3D11Buffer *vb_prog, *ib_prog, *cb_vp, *cb_vc;
    int query_active, query_pending;
    uint32_t zpass_acc;

    GSurf surf[GPU_MAX_SURF];
    GZbuf zb[GPU_MAX_ZBUF];
    GTex  tex[GPU_MAX_TEX];
    uint32_t frame;

    /* The batch being built. */
    Nv2aGpuState cur;
    int cur_valid;
    GSurf *cur_surf;
    GZbuf *cur_zb;
    ID3D11ShaderResourceView *cur_srv[4];
    uint32_t cur_tfl[4];                  /* 1 usable, 16 alpha reads as 1 */
    uint32_t cur_tlev[4];                 /* mip levels on the GPU copy */
    float zscale;
    Nv2aGpuVert *verts;
    uint32_t nverts;

    /* Pipeline-state caches. */
    struct { D3D11_BLEND_DESC d; ID3D11BlendState *s; } bs[64];
    int nbs;
    struct { D3D11_DEPTH_STENCIL_DESC d; ID3D11DepthStencilState *s; } ds[32];
    int nds;
    ID3D11RasterizerState *rs;

    /* Internal resolution scale (RECOMP_GPU_SCALE) and the blits it needs;
     * the window's swap chain (RECOMP_GPU_PRESENT). */
    UINT k;
    ID3D11VertexShader *vs_blit;
    ID3D11PixelShader *ps_down, *ps_up, *ps_present, *ps_overlay;
    ID3D11Texture2D *ov_tex;                  /* PVIDEO overlay frame */
    ID3D11ShaderResourceView *ov_srv;
    UINT ov_w, ov_h;
    ID3D11Buffer *cb_blit;
    ID3D11RasterizerState *rs_blit;
    ID3D11SamplerState *smp;
    int present_on;
    IDXGISwapChain *swap;
    ID3D11RenderTargetView *bb_rtv;
    UINT sw_w, sw_h;

    /* Statistics, printed every few hundred frames. */
    uint32_t st_draws, st_tris, st_uploads, st_readbacks, st_surf_uploads, st_direct;
    LONGLONG t_flip, t_tex, t_zpass, t_draw;
    uint64_t st_upload_bytes;
} G;

static uint8_t *gmem(uint32_t a)
{
    return (uint8_t *)xbox_GetMemoryOffset() + a;
}

/* Is [a, a+n) guest memory the runtime actually maps? */
static int guest_range_ok(uint32_t a, uint32_t n)
{
    uint64_t e = (uint64_t)a + n;
    if (a >= 0x00010000u && e <= 0x04000000u)
        return 1;
    if (a >= 0x80000000u && e <= 0x84000000u)
        return 1;
    return 0;
}

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h)
{
    size_t i = 0;
    h ^= 0x9E3779B97F4A7C15ull + n;
    /* Four independent lanes: one multiply chain left the CPU waiting on
     * each multiply, and the per-frame texture checks were 1.8 ms a frame. */
    if (n >= 64) {
        uint64_t a = h, b = h ^ 0x5851F42D4C957F2Dull, c = h ^ 0x14057B7EF767814Full,
                 d = h ^ 0xD6E8FEB86659FD93ull;
        for (; i + 32 <= n; i += 32) {
            uint64_t v0, v1, v2, v3;
            memcpy(&v0, p + i, 8);
            memcpy(&v1, p + i + 8, 8);
            memcpy(&v2, p + i + 16, 8);
            memcpy(&v3, p + i + 24, 8);
            a = (a ^ v0) * 0x100000001B3ull; a ^= a >> 29;
            b = (b ^ v1) * 0x100000001B3ull; b ^= b >> 29;
            c = (c ^ v2) * 0x100000001B3ull; c ^= c >> 29;
            d = (d ^ v3) * 0x100000001B3ull; d ^= d >> 29;
        }
        h = a ^ (b * 0x9E3779B97F4A7C15ull) ^ (c * 0xC2B2AE3D27D4EB4Full)
              ^ (d * 0x165667B19E3779F9ull);
    }
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint64_t surf_hash(const GSurf *s)
{
    uint64_t h = 0;
    uint32_t y, row = s->w * s->bpp;
    const uint8_t *m = gmem(s->base);
    for (y = 0; y < s->h; y++)
        h = hash_bytes(m + (size_t)y * s->pitch, row, h);
    return h;
}

static void release(IUnknown *u)
{
    if (u)
        IUnknown_Release(u);
}
#define REL(x) do { release((IUnknown *)(x)); (x) = NULL; } while (0)

/* ── Init ──────────────────────────────────────────────────────────────── */

static ID3DBlob *compile_src(PfnCompile fn, const char *src, size_t len,
                             const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = fn(src, len, "nv2a_gpu.hlsl", NULL, NULL,
                    entry, target, (1u << 15) /* OPTIMIZATION_LEVEL3 */, 0,
                    &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU11] %s failed to compile (0x%08lX):\n%s\n", entry,
                (unsigned long)hr,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "(no log)");
        REL(err);
        REL(code);
        return NULL;
    }
    REL(err);
    return code;
}

static ID3DBlob *compile(PfnCompile fn, const char *entry, const char *target)
{
    return compile_src(fn, s_hlsl, sizeof s_hlsl - 1, entry, target);
}

static int vsh_init(PfnCompile comp)
{
    ID3DBlob *b = compile_src(comp, s_hlsl_vsh, sizeof s_hlsl_vsh - 1, "vs_prog", "vs_5_0");
    D3D11_INPUT_ELEMENT_DESC il[16];
    D3D11_BUFFER_DESC bd;
    HRESULT hr;
    int k;

    if (!b) {
        fprintf(stderr, "  [GPU11] vertex programs stay on the CPU\n");
        return 0;
    }
    for (k = 0; k < 16; k++) {
        il[k].SemanticName = "V";
        il[k].SemanticIndex = (UINT)k;
        il[k].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[k].InputSlot = 0;
        il[k].AlignedByteOffset = (UINT)(16 * k);
        il[k].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
        il[k].InstanceDataStepRate = 0;
    }
    hr = ID3D11Device_CreateVertexShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                         ID3D10Blob_GetBufferSize(b), NULL, &G.vs_prog);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(G.dev, il, 16, ID3D10Blob_GetBufferPointer(b),
                                            ID3D10Blob_GetBufferSize(b), &G.il_prog);
    REL(b);
    {   /* points and lines: optional, without them they draw one pixel wide */
        static const char *gsn[2] = { "gs_pt", "gs_ln" };
        ID3D11GeometryShader **gso[2] = { &G.gs_pt, &G.gs_ln };
        int g;
        for (g = 0; g < 2 && SUCCEEDED(hr); g++) {
            ID3DBlob *gb = compile_src(comp, s_hlsl_vsh, sizeof s_hlsl_vsh - 1, gsn[g], "gs_5_0");
            if (!gb || FAILED(ID3D11Device_CreateGeometryShader(G.dev,
                    ID3D10Blob_GetBufferPointer(gb), ID3D10Blob_GetBufferSize(gb), NULL, gso[g]))) {
                fprintf(stderr, "  [GPU11] %s unavailable: points/lines stay one pixel\n", gsn[g]);
                *gso[g] = NULL;
            }
            if (gb)
                REL(gb);
        }
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 65536u * 256u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.vb_prog);
    bd.ByteWidth = 6u * 65536u * 4u;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.ib_prog);
    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DYNAMIC;               /* see cb_write */
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = (UINT)sizeof(GpuVP);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.cb_vp);
    bd.ByteWidth = 192u * 16u;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.cb_vc);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU11] vertex-program objects failed 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    fprintf(stderr, "  [GPU11] vertex programs run on the GPU\n");
    return 1;
}

static int gpu_init(void)
{
    HMODULE d3d = LoadLibraryA("d3d11.dll"), dc;
    PFN_D3D11_CREATE_DEVICE create;
    PfnCompile comp;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0, got;
    ID3DBlob *vsb, *psb;
    HRESULT hr;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    static const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,   0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  64, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  80, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  96, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    if (!d3d) {
        fprintf(stderr, "  [GPU11] d3d11.dll not found -- software renderer\n");
        return 0;
    }
    create = (PFN_D3D11_CREATE_DEVICE)(void *)GetProcAddress(d3d, "D3D11CreateDevice");
    dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!dc) dc = LoadLibraryA("d3dcompiler_46.dll");
    if (!dc) dc = LoadLibraryA("d3dcompiler_43.dll");
    comp = dc ? (PfnCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!create || !comp) {
        fprintf(stderr, "  [GPU11] %s missing -- software renderer\n",
                create ? "D3DCompile (d3dcompiler_47.dll)" : "D3D11CreateDevice");
        return 0;
    }
    hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1,
                D3D11_SDK_VERSION, &G.dev, &got, &G.ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU11] D3D11CreateDevice failed 0x%08lX -- software renderer\n",
                (unsigned long)hr);
        return 0;
    }
    s_spec_comp = comp;
    vsb = compile(comp, "vs_main", "vs_5_0");
    psb = vsb ? compile(comp, "ps_main", "ps_5_0") : NULL;
    if (!vsb || !psb) {
        REL(vsb);
        return 0;
    }
    hr = ID3D11Device_CreateVertexShader(G.dev, ID3D10Blob_GetBufferPointer(vsb),
                                         ID3D10Blob_GetBufferSize(vsb), NULL, &G.vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(psb),
                                            ID3D10Blob_GetBufferSize(psb), NULL, &G.ps);
    if (SUCCEEDED(hr)) {
        ID3DBlob *zb = compile(comp, "ps_zoff", "ps_5_0");
        if (zb) {
            if (FAILED(ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(zb),
                                                      ID3D10Blob_GetBufferSize(zb), NULL, &G.ps_zoff)))
                G.ps_zoff = NULL;
            REL(zb);
        }
        if (!G.ps_zoff)
            fprintf(stderr, "  [GPU11] polygon-offset shader unavailable\n");
    }
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(G.dev, il, 8,
                                            ID3D10Blob_GetBufferPointer(vsb),
                                            ID3D10Blob_GetBufferSize(vsb), &G.il);
    REL(vsb);
    REL(psb);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU11] shader objects failed 0x%08lX\n", (unsigned long)hr);
        return 0;
    }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (UINT)(sizeof(Nv2aGpuVert) * GPU_VB_VERTS);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.vb);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = (UINT)sizeof(GpuCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.cb);
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;            /* clamp, as the software z test does not clip */
    rd.ScissorEnable = TRUE;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateRasterizerState(G.dev, &rd, &G.rs);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU11] buffers/states failed 0x%08lX\n", (unsigned long)hr);
        return 0;
    }
    {
        D3D11_QUERY_DESC qd;
        qd.Query = D3D11_QUERY_OCCLUSION;
        qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(G.dev, &qd, &G.query)))
            G.query = NULL;
    }
    G.verts = (Nv2aGpuVert *)malloc(sizeof(Nv2aGpuVert) * GPU_VB_VERTS);
    if (!G.verts)
        return 0;
    {
        /* RECOMP_GPU_SCALE: draw every surface this many times larger (1-6).
         * RECOMP_GPU_PRESENT=0: show frames through the old GDI window path. */
        const char *e = getenv("RECOMP_GPU_SCALE");
        const char *pe = getenv("RECOMP_GPU_PRESENT");
        int k = e ? atoi(e) : 1;
        ID3DBlob *b;
        D3D11_SAMPLER_DESC smd;
        G.k = (UINT)(k < 1 ? 1 : (k > 6 ? 6 : k));
        G.present_on = !(pe && *pe == '0');
        b = compile_src(comp, s_hlsl_blit, sizeof s_hlsl_blit - 1, "vs_blit", "vs_5_0");
        hr = b ? ID3D11Device_CreateVertexShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                                 ID3D10Blob_GetBufferSize(b), NULL, &G.vs_blit)
               : E_FAIL;
        REL(b);
        if (SUCCEEDED(hr)) {
            b = compile_src(comp, s_hlsl_blit, sizeof s_hlsl_blit - 1, "ps_down", "ps_5_0");
            hr = b ? ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                                    ID3D10Blob_GetBufferSize(b), NULL, &G.ps_down)
                   : E_FAIL;
            REL(b);
        }
        if (SUCCEEDED(hr)) {
            b = compile_src(comp, s_hlsl_blit, sizeof s_hlsl_blit - 1, "ps_up", "ps_5_0");
            hr = b ? ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                                    ID3D10Blob_GetBufferSize(b), NULL, &G.ps_up)
                   : E_FAIL;
            REL(b);
        }
        if (SUCCEEDED(hr)) {
            b = compile_src(comp, s_hlsl_blit, sizeof s_hlsl_blit - 1, "ps_present", "ps_5_0");
            hr = b ? ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                                    ID3D10Blob_GetBufferSize(b), NULL, &G.ps_present)
                   : E_FAIL;
            REL(b);
        }
        if (SUCCEEDED(hr)) {
            /* Optional: without it, movies just stay hidden as before. */
            b = compile_src(comp, s_hlsl_blit, sizeof s_hlsl_blit - 1, "ps_overlay", "ps_5_0");
            if (b) {
                if (FAILED(ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                                          ID3D10Blob_GetBufferSize(b), NULL,
                                                          &G.ps_overlay)))
                    G.ps_overlay = NULL;
                REL(b);
            }
        }
        if (SUCCEEDED(hr)) {
            memset(&bd, 0, sizeof bd);
            bd.ByteWidth = (UINT)sizeof(BlitCB);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            hr = ID3D11Device_CreateBuffer(G.dev, &bd, NULL, &G.cb_blit);
        }
        if (SUCCEEDED(hr)) {
            rd.ScissorEnable = FALSE;
            hr = ID3D11Device_CreateRasterizerState(G.dev, &rd, &G.rs_blit);
        }
        if (SUCCEEDED(hr)) {
            memset(&smd, 0, sizeof smd);
            smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            smd.MaxLOD = D3D11_FLOAT32_MAX;
            hr = ID3D11Device_CreateSamplerState(G.dev, &smd, &G.smp);
        }
        if (FAILED(hr)) {
            fprintf(stderr, "  [GPU11] blit shaders failed 0x%08lX -- 1x, GDI window\n",
                    (unsigned long)hr);
            G.k = 1;
            G.present_on = 0;
        }
        fprintf(stderr, "  [GPU11] internal resolution %ux (%ux%u for a 640x480 frame), "
                        "%s presentation\n", G.k, 640 * G.k, 480 * G.k,
                G.present_on ? "GPU" : "GDI");
    }
    {
        const char *e = getenv("RECOMP_GPU_VSH");
        if (e && *e && strcmp(e, "0") != 0)
            G.vsh_on = vsh_init(comp);
    }
    fprintf(stderr, "  [GPU11] D3D11 renderer on (feature level %X)\n", (unsigned)got);
    fflush(stderr);
    return 1;
}

int nv2a_gpu_enabled(void)
{
    if (G.state == 0) {
        const char *e = getenv("RECOMP_GPU");
        G.state = -1;
        if (e && *e && strcmp(e, "0") != 0) {
            if (gpu_init())
                G.state = 1;
            else
                fprintf(stderr, "  [GPU11] init failed -- staying on the software renderer\n");
        }
    }
    return G.state == 1;
}

/* ── Surfaces ──────────────────────────────────────────────────────────── */

static void cb_write(ID3D11Buffer *b, const void *data, size_t bytes);

/* One full-target triangle from src into dst through ps. The next batch's
 * apply() sets every piece of state again, so nothing is restored here. */
static void blit(ID3D11RenderTargetView *dst, const D3D11_VIEWPORT *vp,
                 ID3D11ShaderResourceView *src, ID3D11PixelShader *ps,
                 int k, const float f[4], const float g[4])
{
    static ID3D11ShaderResourceView *const nulls[4] = {NULL, NULL, NULL, NULL};
    BlitCB c;
    memset(&c, 0, sizeof c);
    c.b[1] = k;
    if (f) memcpy(c.f, f, sizeof c.f);
    if (g) memcpy(c.g, g, sizeof c.g);
    cb_write(G.cb_blit, &c, sizeof c);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, (ID3D11ShaderResourceView **)nulls);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 1, &dst, NULL);
    ID3D11DeviceContext_RSSetViewports(G.ctx, 1, vp);
    ID3D11DeviceContext_RSSetState(G.ctx, G.rs_blit);
    ID3D11DeviceContext_IASetInputLayout(G.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(G.ctx, G.vs_blit, NULL, 0);
    ID3D11DeviceContext_PSSetShader(G.ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(G.ctx, 0, 1, &G.cb_blit);
    ID3D11DeviceContext_PSSetSamplers(G.ctx, 0, 1, &G.smp);
    ID3D11DeviceContext_OMSetBlendState(G.ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 1, &src);
    ID3D11DeviceContext_Draw(G.ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, (ID3D11ShaderResourceView **)nulls);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
}

static void vp_full(D3D11_VIEWPORT *vp, UINT w, UINT h)
{
    vp->TopLeftX = 0; vp->TopLeftY = 0;
    vp->Width = (float)w; vp->Height = (float)h;
    vp->MinDepth = 0.0f; vp->MaxDepth = 1.0f;
}

/* The surface at the guest's size, for a copy to guest memory. */
static ID3D11Resource *surf_lo(GSurf *s)
{
    D3D11_VIEWPORT vp;
    if (G.k <= 1 || !s->lo)
        return (ID3D11Resource *)s->tex;
    vp_full(&vp, s->w, s->h);
    blit(s->lo_rtv, &vp, s->srv, G.ps_down, (int)G.k, NULL, NULL);
    return (ID3D11Resource *)s->lo;
}

static void surf_upload(GSurf *s)
{
    const uint8_t *m = gmem(s->base);
    ID3D11Resource *dst = (G.k > 1 && s->lo) ? (ID3D11Resource *)s->lo
                                             : (ID3D11Resource *)s->tex;
    if (s->bpp == 4) {
        ID3D11DeviceContext_UpdateSubresource(G.ctx, dst, 0,
                                              NULL, m, s->pitch, 0);
    } else {
        uint32_t *tmp = (uint32_t *)malloc((size_t)s->w * s->h * 4), x, y;
        if (!tmp)
            return;
        for (y = 0; y < s->h; y++) {
            const uint16_t *r = (const uint16_t *)(m + (size_t)y * s->pitch);
            for (x = 0; x < s->w; x++) {
                uint32_t t = r[x];
                tmp[(size_t)y * s->w + x] = 0xFF000000u | ((t & 0xF800u) << 8)
                                          | ((t & 0x07E0u) << 5) | ((t & 0x001Fu) << 3);
            }
        }
        ID3D11DeviceContext_UpdateSubresource(G.ctx, dst, 0,
                                              NULL, tmp, s->w * 4, 0);
        free(tmp);
    }
    if (dst != (ID3D11Resource *)s->tex) {
        D3D11_VIEWPORT vp;
        vp_full(&vp, s->w * G.k, s->h * G.k);
        blit(s->rtv, &vp, s->lo_srv, G.ps_up, (int)G.k, NULL, NULL);
    }
    s->hash = surf_hash(s);
    G.st_surf_uploads++;
}

static void flush(void);

/* Memory the title has just been given (MmAllocateContiguousMemory) is no
 * longer whatever the GPU last drew there. A surface cached over a block the
 * title freed and got back for something else was written back on the next
 * texture sync -- old pixels over the new owner's data. Orta's DirectSound
 * lost its voice lists that way (a freed render target reused for voice
 * objects), and its DPC then walked a self-linked list forever. The
 * allocator queues the range; the executor thread drops every surface it
 * overlaps before its next write-back. */
#define GPU_FORGET_MAX 1024
static SRWLOCK s_forget_lock = SRWLOCK_INIT;
static uint32_t s_forget_base[GPU_FORGET_MAX], s_forget_len[GPU_FORGET_MAX];
static volatile LONG s_forget_n;

void nv2a_gpu_forget_range(uint32_t base, uint32_t bytes)
{
    AcquireSRWLockExclusive(&s_forget_lock);
    if (s_forget_n < GPU_FORGET_MAX) {
        s_forget_base[s_forget_n] = base;
        s_forget_len[s_forget_n] = bytes;
        s_forget_n++;
    } else {                                     /* overflow: forget everything */
        s_forget_base[GPU_FORGET_MAX - 1] = 0;
        s_forget_len[GPU_FORGET_MAX - 1] = 0xFFFFFFFFu;
    }
    ReleaseSRWLockExclusive(&s_forget_lock);
}

static void surf_drop(GSurf *s);

static void forget_pending(void)
{
    uint32_t b[GPU_FORGET_MAX], l[GPU_FORGET_MAX];
    int n, i, j;
    if (!s_forget_n || G.state != 1)
        return;
    AcquireSRWLockExclusive(&s_forget_lock);
    n = (int)s_forget_n;
    memcpy(b, s_forget_base, sizeof(uint32_t) * (size_t)n);
    memcpy(l, s_forget_len, sizeof(uint32_t) * (size_t)n);
    s_forget_n = 0;
    ReleaseSRWLockExclusive(&s_forget_lock);
    for (i = 0; i < GPU_MAX_SURF; i++) {
        GSurf *s = &G.surf[i];
        uint64_t lo, hi;
        if (!s->tex)
            continue;
        lo = s->base;
        hi = lo + (uint64_t)s->h * s->pitch;
        for (j = 0; j < n; j++)
            if (lo < (uint64_t)b[j] + l[j] && hi > b[j]) {
                static int said;
                if (said++ < 12)
                    fprintf(stderr, "  [GPU11] surface %08X dropped: its memory was"
                            " allocated again (%08X, %u bytes)\n", s->base, b[j], l[j]);
                if (s == G.cur_surf) {
                    flush();
                    G.cur_valid = 0;
                    G.cur_surf = NULL;
                }
                surf_drop(s);
                break;
            }
    }
}

/* Copy a mapped staging image's rectangle into guest memory. */
static void surf_store(GSurf *s, const D3D11_MAPPED_SUBRESOURCE *mp, uint32_t x0,
                       uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint8_t *m = gmem(s->base);
    uint32_t x, y;
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    for (y = y0; y < y1; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)mp->pData
                                                 + (size_t)y * mp->RowPitch);
        uint8_t *dst = m + (size_t)y * s->pitch;
        if (s->bpp == 4) {
            memcpy(dst + (size_t)x0 * 4, src + x0, (size_t)(x1 - x0) * 4);
        } else {
            uint16_t *d16 = (uint16_t *)dst;
            for (x = x0; x < x1; x++) {
                uint32_t v = src[x];
                d16[x] = (uint16_t)(((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0)
                                  | ((v >> 3) & 0x001F));
            }
        }
    }
}

/* Finish queued flip copies: those already done (or all, with wait). */
static void surf_async_complete(GSurf *s, int wait)
{
    int k, j;
    for (j = 0; j < 2; j++) {
        D3D11_MAPPED_SUBRESOURCE mp;
        HRESULT hr;
        k = (s->anext + j) & 1;                    /* oldest first */
        if (!s->aq[k])
            continue;
        hr = ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)s->ast[k], 0, D3D11_MAP_READ,
                                     wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            return;                                /* keep order: newer waits too */
        s->aq[k] = 0;
        if (FAILED(hr))
            continue;
        surf_store(s, &mp, s->ar[k][0], s->ar[k][1], s->ar[k][2], s->ar[k][3]);
        ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)s->ast[k], 0);
        s->hash = surf_hash(s);
        G.st_readbacks++;
    }
}

/* Queue the shown frame's copy; it lands in guest memory a frame or two
 * later. A synchronous read-back at every flip made the executor wait for
 * the GPU to finish the whole frame, so the two never overlapped. */
static int surf_readback_async(GSurf *s)
{
    int k;
    if (!s->dirty)
        return 1;
    k = s->anext;
    if (!s->ast[k]) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(s->staging, &td);
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &s->ast[k])))
            return 0;
    }
    if (s->aq[k])
        surf_async_complete(s, 1);                 /* slot still busy: drain */
    flush();
    ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)s->ast[k], surf_lo(s));
    s->aq[k] = 1;
    s->ar[k][0] = s->dx0; s->ar[k][1] = s->dy0;
    s->ar[k][2] = s->dx1; s->ar[k][3] = s->dy1;
    s->anext = k ^ 1;
    s->dirty = 0;
    return 1;
}

/* GPU -> guest for the touched rectangle. */
static void surf_readback(GSurf *s)
{
    D3D11_MAPPED_SUBRESOURCE mp;
    uint8_t *m;
    uint32_t x, y;

    if (s->aq[0] || s->aq[1])
        surf_async_complete(s, 1);                 /* memory must be current */
    if (!s->dirty)
        return;
    flush();
    s->dirty = 0;
    ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)s->staging, surf_lo(s));
    if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)s->staging, 0,
                                       D3D11_MAP_READ, 0, &mp)))
        return;
    m = gmem(s->base);
    if (s->dx1 > s->w) s->dx1 = s->w;
    if (s->dy1 > s->h) s->dy1 = s->h;
    for (y = s->dy0; y < s->dy1; y++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)mp.pData
                                                 + (size_t)y * mp.RowPitch);
        uint8_t *dst = m + (size_t)y * s->pitch;
        if (s->bpp == 4) {
            memcpy(dst + (size_t)s->dx0 * 4, src + s->dx0, (size_t)(s->dx1 - s->dx0) * 4);
        } else {
            uint16_t *d16 = (uint16_t *)dst;
            for (x = s->dx0; x < s->dx1; x++) {
                uint32_t v = src[x];
                d16[x] = (uint16_t)(((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0)
                                  | ((v >> 3) & 0x001F));
            }
        }
    }
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)s->staging, 0);
    s->hash = surf_hash(s);
    G.st_readbacks++;
    /* Anything sampled from these bytes must be looked at again. */
    {
        int i;
        uint32_t lo = s->base, hi = s->base + s->h * s->pitch;
        for (i = 0; i < GPU_MAX_TEX; i++) {
            GTex *t = &G.tex[i];
            if (t->srv && t->d.offset < hi
                && t->d.offset + nv2a_exec_tex_bytes(&t->d) > lo)
                t->checked = 0;
        }
    }
}

/* Release a surface without writing it back (its memory is someone else's). */
static void surf_drop(GSurf *s)
{
    s->dirty = 0;
    s->aq[0] = s->aq[1] = 0;
    REL(s->ast[0]);
    REL(s->ast[1]);
    REL(s->srv);
    REL(s->rtv);
    REL(s->tex);
    REL(s->staging);
    REL(s->lo_srv);
    REL(s->lo_rtv);
    REL(s->lo);
    memset(s, 0, sizeof *s);
}

static void surf_free(GSurf *s)
{
    surf_readback(s);
    REL(s->ast[0]);
    REL(s->ast[1]);
    REL(s->srv);
    REL(s->rtv);
    REL(s->tex);
    REL(s->staging);
    REL(s->lo_srv);
    REL(s->lo_rtv);
    REL(s->lo);
    memset(s, 0, sizeof *s);
}

static int surf_create(GSurf *s, uint32_t base, uint32_t pitch, uint32_t bpp,
                       uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    memset(s, 0, sizeof *s);
    s->base = base; s->pitch = pitch; s->bpp = bpp;
    s->w = pitch / bpp; s->h = h;
    {
        static uint32_t seen[96];
        static int nseen;
        int i;
        for (i = 0; i < nseen && seen[i] != (base ^ (pitch << 20) ^ h); i++)
            ;
        if (i == nseen && nseen < 96) {
            seen[nseen++] = base ^ (pitch << 20) ^ h;
            fprintf(stderr, "  [GPU11] surface %08X..%08X (pitch %u, %u bpp, %u rows)\n",
                    base, base + h * pitch, pitch, bpp, h);
        }
    }
    memset(&td, 0, sizeof td);
    td.Width = s->w; td.Height = s->h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (G.k > 1) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &s->lo))
            || FAILED(ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)s->lo,
                                                          NULL, &s->lo_rtv))
            || FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)s->lo,
                                                            &sd, &s->lo_srv))) {
            REL(s->lo_srv); REL(s->lo_rtv); REL(s->lo);
            return 0;
        }
        td.Width = s->w * G.k; td.Height = s->h * G.k;
    }
    if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &s->tex))) {
        REL(s->lo_srv); REL(s->lo_rtv); REL(s->lo);
        return 0;
    }
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
        if (FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)s->tex,
                                                         &sd, &s->srv)))
            s->srv = NULL;
    }
    td.Width = s->w; td.Height = s->h;           /* staging: the guest's size */
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &s->staging))
        || FAILED(ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)s->tex,
                                                      NULL, &s->rtv))) {
        REL(s->tex);
        REL(s->staging);
        return 0;
    }
    surf_upload(s);
    s->checked = G.frame;
    return 1;
}

/* The render target for this state, created or brought up to date. */
static GSurf *surf_get(const Nv2aGpuState *st)
{
    uint32_t need = st->clip_y + st->clip_h, i;
    GSurf *s = NULL, *lru = NULL;

    if (!st->bpp || !st->pitch || st->pitch / st->bpp == 0
        || st->pitch / st->bpp > 4096 || need == 0 || need > 4096
        || !guest_range_ok(st->surf_base, need * st->pitch))
        return NULL;
    for (i = 0; i < GPU_MAX_SURF; i++) {
        GSurf *c = &G.surf[i];
        if (c->tex && c->base == st->surf_base && c->pitch == st->pitch
            && c->bpp == st->bpp) {
            s = c;
            break;
        }
        if (!lru || !c->tex || (lru->tex && c->used < lru->used))
            lru = c;
    }
    if (s && s->h < need) {                /* the title drew further down */
        surf_free(s);
        lru = s;
        s = NULL;
    }
    if (!s) {
        if (lru->tex)
            surf_free(lru);
        if (!surf_create(lru, st->surf_base, st->pitch, st->bpp, need))
            return NULL;
        s = lru;
    } else if (s->checked != G.frame) {
        /* Once a frame: has the CPU written this memory behind our back
         * (a movie decoder, a title's own blit)? Then take its copy. */
        s->checked = G.frame;
        /* Drawn by the GPU last frame: guest memory is only a copy, and the
         * small writes other things make there (semaphores, reports) must
         * not throw the GPU's picture away. */
        /* Not the colour surface at 0x80000000 (Orta's shadow pass): its
         * colour is never written back (color_written), so the bytes there are
         * DirectSound's, changing every frame -- hashing and re-uploading
         * them was a wasted 512x512 upload a frame. */
        if (!s->dirty && s->drawn + 1 < G.frame && s->base != 0x80000000u
            && surf_hash(s) != s->hash) {
            static int said;
            if (said++ < 12)
                fprintf(stderr, "  [GPU11] surface %08X (pitch %u, %ux%u) changed in"
                                " memory since frame %u -- re-uploading\n",
                        s->base, s->pitch, s->w, s->h, G.frame - 1);
            surf_upload(s);
        }
    }
    s->used = G.frame;
    return s;
}

/* Did this draw put colour anywhere a write-back should carry to guest memory?
 * Not with every colour channel masked (depth-only passes), and not into a
 * colour surface at offset 0: Orta's shadow pass leaves SURFACE_COLOR_OFFSET
 * at 0, which here is the start of the contiguous arena, where DirectSound
 * keeps its voice objects. Writing that 512x512 "surface" back over them
 * reverted their list links -- the stage-end freeze. */
static int color_written(const Nv2aGpuState *st)
{
    if (!(st->color_mask & 0x01010101u))
        return 0;
    if (st->surf_base == 0x80000000u)
        return 0;
    return 1;
}

static void surf_touch(GSurf *s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (x1 > s->w) x1 = s->w;
    if (y1 > s->h) y1 = s->h;
    if (x0 >= x1 || y0 >= y1)
        return;
    s->drawn = G.frame;
    if (!s->dirty) {
        s->dirty = 1;
        s->dx0 = x0; s->dy0 = y0; s->dx1 = x1; s->dy1 = y1;
        return;
    }
    if (x0 < s->dx0) s->dx0 = x0;
    if (y0 < s->dy0) s->dy0 = y0;
    if (x1 > s->dx1) s->dx1 = x1;
    if (y1 > s->dy1) s->dy1 = y1;
}

void nv2a_gpu_sync_range(uint32_t base, uint32_t bytes)
{
    int i;
    if (G.state != 1)
        return;
    forget_pending();
    for (i = 0; i < GPU_MAX_SURF; i++) {
        GSurf *s = &G.surf[i];
        if (s->tex && s->dirty && base < s->base + s->h * s->pitch
            && base + bytes > s->base)
            surf_readback(s);
    }
}

void nv2a_gpu_sync_all(void)
{
    int i;
    if (G.state != 1)
        return;
    forget_pending();
    for (i = 0; i < GPU_MAX_SURF; i++)
        if (G.surf[i].tex)
            surf_readback(&G.surf[i]);
}

/* ── Depth ─────────────────────────────────────────────────────────────── */

static GZbuf *zbuf_get(uint32_t zeta, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;
    GZbuf *z = NULL, *lru = NULL;
    int i;

    for (i = 0; i < GPU_MAX_ZBUF; i++) {
        GZbuf *c = &G.zb[i];
        if (c->tex && c->zeta == zeta && c->w == w && c->h == h) {
            c->used = G.frame;
            return c;
        }
        if (!lru || !c->tex || (lru->tex && c->used < lru->used))
            lru = c;
    }
    z = lru;
    REL(z->srv);
    REL(z->dsv);
    REL(z->tex);
    memset(&td, 0, sizeof td);
    td.Width = w * G.k; td.Height = h * G.k;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;   /* depth, and readable as a shadow map */
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        memset(&dd, 0, sizeof dd);
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        memset(&sd, 0, sizeof sd);
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = 1;
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &z->tex))
            || FAILED(ID3D11Device_CreateDepthStencilView(G.dev, (ID3D11Resource *)z->tex,
                                                          &dd, &z->dsv))) {
            REL(z->tex);
            return NULL;
        }
        if (FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)z->tex,
                                                         &sd, &z->srv)))
            z->srv = NULL;
    }
    z->zeta = zeta; z->w = w; z->h = h; z->used = G.frame;
    /* The software buffer starts "infinitely far": far plane here. */
    ID3D11DeviceContext_ClearDepthStencilView(G.ctx, z->dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    return z;
}

/* Depth units per unit of D3D depth. A float depth buffer (SET_CONTROL0
 * Z_FORMAT) takes z in the format's own range -- up to ~1.7e38 for the
 * 24-bit float, 511.9375 for the 16-bit one (xemu) -- and Orta's river
 * reflection is drawn into one with z around 1e10: scaled as fixed-point
 * depth, every triangle of it lay beyond the far plane and was clipped,
 * leaving only the clear colour where its sky should be. */
/* Orta's river, 18 Oct, compared with xemu:
 * - depth as xemu does it: pixels whose z falls outside the title's
 *   SET_CLIP_MIN/MAX range are dropped (SET_ZMIN_MAX_CONTROL CULL) and the
 *   reflection is depth-tested. The reflection's sky and walls come with z
 *   in the billions against a range of 0..16777215: xemu never draws them,
 *   and drawing them left white sheets on the water.
 * - the ripple lookup interpolated linearly on screen: it is screen
 *   position / 512, and perspective-correct it broke the near water into
 *   blocks.
 * - the ripple lookup taken from the pixel's own screen position when the
 *   vertex program made it screen position * k: near water triangles cross
 *   behind the camera, and interpolated from those clipped vertices the
 *   lookup ran far outside the reflection and repeated it.
 * - a swizzled texture that is a whole GPU-drawn surface bound as that
 *   surface (surf_as_texture): the reflection is sampled as SZ_X8R8G8B8.
 * Fixed 19 Oct; g_water_mode stays 0 (it drove the comparison toggles). */
volatile long g_water_mode;

static float zscale_for(uint32_t zeta_format, uint32_t z_float)
{
    if (z_float)
        return zeta_format == 1 ? 1.0f / 511.9375f : 1.0f / 1.7014118e38f;
    return zeta_format == 1 ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
}

/* ── Textures ──────────────────────────────────────────────────────────── */

static int tex_key_eq(const Nv2aGpuTexDesc *a, const Nv2aGpuTexDesc *b)
{
    return a->offset == b->offset && a->width == b->width
        && a->height == b->height && a->pitch == b->pitch
        && a->color == b->color && a->palette == b->palette
        && a->cube == b->cube && a->levels == b->levels;
}

static uint64_t tex_hash(const Nv2aGpuTexDesc *d, uint32_t bytes)
{
    uint64_t h = hash_bytes(gmem(d->offset), bytes, 0);
    if (d->palette && guest_range_ok(d->palette, 1024))
        h = hash_bytes(gmem(d->palette), 1024, h);
    return h;
}

static int tex_upload(GTex *t)
{
    uint32_t w = t->d.width, h = t->d.height, slices = t->d.cube ? 6 : 1, f;
    uint32_t mips = 1, m, bb = 0;
    uint32_t *px;
    int ok = 1;
    Nv2aGpuTexDesc lv;
    DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;

    /* DXT goes to the GPU as it is (BC1/2/3): the blocks are the same, and
     * decoding them on the CPU texel by texel was the bulk of a texture
     * upload. RECOMP_TEX_BC=0 decodes them as before. */
    {
        static int bc = -1;
        if (bc < 0) {
            const char *e = getenv("RECOMP_TEX_BC");
            bc = !(e && *e == '0');
        }
        if (bc && !t->d.cube && !(w & 3) && !(h & 3) && !t->d.palette) {
            if (t->d.color == 0x0C) { fmt = DXGI_FORMAT_BC1_UNORM; bb = 8; }
            else if (t->d.color == 0x0E) { fmt = DXGI_FORMAT_BC2_UNORM; bb = 16; }
            else if (t->d.color == 0x0F) { fmt = DXGI_FORMAT_BC3_UNORM; bb = 16; }
        }
    }
    px = bb ? NULL : (uint32_t *)malloc((size_t)w * h * 4);
    if (!bb && !px)
        return 0;
    while (mips < 13 && ((w >> mips) || (h >> mips))
           && nv2a_exec_tex_level(&t->d, mips, &lv)
           && guest_range_ok(lv.offset, 1))
        mips++;
    if (!t->tex || t->slices != slices || t->mips != mips || t->fmt != fmt) {
        D3D11_TEXTURE2D_DESC td;
        REL(t->srv);
        REL(t->tex);
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h;
        td.MipLevels = mips; td.ArraySize = slices;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &t->tex))) {
            free(px);
            return 0;
        }
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd;
            memset(&sd, 0, sizeof sd);
            sd.Format = fmt;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MostDetailedMip = 0;
            sd.Texture2DArray.MipLevels = mips;
            sd.Texture2DArray.FirstArraySlice = 0;
            sd.Texture2DArray.ArraySize = slices;
            if (FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)t->tex,
                                                             &sd, &t->srv))) {
                REL(t->tex);
                free(px);
                return 0;
            }
        }
        t->slices = slices;
        t->mips = mips;
        t->fmt = fmt;
    }
    if (bb) {
        for (m = 0; m < mips; m++) {
            uint32_t bw, bh;
            if (m == 0)
                lv = t->d;
            else if (!nv2a_exec_tex_level(&t->d, m, &lv))
                break;
            bw = (lv.width + 3) / 4; bh = (lv.height + 3) / 4;
            if (!bw) bw = 1;
            if (!bh) bh = 1;
            if (!guest_range_ok(lv.offset, bw * bh * bb))
                break;
            ID3D11DeviceContext_UpdateSubresource(G.ctx, (ID3D11Resource *)t->tex, m, NULL,
                                                  gmem(lv.offset), bw * bb, 0);
        }
        G.st_uploads++;
        s_uploads_total++;
        G.st_upload_bytes += (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * bb;
        return 1;
    }
    for (f = 0; f < slices; f++) {
        int s = nv2a_exec_tex_decode(&t->d, f, px);
        if (f == 0)
            ok = s;
        ID3D11DeviceContext_UpdateSubresource(G.ctx, (ID3D11Resource *)t->tex,
                                              f * mips, NULL, px, w * 4, 0);
    }
    for (m = 1; m < mips; m++) {           /* never for cube maps */
        if (!nv2a_exec_tex_level(&t->d, m, &lv))
            break;
        nv2a_exec_tex_decode(&lv, 0, px);
        ID3D11DeviceContext_UpdateSubresource(G.ctx, (ID3D11Resource *)t->tex,
                                              m, NULL, px, lv.width * 4, 0);
    }
    free(px);
    G.st_uploads++;
    s_uploads_total++;
    G.st_upload_bytes += (uint64_t)w * h * 4 * slices;
    return ok;
}

/* The GPU copy of a stage's texture, checked against guest memory once a
 * frame (and again whenever a surface over it was written back). */
static GTex *tex_get(const Nv2aGpuTexDesc *d)
{
    GTex *t = NULL, *lru = NULL;
    uint32_t bytes;
    uint64_t h;
    int i;

    if (!d->valid || !d->width || !d->height || d->width > 4096 || d->height > 4096)
        return NULL;
    bytes = nv2a_exec_tex_bytes(d);
    if (!bytes || !guest_range_ok(d->offset, bytes))
        return NULL;
    {   /* A render target that starts inside this texture's mip tail -- but
         * after its top level -- is not part of the texture: stop there.
         * Orta's Episode 3 boss keeps a 512x512 bump map whose mip chain runs
         * into a 256x256 target drawn every frame; counted in, every frame
         * read that target back from the GPU and re-decoded the map (~50 ms,
         * the boss's 20 fps). */
        Nv2aGpuTexDesc top = *d;
        uint32_t b0;
        int k;
        top.levels = 1;
        b0 = nv2a_exec_tex_bytes(&top);
        for (k = 0; k < GPU_MAX_SURF; k++) {
            GSurf *sf = &G.surf[k];
            if (sf->tex && sf->base > d->offset && sf->base < d->offset + bytes
                && b0 && sf->base - d->offset >= b0)
                bytes = sf->base - d->offset;
        }
    }
    for (i = 0; i < GPU_MAX_TEX; i++) {
        GTex *c = &G.tex[i];
        if (c->srv && tex_key_eq(&c->d, d)) {
            t = c;
            break;
        }
        if (!lru || !c->srv || (lru->srv && c->used < lru->used))
            lru = c;
    }
    if (t && t->checked == G.frame && G.frame != 0) {
        t->used = G.frame;
        t->d = *d;                       /* addressing/filter may differ */
        return t->supported ? t : NULL;
    }
    {   /* rendered to? read it first -- and say so: a GPU read-back stalls
         * the frame (Orta's Episode 3 boss lost ~50 ms a frame somewhere here) */
        static int said;
        int k;
        for (k = 0; k < GPU_MAX_SURF && said < 40; k++) {
            GSurf *s = &G.surf[k];
            if (s->tex && s->dirty && d->offset < s->base + s->h * s->pitch
                && d->offset + bytes > s->base) {
                said++;
                fprintf(stderr, "[TEXREAD] texture %08X fmt %02X %ux%u pitch %u reads back surface %08X %ux%u pitch %u bpp %u (frame %u)\n",
                        d->offset, d->color, d->width, d->height, d->pitch,
                        s->base, s->w, s->h, s->pitch, s->bpp, G.frame);
                fflush(stderr);
            }
        }
    }
    nv2a_gpu_sync_range(d->offset, bytes);
    h = tex_hash(d, bytes);
    if (!t) {
        t = lru;
        REL(t->srv);
        REL(t->tex);
        memset(t, 0, sizeof *t);
        t->d = *d;
        t->supported = tex_upload(t);
        if (!t->srv)
            return NULL;
    } else {
        t->d = *d;
        if (h != t->hash)
            t->supported = tex_upload(t);
    }
    t->hash = h;
    t->checked = G.frame;
    t->used = G.frame;
    return t->supported ? t : NULL;
}

/* ── Pipeline state ────────────────────────────────────────────────────── */

static D3D11_BLEND map_blend(uint32_t f, int alpha)
{
    switch (f) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return f ? D3D11_BLEND_ONE : D3D11_BLEND_ZERO;
    }
}

static D3D11_BLEND_OP map_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static ID3D11BlendState *blend_state(const Nv2aGpuState *st, int clear)
{
    D3D11_BLEND_DESC d;
    D3D11_RENDER_TARGET_BLEND_DESC *r = &d.RenderTarget[0];
    UINT8 mask = 0;
    int i;

    memset(&d, 0, sizeof d);
    if (clear) {
        mask = D3D11_COLOR_WRITE_ENABLE_ALL;
    } else {
        if (st->color_mask & 0x00010000u) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
        if (st->color_mask & 0x00000100u) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (st->color_mask & 0x00000001u) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (st->color_mask & 0x01000000u) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        if (st->blend_enable && !(st->blend_sfactor == 1 && st->blend_dfactor == 0)) {
            r->BlendEnable = TRUE;
            r->SrcBlend = map_blend(st->blend_sfactor, 0);
            r->DestBlend = map_blend(st->blend_dfactor, 0);
            r->BlendOp = map_op(st->blend_equation);
            r->SrcBlendAlpha = map_blend(st->blend_sfactor, 1);
            r->DestBlendAlpha = map_blend(st->blend_dfactor, 1);
            r->BlendOpAlpha = r->BlendOp;
        }
    }
    if (st->bpp == 2)                     /* 5:6:5 has no alpha to write */
        mask &= (UINT8)~D3D11_COLOR_WRITE_ENABLE_ALPHA;
    if (!r->BlendEnable) {
        r->SrcBlend = r->SrcBlendAlpha = D3D11_BLEND_ONE;
        r->DestBlend = r->DestBlendAlpha = D3D11_BLEND_ZERO;
        r->BlendOp = r->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    r->RenderTargetWriteMask = mask;
    for (i = 0; i < G.nbs; i++)
        if (memcmp(&G.bs[i].d, &d, sizeof d) == 0)
            return G.bs[i].s;
    if (G.nbs == 64) {                    /* full: start again */
        for (i = 0; i < 64; i++)
            REL(G.bs[i].s);
        G.nbs = 0;
    }
    G.bs[G.nbs].d = d;
    if (FAILED(ID3D11Device_CreateBlendState(G.dev, &d, &G.bs[G.nbs].s)))
        return NULL;
    return G.bs[G.nbs++].s;
}

static ID3D11DepthStencilState *depth_state(const Nv2aGpuState *st, int off)
{
    D3D11_DEPTH_STENCIL_DESC d;
    int i;

    memset(&d, 0, sizeof d);
    if (!off && (st->depth_test || st->depth_mask)) {
        d.DepthEnable = TRUE;
        d.DepthWriteMask = st->depth_mask ? D3D11_DEPTH_WRITE_MASK_ALL
                                          : D3D11_DEPTH_WRITE_MASK_ZERO;
        if (st->depth_test && st->depth_func >= 0x200 && st->depth_func <= 0x207)
            d.DepthFunc = (D3D11_COMPARISON_FUNC)(st->depth_func - 0x200 + 1);
        else
            d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    } else {
        d.DepthEnable = FALSE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    }
    d.FrontFace.StencilFailOp = d.FrontFace.StencilDepthFailOp =
        d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    d.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    d.BackFace = d.FrontFace;
    for (i = 0; i < G.nds; i++)
        if (memcmp(&G.ds[i].d, &d, sizeof d) == 0)
            return G.ds[i].s;
    if (G.nds == 32) {
        for (i = 0; i < 32; i++)
            REL(G.ds[i].s);
        G.nds = 0;
    }
    G.ds[G.nds].d = d;
    if (FAILED(ID3D11Device_CreateDepthStencilState(G.dev, &d, &G.ds[G.nds].s)))
        return NULL;
    return G.ds[G.nds++].s;
}

static void fill_cb(GpuCB *cb, const Nv2aGpuState *st, const uint32_t tfl[4],
                    const uint32_t tlev[4])
{
    int i;
    memset(cb, 0, sizeof *cb);
    cb->mode[0] = st->mode;
    cb->mode[1] = st->mode != NV2A_GPU_MODE_2D && st->alpha_test;
    cb->mode[2] = st->alpha_func;
    cb->mode[3] = st->alpha_ref;
    cb->rc[0] = st->rc.control;
    cb->rc[1] = st->rc.stage_program;
    cb->rc[2] = st->rc.final0;
    cb->rc[3] = st->rc.final1;
    cb->rc2[0] = st->rc.final_c0;
    cb->rc2[1] = st->rc.final_c1;
    cb->rc2[2] = st->clip_plane_mode;
    {
        float zs = zscale_for(st->zeta_format, st->z_float);
        cb->zc[0] = st->zclip[0] * zs;
        cb->zc[1] = st->zclip[1] * zs;
        cb->zc[2] = st->zcull && !(g_water_mode & 2) ? 1.0f : 0.0f;   /* F9 test: 2 or 3 = range test off */
        cb->zc[3] = (float)G.k;
        cb->zo[0] = st->zoff[0] * zs;
        cb->zo[1] = st->zoff[1] * (float)G.k;
        cb->zo[2] = (float)st->shadow_func;
    }
    for (i = 0; i < 4; i++) {
        static int boff = -1;            /* RECOMP_BUMP_OFF=1: no ripple, plain mirror */
        if (boff < 0) {
            const char *e = getenv("RECOMP_BUMP_OFF");
            boff = e && *e == '1';
        }
        cb->bump[i][0] = st->bump[i][0]; cb->bump[i][1] = st->bump[i][1];
        cb->bump[i][2] = st->bump[i][2]; cb->bump[i][3] = st->bump[i][3];
        if (boff)
            cb->bump[i][0] = cb->bump[i][1] = cb->bump[i][2] = cb->bump[i][3] = 0.0f;
        cb->bumpl[i][0] = st->bump[i][4]; cb->bumpl[i][1] = st->bump[i][5];
        /* Stage 1 reads stage 0; 2 and 3 name theirs (xemu psh.c). */
        cb->bumpl[i][2] = (float)(i == 2 ? (st->other_stage_input >> 16) & 0xF
                                : i == 3 ? (st->other_stage_input >> 20) & 0xF : 0);
        {   /* RECOMP_BUMP_PROJ: 0 = s,t used as they are (xemu), 1 = s/q + d
             * (the default: the best-looking river of the 13 Oct tests),
             * 2 = (s + d)/q */
            static int pj = -1;
            if (pj < 0) {
                const char *e = getenv("RECOMP_BUMP_PROJ");
                pj = e ? atoi(e) : 1;
            }
            {   /* RECOMP_BUMP_NOPERSP=1: a bump stage's coordinates are
                 * interpolated linearly in screen space, not perspective-
                 * correct (Orta's reflection lookup is screen position/512) */
                static int np = -1;
                if (np < 0) {
                    const char *e = getenv("RECOMP_BUMP_NOPERSP");
                    np = e && *e == '2';   /* 18 Oct: xemu's perspective lookup looked best; =2 forces linear */
                }
                int bs = ((st->rc.stage_program >> (i * 5)) & 0x1F) >= 6;
                cb->bumpl[i][3] = (float)pj + (bs ? 4.0f + 8.0f : 0.0f);
                (void)np;
            }
        }
    }
    for (i = 0; i < 8; i++) {
        cb->cw[i][0] = st->rc.color_icw[i];
        cb->cw[i][1] = st->rc.color_ocw[i];
        cb->cw[i][2] = st->rc.alpha_icw[i];
        cb->cw[i][3] = st->rc.alpha_ocw[i];
        cb->fac[i][0] = st->rc.factor0[i];
        cb->fac[i][1] = st->rc.factor1[i];
    }
    for (i = 0; i < 4; i++) {
        const Nv2aGpuTexDesc *d = &st->tex[i];
        uint32_t fl = 0;
        uint32_t mag = (d->filter >> 24) & 0xF, min = (d->filter >> 16) & 0xFF;
        fl |= tfl[i];
        if (st->tex_norm[i])
            fl |= 2;
        if (mag == 2 || min == 2 || min == 4 || min == 6)
            fl |= 4;
        if (d->cube)
            fl |= 8;
        if (min >= 3 && min <= 6 && tlev[i] > 1)
            fl |= 32;
        {   /* RECOMP_TRILINEAR=1: blend two mip levels where the filter asks.
             * Off by default: slower here and it did not stop the boss flicker. */
            static int tri = -1;
            if (tri < 0) {
                const char *e = getenv("RECOMP_TRILINEAR");
                tri = e && *e == '1';
            }
            if (tri && (min == 5 || min == 6) && tlev[i] > 1)
                fl |= 128;
        }
        cb->tex[i][0] = fl;
        cb->tex[i][3] = tlev[i];
        cb->tex[i][1] = d->addr_u;
        cb->tex[i][2] = d->addr_v;
        {
            /* RECOMP_GPU_FORCE_WRAP=1: a diagnostic. Every stage wraps, whatever
             * the title asked for -- if streaks vanish, address modes are the bug. */
            static int fw = -1;
            if (fw < 0) {
                const char *e = getenv("RECOMP_GPU_FORCE_WRAP");
                fw = e && *e && strcmp(e, "0") != 0;
            }
            if (fw)
                cb->tex[i][1] = cb->tex[i][2] = 1;
        }
        cb->tsize[i][0] = (float)d->width;
        cb->tsize[i][1] = (float)d->height;
        cb->tsize[i][2] = 1.0f;
        cb->tsize[i][3] = d->color == 0x2E ? 16777215.0f : d->color == 0x2F ? 3.4028235e38f
                        : d->color == 0x31 ? 511.9375f : 65535.0f;   /* a shadow map's depth range */
        if (fl & 64) {                    /* a surface drawn at G.k times */
            cb->tsize[i][0] *= (float)G.k;
            cb->tsize[i][1] *= (float)G.k;
            cb->tsize[i][2] = (float)G.k;
        }
    }
    memcpy(cb->fogc, st->fog_color, sizeof cb->fogc);
    {
        /* RECOMP_GPU_TEXDBG: 1 = never use mip levels below 0,
         * 2 = draw stage 0's texture coordinates as colour (red u, green v). */
        static int td = -1;
        if (td < 0) {
            const char *e = getenv("RECOMP_GPU_TEXDBG");
            td = e ? atoi(e) : 0;
        }
        cb->rc2[3] = (uint32_t)td;
        {   /* never on a pass that samples a linear (render-target) texture:
             * those are the full-screen composites and would hide the scene */
            uint32_t f = st->tex[0].color;
            if (f == 0x10 || f == 0x11 || f == 0x12 || f == 0x16 || f == 0x17
                || f == 0x1D || f == 0x1E || f == 0x1F || (f >= 0x2E && f <= 0x37)
                || !st->depth_test || !st->depth_mask || st->blend_enable)
                cb->rc2[3] &= ~6u;   /* only opaque depth-tested geometry */
        }
    }
}

/* Per-draw constants through Map(WRITE_DISCARD) on dynamic buffers, the
 * path drivers rename cheaply. UpdateSubresource on a DEFAULT buffer that
 * the GPU is still reading -- thousands of times a frame -- had the executor
 * blocked inside the driver for most of its time. */
static void cb_write(ID3D11Buffer *b, const void *data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE mp;
    if (SUCCEEDED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)b, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
        memcpy(mp.pData, data, bytes);
        ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)b, 0);
    }
}

/* Dynamic buffers as rings: append with NO_OVERWRITE, DISCARD only on
 * wrap. Discarding the whole buffer for every batch -- the old way, up to
 * 16 MB a time for program batches, thousands of times a frame -- runs the
 * driver out of renamed copies, and then each Map waits for the GPU. The
 * executor thread spent most of its time blocked exactly there.
 * Returns the byte offset written at, or -1. */
static int ring_map(ID3D11Buffer *b, UINT cap, UINT *pos, UINT need, UINT align,
                    D3D11_MAPPED_SUBRESOURCE *mp)
{
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    UINT at = (*pos + align - 1) / align * align;
    if (need > cap)
        return -1;
    if (at + need > cap || at == 0) {
        at = 0;
        how = D3D11_MAP_WRITE_DISCARD;
    }
    if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)b, 0, how, 0, mp)))
        return -1;
    mp->pData = (uint8_t *)mp->pData + at;
    *pos = at + need;
    return (int)at;
}
static UINT s_vb_pos, s_vbp_pos, s_ibp_pos;

/* ── Specialised combiner shaders ─────────────────────────────────────────
 * The general pixel shader interprets the register combiners from the
 * constant buffer: a loop over up to 8 stages reading registers by index,
 * which the GPU keeps in scratch memory. At 4K that was most of the GPU's
 * time. Each combiner setup a title uses is compiled once more with its
 * control words as constants -- the loop unrolls and the registers become
 * registers -- on a worker thread; until it is ready the general shader
 * draws. RECOMP_GPU_PS_SPEC=0 turns this off. */
#define PS_SPEC_MAX 1024
typedef struct {
    uint32_t key[36];                  /* mode, rc[4], cw[8][4]; 0 = empty slot */
    uint32_t hash;
    volatile LONG state;               /* 0 free, 1 queued, 2 ready, 3 failed */
    ID3D11PixelShader *ps;
} PsSpec;
static PsSpec s_spec[PS_SPEC_MAX];
static int s_spec_on = -1;
static HANDLE s_spec_sem;
static volatile LONG s_spec_q[PS_SPEC_MAX], s_spec_qh, s_spec_qt;
static volatile LONG s_spec_ready, s_spec_used;

static DWORD WINAPI spec_worker(LPVOID arg)
{
    static char src[sizeof s_hlsl + 2048];
    (void)arg;
    for (;;) {
        PsSpec *e;
        int n, k, i;
        ID3DBlob *b;
        WaitForSingleObject(s_spec_sem, INFINITE);
        k = s_spec_q[s_spec_qt % PS_SPEC_MAX];
        InterlockedIncrement(&s_spec_qt);
        e = &s_spec[k];
        n = snprintf(src, sizeof src,
                     "#define SPEC 1\n#define MODEX %uu\n#define RCX %uu\n#define RCY %uu\n"
                     "#define RCZ %uu\n#define RCW %uu\n#define RC_LOOP [unroll]\n"
                     "#define CW(s) k_cw[s]\nstatic const uint4 k_cw[8] = {\n",
                     e->key[0], e->key[1], e->key[2], e->key[3], e->key[4]);
        for (i = 0; i < 8; i++)
            n += snprintf(src + n, sizeof src - n, "  uint4(%uu, %uu, %uu, %uu)%s\n",
                          e->key[5 + i * 4], e->key[6 + i * 4], e->key[7 + i * 4],
                          e->key[8 + i * 4], i < 7 ? "," : "");
        n += snprintf(src + n, sizeof src - n, "};\n");
        memcpy(src + n, s_hlsl, sizeof s_hlsl - 1);
        n += (int)(sizeof s_hlsl - 1);
        b = compile_src(s_spec_comp, src, (size_t)n, "ps_main", "ps_5_0");
        if (b && SUCCEEDED(ID3D11Device_CreatePixelShader(G.dev, ID3D10Blob_GetBufferPointer(b),
                                                         ID3D10Blob_GetBufferSize(b), NULL,
                                                         &e->ps))) {
            InterlockedExchange(&e->state, 2);
            InterlockedIncrement(&s_spec_ready);
        } else {
            InterlockedExchange(&e->state, 3);
        }
        REL(b);
    }
    return 0;
}

/* The shader for this draw: its specialised one if ready, else the general. */
static ID3D11PixelShader *ps_for(const GpuCB *cb)
{
    uint32_t key[36], h = 2166136261u;
    int i, n, slot;
    if (s_spec_on < 0) {
        const char *e = getenv("RECOMP_GPU_PS_SPEC");
        s_spec_on = !(e && *e == '0') && s_spec_comp;
        if (s_spec_on) {
            s_spec_sem = CreateSemaphoreA(NULL, 0, PS_SPEC_MAX, NULL);
            if (!s_spec_sem)
                s_spec_on = 0;
            else
                CloseHandle(CreateThread(NULL, 0, spec_worker, NULL, 0, NULL));
        }
    }
    if (!s_spec_on || cb->mode[0] != 2)
        return G.ps;
    memset(key, 0, sizeof key);
    key[0] = cb->mode[0];
    memcpy(&key[1], cb->rc, 16);
    n = (int)(cb->rc[0] & 0xFF);
    if (n > 8) n = 8;
    for (i = 0; i < n; i++)
        memcpy(&key[5 + i * 4], cb->cw[i], 16);
    for (i = 0; i < 36; i++)
        h = (h ^ key[i]) * 16777619u;
    if (!h) h = 1;
    slot = (int)(h % PS_SPEC_MAX);
    for (i = 0; i < 16; i++, slot = (slot + 1) % PS_SPEC_MAX) {
        PsSpec *e = &s_spec[slot];
        if (e->hash == h && !memcmp(e->key, key, sizeof key)) {
            if (e->state == 2) {
                s_spec_used++;
                return e->ps;
            }
            return G.ps;
        }
        if (!e->hash) {
            memcpy(e->key, key, sizeof key);
            e->hash = h;
            e->state = 1;
            s_spec_q[s_spec_qh % PS_SPEC_MAX] = slot;
            InterlockedIncrement(&s_spec_qh);
            ReleaseSemaphore(s_spec_sem, 1, NULL);
            return G.ps;
        }
    }
    return G.ps;                               /* table crowded here: general */
}

/* Bind everything for state `st` into surface `s`. */
static void apply(const Nv2aGpuState *st, GSurf *s, GZbuf *z,
                  ID3D11ShaderResourceView *const srvs[4],
                  const GpuCB *cb, int clear)
{
    D3D11_VIEWPORT vp;
    D3D11_RECT sc;
    UINT stride = sizeof(Nv2aGpuVert), off = 0;
    ID3D11ShaderResourceView *srv[4];
    float bf[4];
    static ID3D11ShaderResourceView *const nulls[4] = {NULL, NULL, NULL, NULL};
    int i;

    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)(s->w * G.k); vp.Height = (float)(s->h * G.k);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(G.ctx, 1, &vp);
    sc.left = (LONG)(st->clip_x * G.k); sc.top = (LONG)(st->clip_y * G.k);
    sc.right = (LONG)((st->clip_x + st->clip_w) * G.k);
    sc.bottom = (LONG)((st->clip_y + st->clip_h) * G.k);
    ID3D11DeviceContext_RSSetScissorRects(G.ctx, 1, &sc);
    {   /* culling: one rasteriser state per (cull face, winding) */
        static ID3D11RasterizerState *rs_c[2][2];
        static int flip = -1;
        ID3D11RasterizerState *r = G.rs;
        if (flip < 0) {
            const char *e = getenv("RECOMP_CULL_FLIP");   /* =1 if everything looks inside out */
            flip = e && *e == '1';
        }
        if (!clear && (st->cull == 0x404 || st->cull == 0x405)) {
            int back = st->cull == 0x405, ccw = (st->front_ccw != 0) ^ flip;
            if (!rs_c[back][ccw]) {
                D3D11_RASTERIZER_DESC rd;
                memset(&rd, 0, sizeof rd);
                rd.FillMode = D3D11_FILL_SOLID;
                rd.CullMode = back ? D3D11_CULL_BACK : D3D11_CULL_FRONT;
                rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
                rd.DepthClipEnable = FALSE;
                rd.ScissorEnable = TRUE;
                if (FAILED(ID3D11Device_CreateRasterizerState(G.dev, &rd, &rs_c[back][ccw])))
                    rs_c[back][ccw] = NULL;
            }
            if (rs_c[back][ccw])
                r = rs_c[back][ccw];
        }
        ID3D11DeviceContext_RSSetState(G.ctx, r);
    }
    ID3D11DeviceContext_IASetInputLayout(G.ctx, G.il);
    ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(G.ctx, 0, 1, &G.vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(G.ctx, G.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(G.ctx,
        (G.ps_zoff && (cb->zo[0] != 0.0f || cb->zo[1] != 0.0f)) ? G.ps_zoff : ps_for(cb), NULL, 0);
    cb_write(G.cb, cb, sizeof *cb);
    ID3D11DeviceContext_PSSetConstantBuffers(G.ctx, 0, 1, &G.cb);
    /* Unbind first: a surface sampled by the last draw may be this draw's
     * target, and D3D refuses the target while it is still an input. */
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, (ID3D11ShaderResourceView **)nulls);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 1, &s->rtv, z ? z->dsv : NULL);
    for (i = 0; i < 4; i++)
        srv[i] = srvs[i];
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, srv);
    {
        uint32_t c = st->blend_color;
        int a = st->blend_sfactor == 0x8003 || st->blend_sfactor == 0x8004
             || st->blend_dfactor == 0x8003 || st->blend_dfactor == 0x8004;
        bf[0] = (float)((c >> 16) & 0xFF) / 255.0f;
        bf[1] = (float)((c >> 8) & 0xFF) / 255.0f;
        bf[2] = (float)(c & 0xFF) / 255.0f;
        bf[3] = (float)(c >> 24) / 255.0f;
        if (a)                            /* CONSTANT_ALPHA: alpha in every channel */
            bf[0] = bf[1] = bf[2] = bf[3];
    }
    ID3D11DeviceContext_OMSetBlendState(G.ctx, blend_state(st, clear), bf, 0xFFFFFFFFu);
    {
        /* RECOMP_REFL_NODEPTH=1: no depth test on small offscreen targets
         * (Orta's 128x96 river reflection). Its triangles come with z far
         * beyond the depth range, clamp to the far plane and fail LESS
         * against the cleared buffer -- only a sliver of the mirror image
         * survived. Drawn in order instead: sky first, then the walls. */
        static int rn = -1;
        int nd = 0;
        if (rn < 0) {
            const char *e = getenv("RECOMP_REFL_NODEPTH");
            rn = e && *e == '1';
        }
        (void)rn;
        ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, depth_state(st, clear || nd), 0);
    }
}

/* Draw the vertices gathered for the current state. */
static void flush(void)
{
    D3D11_MAPPED_SUBRESOURCE mp;
    GpuCB cb;

    if (!G.nverts || !G.cur_valid || !G.cur_surf)
        return;
    int at = ring_map(G.vb, (UINT)(sizeof(Nv2aGpuVert) * GPU_VB_VERTS), &s_vb_pos,
                      (UINT)(sizeof(Nv2aGpuVert) * G.nverts), (UINT)sizeof(Nv2aGpuVert), &mp);
    if (at < 0) {
        G.nverts = 0;
        return;
    }
    memcpy(mp.pData, G.verts, sizeof(Nv2aGpuVert) * G.nverts);
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.vb, 0);
    fill_cb(&cb, &G.cur, G.cur_tfl, G.cur_tlev);
    /* TEXDBG 2 marks vertex-program draws only; 4 marks these. */
    if (cb.rc2[3] & 4) cb.rc2[3] |= 2; else cb.rc2[3] &= ~2u;
    apply(&G.cur, G.cur_surf, G.cur_zb, G.cur_srv, &cb, 0);
    ID3D11DeviceContext_Draw(G.ctx, G.nverts, (UINT)at / (UINT)sizeof(Nv2aGpuVert));
    if (color_written(&G.cur))
        surf_touch(G.cur_surf, G.cur.clip_x, G.cur.clip_y,
                   G.cur.clip_x + G.cur.clip_w, G.cur.clip_y + G.cur.clip_h);
    G.st_draws++;
    G.nverts = 0;
}

/* A texture that is exactly a GPU surface's bytes, in a format the surface
 * holds as-is: bind the render target itself, no read-back, no decode. Not
 * the surface being drawn into (D3D cannot read and write one resource). */
static GSurf *surf_as_texture(const Nv2aGpuTexDesc *d, const GSurf *target)
{
    int i;
    if (d->cube || d->palette)
        return NULL;
    for (i = 0; i < GPU_MAX_SURF; i++) {
        GSurf *s = &G.surf[i];
        /* Swizzled formats too, when the texture is the whole surface: the
         * title drew it as a swizzled surface to sample it so. Decoded from
         * our linear write-back instead, Orta's river reflection (128x128
         * SZ_X8R8G8B8) came out as a checkerboard of shuffled blocks. */
        int sz = (s->bpp == 4 && (d->color == 0x06 || d->color == 0x07))
              || (s->bpp == 2 && d->color == 0x05);
        if (!s->tex || !s->srv || s == target || s->base != d->offset)
            continue;
        if (sz ? (d->width != s->w || d->height != s->h)
               : (s->pitch != d->pitch || d->width > s->w || d->height > s->h))
            continue;
        if (sz || (s->bpp == 4 && (d->color == 0x12 || d->color == 0x1E))
            || (s->bpp == 2 && d->color == 0x11)) {
            if (s->checked != G.frame) {          /* CPU wrote it since? */
                s->checked = G.frame;
                if (!s->dirty && s->base != 0x80000000u && surf_hash(s) != s->hash)
                    surf_upload(s);
            }
            s->used = G.frame;
            G.st_direct++;
            return s;
        }
    }
    return NULL;
}

/* ── Batches ───────────────────────────────────────────────────────────── */

void nv2a_gpu_begin(const Nv2aGpuState *st)
{
    int i;

    if (G.state != 1)
        return;
    forget_pending();
    if (G.cur_valid && memcmp(&G.cur, st, sizeof *st) == 0)
        return;
    flush();
    G.cur = *st;
    G.cur_valid = 1;
    {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    G.cur_surf = surf_get(st);
    for (i = 0; i < 4; i++) {
        GSurf *rs;
        GTex *t;
        G.cur_srv[i] = NULL;
        G.cur_tfl[i] = 0;
        G.cur_tlev[i] = 1;
        if (!st->tex[i].valid)
            continue;
        {   /* A depth format: the depth buffer a shadow pass drew, as xemu
             * binds it. Undecoded before 19 Oct -- every shadow was missing,
             * and each draw re-hashed 512 KB of guest memory for nothing. */
            uint32_t c = st->tex[i].color;
            if (c == 0x2C || c == 0x2E || c == 0x2F || c == 0x30 || c == 0x31) {
                int j;
                for (j = 0; j < GPU_MAX_ZBUF; j++) {
                    GZbuf *zb = &G.zb[j];
                    if (zb->tex && zb->srv
                        && (zb->zeta & 0x03FFFFFFu) == (st->tex[i].offset & 0x03FFFFFFu)
                        && zb->w >= st->tex[i].width && zb->h >= st->tex[i].height) {
                        G.cur_srv[i] = zb->srv;
                        G.cur_tfl[i] = 1u | 256u | (G.k > 1 ? 64u : 0u);
                        break;
                    }
                }
                continue;                 /* no such buffer: the stage reads white */
            }
        }
        /* A surface the GPU drew: sample it where it is. */
        rs = surf_as_texture(&st->tex[i], G.cur_surf);
        if (rs) {
            G.cur_srv[i] = rs->srv;
            G.cur_tfl[i] = 1u | (st->tex[i].color == 0x1E || st->tex[i].color == 0x07 ? 16u : 0u)
                         | (G.k > 1 ? 64u : 0u);
            continue;
        }
        t = tex_get(&st->tex[i]);
        if (t) {
            G.cur_srv[i] = t->srv;
            G.cur_tfl[i] = 1u;
            G.cur_tlev[i] = t->mips;
            if (st->tex[i].color == 0x28) {    /* the ripple map: how it is mipmapped */
                static int said;
                if (said++ < 4) {
                    fprintf(stderr, "[RIPPLE] stage %d tex %08X %ux%u levels %u mips %u filter %08X addr %u/%u\n",
                            i, st->tex[i].offset, st->tex[i].width, st->tex[i].height,
                            st->tex[i].levels, (unsigned)t->mips, st->tex[i].filter,
                            st->tex[i].addr_u, st->tex[i].addr_v);
                    fflush(stderr);
                }
            }
        }
    }
    QueryPerformanceCounter(&t1);
    G.t_tex += t1.QuadPart - t0.QuadPart;
    s_tex_total += t1.QuadPart - t0.QuadPart;
    }
    G.cur_zb = NULL;
    if (G.cur_surf && st->mode != NV2A_GPU_MODE_2D && (st->depth_test || st->depth_mask))
        G.cur_zb = zbuf_get(st->zeta_offset, G.cur_surf->w, G.cur_surf->h);
    if (G.cur_zb)                         /* never read the depth being written */
        for (i = 0; i < 4; i++)
            if (G.cur_srv[i] && G.cur_srv[i] == G.cur_zb->srv) {
                G.cur_srv[i] = NULL;
                G.cur_tfl[i] = 0;
            }
    G.zscale = zscale_for(st->zeta_format, st->z_float);
}

static void put_vert(Nv2aGpuVert *o, const Nv2aGpuVert *v, const GSurf *s)
{
    float w = v->pos[3];
    *o = *v;
    o->pos[0] = ((v->pos[0] / (float)s->w) * 2.0f - 1.0f) * w;
    o->pos[1] = (1.0f - (v->pos[1] / (float)s->h) * 2.0f) * w;
    o->pos[2] = v->pos[2] * G.zscale * w;
    o->pos[3] = w;
}

void nv2a_gpu_tri(const Nv2aGpuVert *a, const Nv2aGpuVert *b, const Nv2aGpuVert *c)
{
    if (G.state != 1 || !G.cur_valid || !G.cur_surf)
        return;
    if (G.nverts + 3 > GPU_VB_VERTS)
        flush();
    put_vert(&G.verts[G.nverts++], a, G.cur_surf);
    put_vert(&G.verts[G.nverts++], b, G.cur_surf);
    put_vert(&G.verts[G.nverts++], c, G.cur_surf);
    G.st_tris++;
}

void nv2a_gpu_clear(const Nv2aGpuState *st, int color, uint32_t argb,
                    int depth, uint32_t zstencil_clear)
{
    GSurf *s;
    ID3D11ShaderResourceView *none[4] = {NULL, NULL, NULL, NULL};

    if (G.state != 1)
        return;
    flush();
    G.cur_valid = 0;                      /* re-bind on the next batch */
    s = surf_get(st);
    if (!s)
        return;
    if (depth) {
        GZbuf *z = zbuf_get(st->zeta_offset, s->w, s->h);
        if (z) {
            float v = st->zeta_format == 1 ? (float)(zstencil_clear & 0xFFFF)
                                           : (float)(zstencil_clear >> 8);
            v *= zscale_for(st->zeta_format, st->z_float);
            if (v > 1.0f || st->z_float) v = 1.0f;   /* float clears: the far plane */
            ID3D11DeviceContext_ClearDepthStencilView(G.ctx, z->dsv, D3D11_CLEAR_DEPTH, v, 0);
        }
    }
    if (color && st->clip_w && st->clip_h) {
        /* A quad over the clip rectangle rather than ClearView: the clear
         * honours the clip exactly as the software clear does, and needs
         * nothing past D3D11.0. */
        D3D11_MAPPED_SUBRESOURCE mp;
        Nv2aGpuVert q[6];
        GpuCB cb;
        int i;
        static const float cx[6] = {-1, 1, -1, -1, 1, 1}, cy[6] = {1, 1, -1, -1, 1, -1};
        memset(q, 0, sizeof q);
        for (i = 0; i < 6; i++) {
            q[i].pos[0] = cx[i]; q[i].pos[1] = cy[i];
            q[i].pos[2] = 0.0f;  q[i].pos[3] = 1.0f;
        }
        int at = ring_map(G.vb, (UINT)(sizeof(Nv2aGpuVert) * GPU_VB_VERTS), &s_vb_pos,
                          (UINT)sizeof q, (UINT)sizeof(Nv2aGpuVert), &mp);
        if (at < 0)
            return;
        memcpy(mp.pData, q, sizeof q);
        ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.vb, 0);
        memset(&cb, 0, sizeof cb);
        cb.mode[0] = 3;
        cb.clear[0] = (float)((argb >> 16) & 0xFF) / 255.0f;
        cb.clear[1] = (float)((argb >> 8) & 0xFF) / 255.0f;
        cb.clear[2] = (float)(argb & 0xFF) / 255.0f;
        cb.clear[3] = (float)(argb >> 24) / 255.0f;
        apply(st, s, NULL, none, &cb, 1);
        ID3D11DeviceContext_Draw(G.ctx, 6, (UINT)at / (UINT)sizeof(Nv2aGpuVert));
        if (st->surf_base != 0x80000000u)
            surf_touch(s, st->clip_x, st->clip_y, st->clip_x + st->clip_w,
                       st->clip_y + st->clip_h);
    }
}

/* ── Presentation ─────────────────────────────────────────────────────────
 * The shown frame straight from the GPU into the window, at whatever size the
 * window is, so a scaled render is actually seen at its resolution. Guest
 * memory still gets its 640x480 copy (screenshots, titles that read it). */
extern HWND xbox_FramebufferWindowHwnd(void);
extern int xbox_FramebufferWidescreen(void);
extern void xbox_FramebufferGpuPresented(void);

static const GUID k_IID_IDXGIDevice =
    {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
static const GUID k_IID_IDXGIFactory =
    {0x7b7166ec, 0x21c7, 0x44ae, {0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69}};
static const GUID k_IID_ID3D11Texture2D =
    {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

static int swap_create(HWND hwnd, UINT w, UINT h)
{
    IDXGIDevice *dd = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory *f = NULL;
    DXGI_SWAP_CHAIN_DESC sd;
    HRESULT hr;
    hr = ID3D11Device_QueryInterface(G.dev, &k_IID_IDXGIDevice, (void **)&dd);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dd, &ad);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_GetParent(ad, &k_IID_IDXGIFactory, (void **)&f);
    if (SUCCEEDED(hr)) {
        memset(&sd, 0, sizeof sd);
        sd.BufferDesc.Width = w;
        sd.BufferDesc.Height = h;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.OutputWindow = hwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = (DXGI_SWAP_EFFECT)4;          /* FLIP_DISCARD (Windows 10) */
        hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)G.dev, &sd, &G.swap);
        if (FAILED(hr)) {
            sd.BufferCount = 1;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = IDXGIFactory_CreateSwapChain(f, (IUnknown *)G.dev, &sd, &G.swap);
        }
        if (SUCCEEDED(hr))
            IDXGIFactory_MakeWindowAssociation(f, hwnd, DXGI_MWA_NO_ALT_ENTER);
    }
    REL(f);
    REL(ad);
    REL(dd);
    if (FAILED(hr)) {
        fprintf(stderr, "  [GPU11] swap chain failed 0x%08lX -- GDI window\n", (unsigned long)hr);
        return 0;
    }
    G.sw_w = w; G.sw_h = h;
    fprintf(stderr, "  [GPU11] presenting through a swap chain (%ux%u)\n", w, h);
    return 1;
}

/* RECOMP_GPU_SHOT=<path>: every 600 frames, the shown surface at full
 * internal resolution as <path>NNN.bmp, to check what the scaled renderer
 * draws (the guest-memory dumps are always 640x480). */
static void gpu_shot(GSurf *s)
{
    static const char *path = (const char *)1;
    static int n;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE mp;
    char fn[512];
    FILE *f;
    UINT w, h, y;
    if (path == (const char *)1)
        path = getenv("RECOMP_GPU_SHOT");
    if (!path || G.frame % 600 != 300 || n >= 20)
        return;
    ID3D11Texture2D_GetDesc(s->tex, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &st)))
        return;
    ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)st, (ID3D11Resource *)s->tex);
    if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp))) {
        REL(st);
        return;
    }
    w = td.Width;
    h = s->h > 480 ? 480 * G.k : td.Height;
    snprintf(fn, sizeof fn, "%s%03d.bmp", path, n++);
    f = fopen(fn, "wb");
    if (f) {
        uint32_t row = w * 4, size = 54 + row * h;
        uint8_t hd[54];
        memset(hd, 0, sizeof hd);
        hd[0] = 'B'; hd[1] = 'M';
        memcpy(hd + 2, &size, 4);
        hd[10] = 54; hd[14] = 40;
        memcpy(hd + 18, &w, 4);
        {
            int32_t nh = -(int32_t)h;             /* top-down */
            memcpy(hd + 22, &nh, 4);
        }
        hd[26] = 1; hd[28] = 32;
        fwrite(hd, 1, 54, f);
        for (y = 0; y < h; y++)
            fwrite((const uint8_t *)mp.pData + (size_t)y * mp.RowPitch, 1, row, f);
        fclose(f);
        fprintf(stderr, "  [GPU11] shot %s (%ux%u)\n", fn, w, h);
    }
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)st, 0);
    REL(st);
}

/* RECOMP_GPU_SURFDUMP=<path>: every 600 frames, the small offscreen
 * surfaces Orta builds its water from (128x96 and 128x128 targets), as the
 * GPU holds them. Twelve files at most. */
static void gpu_surfdump(void)
{
    static const char *path = (const char *)1;
    static int n;
    int i;
    if (path == (const char *)1)
        path = getenv("RECOMP_GPU_SURFDUMP");
    static unsigned fc;
    if (!path || (++fc % 600) != 310 || n >= 12)
        return;
    fprintf(stderr, "  [GPU11] surface dump at flip %u\n", fc);
    for (i = 0; i < GPU_MAX_SURF && n < 12; i++) {
        GSurf *s = &G.surf[i];
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D *st = NULL;
        D3D11_MAPPED_SUBRESOURCE mp;
        char fn[512];
        FILE *f;
        UINT y;
        if (!s->tex)
            continue;
        if (s->w > 128) {
            /* Orta's 512x384 scene target, once, a minute in: what lies
             * outside its viewport (the letterbox rows). */
            static int big;
            if (s->base != 0x80026000u || big || fc < 3600)
                continue;
            big = 1;
        }
        ID3D11Texture2D_GetDesc(s->tex, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &st)))
            continue;
        ID3D11DeviceContext_CopyResource(G.ctx, (ID3D11Resource *)st, (ID3D11Resource *)s->tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &mp))) {
            snprintf(fn, sizeof fn, "%s%02d_%08X.bmp", path, n++, s->base);
            f = fopen(fn, "wb");
            if (f) {
                uint32_t w = td.Width, h = td.Height, row = w * 4, size = 54 + row * h;
                uint8_t hd[54];
                int32_t nh = -(int32_t)h;
                memset(hd, 0, sizeof hd);
                hd[0] = 'B'; hd[1] = 'M';
                memcpy(hd + 2, &size, 4);
                hd[10] = 54; hd[14] = 40;
                memcpy(hd + 18, &w, 4);
                memcpy(hd + 22, &nh, 4);
                hd[26] = 1; hd[28] = 32;
                fwrite(hd, 1, 54, f);
                for (y = 0; y < h; y++)
                    fwrite((const uint8_t *)mp.pData + (size_t)y * mp.RowPitch, 1, row, f);
                fclose(f);
            }
            ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)st, 0);
        }
        REL(st);
    }
}

/* The frame with the PVIDEO overlay composited over it, when the title has
 * one up (Orta's opening movie: sound and subtitles, but the picture is on
 * the overlay). Returns 0 when there is none, and the plain blit runs. */
int nv2a_exec_pvideo(const uint32_t **rgba, uint32_t *w, uint32_t *h,
                     float rect[4], float key[4]);

static int present_overlay(const D3D11_VIEWPORT *vp, GSurf *s,
                           const float f[4], const float g[4])
{
    static ID3D11ShaderResourceView *const nulls[4] = {NULL, NULL, NULL, NULL};
    const uint32_t *px;
    uint32_t w, h, y;
    float rect[4], key[4];
    D3D11_MAPPED_SUBRESOURCE mp;
    BlitCB c;
    ID3D11ShaderResourceView *srv[2];
    if (!G.ps_overlay || !nv2a_exec_pvideo(&px, &w, &h, rect, key))
        return 0;
    if (!G.ov_tex || G.ov_w != w || G.ov_h != h) {
        D3D11_TEXTURE2D_DESC td;
        REL(G.ov_srv);
        REL(G.ov_tex);
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(G.dev, &td, NULL, &G.ov_tex))
            || FAILED(ID3D11Device_CreateShaderResourceView(G.dev, (ID3D11Resource *)G.ov_tex,
                                                            NULL, &G.ov_srv))) {
            REL(G.ov_srv);
            REL(G.ov_tex);
            return 0;
        }
        G.ov_w = w; G.ov_h = h;
    }
    if (FAILED(ID3D11DeviceContext_Map(G.ctx, (ID3D11Resource *)G.ov_tex, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &mp)))
        return 0;
    for (y = 0; y < h; y++)
        memcpy((uint8_t *)mp.pData + (size_t)y * mp.RowPitch, px + (size_t)y * w, (size_t)w * 4);
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.ov_tex, 0);

    memset(&c, 0, sizeof c);
    c.b[1] = 1;
    memcpy(c.f, f, sizeof c.f);
    memcpy(c.g, g, sizeof c.g);
    memcpy(c.o, rect, sizeof c.o);
    memcpy(c.k, key, sizeof c.k);
    cb_write(G.cb_blit, &c, sizeof c);
    srv[0] = s->srv; srv[1] = G.ov_srv;
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, (ID3D11ShaderResourceView **)nulls);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 1, &G.bb_rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(G.ctx, 1, vp);
    ID3D11DeviceContext_RSSetState(G.ctx, G.rs_blit);
    ID3D11DeviceContext_IASetInputLayout(G.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(G.ctx, G.vs_blit, NULL, 0);
    ID3D11DeviceContext_PSSetShader(G.ctx, G.ps_overlay, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(G.ctx, 0, 1, &G.cb_blit);
    ID3D11DeviceContext_PSSetSamplers(G.ctx, 0, 1, &G.smp);
    ID3D11DeviceContext_OMSetBlendState(G.ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(G.ctx, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 2, srv);
    ID3D11DeviceContext_Draw(G.ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(G.ctx, 0, 4, (ID3D11ShaderResourceView **)nulls);
    ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
    return 1;
}

/* Hooks a game can install (orta/src/overlay.cpp): something drawn over
 * each presented frame, and a pause that holds the flip -- the last frame
 * redrawn with the overlay on it -- while it says so. Live settings: -1
 * means "as settings.ini said at start". */
void (*g_present_overlay_hook)(void *dev, void *ctx, void *rtv, unsigned w, unsigned h);
int  (*g_present_pause_hook)(void);
volatile long  g_present_vsync = -1;
volatile float g_present_crop[2] = { -1.0f, -1.0f };

static void gpu_present(GSurf *s)
{
    static const float black[4] = {0, 0, 0, 1};
    HWND hwnd;
    RECT c;
    UINT cw, ch, w, h;
    D3D11_VIEWPORT vp;
    float f[4], g[4];
    if (!G.present_on || !s || !s->srv)
        return;
    hwnd = xbox_FramebufferWindowHwnd();
    if (!hwnd || !GetClientRect(hwnd, &c))
        return;
    cw = (UINT)(c.right - c.left); ch = (UINT)(c.bottom - c.top);
    if (cw < 8 || ch < 8)
        return;                                   /* minimised */
    if (!G.swap) {
        if (!swap_create(hwnd, cw, ch)) {
            G.present_on = 0;
            return;
        }
    } else if (cw != G.sw_w || ch != G.sw_h) {
        ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
        REL(G.bb_rtv);
        IDXGISwapChain_ResizeBuffers(G.swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
        G.sw_w = cw; G.sw_h = ch;
    }
    if (!G.bb_rtv) {
        ID3D11Texture2D *bb = NULL;
        if (FAILED(IDXGISwapChain_GetBuffer(G.swap, 0, &k_IID_ID3D11Texture2D, (void **)&bb))
            || FAILED(ID3D11Device_CreateRenderTargetView(G.dev, (ID3D11Resource *)bb, NULL,
                                                          &G.bb_rtv))) {
            REL(bb);
            return;
        }
        REL(bb);
    }
    ID3D11DeviceContext_ClearRenderTargetView(G.ctx, G.bb_rtv, black);
    if (xbox_FramebufferWidescreen()) {
        w = cw; h = cw * 9 / 16;
        if (h > ch) { h = ch; w = ch * 16 / 9; }
    } else {
        w = cw; h = cw * 3 / 4;
        if (h > ch) { h = ch; w = ch * 4 / 3; }
    }
    vp.TopLeftX = (float)((cw - w) / 2); vp.TopLeftY = (float)((ch - h) / 2);
    vp.Width = (float)w; vp.Height = (float)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    f[0] = vp.TopLeftX; f[1] = vp.TopLeftY; f[2] = 1.0f / (float)w; f[3] = 1.0f / (float)h;
    g[0] = 1.0f;                                  /* the 640x480 picture of a taller surface */
    g[1] = s->h > 480 ? 480.0f / (float)s->h : 1.0f;
    {   /* Crop (percent per side, RECOMP_CROP_X / _Y, settings.ini [Video]
         * CropX / CropY): Xbox games leave a dark border inside the picture
         * for TV overscan. On a PC display it shows; cropping zooms past it. */
        static float cx = -1.0f, cy;
        if (cx < 0.0f) {
            const char *e = getenv("RECOMP_CROP_X");
            cx = e ? (float)atof(e) / 100.0f : 0.0f;
            e = getenv("RECOMP_CROP_Y");
            cy = e ? (float)atof(e) / 100.0f : 0.0f;
            if (cx < 0.0f || cx > 0.2f) cx = 0.0f;
            if (cy < 0.0f || cy > 0.2f) cy = 0.0f;
        }
        g[2] = g_present_crop[0] >= 0.0f ? g_present_crop[0] : cx;
        g[3] = g_present_crop[1] >= 0.0f ? g_present_crop[1] : cy;
    }
    if (!present_overlay(&vp, s, f, g))
        blit(G.bb_rtv, &vp, s->srv, G.ps_present, 1, f, g);
    if (g_present_overlay_hook) {
        ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 1, &G.bb_rtv, NULL);
        g_present_overlay_hook(G.dev, G.ctx, G.bb_rtv, cw, ch);
        ID3D11DeviceContext_OMSetRenderTargets(G.ctx, 0, NULL, NULL);
    }
    {
        static int vs = -1;
        if (vs < 0) {
            const char *e = getenv("RECOMP_VSYNC");
            vs = e && *e == '1';
        }
        IDXGISwapChain_Present(G.swap, (UINT)(g_present_vsync >= 0 ? g_present_vsync : vs), 0);
    }
    xbox_FramebufferGpuPresented();
}

void nv2a_gpu_flip(uint32_t present_base)
{
    int i;
    LARGE_INTEGER t0, t1;
    if (G.state != 1)
        return;
    forget_pending();
    flush();
    /* Only the frame being shown. Everything else stays on the GPU until
     * something reads it: a texture fetch or a dump syncs it then. */
    QueryPerformanceCounter(&t0);
    {
        static int async = -1;
        if (async < 0) {
            const char *e = getenv("RECOMP_GPU_ASYNC_FLIP");
            async = !e || strcmp(e, "0") != 0;
        }
        for (i = 0; i < GPU_MAX_SURF; i++) {
            GSurf *s = &G.surf[i];
            if (!s->tex)
                continue;
            if (s->aq[0] || s->aq[1])
                surf_async_complete(s, 0);
            if (s->base == present_base && !(async && surf_readback_async(s)))
                surf_readback(s);
        }
        for (i = 0; i < GPU_MAX_SURF; i++)
            if (G.surf[i].tex && G.surf[i].base == present_base) {
                gpu_shot(&G.surf[i]);
                gpu_surfdump();
                gpu_present(&G.surf[i]);
                /* Paused (the settings menu): keep showing this frame,
                 * with the menu over it, until the pause ends. The game
                 * waits on its flip meanwhile. */
                while (g_present_pause_hook && g_present_pause_hook()) {
                    Sleep(15);
                    gpu_present(&G.surf[i]);
                }
                break;
            }
    }
    QueryPerformanceCounter(&t1);
    G.t_flip += t1.QuadPart - t0.QuadPart;
    G.cur_valid = 0;                      /* textures are re-checked each frame */
    G.frame++;
    if (G.frame % 300 == 0) {
        fprintf(stderr, "  [GPU11] frame %u: %u draws, %u tris, %u texture uploads"
                        " (%.1f MB), %u surface uploads, %u write-backs, %u surfaces sampled"
                        " directly (last 300 frames)\n",
                G.frame, G.st_draws, G.st_tris, G.st_uploads,
                (double)G.st_upload_bytes / (1024.0 * 1024.0), G.st_surf_uploads,
                G.st_readbacks, G.st_direct);
        {
            LARGE_INTEGER f;
            double ms;
            QueryPerformanceFrequency(&f);
            ms = 1000.0 / (double)f.QuadPart / 300.0;
            fprintf(stderr, "  [GPU11]   per frame: %.1f ms flip write-back, %.1f ms texture"
                            " checks/uploads, %.1f ms visibility waits, %.1f ms draw calls\n",
                    (double)G.t_flip * ms, (double)G.t_tex * ms, (double)G.t_zpass * ms,
                    (double)G.t_draw * ms);
            G.t_flip = G.t_tex = G.t_zpass = G.t_draw = 0;
        }
        fflush(stderr);
        G.st_draws = G.st_tris = G.st_uploads = G.st_readbacks = G.st_surf_uploads = 0;
        G.st_direct = 0;
        G.st_upload_bytes = 0;
        if (s_spec_on > 0)
            fprintf(stderr, "  [GPU11]   combiner shaders: %ld specialised and ready,"
                            " %ld draws used one\n", (long)s_spec_ready, (long)s_spec_used);
        s_spec_used = 0;
    }
}

int nv2a_gpu_vsh_enabled(void)
{
    return nv2a_gpu_enabled() && G.vsh_on;
}

/* The executor writes a batch's vertices straight into the vertex ring:
 * nv2a_gpu_vtx_begin maps room for up to nmax of them, and
 * nv2a_gpu_draw_program(in = NULL) draws what was written (or
 * nv2a_gpu_vtx_cancel gives it back). Building them in a side array first
 * and copying -- 256 bytes a vertex, ~25 MB a frame -- was most of the
 * "d3d" share of the executor's time. */
static int s_vtx_mapped, s_vtx_at;

void nv2a_gpu_hitch_counters(double *tex_ms, uint32_t *uploads)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    *tex_ms = (double)s_tex_total * 1000.0 / (double)f.QuadPart;
    *uploads = s_uploads_total;
}

float *nv2a_gpu_vtx_begin(uint32_t nmax)
{
    D3D11_MAPPED_SUBRESOURCE mp;
    int at;
    if (G.state != 1 || !G.vsh_on || !nmax || nmax > 65536u || s_vtx_mapped)
        return NULL;
    at = ring_map(G.vb_prog, 65536u * 256u, &s_vbp_pos, nmax * 256u, 256u, &mp);
    if (at < 0)
        return NULL;
    s_vtx_mapped = 1;
    s_vtx_at = at;
    return (float *)mp.pData;
}

void nv2a_gpu_vtx_cancel(void)
{
    if (s_vtx_mapped) {
        ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.vb_prog, 0);
        s_vtx_mapped = 0;
    }
}

void nv2a_gpu_draw_program(const Nv2aGpuState *st, const float *in, uint32_t nverts,
                           const uint32_t *idx, uint32_t nidx, const uint32_t *prog,
                           uint32_t start, const float *consts)
{
    int premapped = 0, pre_at = 0;
    if (!in) {                            /* written by nv2a_gpu_vtx_begin's caller */
        if (!s_vtx_mapped)
            return;
        nv2a_gpu_vtx_cancel();            /* unmap: the data stays */
        premapped = 1;
        pre_at = s_vtx_at;
    }
    D3D11_MAPPED_SUBRESOURCE mp;
    static GpuVP vp;
    GpuCB cb;
    UINT stride = 256, off = 0;
    int vat, iat;
    ID3D11Buffer *vcb[2];

    LARGE_INTEGER t0, t1;
    if (G.state != 1 || !G.vsh_on || !nverts || !nidx
        || nverts > 65536u || nidx > 6u * 65536u)
        return;
    QueryPerformanceCounter(&t0);
    nv2a_gpu_begin(st);
    flush();                              /* keep submission order */
    if (!G.cur_surf)
        return;

    if (premapped) {
        vat = pre_at;
    } else {
        vat = ring_map(G.vb_prog, 65536u * 256u, &s_vbp_pos, nverts * 256u, 256u, &mp);
        if (vat < 0)
            return;
        memcpy(mp.pData, in, (size_t)nverts * 256u);
        ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.vb_prog, 0);
    }
    iat = ring_map(G.ib_prog, 6u * 65536u * 4u, &s_ibp_pos, nidx * 4u, 4u, &mp);
    if (iat < 0)
        return;
    memcpy(mp.pData, idx, (size_t)nidx * 4u);
    ID3D11DeviceContext_Unmap(G.ctx, (ID3D11Resource *)G.ib_prog, 0);

    memcpy(vp.prog, prog, sizeof vp.prog);
    vp.vinfo[0] = start;
    vp.vinfo[1] = st->fog_enable;
    vp.vinfo[2] = st->fog_mode;
    vp.vinfo[3] = st->mode == NV2A_GPU_MODE_RC;
    vp.vpar[0] = 2.0f / (float)G.cur_surf->w;
    vp.vpar[1] = 2.0f / (float)G.cur_surf->h;
    vp.vpar[2] = G.zscale;
    vp.vpar[3] = 0.0f;
    {   /* RECOMP_REFL_CENTRE=1: a vertex program drawing into a small
         * offscreen target (Orta's 128x96 river reflection) is shifted by
         * half the target, as if the viewport offset had the centre in it:
         * its positions arrive centred on 0 (c59 is only 0.53). */
        static int rc = -1;
        if (rc < 0) {
            const char *e = getenv("RECOMP_REFL_CENTRE");
            rc = e ? atoi(e) : 0;              /* 1 both, 2 x only, 3 y only */
        }
        if (rc && start != 0xFFFFu && G.cur_surf->w <= 128 && G.cur_surf->h < G.cur_surf->w)
            vp.vpar[3] = (float)rc;
    }
    {   /* Lift Orta's letterbox: the 3D scene is drawn with a viewport
         * ~6% shorter than its 512x384 target, then pasted onto the 640x480
         * frame with a viewport just as much shorter -- the black bands are
         * the frame outside that paste. The scene's triangles run past its
         * viewport anyway, so the target already holds picture there: widen
         * the paste to the full frame and stretch its texture coordinates by
         * the same factor about the centre, and the bands fill with the real
         * scene at the same scale. RECOMP_LETTERBOX=1 keeps the bands. */
        static int keep = -1;
        if (keep < 0) {
            const char *e = getenv("RECOMP_LETTERBOX");
            keep = e && *e == '1';
        }
        memset(vp.lb, 0, sizeof vp.lb);
        if (!keep && G.cur_surf->w == 640 && G.cur_surf->h == 480 && !st->tex[0].valid)
            vp.lb[3] = 2.0f;                  /* the title's own bands: see lb_band */
        else if (!keep && start != 0xFFFFu && G.cur_surf->w == 640 && G.cur_surf->h == 480
            && st->tex[0].valid && st->tex[0].width == 512 && st->tex[0].height == 384
            && st->vpy[0] < -200.0f && st->vpy[0] > -239.0f
            && st->vpy[1] > 239.0f && st->vpy[1] < 242.0f) {
            vp.lb[0] = 240.0f / -st->vpy[0];
            vp.lb[1] = st->vpy[1];
            vp.lb[2] = 192.0f;
            vp.lb[3] = 1.0f;
        }
    }
    vp.fogp[0] = st->fog_param[0];
    vp.fogp[1] = st->fog_param[1];
    vp.fogp[2] = vp.fogp[3] = 0.0f;
    vp.pt[0] = st->pt_size > 0.0f ? st->pt_size : 1.0f;
    vp.pt[1] = st->pt_params_on ? 1.0f : 0.0f;
    vp.pt[2] = st->pt_sprite ? 1.0f : 0.0f;
    vp.pt[3] = 1.0f;
    memcpy(vp.ptp, st->pt_params, sizeof vp.ptp);
    cb_write(G.cb_vp, &vp, sizeof vp);
    cb_write(G.cb_vc, consts, 192u * 16u);

    fill_cb(&cb, st, G.cur_tfl, G.cur_tlev);
    apply(st, G.cur_surf, G.cur_zb, G.cur_srv, &cb, 0);
    ID3D11DeviceContext_VSSetShader(G.ctx, G.vs_prog, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(G.ctx, G.il_prog);
    ID3D11DeviceContext_IASetVertexBuffers(G.ctx, 0, 1, &G.vb_prog, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(G.ctx, G.ib_prog, DXGI_FORMAT_R32_UINT, 0);
    vcb[0] = G.cb_vp;
    vcb[1] = G.cb_vc;
    ID3D11DeviceContext_VSSetConstantBuffers(G.ctx, 1, 2, vcb);
    if (st->prim_kind) {
        ID3D11GeometryShader *gs = st->prim_kind == 1 ? G.gs_pt : G.gs_ln;
        ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, st->prim_kind == 1
            ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST : D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        ID3D11DeviceContext_GSSetShader(G.ctx, gs, NULL, 0);
        ID3D11DeviceContext_GSSetConstantBuffers(G.ctx, 1, 2, vcb);
    }
    ID3D11DeviceContext_DrawIndexed(G.ctx, nidx, (UINT)iat / 4u, (INT)((UINT)vat / 256u));
    if (st->prim_kind) {
        ID3D11DeviceContext_GSSetShader(G.ctx, NULL, NULL, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(G.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    if (color_written(st))
        surf_touch(G.cur_surf, st->clip_x, st->clip_y, st->clip_x + st->clip_w,
                   st->clip_y + st->clip_h);
    G.st_draws++;
    G.st_tris += nidx / 3;
    QueryPerformanceCounter(&t1);
    G.t_draw += t1.QuadPart - t0.QuadPart;
}

/* ── Visibility tests ──────────────────────────────────────────────────── */

static void query_wait(void)
{
    UINT64 n = 0;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    while (ID3D11DeviceContext_GetData(G.ctx, (ID3D11Asynchronous *)G.query,
                                       &n, sizeof n, 0) == S_FALSE)
        ;
    QueryPerformanceCounter(&t1);
    G.t_zpass += t1.QuadPart - t0.QuadPart;
    G.zpass_acc += (uint32_t)n;
    G.query_pending = 0;
}

void nv2a_gpu_zpass_enable(uint32_t on)
{
    if (G.state != 1 || !G.query)
        return;
    flush();
    if (on && !G.query_active) {
        if (G.query_pending)
            query_wait();
        ID3D11DeviceContext_Begin(G.ctx, (ID3D11Asynchronous *)G.query);
        G.query_active = 1;
    } else if (!on && G.query_active) {
        ID3D11DeviceContext_End(G.ctx, (ID3D11Asynchronous *)G.query);
        G.query_active = 0;
        G.query_pending = 1;
    }
}

uint32_t nv2a_gpu_zpass_collect(void)
{
    uint32_t n;
    if (G.state != 1 || !G.query)
        return 0;
    flush();
    if (G.query_active) {
        ID3D11DeviceContext_End(G.ctx, (ID3D11Asynchronous *)G.query);
        query_wait();
        ID3D11DeviceContext_Begin(G.ctx, (ID3D11Asynchronous *)G.query);
    } else if (G.query_pending) {
        query_wait();
    }
    n = G.zpass_acc / (G.k * G.k);       /* samples at the guest's resolution */
    G.zpass_acc = 0;
    return n;
}

#else  /* !_WIN32: no D3D11, the software rasteriser is all there is. */

int  nv2a_gpu_enabled(void) { return 0; }
void nv2a_gpu_begin(const Nv2aGpuState *st) { (void)st; }
void nv2a_gpu_tri(const Nv2aGpuVert *a, const Nv2aGpuVert *b, const Nv2aGpuVert *c)
{ (void)a; (void)b; (void)c; }
void nv2a_gpu_clear(const Nv2aGpuState *st, int color, uint32_t argb, int depth,
                    uint32_t z) { (void)st; (void)color; (void)argb; (void)depth; (void)z; }
void nv2a_gpu_flip(uint32_t present_base) { (void)present_base; }
void nv2a_gpu_sync_range(uint32_t base, uint32_t bytes) { (void)base; (void)bytes; }
void nv2a_gpu_sync_all(void) {}
void nv2a_gpu_forget_range(uint32_t base, uint32_t bytes) { (void)base; (void)bytes; }
void nv2a_gpu_zpass_enable(uint32_t on) { (void)on; }
uint32_t nv2a_gpu_zpass_collect(void) { return 0; }
int  nv2a_gpu_vsh_enabled(void) { return 0; }
float *nv2a_gpu_vtx_begin(uint32_t nmax) { (void)nmax; return NULL; }
void nv2a_gpu_hitch_counters(double *t, uint32_t *u) { *t = 0; *u = 0; }
void nv2a_gpu_vtx_cancel(void) {}
void nv2a_gpu_draw_program(const Nv2aGpuState *st, const float *in, uint32_t nverts,
                           const uint32_t *idx, uint32_t nidx, const uint32_t *prog,
                           uint32_t start, const float *consts)
{ (void)st; (void)in; (void)nverts; (void)idx; (void)nidx; (void)prog; (void)start; (void)consts; }

#endif
