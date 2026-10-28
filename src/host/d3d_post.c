// The port's own full-screen passes, split out of d3d.c: background upscaling (denoise + super-xBR), the HUD's
// xBR, FXAA, resolving and composing the frame for picture = modern (the 2D menus' side fill), and presenting it.
#define COBJMACROS
#include "host.h"
#include "xd3d.h"
#include "game.h"
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <d3dcompiler.h>
#include "d3d_internal.h"

// ---- our own full-screen passes: background upscaling, FXAA --------------------------------------------

IDirect3DPixelShader9 *compile_ps(const char *name, const char *src)
{
    ID3DBlob *bin = 0, *err = 0;
    IDirect3DPixelShader9 *ps = 0;
    HRESULT hr = D3DCompile(src, strlen(src), name, 0, 0, "main", "ps_3_0", 0, 0, &bin, &err);
    if (FAILED(hr)) xlog("[d3d] %s shader: %s", name, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) return 0;
    if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)ID3D10Blob_GetBufferPointer(bin), &ps))) ps = 0;
    ID3D10Blob_Release(bin);
    return ps;
}

// Device state for our own pre-transformed quads: no depth, stencil, alpha test, scissor, culling, fog or blending, all
// colour channels written; samplers 0 .. stages-1 clamped, `filter`, no mips. Every Xbox state is re-applied from the
// Xbox state on the next game draw.
void quad_state(int stages, DWORD filter)
{
    IDirect3DDevice9_SetVertexShader(dev, 0);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    for (int st = 0; st < stages; st++) {
        IDirect3DDevice9_SetSamplerState(dev, st, D3DSAMP_MINFILTER, filter);
        IDirect3DDevice9_SetSamplerState(dev, st, D3DSAMP_MAGFILTER, filter);
        IDirect3DDevice9_SetSamplerState(dev, st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
}

// Draws src over all of dst with ps; src2 (optional) is bound as s1. The shader gets c40 = (1/w, 1/h, w, h) of src
// (c40+: clear of the game's pixel-shader constants). Device state is re-applied from the Xbox state on the next game draw.
static void post_pass(IDirect3DPixelShader9 *ps, IDirect3DTexture9 *src, IDirect3DTexture9 *src2, IDirect3DSurface9 *dst, DWORD filter)
{
    D3DSURFACE_DESC sd, dd;
    IDirect3DTexture9_GetLevelDesc(src, 0, &sd);
    IDirect3DSurface9_GetDesc(dst, &dd);
    IDirect3DSurface9 *rt = 0, *ds = 0;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
    IDirect3DDevice9_SetRenderTarget(dev, 0, dst);
    IDirect3DDevice9_SetDepthStencilSurface(dev, 0);
    float k[4] = {1.0f / sd.Width, 1.0f / sd.Height, (float)sd.Width, (float)sd.Height};
    float w = (float)dd.Width, h = (float)dd.Height;
    struct { float x, y, z, rhw, u, v; } q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {w - 0.5f, -0.5f, 0, 1, 1, 0},
                                                {-0.5f, h - 0.5f, 0, 1, 0, 1}, {w - 0.5f, h - 0.5f, 0, 1, 1, 1}};
    quad_state(2, filter);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 40, k, 1);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)src);
    IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)src2);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    if (rt) IDirect3DSurface9_Release(rt);
    if (ds) IDirect3DSurface9_Release(ds);
    apply_viewport();
    rs_valid = 0;
    passes_ran = 1;
}

// Edge-directed upscaling of screen-space images, once per upload, to the render scale:
// - background_filter = enhanced: pre-rendered backgrounds, title and menu art (Hyllian's fast-bilateral denoise,
//   then super-xBR);
// - hud_filter = xbr: paletted HUD, font and menu sheets (super-xBR only; still sampled nearest-texel, so edges
//   stay hard but curves and diagonals are redrawn smooth).
// super-xBR 2x is repeated until >= scale, then resized to exactly scale. Colour is premultiplied by alpha for the
// passes, so transparency (which cuts out the letter shapes) is upscaled the same way and transparent texels don't
// bleed into edges. Both shaders are ported from libretro/slang-shaders (MIT, Copyright (c) 2011-2016 Hyllian);
// super-xBR in its default "details" mode with edge strength 3, as in libretro's super-xbr preset. The game samples
// the big copy at 1:1; texture coordinates are 0..1, so it drops in unchanged.
#define XBR_COMMON \
    "sampler2D s0 : register(s0);\n" \
    "sampler2D s1 : register(s1);\n" \
    "float4 k : register(c40);\n"   /* 1/w, 1/h, w, h of s0 */ \
    "float4 prm : register(c41);\n" /* x: denoise strength (SIGMA_R), y: edge strength */ \
    "float4 T(sampler2D s, float2 uv) { return tex2Dlod(s, float4(uv, 0, 0)); }\n" \
    "float Y(float4 c) { return dot(c.rgb, float3(0.2126, 0.7152, 0.0722)) + c.a; }\n" /* alpha edges count too */ \
    "float df(float a, float b) { return abs(a - b); }\n" \
    "float d_wd(float wp1, float wp2, float wp3, float wp4, float wp5, float wp6, float b0, float b1, float c0, float c1, float c2,\n" \
    "           float d0, float d1, float d2, float d3, float e1, float e2, float e3, float f2, float f3) {\n" \
    "    return wp1 * (df(c1, c2) + df(c1, c0) + df(e2, e1) + df(e2, e3)) + wp2 * (df(d2, d3) + df(d0, d1)) + wp3 * (df(d1, d3) + df(d0, d2))\n" \
    "         + wp4 * df(d1, d2) + wp5 * (df(c0, c2) + df(e1, e3)) + wp6 * (df(b0, b1) + df(f2, f3));\n" \
    "}\n" \
    "float hv_wd(float wp1, float wp2, float wp3, float wp4, float wp5, float wp6, float i1, float i2, float i3, float i4, float e1, float e2, float e3, float e4) {\n" \
    "    return wp4 * (df(i1, i2) + df(i3, i4)) + wp1 * (df(i1, e1) + df(i2, e2) + df(i3, e3) + df(i4, e4)) + wp3 * (df(i1, e2) + df(i3, e4) + df(e1, i2) + df(e3, i4));\n" \
    "}\n" \
    "float4 comb(float4 w, float4 a, float4 b, float4 c, float4 d) { return w.x * a + w.y * b + w.z * c + w.w * d; }\n" \
    "float4 xbr(float4 P0, float4 P1, float4 P2, float4 P3, float4 B, float4 C, float4 D, float4 E, float4 F, float4 G, float4 H, float4 I,\n" \
    "           float4 F4, float4 I4, float4 H5, float4 I5, float wp1, float wp2, float wp3, float wp4, float wp5, float wp6, float weight) {\n" \
    "    float p0 = Y(P0), p1 = Y(P1), p2 = Y(P2), p3 = Y(P3), b = Y(B), c = Y(C), d = Y(D), e = Y(E), f = Y(F), g = Y(G), h = Y(H), i = Y(I);\n" \
    "    float f4 = Y(F4), i4 = Y(I4), h5 = Y(H5), i5 = Y(I5);\n" \
    "    float d_edge = d_wd(wp1, wp2, wp3, wp4, wp5, wp6, d, b, g, e, c, p2, h, f, p1, h5, i, f4, i5, i4)\n" \
    "                 - d_wd(wp1, wp2, wp3, wp4, wp5, wp6, c, f4, b, f, i4, p0, e, i, p3, d, h, i5, g, h5);\n" \
    "    float hv_edge = hv_wd(wp1, wp2, wp3, wp4, wp5, wp6, f, i, e, h, c, i5, b, h5) - hv_wd(wp1, wp2, wp3, wp4, wp5, wp6, e, f, h, i, d, f4, g, i4);\n" \
    "    float edge_strength = smoothstep(0.0, prm.y + 0.000001, abs(d_edge));\n" \
    "    float w1 = weight * 1.29633 / 10.0, w2 = weight * 1.75068 / 10.0 / 2.0;\n" \
    "    float4 W1 = float4(-w1, w1 + 0.5, w1 + 0.5, -w1), W2 = float4(-w2, w2 + 0.25, w2 + 0.25, -w2);\n" \
    "    float4 c1 = comb(W1, P2, H, F, P1), c2 = comb(W1, P0, E, I, P3);\n" \
    "    float4 c3 = comb(W2, D + G, E + H, F + I, F4 + I4), c4 = comb(W2, C + B, F + E, I + H, I5 + H5);\n" \
    "    float4 color = lerp(lerp(c1, c2, step(0.0, d_edge)), lerp(c3, c4, step(0.0, hv_edge)), 1.0 - edge_strength);\n" \
    "    return clamp(color, min(min(E, F), min(H, I)), max(max(E, F), max(H, I)));\n" \
    "}\n"

enum { XBR_DENOISE, XBR_PASS0, XBR_PASS1, XBR_PASS2, XBR_UNPREMULTIPLY, XBR_PREMULTIPLY, XBR_PASSES };
static const char *const xbr_src[XBR_PASSES] = {
    // XBR_DENOISE: fast-bilateral, source size (colour only; alpha kept)
    XBR_COMMON
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float4 c0 = T(s0, uv);\n"
    "    float3 center = c0.rgb, color = 0, wsum = 0;\n"
    "    float sr = prm.x * 0.04, si2 = 2.0 * sr * sr, sd2 = 2.0 * 3.0 * 3.0;\n"
    "    [unroll] for (int j = -2; j <= 2; j++) [unroll] for (int i = -2; i <= 2; i++) {\n"
    "        float3 c = T(s0, uv + float2(i, j) * k.xy).rgb;\n"
    "        float3 w = exp(-(i * i + j * j) / sd2) * exp(-(c - center) * (c - center) / si2);\n"
    "        color += w * c; wsum += w;\n"
    "    }\n"
    "    return float4(color / wsum, c0.a);\n"
    "}\n",
    // XBR_PASS0: super-xBR pass 0, source size: the value at each texel's bottom-right corner
    XBR_COMMON
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "#define S(a, b) T(s0, uv + float2((a) * k.x, (b) * k.y))\n"
    "    return xbr(S(-1, -1), S(2, -1), S(-1, 2), S(2, 2), S(0, -1), S(1, -1), S(-1, 0), S(0, 0), S(1, 0), S(-1, 1), S(0, 1), S(1, 1),\n"
    "               S(2, 0), S(2, 1), S(0, 2), S(1, 2), 2, 1, -1, 4, -1, 1, 0);\n"
    "}\n",
    // XBR_PASS1: super-xBR pass 1, 2x: s0 = pass 0, s1 = its input; fills in the remaining two pixels of each 2x2
    XBR_COMMON
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float2 fp = frac(uv * k.zw), dir = fp - 0.5;\n"
    "    if (dir.x * dir.y > 0) return dir.x >= 0 ? T(s0, uv) : T(s1, uv);\n"
    "    float2 g1 = fp.x > 0.5 ? float2(0.5 * k.x, 0) : float2(0, 0.5 * k.y);\n"
    "    float2 g2 = fp.x > 0.5 ? float2(0, 0.5 * k.y) : float2(0.5 * k.x, 0);\n"
    "    return xbr(T(s1, uv - 3 * g1), T(s0, uv - 3 * g2), T(s0, uv + 3 * g2), T(s1, uv + 3 * g1),\n"
    "               T(s0, uv - 2 * g1 - g2), T(s1, uv - g1 - 2 * g2), T(s0, uv - 2 * g1 + g2), T(s1, uv - g1),\n"
    "               T(s0, uv - g2), T(s1, uv - g1 + 2 * g2), T(s0, uv + g2), T(s1, uv + g1),\n"
    "               T(s1, uv + g1 - 2 * g2), T(s0, uv + 2 * g1 - g2), T(s1, uv + g1 + 2 * g2), T(s0, uv + 2 * g1 + g2),\n"
    "               1, 0, 0, 0, 0, 0, 1);\n"
    "}\n",
    // XBR_PASS2: super-xBR pass 2, 2x: re-centres the grid (each pixel from the corner up-left of it)
    XBR_COMMON
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "#define S(a, b) T(s0, uv + float2((a) * k.x, (b) * k.y))\n"
    "    return xbr(S(-2, -2), S(1, -2), S(-2, 1), S(1, 1), S(-1, -2), S(0, -2), S(-2, -1), S(-1, -1), S(0, -1), S(-2, 0), S(-1, 0), S(0, 0),\n"
    "               S(1, -1), S(1, 0), S(-1, 1), S(0, 1), 0, 0, 0, 1, 0, 0, 1);\n"
    "}\n",
    // XBR_UNPREMULTIPLY: resize to the final size, colour back to straight alpha. prm.z: images drawn with an alpha
    // test and no blending (menus, text), where alpha only says drawn or not: they were upscaled as coverage (see
    // XBR_PREMULTIPLY); drawn where the upscaled coverage is at least half, so edges don't fatten, with the image's
    // own alpha (s1, the original) for the game's alpha test.
    "sampler2D s0 : register(s0);\n"
    "sampler2D s1 : register(s1);\n"
    "float4 prm : register(c41);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "    float4 c = tex2Dlod(s0, float4(uv, 0, 0));\n"
    "    float a = prm.z > 0 ? (c.a < 0.5 ? 0 : max(tex2Dlod(s1, float4(uv, 0, 0)).a, 1.0 / 255)) : c.a;\n"
    "    return float4(c.a > 0 ? c.rgb / c.a : 0, a);\n"
    "}\n",
    // XBR_PREMULTIPLY. prm.z: alpha as coverage (any alpha = 1). Those images use alpha only for an alpha test, and
    // weighting colour by it lost the colours where it is low (save thumbnails carry noise-like alpha, 0 to 0x80).
    "sampler2D s0 : register(s0);\n"
    "float4 prm : register(c41);\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR { float4 c = tex2Dlod(s0, float4(uv, 0, 0)); float a = prm.z > 0 ? (c.a > 0 ? 1 : 0) : c.a;\n"
    "    return float4(c.rgb * a, a); }\n",
};

// Scratch render targets for the passes, kept for reuse (movies re-upload every frame). role: which pass.
#define XBR_POOL 96   // a role and size each: 7 per image size at scale 4-5 (32 ran out with the flames of room 204)
static struct { int role; UINT w, h; IDirect3DTexture9 *t; } xbr_pool[XBR_POOL];
void xbr_pool_drop(void)   // reset_device: the pool's render targets go with the device
{
    for (int i = 0; i < XBR_POOL; i++) {
        if (!xbr_pool[i].t) continue;
        ULONG left_ = IUnknown_Release((IUnknown *)xbr_pool[i].t);
        if (left_) xlog("[d3d] reset: %s still has %lu references", "xbr_pool[i].t", left_);
        xbr_pool[i].t = 0;
    }
}
static IDirect3DTexture9 *xbr_target(int role, UINT w, UINT h)
{
    for (int i = 0; i < XBR_POOL; i++) if (xbr_pool[i].t && xbr_pool[i].role == role && xbr_pool[i].w == w && xbr_pool[i].h == h) return xbr_pool[i].t;
    for (int i = 0; i < XBR_POOL; i++)
        if (!xbr_pool[i].t) {
            if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &xbr_pool[i].t, 0))) return 0;
            xbr_pool[i].role = role; xbr_pool[i].w = w; xbr_pool[i].h = h;
            return xbr_pool[i].t;
        }
    return 0;   // ponytail: pool full (many image sizes at large scales) -> caller keeps the plain texture
}

// One pass into a pooled target (or `into`); returns the target, 0 on failure.
static IDirect3DTexture9 *xbr_pass(int n, IDirect3DTexture9 *src, IDirect3DTexture9 *src2, IDirect3DTexture9 *into, UINT w, UINT h)
{
    static IDirect3DPixelShader9 *ps[XBR_PASSES];
    static int failed;
    if (!ps[n] && !failed) failed = !(ps[n] = compile_ps("upscale", xbr_src[n]));
    IDirect3DTexture9 *t = into ? into : xbr_target(n, w, h);
    if (failed || !t) return 0;
    IDirect3DSurface9 *dst = 0;
    IDirect3DTexture9_GetSurfaceLevel(t, 0, &dst);
    post_pass(ps[n], src, src2, dst, n == XBR_UNPREMULTIPLY ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    IDirect3DSurface9_Release(dst);
    return t;
}

// hud: 0 = background (denoise first); 1 = paletted HUD/font sheet; 2 = one drawn alpha-tested without blending
// (text). Falls back to the plain texture if anything fails.
#define UPSCALE_MAX 3200
IDirect3DTexture9 *upscaled(struct tex_entry *h, int hud)
{
    // Made (or failed) for this upload already. A failure isn't retried until the image changes: each try runs passes
    // in the middle of setting up the game's draw, and retrying on every draw (the pool full) left the flames and
    // glows of room 204 drawn with the passes' state, as black squares.
    if (h->big_upload == h->uploads) return h->big ? h->big : h->tex;
    D3DSURFACE_DESC d, b;
    IDirect3DTexture9_GetLevelDesc(h->tex, 0, &d);
    // Every target here at most UPSCALE_MAX px a side: drawing into 4096 px and larger ones (a 1024 px sheet at scale
    // 5) left the game's scene black from then on (a new game, opening a scroll), while the scene's own 3200 x 2400
    // is fine. A large sheet is upscaled less; it's sampled 0..1 all the same.
    UINT k = (UINT)scale;
    while (k > 1 && (d.Width * k > UPSCALE_MAX || d.Height * k > UPSCALE_MAX)) k--;
    if (k < 2) return h->tex;
    if (h->big && (IDirect3DTexture9_GetLevelDesc(h->big, 0, &b), b.Width != d.Width * k || b.Height != d.Height * k)) {
        IDirect3DTexture9_Release(h->big);   // upload_texture made tex again at another size
        h->big = 0;
    }
    if (!h->big && FAILED(IDirect3DDevice9_CreateTexture(dev, d.Width * k, d.Height * k, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                         D3DPOOL_DEFAULT, &h->big, 0))) {
        h->big = 0;
        return h->tex;
    }
    static int n;
    if (!h->big_upload && n++ < 40) xlog("[d3d] upscaling %ux%u %s x%u", d.Width, d.Height, hud ? "HUD/font sheet" : "screen image", k);
    float prm[4] = {settings.denoise, 3.0f, hud == 2 ? 1.0f : 0.0f, 0};
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 41, prm, 1);
    IDirect3DTexture9 *cur = xbr_pass(XBR_PREMULTIPLY, h->tex, 0, 0, d.Width, d.Height);
    if (cur && !hud && settings.denoise > 0) cur = xbr_pass(XBR_DENOISE, cur, 0, 0, d.Width, d.Height);
    // Doubling up to 4x at most: at scale 5 a third doubling (8x) came out black where the art is transparent (the
    // save screen). The last step to the size is the linear resize below.
    for (UINT f = 1; cur && f < k && f < 4 && d.Width * f * 2 <= UPSCALE_MAX && d.Height * f * 2 <= UPSCALE_MAX; f *= 2) {
        IDirect3DTexture9 *p0 = xbr_pass(XBR_PASS0, cur, 0, 0, d.Width * f, d.Height * f);
        IDirect3DTexture9 *p1 = p0 ? xbr_pass(XBR_PASS1, p0, cur, 0, d.Width * f * 2, d.Height * f * 2) : 0;
        cur = p1 ? xbr_pass(XBR_PASS2, p1, 0, 0, d.Width * f * 2, d.Height * f * 2) : 0;
    }
    h->big_upload = h->uploads;
    if (!cur || !xbr_pass(XBR_UNPREMULTIPLY, cur, h->tex, h->big, 0, 0)) { IDirect3DTexture9_Release(h->big); h->big = 0; return h->tex; }
    return h->big;
}

// settings: enhanced_text. If this paletted texture is the game's font sheet, its copy redrawn by text.c at the render
// scale, coloured with this palette (index for index, as the game's own would be); else 0. Decided once per upload.
// Same 0..1 texture coordinates, so the game's glyph rectangles land on the redrawn letters.
// The game draws its text about 1.56 times the font sheet's size (an 11.5-texel glyph on 18 Xbox pixels), so the redrawn
// sheet is made at that much over the render scale: one of its pixels to about one on screen, sampled smoothly.
int text_k(void)
{
    int k = (int)(scale * 1.56f + 0.5f);
    return k < 2 ? 2 : k > 12 ? 12 : k;
}

IDirect3DTexture9 *font_sheet_texture(struct tex_entry *h, const XPix *x, const uint32_t *pal)
{
    if (h->text_upload == h->uploads && (!h->text || h->big)) return h->text ? h->big : 0;
    int was = h->text;
    h->text_upload = h->uploads;
    h->text = 0;
    struct xtex_info t = tex_info(x->Format, x->Size);
    const uint8_t *big = 0;
    if (pal && t.ok && t.kind == F_P8 && !t.linear && t.w == 256 && t.h == 256 && readable_bytes((void *)x->Data, 65536) == 65536) {
        static uint8_t idx[65536];
        uint32_t mx, my;
        swizzle_masks(256, 256, &mx, &my);
        const uint8_t *src = (const uint8_t *)x->Data;
        for (uint32_t y = 0; y < 256; y++)
            for (uint32_t xx = 0, oy = pdep(y, my); xx < 256; xx++) idx[y * 256 + xx] = src[pdep(xx, mx) | oy];
        big = text_sheet(idx, text_k());
    }
    if (!big) {
        if (was && h->big) { IDirect3DTexture9_Release(h->big); h->big = 0; }   // ours, not upscaled()'s render target
        return 0;
    }
    UINT W = 256 * text_k();
    if (h->big) { IDirect3DTexture9_Release(h->big); h->big = 0; }
    if (FAILED(tex_create(W, W, 1, D3DFMT_A8R8G8B8, &h->big))) { h->big = 0; return 0; }
    D3DLOCKED_RECT lr;
    if (FAILED(tex_lock(h->big, 0, &lr, 0))) { IDirect3DTexture9_Release(h->big); h->big = 0; return 0; }
    const uint8_t *cov = text_coverage();   // the palette's colour, the edge's coverage as alpha (drawn blended)
    for (UINT y = 0; y < W; y++) {
        uint32_t *d = (uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch);
        for (UINT xx = 0; xx < W; xx++) {
            uint32_t c = pal[big[y * W + xx]];
            d[xx] = cov ? (c & 0xFFFFFFu) | (uint32_t)(c >> 24 ? cov[y * W + xx] : 0) << 24 : c;
        }
    }
    tex_unlock(h->big, 0);
    h->text = cov ? 2 : 1;
    h->big_upload = h->uploads;
    return h->big;
}

// Debug: ONI_DUMP_BG=1 saves each newly uploaded large screen image (pre-rendered backgrounds, title art) as
// bg_<frame>_<w>x<h>.bmp, for trying upscalers offline. The texture's full size; the image sits in its top-left.
void dump_image(struct tex_entry *h)
{
    static int on = -1;
    static struct tex_entry *last;
    static uint32_t last_frame;
    if (on < 0) on = debug_env_int("ONI_DUMP_BG", 0) == 1;
    struct xtex_info t = tex_info(h->format, h->size);   // DXT only: backgrounds are DXT1; movie frames (re-uploaded every frame) are not
    if (!on || !is_dxt(t.kind) || t.w < 512 || (h == last && frame_no == last_frame)) return;   // once per upload
    last = h; last_frame = frame_no;
    D3DLOCKED_RECT lr;
    uint32_t *px = malloc(t.w * t.h * 4);
    if (px && SUCCEEDED(tex_lock(h->tex, 0, &lr, D3DLOCK_READONLY))) {
        dxt_decode(t.kind, lr.pBits, lr.Pitch, t.w, t.h, px);
        tex_unlock(h->tex, 0);
        char file[64], name[MAX_PATH * 3 + 64];
        snprintf(file, sizeof file, "bg_%u_%ux%u.bmp", frame_no, t.w, t.h);
        dump_path(name, sizeof name, file);
        D3DLOCKED_RECT img = {(INT)(t.w * 4), px};
        write_bmp(name, &img, t.w, t.h);
    }
    free(px);
}

// Debug: ONI_DUMP_XBR=1 saves each HUD/menu sheet's hud_filter = xbr upscale once, as xbr_<pack name>_<w>x<h>.rgba
// (raw 8-bit RGBA, straight alpha) next to the exe: the base for HD pack sheets whose words are retyped
// (tools/hdpack/menu_retype.py), so everything else on the sheet looks as it does in game.
void dump_xbr(struct tex_entry *h, const XPix *x, const uint32_t *pal, IDirect3DTexture9 *t)
{
    static int on = -1;
    static uint64_t done[512];
    static int ndone;
    if (on < 0) on = debug_env_int("ONI_DUMP_XBR", 0) == 1;
    if (!on || ndone == 512) return;
    struct xtex_info ti = tex_info(x->Format, x->Size);
    char name[64], wild[64];
    uint64_t key = tex_name(x, pal, &ti, name, wild, sizeof name);
    for (int i = 0; i < ndone; i++) if (done[i] == key) return;
    done[ndone++] = key;
    D3DSURFACE_DESC d;
    IDirect3DTexture9_GetLevelDesc(t, 0, &d);
    IDirect3DSurface9 *rt = 0, *sys = 0;
    IDirect3DTexture9_GetSurfaceLevel(t, 0, &rt);
    D3DLOCKED_RECT lr;
    if (rt && SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, 0))
        && SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys)) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, 0, D3DLOCK_READONLY))) {
        char file[128], path[MAX_PATH * 3 + 128];
        snprintf(file, sizeof file, "xbr_%s_%ux%u.rgba", name, d.Width, d.Height);
        dump_path(path, sizeof path, file);
        FILE *f = ufopen(path, L"wb");
        for (UINT y = 0; f && y < d.Height; y++)
            for (UINT xx = 0; xx < d.Width; xx++) {
                uint32_t c = ((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[xx];
                uint8_t px[4] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 24)};
                fwrite(px, 4, 1, f);
            }
        if (f) fclose(f);
        IDirect3DSurface9_UnlockRect(sys);
        xlog("[d3d] saved %s", file);
    }
    if (sys) IDirect3DSurface9_Release(sys);
    if (rt) IDirect3DSurface9_Release(rt);
    (void)h;
}

// settings: fxaa. FXAA (Timothy Lottes' simplified version) on the resolved frame, src -> dst.
static int fxaa_pass(IDirect3DTexture9 *src, IDirect3DSurface9 *dst)
{
    static IDirect3DPixelShader9 *ps;
    static int failed;
    if (!ps && !failed) failed = !(ps = compile_ps("fxaa",
        "sampler2D s : register(s0);\n"
        "float4 k : register(c40);\n"
        "float3 at(float2 uv) { return tex2Dlod(s, float4(uv, 0, 0)).rgb; }\n"
        "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
        "    const float3 L = float3(0.299, 0.587, 0.114);\n"
        "    float nw = dot(at(uv - k.xy), L), ne = dot(at(uv + float2(k.x, -k.y)), L);\n"
        "    float sw = dot(at(uv + float2(-k.x, k.y)), L), se = dot(at(uv + k.xy), L);\n"
        "    float m = dot(at(uv), L);\n"
        "    float lo = min(m, min(min(nw, ne), min(sw, se))), hi = max(m, max(max(nw, ne), max(sw, se)));\n"
        "    float2 dir = float2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));\n"
        "    float reduce = max((nw + ne + sw + se) * (0.25 / 8), 1.0 / 128);\n"
        "    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8, 8) * k.xy;\n"
        "    float3 a = 0.5 * (at(uv + dir * (1.0 / 3 - 0.5)) + at(uv + dir * (2.0 / 3 - 0.5)));\n"
        "    float3 b = a * 0.5 + 0.25 * (at(uv - dir * 0.5) + at(uv + dir * 0.5));\n"
        "    float lb = dot(b, L);\n"
        "    return float4(lb < lo || lb > hi ? a : b, 1);\n"
        "}\n"));
    if (failed) return 0;
    post_pass(ps, src, 0, dst, D3DTEXF_LINEAR);
    return 1;
}

// The finished frame at render resolution: MSAA resolved, FXAA applied if enabled. Caller releases.
IDirect3DSurface9 *resolve_frame(void)
{
    IDirect3DSurface9 *frame = 0, *post = 0;
    IDirect3DTexture9_GetSurfaceLevel(frame_tex, 0, &frame);
    IDirect3DDevice9_StretchRect(dev, scene_rt, 0, frame, 0, D3DTEXF_NONE);   // resolves MSAA
    if (!settings.fxaa) return frame;
    D3DSURFACE_DESC d;
    IDirect3DSurface9_GetDesc(frame, &d);
    if (!post_tex && FAILED(IDirect3DDevice9_CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                                           D3DPOOL_DEFAULT, &post_tex, 0))) post_tex = 0;
    if (!post_tex) return frame;
    IDirect3DTexture9_GetSurfaceLevel(post_tex, 0, &post);
    if (!fxaa_pass(frame_tex, post)) { IDirect3DSurface9_Release(post); return frame; }
    IDirect3DSurface9_Release(frame);
    return post;
}

// picture = modern, the full-screen 2D screens in a window wider than 4:3 (the in-game menu: equip, item, file, map;
// the Magic Mirror's save and enhance screens; the load screen from the title): the picture (y 16-464) to the
// window's height, and the sides filled by carrying the art on outwards. A mirror of the edge for the first few
// pixels eases into new blotches made of the edge's own colours (the edge band read at places moved by smooth noise)
// with grain on top, darkening towards the window edge. Everything else that isn't wide (title, letters, movies)
// keeps the 4:3 picture. The game's mode word (low byte): 2 play and pause, 0 title, 6 the opening letter (0x106),
// 8 the menu on every tab (0x308), 7 the mirror's save screen, 0x0b its enhance screen, 0x0e the altar where souls go
// into an orb (room 602). On the title, the two bytes
// after the word: 0 0 on the title itself; 0 1 then 0 2 while the load screen fades in, 1 0 once it's up, 2 1 while
// it fades out (both fades show the load screen, so both get the fill). The word is G_STATE (game.h).
int menu_fill(UINT cw, UINT ch)
{
    uint8_t m = *(volatile uint8_t *)G_STATE, sub = *(volatile uint8_t *)(G_STATE + 2), fade = *(volatile uint8_t *)(G_STATE + 3);
    return settings.picture == PICTURE_MODERN && cw * 3 > ch * 4 && (m == 8 || m == 7 || m == 0x0b || m == 0x0e || (m == 0 && (sub || fade)) || screen2d_on || fullpic_on);
}
// Whether this frame's overlays (wheel, subtitles, achievement popup) go on the window-sized picture (wide, or a 4:3
// screen with the sides filled) rather than on the 4:3 frame: drawn on the frame, the fill carried a popup sliding in
// at its right edge outwards and scaled it to 448 lines, and it jumped when the next frame was wide again (the weapon
// scenes: the game is in its menu state for the few frames the achievement unlocks).
int overlays_late(int wide)
{
    RECT cr;
    GetClientRect(hwnd, &cr);
    return wide || (cr.bottom > 0 && menu_fill(cr.right, cr.bottom));
}
void compose_fill(IDirect3DSurface9 *frame, IDirect3DSurface9 *dst, UINT w, UINT h)
{
    static IDirect3DPixelShader9 *ps;
    static int failed;
    if (!ps && !failed) failed = !(ps = compile_ps("menu_fill",
        "sampler2D s : register(s0);\n"
        "float4 c0 : register(c0);\n"   // x: left edge of the picture (window px); y: Xbox px per window px; z: Xbox px to the window edge
        "float4 c1 : register(c1);\n"   // xy: rows not to carry outwards (art y; none when x >= y)
        "float fold(float y) { return y > c1.x && y < c1.y ? (y < (c1.x + c1.y) / 2 ? 2 * c1.x - y : 2 * c1.y - y) : y; }\n"
        "float3 pic(float x, float y) { return tex2Dlod(s, float4((21.3333 + x) / 640, (16 + clamp(y, 2, 445.5)) / 480, 0, 0)).rgb; }\n"
        "float3 at(float x, float y) { return pic(x, fold(y)); }\n"   // the sides: rows in the band come from outside it
        "float hash(float2 i) { return frac(sin(dot(i, float2(127.1, 311.7))) * 43758.5453); }\n"
        "float noise(float2 p) { float2 i = floor(p), f = p - i; f = f * f * (3 - 2 * f);\n"
        "  return lerp(lerp(hash(i), hash(i + float2(1, 0)), f.x), lerp(hash(i + float2(0, 1)), hash(i + 1), f.x), f.y); }\n"
        "float fbm(float2 p, int o) { float v = 0, a = .5, t = 0; for (int i = 0; i < o; i++) { v += a * noise(p); t += a; p = p * 2.03 + float2(17.1, 9.7); a *= .5; } return v / t; }\n"
        "float4 main(float2 p : TEXCOORD0) : COLOR {\n"
        "  float ax = (p.x - c0.x) * c0.y, ay = p.y * c0.y;\n"
        "  if (ax >= 1.5 && ax <= 595.833) return float4(pic(ax, ay), 1);\n"
        // the last 1.5 px of the picture are blended with the black beside it; the 8 px inside that are plain edge
        // (the scroll rods end 12 px in)
        "  bool l = ax < 298; float side = l ? 1.5 - ax : ax - 595.833, e = l ? 2 : 595.333, sg = l ? 1 : -1, t = fmod(side, 16);\n"
        "  float2 q = float2((l ? -ax : ax) + (l ? 0 : 53 * 40), ay);\n"
        "  float wy = (fbm(q / 40, 4) - .5) * 120, wx = fbm(q / 25 + float2(5, 3), 4) * 8, g = (fbm(q / 3, 3) - .5) * 2;\n"
        "  float3 c = at(e + sg * wx, ay + wy) * (1 + g * .18), n = at(e + sg * (t < 8 ? t : 16 - t), ay);\n"
        "  return float4(lerp(n, c, smoothstep(0, 24, side)) * (1 - .45 * saturate(side / c0.z)), 1);\n"
        "}\n"));
    D3DSURFACE_DESC fd;
    IDirect3DSurface9_GetDesc(frame, &fd);
    IDirect3DTexture9 *t = ps ? xbr_target(90, fd.Width, fd.Height) : 0;   // the frame as a texture to sample
    if (!t) { IDirect3DDevice9_StretchRect(dev, frame, 0, dst, 0, D3DTEXF_LINEAR); return; }
    IDirect3DSurface9 *ts = 0, *rt = 0, *ds = 0;
    IDirect3DTexture9_GetSurfaceLevel(t, 0, &ts);
    IDirect3DDevice9_StretchRect(dev, frame, 0, ts, 0, D3DTEXF_NONE);
    IDirect3DSurface9_Release(ts);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
    IDirect3DDevice9_SetRenderTarget(dev, 0, dst);
    IDirect3DDevice9_SetDepthStencilSurface(dev, 0);
    float fw = (float)w, fh = (float)h, k = 448.0f / h, left = (w - (ART_X1 - ART_X0) / k) / 2, c[4] = {left, k, left * k, 0};
    struct { float x, y, z, rhw, u, v; } q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {fw - 0.5f, -0.5f, 0, 1, fw, 0},
                                                {-0.5f, fh - 0.5f, 0, 1, 0, fh}, {fw - 0.5f, fh - 0.5f, 0, 1, fw, fh}};
    for (int i = 0; i < 4; i++) q[i].u += 0.5f, q[i].v += 0.5f;   // window pixel centres
    quad_state(1, settings.filter == FILTER_SHARP ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c, 1);
    float band[4] = {screen2d_on ? screen2d_band[0] : 0, screen2d_on ? screen2d_band[1] : 0, 0, 0};
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, band, 1);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
    IDirect3DDevice9_SetTexture(dev, 0, 0);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    if (rt) IDirect3DSurface9_Release(rt);
    if (ds) IDirect3DSurface9_Release(ds);
    draw_wheel(dst), draw_subtitle(dst), draw_popup(dst);   // over the whole picture, as compose_wide does
    apply_viewport();
    rs_valid = 0;
}

// picture = modern: the slice of the finished 4:3 frame the shape of dst (w x h), then the HUD layer and the weapon
// wheel over it.
void compose_wide(IDirect3DSurface9 *frame, IDirect3DSurface9 *dst, UINT w, UINT h)
{
    static IDirect3DPixelShader9 *ps;
    static int failed;
    float sh = (ART_X1 - ART_X0) * h / w, y0 = pan_y < 0 ? (ART_Y0 + ART_Y1 - sh) / 2 : pan_y;
    if (y0 > ART_Y1 - sh) y0 = ART_Y1 - sh;
    if (y0 < ART_Y0) y0 = ART_Y0;
    RECT src = {(LONG)(ART_X0 * scale + 0.5f), (LONG)(y0 * scale + 0.5f), (LONG)(ART_X1 * scale + 0.5f), (LONG)((y0 + sh) * scale + 0.5f)};
    IDirect3DDevice9_StretchRect(dev, frame, &src, dst, 0, D3DTEXF_LINEAR);
    if (!ps && !failed) failed = !(ps = compile_ps("hud_layer", "sampler2D s : register(s0);\nfloat4 main(float2 uv : TEXCOORD0) : COLOR { return tex2D(s, uv); }\n"));
    IDirect3DSurface9 *rt = 0, *ds = 0;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &rt);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
    IDirect3DDevice9_SetRenderTarget(dev, 0, dst);
    IDirect3DDevice9_SetDepthStencilSurface(dev, 0);
    if (pres_hud && hud_tex && ps) {
        float fw = (float)w, fh = (float)h;
        struct { float x, y, z, rhw, u, v; } q[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {fw - 0.5f, -0.5f, 0, 1, 1, 0},
                                                    {-0.5f, fh - 0.5f, 0, 1, 0, 1}, {fw - 0.5f, fh - 0.5f, 0, 1, 1, 1}};
        quad_state(1, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetPixelShader(dev, ps);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)hud_tex);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
        IDirect3DDevice9_SetTexture(dev, 0, 0);
    }
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ds);
    if (rt) IDirect3DSurface9_Release(rt);
    if (ds) IDirect3DSurface9_Release(ds);
    draw_wheel(dst);   // the wheel at the window's own scale, over everything
    draw_subtitle(dst), draw_popup(dst);
    apply_viewport();
    rs_valid = 0;
}

// Draw a finished frame into the window, shaped as settings.picture says.
void present_surface(IDirect3DSurface9 *frame)
{
    RECT cr;
    GetClientRect(hwnd, &cr);
    UINT cw = cr.right > 0 ? cr.right : 1, ch = cr.bottom > 0 ? cr.bottom : 1;
    // With frame interpolation (or the wheel's slow-motion in-betweens) the port times every frame itself
    // (wait_vblank); vsync on top made each Present wait ~30 ms when presenting twice per game step.
    static UINT swap_iv;
    UINT iv = fast_mode || !settings.vsync || recording ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
    // New window size, or the interval changed (the wheel opened or closed): a new swap chain, no device reset needed.
    // The interval used to be fixed by the moment of the last resize, so F11 with the wheel open left vsync off.
    if (!swapchain || cw != swap_w || ch != swap_h || iv != swap_iv) {
        if (swapchain) IDirect3DSwapChain9_Release(swapchain);
        swapchain = 0;
        D3DPRESENT_PARAMETERS sp = {0};
        sp.BackBufferWidth = cw; sp.BackBufferHeight = ch; sp.BackBufferFormat = D3DFMT_X8R8G8B8; sp.BackBufferCount = 1;
        sp.SwapEffect = D3DSWAPEFFECT_DISCARD; sp.hDeviceWindow = hwnd; sp.Windowed = TRUE;
        sp.PresentationInterval = iv;
#ifdef _WIN32
        // D3D9Ex: flip model. The frame is handed to the compositor, or straight to the display when the window covers
        // the screen, instead of being copied to the desktop; that is what lets G-SYNC and FreeSync drive the refresh.
        // Two buffers, as Microsoft advises for it. The old copy if the driver refuses.
        static int flip_failed;
        if (dev_ex && !flip_failed) {
            D3DPRESENT_PARAMETERS fp = sp;
            fp.SwapEffect = D3DSWAPEFFECT_FLIPEX; fp.BackBufferCount = 2;
            HRESULT hr = IDirect3DDevice9_CreateAdditionalSwapChain(dev, &fp, &swapchain);
            if (FAILED(hr)) { flip_failed = 1; swapchain = 0; xlog("[d3d] flip-model swap chain failed (%08lx): copied presents", hr); }
        }
#endif
        if (!swapchain && FAILED(IDirect3DDevice9_CreateAdditionalSwapChain(dev, &sp, &swapchain))) { xlog("[d3d] swap chain %ux%u failed", cw, ch); return; }
        swap_w = cw; swap_h = ch; swap_iv = iv;
    }
    IDirect3DSurface9 *back = 0;
    if (FAILED(IDirect3DSwapChain9_GetBackBuffer(swapchain, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return;
    if (pres_wide && cw * 3 > ch * 4 || !pres_wide && menu_fill(cw, ch)) {
        if (pres_wide) compose_wide(frame, back, cw, ch);
        else compose_fill(frame, back, cw, ch);
        draw_fps(back);
        IDirect3DSurface9_Release(back);
        IDirect3DDevice9_EndScene(dev);
        IDirect3DSwapChain9_Present(swapchain, 0, 0, 0, 0, 0);
        IDirect3DDevice9_BeginScene(dev);
        return;
    }
    IDirect3DDevice9_ColorFill(dev, back, 0, 0);
    // settings: picture. original = largest 4:3 rectangle that fits; stretch = the whole window; zoom = the whole
    // window from the middle of the frame, cutting top and bottom (or sides, in a tall window).
    UINT w = cw, h = cw * bb_h / bb_w;
    if (h > ch) { h = ch; w = ch * bb_w / bb_h; }
    D3DSURFACE_DESC fd;
    IDirect3DSurface9_GetDesc(frame, &fd);   // the frame at render resolution (640x480 times scale)
    RECT dst = {(LONG)(cw - w) / 2, (LONG)(ch - h) / 2, (LONG)((cw - w) / 2 + w), (LONG)((ch - h) / 2 + h)}, src = {0, 0, (LONG)fd.Width, (LONG)fd.Height};
    // (modern: frames that aren't wide, and windows narrower than 4:3, show the 4:3 picture as original does)
    if (settings.picture != PICTURE_ORIGINAL && settings.picture != PICTURE_MODERN) dst = (RECT){0, 0, (LONG)cw, (LONG)ch};
    if (settings.picture == PICTURE_ZOOM) {
        UINT sw = fd.Width, sh = fd.Width * ch / cw;   // the part of the frame with the window's shape
        if (sh > fd.Height) { sh = fd.Height; sw = fd.Height * cw / ch; }
        src = (RECT){(LONG)(fd.Width - sw) / 2, (LONG)(fd.Height - sh) / 2, (LONG)((fd.Width - sw) / 2 + sw), (LONG)((fd.Height - sh) / 2 + sh)};
    }
    IDirect3DDevice9_StretchRect(dev, frame, &src, back, &dst, settings.filter == FILTER_SHARP ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    draw_fps(back);
    IDirect3DSurface9_Release(back);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DSwapChain9_Present(swapchain, 0, 0, 0, 0, 0);
    IDirect3DDevice9_BeginScene(dev);
}

// The device is lost while another program owns the display (lock screen, UAC prompt, display mode change, GPU
// reset): every call does nothing and the window stays black until the device is reset. Checked once per game
// frame at the end of Present, where nothing of ours holds a surface (a held one makes Reset fail).
void check_device(void)
{
    static HRESULT last_state;
    static uint32_t test_reset = ~0u;   // debug: ONI_TEST_RESET=frame resets the device then, as after a loss
    if (test_reset == ~0u) test_reset = (uint32_t)debug_env_int("ONI_TEST_RESET", 0);
    HRESULT state = IDirect3DDevice9_TestCooperativeLevel(dev);
    if (state != last_state) xlog("[d3d] frame %u: device %s (%08lx)", frame_no, state == D3D_OK ? "ok" : state == D3DERR_DEVICELOST ? "lost"
                                  : state == D3DERR_DEVICENOTRESET ? "can be reset" : "failed", state);
    last_state = state;
    if (state != D3DERR_DEVICENOTRESET && frame_no != test_reset) return;
    IDirect3DDevice9_EndScene(dev);
    reset_device();
    IDirect3DDevice9_BeginScene(dev);
}

// Resolve the scene and show it.
void present_frame(void)
{
    IDirect3DSurface9 *f = resolve_frame();
    present_surface(f);
    IDirect3DSurface9_Release(f);
}

