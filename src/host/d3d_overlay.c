// Overlays drawn over the game at Present (d3d.c): the weapon wheel, achievement popups, the FPS counter, movie
// subtitles, and what they draw with (GDI text textures, the game's item pictures and HUD plate art).
#include "host.h"
#include "xd3d.h"
#include "game.h"
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <d3dcompiler.h>
#include "d3d_internal.h"

// ---- mod overlay: the weapon wheel (input.c fills `wheel`) -----------------------------------------------------
// A ring of segments, one per weapon, with the weapon's picture in each; the selected segment lights up and pops out,
// a highlight arc slides to it, and the middle shows its name. Opens with a zoom and fade, fades out on release.
// Everything is in Xbox pixels around the screen centre, scaled to the render target.
// Pictures are the game's own item pictures, read from the disc's ITEMI.IDA (game\ next to the exe).

struct wheel wheel;

// White text on transparent, one string per th-pixel row, drawn with GDI.
// face: 0 Segoe UI; 1 Kabuto Sans (tools/font, built into the DLL: text.c kabuto_font), the stand-in for the game's own letters.
static IDirect3DTexture9 *text_texture_face(const char *strs, int stride, int n, int tw, int th, int px, int face)
{
    int kabuto = face && kabuto_font();
    HDC dc = CreateCompatibleDC(0);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), tw, -th * n, 1, 32}};
    void *bits = 0;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    HGDIOBJ old_bm = SelectObject(dc, bm);
    HFONT font = face && kabuto ? CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, 0, ANTIALIASED_QUALITY, 0, L"Kabuto Sans")
                                : CreateFontA(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int i = 0; i < n; i++) {
        RECT r = {0, i * th, tw, (i + 1) * th};
        DrawTextA(dc, strs + i * stride, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    GdiFlush();
    IDirect3DTexture9 *t = 0;
    D3DLOCKED_RECT lr;
    if (bits && SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, tw, th * n, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, 0))
        && SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &lr, 0, 0))) {
        for (int y = 0; y < th * n; y++) {
            const uint32_t *s = (const uint32_t *)bits + y * tw;
            uint32_t *d = (uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch);
            for (int x = 0; x < tw; x++) d[x] = (s[x] & 0xFF) << 24 | 0xFFFFFF;   // grey level -> alpha
        }
        IDirect3DTexture9_UnlockRect(t, 0);
    }
    SelectObject(dc, old_font); DeleteObject(font);
    SelectObject(dc, old_bm); DeleteObject(bm);
    DeleteDC(dc);
    return t;
}
static IDirect3DTexture9 *text_texture(const char *strs, int stride, int n, int tw, int th, int px) { return text_texture_face(strs, stride, n, tw, th, px, 0); }

typedef struct { float x, y, z, rhw, u, v; } overt;
static IDirect3DPixelShader9 *ov_solid, *ov_text, *ov_art_ps;
static float ov_k, ov_cx, ov_cy, ov_zoom, ov_fade;   // render scale, centre (Xbox px), open zoom, open fade

static void ov_color(IDirect3DPixelShader9 *ps, uint32_t argb, float alpha)
{
    float c[4] = {(argb >> 16 & 255) / 255.0f, (argb >> 8 & 255) / 255.0f, (argb & 255) / 255.0f, (argb >> 24) / 255.0f * alpha * ov_fade};
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 40, c, 1);
}
static overt ov_pt(float x, float y, float u, float v)   // (x, y) relative to the centre, before the open zoom
{
    return (overt){(ov_cx + x * ov_zoom) * ov_k - 0.5f, (ov_cy + y * ov_zoom) * ov_k - 0.5f, 0, 1, u, v};
}
static void ov_draw(D3DPRIMITIVETYPE t, const overt *v, int prims, uint32_t argb, float alpha)
{
    ov_color(ov_solid, argb, alpha);
    IDirect3DDevice9_DrawPrimitiveUP(dev, t, prims, v, sizeof *v);
}
// Ring piece from angle a0 to a1 (0 = up, clockwise), radii r0..r1.
// The game's item pictures: IMAGE\ETC\ITEMI.IDA on the disc holds 256 offsets (from 0x400) to CMP-compressed OIM
// images, the art the menus show. CMP (the game's FUN_0001b690 / FUN_0001b720): "CMP ", output size, size including
// this 20-byte header, a shift; then LZSS: flag bytes, high bit first; 0 = a literal byte, 1 = a 16-bit word copying
// (w & ((1 << shift) - 1)) bytes from (w >> shift) back. OIM: 16-byte header (+0x0b: image count), then 16-byte entries
// {.., u32 offset, u32 size}: the first is 8-bit pixels 256 wide, the one after the images a 256-colour RGBA palette.
static uint8_t *cmp_decode(const uint8_t *f, uint32_t fsize, uint32_t at, uint32_t *out_size)
{
    if (at > fsize || fsize - at < 0x14 || memcmp(f + at, "CMP ", 4)) return 0;
    uint32_t n = *(const uint32_t *)(f + at + 4), end = at + *(const uint32_t *)(f + at + 8), shift = f[at + 12] & 15, o = 0;
    uint8_t *out = n && n < (1u << 24) ? malloc(n) : 0;
    if (!out || end > fsize) { free(out); return 0; }
    for (uint32_t p = at + 0x14; p < end && o < n;) {
        uint8_t flags = f[p++];
        for (int bit = 7; bit >= 0 && p < end && o < n; bit--) {
            if (!(flags >> bit & 1)) { out[o++] = f[p++]; continue; }
            if (end - p < 2) break;
            uint32_t w = f[p] | f[p + 1] << 8, len = w & ((1u << shift) - 1), back = w >> shift;
            p += 2;
            if (back > o || back == 0) { free(out); return 0; }
            for (; len && o < n; len--, o++) out[o] = out[o - back];
        }
    }
    *out_size = o;
    return out;
}

// A texture pack's version of an item picture (px, 8-bit, w x h; pl, its RGBA palette), under the name the game's own
// upload of it gets (tex_name): P8 with mip levels down to 1x1, each level the centre texel of every 2^l block,
// swizzled; the palette in the PS2's CLUT order (in every 32 entries the 2nd and 3rd 8 swapped), as ARGB. Exact name
// first, then the _$ one. Opaque BGRA, or 0.
static uint32_t *item_pack_picture(const uint8_t *px, const uint8_t *pl, UINT w, UINT h, UINT *out_w, UINT *out_h)
{
    if (!texpack_replacing() || (w & (w - 1)) || (h & (h - 1))) return 0;   // swizzled: powers of two only (the indexing below needs it)
    uint32_t levels = ilog2(w > h ? w : h) + 1, total = 0, at = 0;
    for (uint32_t l = 0, lw = w, lh = h; l < levels; l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) total += lw * lh;
    uint8_t *lv = calloc(total, 1);
    if (!lv) return 0;
    for (uint32_t l = 0, lw = w, lh = h; l < levels; at += lw * lh, l++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1) {
        uint32_t mx, my, s = 1u << l, o = s / 2;
        swizzle_masks(lw, lh, &mx, &my);
        for (uint32_t y = 0; y < lh; y++)
            for (uint32_t x = 0; x < lw; x++)
                if (o + y * s < h && o + x * s < w) lv[at + (pdep(x, mx) | pdep(y, my))] = px[(o + y * s) * w + o + x * s];
    }
    uint64_t th = pack_texel_hash(lv, total);
    free(lv);
    uint32_t argb[256];
    for (uint32_t i = 0; i < 256; i++) {
        const uint8_t *c = pl + (i ^ ((i >> 3 ^ i >> 4) & 1) * 0x18) * 4;   // RGBA
        argb[i] = (uint32_t)c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2];
    }
    char name[64], wild[64];
    const char *got;
    pack_name(name, wild, sizeof name, w, h, 0x0b, th, 1, pack_pal_hash(argb));
    uint32_t W = 0, H = 0, *hd = texpack_load_either(name, wild, &W, &H, &got);
    if (!hd) return 0;
    for (uint32_t k = 0; k < W * H; k++) hd[k] |= 0xFF000000u;
    *out_w = W; *out_h = H;
    xlog("[wheel] item picture %s: HD version from the pack (%ux%u)", name, W, H);
    return hd;
}

// Item picture `item` as 256-wide BGRA, or 0: a texture pack's HD version when it has one. ITEMI.IDA is read once.
static uint32_t *item_picture(int item, UINT *w, UINT *h)
{
    static uint8_t *f;
    static DWORD fsize;
    static int tried;
    if (!tried++) {
        wchar_t path[MAX_PATH];
        game_file(path, L"IMAGE\\ETC\\ITEMI.IDA");   // a mod's copy if it has one, as the game reads it
        uint32_t size = 0;
        if ((f = read_file(path, 64u << 20, &size)) && size <= 0x400) { free(f); f = 0; }
        fsize = size;
        if (!f) xlog("[wheel] couldn't read %ls; the wheel shows names only", path);
    }
    if (!f || item < 0 || item > 255 || (!((const uint32_t *)f)[item] && item)) return 0;
    uint32_t n = 0, *bgra = 0;
    uint8_t *oim = cmp_decode(f, fsize, 0x400 + ((const uint32_t *)f)[item], &n);
    if (oim && n >= 0x30 && !memcmp(oim, "OIM ", 4) && oim[0x0b] >= 1 && n >= 0x10 + 0x10 * (oim[0x0b] + 1u)) {
        const uint32_t *pix = (const uint32_t *)(oim + 0x10), *pal = (const uint32_t *)(oim + 0x10 + 0x10 * oim[0x0b]);
        if (pix[1] <= n && pix[2] <= n - pix[1] && pal[1] <= n && pal[2] >= 0x400 && pal[2] <= n - pal[1] && pix[2] >= 256) {
            const uint8_t *px = oim + pix[1], *pl = oim + pal[1];
            *w = 256; *h = pix[2] / 256;
            if (!(bgra = item_pack_picture(px, pl, *w, *h, w, h)) && (bgra = malloc(*w * *h * 4)))
                for (UINT k = 0; k < *w * *h; k++) {
                    const uint8_t *col = pl + px[k] * 4;   // RGBA
                    bgra[k] = 0xFF000000u | col[0] << 16 | col[1] << 8 | col[2];
                }
        }
    }
    free(oim);
    return bgra;
}

// A wheel picture from item picture `item`: cropped to its content, with the background made transparent
// (brightness above the background becomes coverage; colours stay premultiplied, drawn with ONE / INVSRCALPHA).
// Dark art is levelled up.
static IDirect3DTexture9 *load_picture(int item, float *aspect)
{
    UINT w = 0, h = 0;
    uint32_t *px = item_picture(item, &w, &h);
    IDirect3DTexture9 *t = 0;
    if (px) {
        // The art sits on a near-black that isn't always black (the Normal Sword's is navy, 0,0,7): take each
        // channel's most common value along the edges as the background and subtract it, or the levelling below
        // turns it into a faint box that shows against the gold highlight.
        UINT edge[3][256] = {{0}}, bg[3] = {0};
        for (UINT k = 0; k < 2 * (w + h); k++) {
            UINT x = k < w ? k : k < 2 * w ? k - w : k < 2 * w + h ? 0 : w - 1, y = k < w ? 0 : k < 2 * w ? h - 1 : k < 2 * w + h ? k - 2 * w : k - 2 * w - h;
            uint32_t p = px[y * w + x];
            for (int ch = 0; ch < 3; ch++) edge[ch][p >> (16 - 8 * ch) & 255]++;
        }
        for (int ch = 0; ch < 3; ch++)
            for (UINT v = 1; v < 256; v++) if (edge[ch][v] > edge[ch][bg[ch]]) bg[ch] = v;
        for (UINT k = 0; k < w * h; k++) {
            uint32_t p = px[k], r = p >> 16 & 255, g = p >> 8 & 255, b = p & 255;
            r = r > bg[0] ? r - bg[0] : 0; g = g > bg[1] ? g - bg[1] : 0; b = b > bg[2] ? b - bg[2] : 0;
            px[k] = 0xFF000000u | r << 16 | g << 8 | b;
        }
        UINT x0 = w, y0 = h, x1 = 0, y1 = 0;
        for (UINT y = 0; y < h; y++)
            for (UINT x = 0; x < w; x++) {
                uint32_t p = px[y * w + x];
                UINT m = max(max(p & 255, p >> 8 & 255), p >> 16 & 255);
                if (m > 24) { x0 = min(x0, x); y0 = min(y0, y); x1 = max(x1, x); y1 = max(y1, y); }
            }
        UINT hist[256] = {0}, total = 0, top = 255;
        for (UINT y = y0; y1 >= y0 && y <= y1; y++)
            for (UINT x = x0; x <= x1; x++) {
                uint32_t p = px[y * w + x];
                UINT m = max(max(p & 255, p >> 8 & 255), p >> 16 & 255);
                if (m > 24) { hist[m]++; total++; }
            }
        for (UINT acc = 0; top > 24 && (acc += hist[top]) < total / 50; top--) {}   // 98th percentile brightness
        float gain = top > 0 ? 230.0f / top : 1;
        if (gain < 1) gain = 1;
        if (gain > 2.5f) gain = 2.5f;
        if (item == 0x15 || item == 0x16) gain *= 2.2f;   // the bow and Matchlock art is dark grey: lift it further
        D3DLOCKED_RECT lr;
        if (x1 >= x0 && y1 >= y0
            && SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, x1 - x0 + 1, y1 - y0 + 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, 0))
            && SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &lr, 0, 0))) {
            for (UINT y = y0; y <= y1; y++) {
                uint32_t *d = (uint32_t *)((uint8_t *)lr.pBits + (y - y0) * lr.Pitch);
                for (UINT x = x0; x <= x1; x++) {
                    uint32_t p = px[y * w + x];
                    UINT r = min(255u, (UINT)((p >> 16 & 255) * gain)), g = min(255u, (UINT)((p >> 8 & 255) * gain));
                    UINT b = min(255u, (UINT)((p & 255) * gain)), a = min(255u, max(max(r, g), b) * 3);
                    d[x - x0] = a << 24 | r << 16 | g << 8 | b;
                }
            }
            IDirect3DTexture9_UnlockRect(t, 0);
            *aspect = (float)(x1 - x0 + 1) / (y1 - y0 + 1);
        }
    }
    free(px);
    return t;
}

// Which item picture shows a weapon (ids as in input.c): the orb swords' pictures run 0x63.. in the same order as the
// weapon ids (Raizan Lv1-3, Enryuu, Shippuu); Normal Sword 0x10; a launcher shows the bow (0x15) or Matchlock (0x16),
// with its ammo (0x17-0x1a: Normal Arrow, Fire Arrow, Bullet, Burst Bullet) on the badge.
static int weapon_picture_index(int id)   // items page ids are 0x100 + item id
{
    return id < 0 ? -1 : id >= 0x100 ? id & 0xff : id < 9 ? 0x63 + id : id == 9 ? 0x10 : id <= 0x0c ? 0x15 : id <= 0x0e ? 0x16 : -1;
}

static IDirect3DTexture9 *picture(int item, float *aspect)
{
    static IDirect3DTexture9 *tex[256];
    static float asp[256];
    static uint8_t tried[256];
    if (item < 0 || item > 255) return 0;
    static int dumped;   // debug: ONI_DUMP_PICS=1 saves the wheel's item pictures as pic_<item>.bmp, as the disc has them
    if (!dumped++ && debug_env_int("ONI_DUMP_PICS", 0) == 1) {
        static const int ids[] = {0x01, 0x02, 0x10, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b};
        for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) {
            UINT w = 0, h = 0;
            uint32_t *px = item_picture(ids[i], &w, &h);
            if (!px) continue;
            char file[32], name[MAX_PATH * 3 + 32];
            snprintf(file, sizeof file, "pic_%02x.bmp", ids[i]);
            dump_path(name, sizeof name, file);
            D3DLOCKED_RECT img = {(INT)(w * 4), px};
            write_bmp(name, &img, w, h);
            free(px);
        }
    }
    if (!tried[item]) { tried[item] = 1; tex[item] = load_picture(item, &asp[item]); }
    *aspect = asp[item];
    return tex[item];
}

// Draws item picture `k` as big as fits a bw x bh box around (x, y) (Xbox px, wheel layout). Its edges fade out, so
// backgrounds that aren't flat (Raizan Lv3's dark sky) don't show as a box.
static void draw_picture(int k, float x, float y, float bw, float bh, float alpha)
{
    static IDirect3DPixelShader9 *ps;
    static int failed;
    float aspect;
    IDirect3DTexture9 *t = picture(k, &aspect);
    if (!t || failed) return;
    if (!ps && (failed = !(ps = compile_ps("wheel_picture",
            "sampler2D s : register(s0);\n"
            "float4 c : register(c40);    // .a: opacity\n"
            "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
            "    float2 e = min(uv, 1 - uv);\n"
            "    float fade = saturate(min(e.x, e.y) / 0.1);\n"
            "    return tex2D(s, uv) * (c.a * fade);\n"
            "}\n")))) return;
    float hw = bw / 2, hh = hw / aspect;   // as big as fits the bw x bh box
    if (hh > bh / 2) { hh = bh / 2; hw = hh * aspect; }
    overt v[4];
    static const float corner[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int i = 0; i < 4; i++) v[i] = ov_pt(x + corner[i][0] * hw, y + corner[i][1] * hh, (corner[i][0] + 1) / 2, (corner[i][1] + 1) / 2);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    ov_color(ps, 0xFFFFFFFF, alpha);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof *v);
    IDirect3DDevice9_SetTexture(dev, 0, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
}

static void ov_text_row(IDirect3DTexture9 *t, int row, int rows, float x0, float y0, float x1, float y1, uint32_t argb, float alpha)
{
    if (!t) return;
    overt v[4] = {ov_pt(x0, y0, 0, (float)row / rows), ov_pt(x1, y0, 1, (float)row / rows),
                  ov_pt(x0, y1, 0, (float)(row + 1) / rows), ov_pt(x1, y1, 1, (float)(row + 1) / rows)};
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    ov_color(ov_text, argb, alpha);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof *v);
    IDirect3DDevice9_SetTexture(dev, 0, 0);
}

static float approach(float v, float target, float rate, float dt) { return target + (v - target) * expf(-rate * dt); }

// Seconds since *last (0 the first time), at most 0.1: one step of an overlay's animation.
static float overlay_dt(LONGLONG *last)
{
    LONGLONG now = qpc();
    float dt = *last ? (float)(qpc_ms(now - *last) / 1000) : 0;
    *last = now;
    return dt > 0.1f ? 0.1f : dt;
}

// Overlay drawing (the wheel, achievement popups) into target: ov_begin sets up the overlay's own state (0 when its
// shaders can't be made), ov_end puts the render target back; every Xbox render state is re-applied on the next draw.
static IDirect3DSurface9 *ov_rt, *ov_ds;
static int ov_begin(IDirect3DSurface9 *target)
{
    static int failed;
    if (failed) return 0;
    if (!ov_solid) {
        ov_solid = compile_ps("wheel", "float4 c : register(c40);\nfloat4 main(float2 uv : TEXCOORD0) : COLOR { return c; }\n");
        ov_art_ps = compile_ps("wheel_art", "sampler2D s : register(s0);\nfloat4 c : register(c40);\n"
                                            "float4 main(float2 uv : TEXCOORD0) : COLOR { return tex2D(s, uv) * c; }\n");
        ov_text = compile_ps("wheel_text", "sampler2D s : register(s0);\nfloat4 c : register(c40);\n"
                                           "float4 main(float2 uv : TEXCOORD0) : COLOR { return float4(c.rgb, c.a * tex2D(s, uv).a); }\n");
        if ((failed = !ov_solid || !ov_text)) return 0;
    }
    ov_rt = ov_ds = 0;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &ov_rt);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &ov_ds);
    IDirect3DDevice9_SetRenderTarget(dev, 0, target);   // also resets the viewport to the whole target
    IDirect3DDevice9_SetDepthStencilSurface(dev, 0);
    quad_state(1, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetTexture(dev, 0, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    return 1;
}
static void ov_end(void)
{
    IDirect3DDevice9_SetRenderTarget(dev, 0, ov_rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, ov_ds);
    if (ov_rt) IDirect3DSurface9_Release(ov_rt);
    if (ov_ds) IDirect3DSurface9_Release(ov_ds);
    apply_viewport();
    rs_valid = 0;   // everything above gets re-applied from the Xbox state on the next draw
}

// ---- the weapon wheel, in the style of the game's own menus ----------------------------------------------------
// An iron octagon with the pause plates' light rails; a panel per weapon around it, the chosen one a little bigger in
// the menus' orange frame; in the middle the chosen weapon's own picture over a plate with carved ends, its name and
// level or ammo. Drawn into the frame about to be shown (without frame interpolation, the scene itself before
// screenshots, so it shows in captures too). Layout in Xbox px, 480 to the screen's height.
#define WHEEL_SCALE 0.7f  // the whole wheel, against the layout below
#define WHEEL_RO 187.0f   // the ring: corners of its outer and inner octagon
#define WHEEL_RI 147.0f
#define WHEEL_RS 167.0f   // slot centres
#define RAIL 0xFFA8ACB6u  // the plates' light edge
#define GROOVE 0xFF34363Eu
#define PANEL 0xFF202227u

static void ov_rect(float x0, float y0, float x1, float y1, uint32_t argb, float alpha)
{
    overt v[4] = {ov_pt(x0, y0, 0, 0), ov_pt(x1, y0, 0, 0), ov_pt(x0, y1, 0, 0), ov_pt(x1, y1, 0, 0)};
    ov_draw(D3DPT_TRIANGLESTRIP, v, 2, argb, alpha);
}
static void ov_frame(float x0, float y0, float x1, float y1, float w, uint32_t argb, float alpha)   // a border w wide, inside
{
    ov_rect(x0, y0, x1, y0 + w, argb, alpha);
    ov_rect(x0, y1 - w, x1, y1, argb, alpha);
    ov_rect(x0, y0 + w, x0 + w, y1 - w, argb, alpha);
    ov_rect(x1 - w, y0 + w, x1, y1 - w, argb, alpha);
}
// Many small squares in one draw: n boxes {x0, y0, x1, y1}.
static void ov_rects(const float (*r)[4], int n, uint32_t argb, float alpha)
{
    static overt v[6 * 256];
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        overt a = ov_pt(r[i][0], r[i][1], 0, 0), b = ov_pt(r[i][2], r[i][1], 0, 0), c = ov_pt(r[i][0], r[i][3], 0, 0), d = ov_pt(r[i][2], r[i][3], 0, 0);
        v[6 * i] = a; v[6 * i + 1] = b; v[6 * i + 2] = c; v[6 * i + 3] = b; v[6 * i + 4] = d; v[6 * i + 5] = c;
    }
    if (n) ov_draw(D3DPT_TRIANGLELIST, v, 2 * n, argb, alpha);
}
// The band between two octagons (corners at radius r0 and r1, a flat side up); r0 = 0 fills it.
static void ov_octagon(float r0, float r1, uint32_t argb, float alpha)
{
    overt v[18];
    for (int i = 0; i <= 8; i++) {
        float a = 0.3926991f + i * 0.7853982f, x = cosf(a), y = sinf(a);
        v[2 * i] = ov_pt(x * r1, y * r1, 0, 0);
        v[2 * i + 1] = ov_pt(x * r0, y * r0, 0, 0);
    }
    ov_draw(D3DPT_TRIANGLESTRIP, v, 16, argb, alpha);
}
// A dark panel with the plates' light rail and a groove inside it.
static void ov_panel(float x0, float y0, float x1, float y1, float alpha)
{
    ov_rect(x0, y0, x1, y1, PANEL, alpha);
    ov_rect(x0, y0, x1, (y0 + y1) / 2, 0xFF2A2C32, alpha * 0.5f);   // a little lighter at the top
    ov_frame(x0, y0, x1, y1, 2.2f, RAIL, alpha);
    ov_frame(x0 + 2.2f, y0 + 2.2f, x1 - 2.2f, y1 - 2.2f, 0.9f, GROOVE, alpha);
}
// The pause menu's plate: carved stone ends and borders round a dark panel.
static void ov_plate(float x0, float y0, float x1, float y1, float alpha)
{
    const float cap = 15, edge = 5.5f, step = 3.4f, sq = 2;
    float r[256][4];
    int n = 0;
    ov_rect(x0, y0, x1, y1, 0xFF8E929C, alpha);
    for (float y = y0 + 1.6f; y + sq < y1 - 1.2f && n < 240; y += step)   // the carving: a lattice of small squares
        for (float x = x0 + 2.6f; x + sq < x0 + cap - 1; x += step) {
            r[n][0] = x; r[n][1] = y; r[n][2] = x + sq; r[n][3] = y + sq; n++;
            r[n][0] = x1 - (x - x0) - sq; r[n][1] = y; r[n][2] = x1 - (x - x0); r[n][3] = y + sq; n++;
        }
    for (float x = x0 + cap; x + sq < x1 - cap && n < 252; x += step) {
        r[n][0] = x; r[n][1] = y0 + 1.7f; r[n][2] = x + sq; r[n][3] = y0 + 1.7f + sq; n++;
        r[n][0] = x; r[n][1] = y1 - 1.7f - sq; r[n][2] = x + sq; r[n][3] = y1 - 1.7f; n++;
    }
    ov_rects((const float (*)[4])r, n, 0xFF3C3E46, alpha);
    ov_rect(x0, y0, x0 + 1.6f, y1, RAIL, alpha);
    ov_rect(x1 - 1.6f, y0, x1, y1, RAIL, alpha);
    ov_rect(x0 + cap, y0 + edge, x1 - cap, y1 - edge, PANEL, alpha);
    ov_frame(x0 + cap, y0 + edge, x1 - cap, y1 - edge, 1.2f, 0xFF4A4C56, alpha);
}
// The game's HUD sheet (256x256, 8-bit: the orb's frame and soul gems, the digits, the bars, and the plate the menus
// are built from), found by its contents when the HUD draws it; with it the palette the pause menu colours the plate
// with, one of the palettes loaded beside it (64 KB apart). The wheel's name plate is cut from it, so it is the
// pause menu's own, from the player's game, and goes through the same super-xBR smoothing (hud_filter = xbr).

#define PLATE_PAL_HASH 0x52f603f5u             // the pause menu's palette for it (fnv32, as texture pack names)
static uint32_t hud_sheet;                     // its texels in Xbox memory; 0 until seen
static uint32_t hud_pal;                       // the record of a palette the HUD draws it with: the pause menu's is
                                               // among the same records (its data can be anywhere)

void note_hud_sheet(const XPix *x, const XRes *pal)
{
    if (x->Data == hud_sheet) { hud_pal = (uint32_t)pal; return; }
    static struct { uint32_t data, frame; } seen[64];   // checked recently and not it
    static int next;
    if (x->Size || XFMT_ULOG(x->Format) != 8 || XFMT_VLOG(x->Format) != 8) return;
    for (int i = 0; i < 64; i++) if (seen[i].data == x->Data && frame_no - seen[i].frame < 600) return;
    if (content_hash(x, 0) == HUD_SHEET_HASH) { hud_sheet = x->Data; hud_pal = (uint32_t)pal; xlog("[wheel] found the HUD sheet at %08x", hud_sheet); return; }
    seen[next].data = x->Data; seen[next].frame = frame_no; next = (next + 1) & 63;
}

// The plate art as a texture (point-sampled art, or its super-xBR copy at the render scale), 0 if not found yet.
struct tex_entry plate;   // ours, not in the cache: upscaled() keeps the smoothed copy in it (reset_device drops that)
static IDirect3DTexture9 *plate_art(int *smooth)
{
    static uint32_t built_from, tried_frame;
    *smooth = settings.hud_xbr && scale > 1;
    if (!hud_sheet) return 0;
    if (built_from != hud_sheet) {
        if (tried_frame && frame_no - tried_frame < 120) return 0;   // its palette isn't loaded yet: look again in 2 s
        tried_frame = frame_no;
        const uint32_t *pal = 0;
        // Every word within 16 KB of that record that points at a 64 KB-aligned block (palettes get their own) could
        // be the pause palette's record's data pointer: the one whose 1 KB hashes right is.
        uint32_t lo = hud_pal - 0x4000, n = readable_bytes((void *)lo, 0x8000);
        if (n < 0x8000) { lo = hud_pal; n = readable_bytes((void *)lo, 0x4000); }
        for (uint32_t w = lo; w + 4 <= lo + n && !pal; w += 4) {
            uint32_t v = *(const uint32_t *)w;
            const uint32_t *a = (const uint32_t *)v;
            if (!v || (v & 0xFFFF) || readable_bytes(a, 1024) < 1024) continue;
            if (pack_pal_hash(a) == PLATE_PAL_HASH) pal = a;
        }
        if (!pal || readable_bytes((void *)hud_sheet, 65536) < 65536) {
            static int said;
            if (!said++) xlog("[wheel] the plate's palette isn't loaded (yet): drawn plate meanwhile");
            return 0;
        }
        if (plate.tex) IDirect3DTexture9_Release(plate.tex);
        if (plate.big) IDirect3DTexture9_Release(plate.big);
        memset(&plate, 0, sizeof plate);
        D3DLOCKED_RECT lr;
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, 256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &plate.tex, 0))) { plate.tex = 0; return 0; }
        if (SUCCEEDED(IDirect3DTexture9_LockRect(plate.tex, 0, &lr, 0, 0))) {
            uint32_t mx, my;
            swizzle_masks(256, 256, &mx, &my);
            const uint8_t *src = (const uint8_t *)hud_sheet;
            for (uint32_t y = 0; y < 256; y++) {
                uint32_t *row = (uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch), oy = pdep(y, my);
                for (uint32_t x = 0; x < 256; x++) {
                    uint32_t c = pal[src[pdep(x, mx) | oy]], a = (c >> 24) * 2;   // PS2-style alpha: 0x80 is opaque
                    row[x] = (a > 255 ? 255 : a) << 24 | (c & 0xFFFFFF);
                }
            }
            // The plate's middle without its pattern: each row averaged across it, into the unused texels at x 248-255
            // of the same rows, a strip the name plate stretches over its middle (seamless with the carved ends).
            // The panel's own rows (106-131) are smoothed over 7 rows as well, or the pattern left stripes.
            float avg[40][4] = {{0}};
            for (uint32_t y = 98; y < 138; y++) {
                const uint32_t *row = (const uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch);
                for (uint32_t x = 22; x < 203; x++)
                    for (int ch = 0; ch < 4; ch++) avg[y - 98][ch] += (row[x] >> (8 * ch) & 255) / 181.0f;
            }
            for (int y = 0; y < 40; y++) {
                float c[4] = {0};
                int lo = y >= 8 && y < 34 ? (y - 3 < 8 ? 8 : y - 3) : y, hi = y >= 8 && y < 34 ? (y + 3 > 33 ? 33 : y + 3) : y;
                for (int j = lo; j <= hi; j++) for (int ch = 0; ch < 4; ch++) c[ch] += avg[j][ch] / (hi - lo + 1);
                uint32_t v = 0;
                for (int ch = 0; ch < 4; ch++) v |= (uint32_t)(c[ch] + 0.5f) << (8 * ch);
                uint32_t *row = (uint32_t *)((uint8_t *)lr.pBits + (98 + y) * lr.Pitch);
                for (uint32_t x = 248; x < 256; x++) row[x] = v;
            }
            IDirect3DTexture9_UnlockRect(plate.tex, 0);
        }
        plate.uploads = 1;
        built_from = hud_sheet;
        xlog("[wheel] name plate from the game's own art");
    }
    return *smooth ? upscaled(&plate, 2) : plate.tex;
}

// Part of a texture on screen: (x0, y0)-(x1, y1) in wheel units from texels (u0, v0)-(u1, v1) of a 256x256 sheet.
static void ov_art(IDirect3DTexture9 *t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float alpha)
{
    overt v[4] = {ov_pt(x0, y0, u0 / 256, v0 / 256), ov_pt(x1, y0, u1 / 256, v0 / 256), ov_pt(x0, y1, u0 / 256, v1 / 256), ov_pt(x1, y1, u1 / 256, v1 / 256)};
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    ov_color(ov_art_ps, 0xFFFFFFFF, alpha);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof *v);
    IDirect3DDevice9_SetTexture(dev, 0, 0);
}

// The name plate: the pause menu's plate, its carved ends and borders from the game's art (scaled to the height), its middle the
// plate's own panel with each row averaged, so without its pattern. Falls back to a drawn likeness until the art has been found.
static void ov_name_plate(float x0, float y0, float x1, float y1, float alpha)
{
    int smooth;
    IDirect3DTexture9 *t = plate_art(&smooth);
    if (!t || !ov_art_ps) { ov_plate(x0, y0, x1, y1, alpha); return; }
    const float L = 21, R = 21, T = 6, B = 5, k = (y1 - y0) / 40;   // in texels: the plate is (1, 98)-(224, 138)
    DWORD f = smooth ? D3DTEXF_LINEAR : D3DTEXF_POINT;   // pixel art stays pixel art, as the pause menu's does
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, f);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, f);
    ov_art(t, x0, y0, x0 + L * k, y1, 1, 98, 1 + L, 138, alpha);                                 // ends
    ov_art(t, x1 - R * k, y0, x1, y1, 224 - R, 98, 224, 138, alpha);
    ov_art(t, x0 + L * k, y0, x1 - R * k, y0 + T * k, 1 + L, 98, 224 - R, 98 + T, alpha);        // top and bottom borders
    ov_art(t, x0 + L * k, y1 - B * k, x1 - R * k, y1, 1 + L, 138 - B, 224 - R, 138, alpha);
    ov_art(t, x0 + L * k, y0 + T * k, x1 - R * k, y1 - B * k, 251.5f, 98 + T, 252.5f, 138 - B, alpha);   // its middle, unpatterned
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
}

// Text with a dark outline, as the game's own letters have.
static void ov_text_outlined(IDirect3DTexture9 *t, int row, int rows, float x0, float y0, float x1, float y1, uint32_t argb, float alpha)
{
    static const float o[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-0.7f, -0.7f}, {0.7f, -0.7f}, {-0.7f, 0.7f}, {0.7f, 0.7f}};
    const float k = 0.75f;
    for (int i = 0; i < 8; i++) ov_text_row(t, row, rows, x0 + o[i][0] * k, y0 + o[i][1] * k, x1 + o[i][0] * k, y1 + o[i][1] * k, 0xFF0A0806, alpha * 0.8f);
    ov_text_row(t, row, rows, x0, y0, x1, y1, argb, alpha);
}

void draw_wheel(IDirect3DSurface9 *target)
{
    static struct wheel shown;   // what is on screen; kept after close so it can fade out
    static IDirect3DTexture9 *names, *subs, *badges, *hint, *tabs;
    static char text_key[sizeof shown.name + sizeof shown.sub + sizeof shown.badge];
    static float open, pop[8];
    static LONGLONG last;
    float dt = overlay_dt(&last);
    if (wheel.n) {
        if (!shown.n || open < 0.05f) memset(pop, 0, sizeof pop);
        shown = wheel;
    }
    open = approach(open, wheel.n ? 1.0f : 0.0f, wheel.n ? 14.0f : 18.0f, dt);
    if (!wheel.n && open < 0.02f) { shown.n = 0; open = 0; }
    if (!shown.n) return;
    const int n = shown.n, NW = 148, NH = 24, SW = 148, SH = 16, BW = 40, BH = 14, HW = 150, HH = 12, TW = 80, TH = 20;   // text boxes
    int ts = scale < 4 ? scale : 4;   // text texture resolution
    char key[sizeof text_key];
    memcpy(key, shown.name, sizeof shown.name);
    memcpy(key + sizeof shown.name, shown.sub, sizeof shown.sub);
    memcpy(key + sizeof shown.name + sizeof shown.sub, shown.badge, sizeof shown.badge);
    if (!names || memcmp(text_key, key, sizeof key)) {   // only when a label changes (open, LB/RB)
        if (names) IDirect3DTexture9_Release(names);
        if (subs) IDirect3DTexture9_Release(subs);
        if (badges) IDirect3DTexture9_Release(badges);
        names = text_texture_face(shown.name[0], sizeof shown.name[0], n, NW * ts, NH * ts, 18 * ts, 1);
        subs = text_texture_face(shown.sub[0], sizeof shown.sub[0], n, SW * ts, SH * ts, 12 * ts, 1);
        badges = text_texture_face(shown.badge[0], sizeof shown.badge[0], n, BW * ts, BH * ts, 11 * ts, 1);
        memcpy(text_key, key, sizeof key);
    }
    if (!hint) hint = text_texture_face("LB / RB  ammo\0\0\0A  use", 16, 2, HW * ts, HH * ts, 10 * ts, 1);
    // The page tabs and the control that turns the page: the right stick with Modern controls (RT holds the wheel
    // open), otherwise LT / RT.
    static const char tab_text[2][4][16] = {{"Weapons", "Items", "LT", "RT"}, {"Weapons", "Items", "RS", "RS"}};
    if (!tabs) tabs = text_texture_face(tab_text[settings.modern_controls != 0][0], sizeof tab_text[0][0], 4, TW * ts, TH * ts, 14 * ts, 1);
    for (int i = 0; i < n; i++) pop[i] = approach(pop[i], i == shown.sel ? 1.0f : 0.0f, 16.0f, dt);
    float e = open * open * (3 - 2 * open);   // smoothstep
    ov_k = (float)scale; ov_cx = bb_w / 2.0f; ov_cy = bb_h / 2.0f; ov_zoom = (0.9f + 0.1f * e) * WHEEL_SCALE; ov_fade = e;
    if (pres_wide) {   // drawn straight into the wide picture: 480 Xbox px to its height
        D3DSURFACE_DESC td;
        IDirect3DSurface9_GetDesc(target, &td);
        ov_k = td.Height / 480.0f; ov_cx = td.Width / ov_k / 2; ov_cy = 240;
    }

    if (!ov_begin(target)) return;

    {   // dim the game
        float z = ov_zoom; ov_zoom = 1;
        overt v[4] = {ov_pt(-ov_cx, -ov_cy, 0, 0), ov_pt(ov_cx, -ov_cy, 0, 0), ov_pt(-ov_cx, ov_cy, 0, 0), ov_pt(ov_cx, ov_cy, 0, 0)};
        ov_draw(D3DPT_TRIANGLESTRIP, v, 2, 0xFF000000, 0.5f);
        ov_zoom = z;
    }
    // the ring: a dark iron band between light rails, the middle darker still
    ov_octagon(0, WHEEL_RI, 0xFF08080A, 0.6f);
    ov_octagon(WHEEL_RI, WHEEL_RO, 0xFF1F2025, 0.95f);
    ov_octagon(WHEEL_RO - 2.2f, WHEEL_RO, RAIL, 1);
    ov_octagon(WHEEL_RO - 3.1f, WHEEL_RO - 2.2f, GROOVE, 1);
    ov_octagon(WHEEL_RI, WHEEL_RI + 2.2f, RAIL, 1);
    ov_octagon(WHEEL_RI + 2.2f, WHEEL_RI + 3.1f, GROOVE, 1);

    for (int i = 0; i < n; i++) {   // the slots, the first at the top, clockwise
        float a = i * 6.2831853f / n, cx = sinf(a) * WHEEL_RS, cy = -cosf(a) * WHEEL_RS, p = pop[i];
        float hw = 50 + 5.5f * p, hh = 26 + 3.5f * p, al = 0.82f + 0.18f * p;
        float x0 = cx - hw, y0 = cy - hh, x1 = cx + hw, y1 = cy + hh;
        ov_panel(x0, y0, x1, y1, 1);
        draw_picture(weapon_picture_index(shown.id[i]), cx, cy, 2 * hw - 15, 2 * hh - 12, al);
        if (shown.badge[i][0]) ov_text_outlined(badges, i, n, x1 - BW - 3, y1 - BH - 2, x1 - 3, y1 - 2, 0xFFFFF0D0, al);
        if (p > 0.01f) {   // the menus' selection: an orange frame with a lit inner line
            ov_frame(x0 - 3.2f, y0 - 3.2f, x1 + 3.2f, y1 + 3.2f, 2.7f, 0xFFD98A2A, p);
            ov_frame(x0 - 0.5f, y0 - 0.5f, x1 + 0.5f, y1 + 0.5f, 0.9f, 0xFFFFD68C, p);
        }
    }

    int s = shown.sel;
    ov_panel(-66, -78, 66, -7, 1);   // the chosen weapon, big
    draw_picture(weapon_picture_index(shown.id[s]), 0, -42.5f, 118, 59, 1);
    ov_name_plate(-96, 2, 96, 56, 1);   // its name, and level or ammo
    ov_text_outlined(names, s, n, -NW / 2.0f, shown.sub[s][0] ? 10.0f : 17.0f, NW / 2.0f, (shown.sub[s][0] ? 10.0f : 17.0f) + NH, 0xFFECECE6, 1);
    if (shown.sub[s][0]) ov_text_outlined(subs, s, n, -SW / 2.0f, 33, SW / 2.0f, 33.0f + SH, 0xFFD8B870, 1);
    if (shown.cycle[s] || shown.page) ov_text_outlined(hint, shown.page ? 1 : 0, 2, -HW / 2.0f, 60, HW / 2.0f, 60.0f + HH, 0xFFB0A080, 1);

    {   // the page tabs above the ring, as the inventory's: the current page lit; the control that turns it beside them
        float ty = -WHEEL_RO - 46;
        for (int k = 0; k < 2; k++) {
            float x0 = k ? 6.0f : -86.0f;
            int on = shown.page == k;
            ov_rect(x0, ty, x0 + TW, ty + TH + 2, on ? 0xFF7A5626 : 0xFF283634, 0.95f);
            ov_frame(x0, ty, x0 + TW, ty + TH + 2, 1, on ? 0xFFE8B850 : 0xFF4E6A66, 1);
            ov_text_outlined(tabs, k, 4, x0, ty + 1, x0 + TW, ty + 1 + TH, on ? 0xFFFAECC8 : 0xFFA0AAA5, 1);
        }
        for (int k = 0; k < 2; k++) {
            float sgn = k ? 1.0f : -1.0f, ax = sgn * 150, my = ty + 11;
            overt v[3] = {ov_pt(ax + sgn * 9, my, 0, 0), ov_pt(ax, my - 7, 0, 0), ov_pt(ax, my + 7, 0, 0)};   // the inventory's green arrows
            ov_draw(D3DPT_TRIANGLELIST, v, 1, 0xFF46C850, 1);
            ov_text_outlined(tabs, 2 + k, 4, ax - sgn * 26 - 18, ty + 1, ax - sgn * 26 + 18, ty + 1 + TH, 0xFFC8D2C8, 1);
        }
    }
    ov_end();
}

// Achievement popup (achievements.c queues them): slides in at the top right of the picture, stays 4 s, fades; one at
// a time. Placed in Xbox px with 480 to the target's height, so it sits the same in the 4:3 and the wide picture.
// Movie subtitles (movie.c picks the line for the frame Bink shows): centred in the black band under the movie, in
// Xbox px with 480 to the target's height like the popup; white with a dark outline (the text drawn offset in eight
// directions first). The text is drawn with GDI's wide-character calls, so accents and (through Windows' font
// fallback) Japanese work.
static IDirect3DTexture9 *sub_texture(const wchar_t *text, int tw, int th, int px)
{
    HDC dc = CreateCompatibleDC(0);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), tw, -th, 1, 32}};
    void *bits = 0;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    HGDIOBJ old_bm = SelectObject(dc, bm);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT r = {0, 0, tw, th};
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    int h = r.bottom;
    r.left = 0; r.right = tw; r.top = th - h; r.bottom = th;   // bottom-aligned: one line sits where the last of two would
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_WORDBREAK);
    GdiFlush();
    IDirect3DTexture9 *t = 0;
    D3DLOCKED_RECT lr;
    if (bits && SUCCEEDED(IDirect3DDevice9_CreateTexture(dev, tw, th, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, 0))
        && SUCCEEDED(IDirect3DTexture9_LockRect(t, 0, &lr, 0, 0))) {
        for (int y = 0; y < th; y++) {
            const uint32_t *s = (const uint32_t *)bits + y * tw;
            uint32_t *d = (uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch);
            for (int x = 0; x < tw; x++) d[x] = (s[x] & 0xFF) << 24 | 0xFFFFFF;   // grey level -> alpha
        }
        IDirect3DTexture9_UnlockRect(t, 0);
    }
    SelectObject(dc, old_font); DeleteObject(font);
    SelectObject(dc, old_bm); DeleteObject(bm);
    DeleteDC(dc);
    return t;
}

void draw_subtitle(IDirect3DSurface9 *target)
{
    static IDirect3DTexture9 *text;
    static const wchar_t *shown;
    const float W = 600, H = 54;   // Xbox px: room for two lines
    const wchar_t *line = movie_subtitle();
    if (line != shown) {
        if (text) IDirect3DTexture9_Release(text);
        text = 0;
        shown = line;
        int ts = scale < 4 ? scale : 4;
        if (line) text = sub_texture(line, (int)W * ts, (int)H * ts, 20 * ts);
    }
    if (!text) return;
    D3DSURFACE_DESC td;
    IDirect3DSurface9_GetDesc(target, &td);
    if (!ov_begin(target)) return;
    ov_k = td.Height / 480.0f; ov_zoom = 1; ov_fade = 1;
    ov_cx = td.Width / ov_k / 2 - W / 2; ov_cy = 480 - 6 - H;   // its top-left corner
    static const float o[8][2] = {{-1, -1}, {0, -1.4f}, {1, -1}, {-1.4f, 0}, {1.4f, 0}, {-1, 1}, {0, 1.4f}, {1, 1}};
    for (int i = 0; i < 8; i++) ov_text_row(text, 0, 1, o[i][0], o[i][1], W + o[i][0], H + o[i][1], 0xFF000000, 0.9f);
    ov_text_row(text, 0, 1, 0, 0, W, H, 0xFFFFFFFF, 1);
    ov_end();
}

void draw_popup(IDirect3DSurface9 *target)
{
    static IDirect3DTexture9 *head, *text;
    static float t = -1;   // seconds since it appeared; < 0: none showing
    static LONGLONG last;
    static uint32_t sound = ~0u;
    float dt = overlay_dt(&last);
    const float SHOW = 4.5f, W = 304, H = 50, TW = 250;   // Xbox px; TW: the text, inside the plate's carved ends
    int ts = scale < 4 ? scale : 4;
    if (t < 0 || t > SHOW) {
        static float waited;
        int smooth;
        if (ach_popup_pending() && !plate_art(&smooth) && waited < 3) { waited += dt; t = -1; return; }   // its plate's art is still loading
        const char *name = ach_popup_next();
        if (name) waited = 0;
        if (!name) { t = -1; return; }
        char row[64];
        snprintf(row, sizeof row, "%s", name);
        if (text) IDirect3DTexture9_Release(text);
        text = text_texture_face(row, sizeof row, 1, (int)TW * ts, 20 * ts, 15 * ts, 1);
        if (!head) head = text_texture_face("Achievement unlocked", 32, 1, (int)TW * ts, 14 * ts, 10 * ts, 1);
        // The game's own menu chime ("decide", as an equip). Debug: ONI_ACH_SOUND=<hex id> plays another of its sounds.
        if (sound == ~0u) { char v[16]; sound = env_str("ONI_ACH_SOUND", v, sizeof v) ? strtoul(v, 0, 16) : 0x0201000fu; }
        game_sound(sound);
        t = 0;
    }
    t += dt;
    float in = t < 0.35f ? t / 0.35f : 1, out = t > SHOW - 0.5f ? (SHOW - t) / 0.5f : 1, a = in < out ? in : out;
    if (a <= 0) return;
    D3DSURFACE_DESC td;
    IDirect3DSurface9_GetDesc(target, &td);
    if (!ov_begin(target)) return;
    ov_k = td.Height / 480.0f; ov_zoom = 1; ov_fade = a;
    ov_cx = td.Width / ov_k - 14 - W + (1 - in) * (1 - in) * 40; ov_cy = 18;   // its top-left corner; slides in from the right
    ov_name_plate(0, 0, W, H, 1);   // the pause menu's plate, as the weapon wheel's name
    ov_text_outlined(head, 0, 1, (W - TW) / 2, 8, (W + TW) / 2, 22, 0xFFD8B870, 1);
    ov_text_outlined(text, 0, 1, (W - TW) / 2, 21, (W + TW) / 2, 41, 0xFFF0ECE0, 1);
    ov_end();
}

// settings: show_fps. Frames shown per second (in-betweens included) and, when frame interpolation makes it differ,
// the game's own steps per second; in the window's top-left corner, drawn on the window-sized picture as it is shown,
// so neither the picture mode nor the HUD layer moves it. Counted over half a second.
unsigned fps_steps;   // D3DDevice_Present calls: game steps
void draw_fps(IDirect3DSurface9 *target)
{
    static IDirect3DTexture9 *text;
    static unsigned shown, steps0, text_h;
    static float W;
    static LONGLONG t0;
    static char str[32];
    if (!settings.show_fps) return;
    LONGLONG now = qpc();
    shown++;
    if (!t0) t0 = now, steps0 = fps_steps;
    double dt = qpc_ms(now - t0) / 1000;
    D3DSURFACE_DESC td;
    IDirect3DSurface9_GetDesc(target, &td);
    const float H = 20;   // Xbox px, 480 to the window's height
    if (dt >= 0.5) {
        int fps = (int)(shown / dt + 0.5), steps = (int)((fps_steps - steps0) / dt + 0.5);
        char s[32];
        if (abs(fps - steps) > 2) snprintf(s, sizeof s, "%d fps  (game %d)", fps, steps);
        else snprintf(s, sizeof s, "%d fps", fps);
        shown = 0; steps0 = fps_steps; t0 = now;
        if (strcmp(s, str) || text_h != td.Height) {
            strcpy(str, s);
            W = 14 + 6.5f * (float)strlen(str);   // about the text's width
            text_h = td.Height;
            int k = (int)ceilf(td.Height / 480.0f);
            if (text) IDirect3DTexture9_Release(text);
            text = text_texture(str, sizeof str, 1, (int)W * k, (int)H * k, 13 * k);
        }
    }
    if (!text || !ov_begin(target)) return;
    ov_k = td.Height / 480.0f; ov_zoom = 1; ov_fade = 1;
    ov_cx = 6; ov_cy = 6;   // its top-left corner
    overt v[4] = {ov_pt(0, 0, 0, 0), ov_pt(W, 0, 0, 0), ov_pt(0, H, 0, 0), ov_pt(W, H, 0, 0)};
    ov_draw(D3DPT_TRIANGLESTRIP, v, 2, 0xFF0A0806, 0.6f);
    ov_text_row(text, 0, 1, 0, 0, W, H, 0xFFFFF0D0, 1);
    ov_end();
}
