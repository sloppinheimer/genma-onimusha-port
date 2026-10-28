// picture = modern, split out of d3d.c: the 2019 remaster's widescreen (the slice that follows Samanosuke,
// the HUD layer pinned to the screen's corners, fades, letterbox bars, wide movies and stills, where the slice sits).
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

// ---- picture = modern: widescreen the way the 2019 remaster does it ------------------------------------
// The game draws 640x448 art (y 16-464, black strips above and below) and ends each frame by copying it and drawing
// it back squeezed to x 21.33-618.67 (the TV's pixel shape): a 4:3 picture. Modern fills the window with a slice of
// that picture the window's shape, sliding up and down to keep Samanosuke in view, as the remaster does. The HUD
// (2D drawn with the depth test off or "always", narrower than the screen) goes to its own layer instead, each piece
// pinned to the screen edges it sits near, so the crop never cuts it. Only while the game is in play, and for movies
// whose own picture is wide (they fill the width, uncut): menus and the title screen keep the 4:3 picture.
int wide;                       // the frame being drawn is a modern one (decided at the end of each Present)
int pres_wide;
int held_wide;       // the frame being shown is; the held one is
// The shots the remaster frames by hand in normal play, by stage, room and camera cut; each is the same shot in Genma
// (backgrounds compared, docs/widescreen.md). Heights are wide_pan's (+1 the picture's top). In (4,4,4) the remaster
// follows another object, not traced: here it follows him, without the dead zone.
enum { SHOT_HOLD, SHOT_HOLD_SNAP, SHOT_RAISE, SHOT_STRETCH, SHOT_NO_DEAD, SHOT_WHOLE };
static const struct { uint8_t stage, room, cut, kind; float v; } wide_shots[] = {
    {1, 6, 6, SHOT_HOLD, 0}, {2, 35, 3, SHOT_HOLD, 0.8f}, {5, 1, 2, SHOT_HOLD, 0.8f}, {5, 21, 16, SHOT_HOLD, 0.8f},
    {5, 18, 1, SHOT_RAISE, 0.8f}, {5, 18, 3, SHOT_HOLD, -0.6426f}, {5, 18, 4, SHOT_HOLD, -0.5573f},
    {5, 18, 13, SHOT_HOLD_SNAP, -0.355f}, {4, 4, 2, SHOT_RAISE, 1.75f}, {4, 4, 4, SHOT_NO_DEAD, 0},
    {4, 4, 1, SHOT_STRETCH, 0}, {4, 4, 25, SHOT_STRETCH, 0},   // his height from the bottom to 0.4 spans the picture
    {5, 21, 5, SHOT_WHOLE, 0},                                // the whole 4:3 picture, bars at the sides
};

static int wide_shot(float *v)   // the current shot's wide_shots kind (and value), or -1
{
    uint16_t st = *(volatile uint16_t *)G_STAGE, rm = *(volatile uint16_t *)G_ROOM, ct = *(volatile uint16_t *)G_CUT;
    for (size_t i = 0; i < sizeof wide_shots / sizeof *wide_shots; i++)
        if (wide_shots[i].stage == st && wide_shots[i].room == rm && wide_shots[i].cut == ct) {
            *v = wide_shots[i].v;
            return wide_shots[i].kind;
        }
    return -1;
}

float wide_aspect = 16.0f / 9;
float pan_y = -1;   // window shape; top of the slice (Xbox px)
IDirect3DTexture9 *hud_tex;     // the HUD layer: 480 high, the window's shape, premultiplied alpha
IDirect3DSurface9 *hud_surf;
static UINT hud_w, hud_h;
int hud_drawn;                  // the HUD layer was cleared and drawn into this frame
int pres_hud;
int held_hud, frame_hud;   // ... in the frame being shown; in the held one; at the last Present
float hud_map[4];               // for the draw in progress: x' = x * [0] + [1], y' = y * [2] + [3] (Xbox px)
int hud_draw;
static int hud_aop_stage = -1;          // a stage whose alpha op hud_begin raised, and what it was
static DWORD hud_aop;
static int view_playing;               // game_view at the last Present
static float lbox[2];                  // in-engine scenes: the picture between the game's letterbox bars in the frame
                                       // being drawn (y, Xbox px; 0 = no bar)
static float wide_pic[2];              // a 2D picture wider than 4:3 drawn the full width this frame (y, Xbox px)

int held;                       // a real frame is waiting to be shown at the end of its slot
uint32_t mid_shot;              // debug: ONI_SHOTS also saves the in-between before that frame (mid_<frame>.bmp)
uint32_t inb_shown, inb_skipped;
// Capture paths are UTF-8 (capture_path), opened through wide calls: an install under a folder name outside the ANSI
// code page (a Cyrillic or Japanese user name, on a drive without 8.3 names) made every capture fail silently.
int utf8_wide(const char *path, wchar_t *w, int n) { return MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n); }
FILE *ufopen(const char *path, const wchar_t *mode)
{
    wchar_t w[MAX_PATH + 160];
    return utf8_wide(path, w, MAX_PATH + 160) ? _wfopen(w, mode) : 0;
}
int uexists(const char *path)
{
    wchar_t w[MAX_PATH + 160];
    return utf8_wide(path, w, MAX_PATH + 160) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}
// A capture from an earlier session at the same frame number (frame numbers start at 1 each launch) is kept: the new
// one gets _2, _3... before its extension.
void unique_path(char *path, size_t cap)
{
    if (!uexists(path)) return;
    char ext[16] = "", *dot = strrchr(path, '.');
    if (dot && !strchr(dot, '\\') && strlen(dot) < sizeof ext) { strcpy(ext, dot); *dot = 0; }
    size_t n = strlen(path);
    for (int k = 2; k < 1000; k++) { snprintf(path + n, cap - n, "_%d%s", k, ext); if (!uexists(path)) return; }
}


// picture = modern: if this draw is a HUD piece, map it into the HUD layer (pinned to the edges it sits near) and
// draw it there. verts: the draw's vertices, Xbox px.
int hud_begin(const uint8_t *verts, uint32_t stride, uint32_t n)
{
    if ((vertex_shader & 1) || (vertex_shader & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || cur_rt != &backbuffer_hdr) return 0;
    if (RS[XRS_ZENABLE] && RS[XRS_ZFUNC] != 0x207) return 0;   // depth-tested: part of the scene (the background, effects)
    // Added on (ONE/ONE) from a DXT1 picture: an animated background sheet laid over the scene (the fire in room 312's
    // scene, xmov312a), not HUD. In the HUD layer it kept its 4:3 size, centred, and its cut right and bottom edges,
    // which the 4:3 picture hides at the screen's edge and under the letterbox bar, showed as a glowing rectangle.
    if (RS[XRS_ALPHABLENDENABLE] && RS[XRS_SRCBLEND] == 1 && RS[XRS_DESTBLEND] == 1 && bound_tex[0]
        && XFMT_CODE(bound_tex[0]->Format) == 0x0c) return 0;
    float x0 = 1e9f, x1 = -1e9f, y1 = -1e9f, wide_piece = 0, g0 = 1e9f, g1 = -1e9f;
    for (uint32_t i = 0; i < n; i++) {
        const float *p = (const float *)(verts + i * stride);
        if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] > y1) y1 = p[1];
        if (p[0] < g0) g0 = p[0]; if (p[0] > g1) g1 = p[0];
        if (i % 4 == 3 || i == n - 1) { if (g1 - g0 > wide_piece) wide_piece = g1 - g0; g0 = 1e9f; g1 = -1e9f; }
    }
    // A (nearly) full-width piece: the letterbox strips, fades, the final squeeze: part of the picture. By piece (each 4
    // vertices, a quad): a long subtitle line spans as much, but in letters (the first boss's death scene, zoomed in).
    if (wide_piece >= 560) return 0;
    UINT w = (UINT)(480 * wide_aspect + 0.5f) * scale, h = 480 * scale;
    if (hud_tex && (hud_w != w || hud_h != h)) { IDirect3DSurface9_Release(hud_surf); IDirect3DTexture9_Release(hud_tex); hud_tex = 0; hud_surf = 0; }
    if (!hud_tex) {
        if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &hud_tex, 0))) { hud_tex = 0; return 0; }
        IDirect3DTexture9_GetSurfaceLevel(hud_tex, 0, &hud_surf);
        hud_w = w; hud_h = h;
    }
    // The layer is 480 Xbox px high, so HUD pieces and menus keep the size and height they have in the 4:3 picture
    // (squeezed to the TV's pixel shape like it). Pieces in the top band (the health HUD) are pinned to the left or
    // right edge they sit near; everything else (messages, which type out from the left, menus, subtitles) stays
    // centred, so a growing line never changes side. The top band also moves up by the black strip above the art
    // (16 px), which the slice crops away: it keeps its distance from the top of the picture, not of the TV frame.
    float cw = 480 * wide_aspect, zx = (ART_X1 - ART_X0) / 640;
    int top = y1 <= 160;
    hud_map[0] = zx; hud_map[2] = 1; hud_map[3] = top ? -ART_Y0 : 0;
    hud_map[1] = top && x1 <= 300 ? 0 : top && x0 >= 340 ? cw - 640 * zx : cw / 2 - 320 * zx;
    hud_draw = 1;
    IDirect3DDevice9_SetRenderTarget(dev, 0, hud_surf);
    IDirect3DDevice9_SetDepthStencilSurface(dev, 0);
    rec_target(hud_surf, 0);
    if (!hud_drawn) { dev_clear(0, 0, D3DCLEAR_TARGET, 0, 1, 0); hud_drawn = 1; }
    // Premultiplied alpha in the layer: colour blends as the game asks; alpha builds up as coverage.
    DWORD on = 0, db = 0;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &on);
    if (!on) {
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO);
    }
    IDirect3DDevice9_GetRenderState(dev, D3DRS_DESTBLEND, &db);
    int over = db == D3DBLEND_INVSRCALPHA, opaque = !on;
    hud_aop_stage = -1;
    if (opaque && !pixel_shader) {   // unblended pieces (menu boxes) carry PS2-style alpha (0x80 = solid): x4, as the
        DWORD op = 0;                // game's own blended HUD does, so the layer sees them solid; 0 stays 0 (alpha test)
        for (int st = 0; st < 4; st++) {
            IDirect3DDevice9_GetTextureStageState(dev, st, D3DTSS_COLOROP, &op);
            if (op == D3DTOP_DISABLE) break;
            hud_aop_stage = st;
        }
        if (hud_aop_stage >= 0) {
            IDirect3DDevice9_GetTextureStageState(dev, hud_aop_stage, D3DTSS_ALPHAOP, &hud_aop);
            if (hud_aop == D3DTOP_MODULATE || hud_aop == D3DTOP_MODULATE2X)
                IDirect3DDevice9_SetTextureStageState(dev, hud_aop_stage, D3DTSS_ALPHAOP, D3DTOP_MODULATE4X);
            else hud_aop_stage = -1;
        }
    }
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLENDALPHA, over || opaque ? D3DBLEND_ONE : D3DBLEND_ZERO);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLENDALPHA, over ? D3DBLEND_INVSRCALPHA : opaque ? D3DBLEND_ZERO : D3DBLEND_ONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    return 1;
}

// picture = modern: a fade (a blended quad over the whole art, drawn after the HUD) covers the scene under the HUD
// layer only; in 4:3 it covers the HUD as well. Draws it into the HUD layer too, where the layer (premultiplied, its
// alpha the HUD's coverage) becomes HUD * (1 - a) + colour * a * coverage, coverage untouched: composited over the
// faded scene that is the fade over HUD and scene, as in 4:3. (Scaling by 1 - a alone turned the HUD into a black
// silhouette on the white flash of a talisman's revival.)
int is_fade(const uint8_t *verts, uint32_t stride, uint32_t n)
{
    if ((vertex_shader & 1) || (vertex_shader & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || cur_rt != &backbuffer_hdr) return 0;
    if (!RS[XRS_ALPHABLENDENABLE] || RS[XRS_SRCBLEND] != 0x302 || RS[XRS_DESTBLEND] != 0x303) return 0;
    if (RS[XRS_ZENABLE] && RS[XRS_ZFUNC] != 0x207) return 0;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < n; i++) {
        const float *p = (const float *)(verts + i * stride);
        if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
    }
    return x0 <= 1 && x1 >= 639 && y0 <= ART_Y0 + 1 && y1 >= ART_Y1 - 1;
}

// A textured fade whose texture coordinates stay inside one texel (the game's fades and bars take one texel of a small
// sheet as a solid colour: the talisman's white flash, uv 0.994-0.996 of a 16x16): that texel's colour, or 0.
static int one_texel(const uint8_t *verts, UINT stride, UINT count, uint32_t *argb)
{
    const XPix *x = bound_tex[0];
    if (!x || stride < 24) return 0;
    struct xtex_info t = tex_info(x->Format, x->Size);
    if (!t.w || !t.h || is_dxt(t.kind) || t.kind == F_DEPTH) return 0;
    float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
    for (UINT i = 0; i < count; i++) {
        const float *uv = (const float *)(verts + i * stride + stride - 8);
        if (uv[0] < u0) u0 = uv[0]; if (uv[0] > u1) u1 = uv[0]; if (uv[1] < v0) v0 = uv[1]; if (uv[1] > v1) v1 = uv[1];
    }
    if ((u1 - u0) * t.w > 1 || (v1 - v0) * t.h > 1 || u0 < 0 || v0 < 0 || u1 > 1 || v1 > 1) return 0;
    uint32_t tx = (uint32_t)((u0 + u1) / 2 * t.w), ty = (uint32_t)((v0 + v1) / 2 * t.h);
    if (tx >= t.w) tx = t.w - 1; if (ty >= t.h) ty = t.h - 1;
    const uint32_t *pal = 0;
    if (t.kind == F_P8) {
        if (!(pal = stage_palette(0))) return 0;
    }
    const uint8_t *src = (const uint8_t *)x->Data;
    if (readable_bytes(src, texture_bytes(&t)) < texture_bytes(&t)) return 0;
    uint32_t off;
    if (t.linear) off = ty * t.pitch + tx * t.bpp;
    else { uint32_t mx, my; swizzle_masks(t.w, t.h, &mx, &my); off = (pdep(tx, mx) | pdep(ty, my)) * t.bpp; }
    *argb = texel_argb(t.kind, src + off, pal);
    return 1;
}
void hud_fade(D3DPRIMITIVETYPE pt, UINT minv, UINT nverts, UINT prims, const uint16_t *idx, const uint8_t *verts, UINT stride, UINT count)
{
    hud_map[0] = 480 * wide_aspect / 640; hud_map[1] = 0;   // the whole layer
    hud_map[2] = 1; hud_map[3] = 0;
    hud_draw = 1;
    hud_aop_stage = -1;
    IDirect3DDevice9_SetRenderTarget(dev, 0, hud_surf);
    IDirect3DDevice9_SetDepthStencilSurface(dev, 0);
    rec_target(hud_surf, 0);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    // The fade's colour times its alpha (on a copy of the vertices), weighted by the layer's coverage (DESTALPHA). A
    // textured fade counts when it takes one texel as its colour (one_texel): vertex colour times that texel, the
    // stage set to the vertex colour so it isn't applied twice. ponytail: other textured fades keep the old dimming.
    static uint8_t *pm;   // a failed realloc used to lose it and keep the new size: the next fade wrote through null
    static size_t pm_cap;
    uint32_t texel = 0xFFFFFFFFu;
    int textured = (vertex_shader & D3DFVF_TEXCOUNT_MASK) != 0;
    int colour = (vertex_shader & D3DFVF_DIFFUSE) && (!textured || one_texel(verts, stride, count, &texel))
                 && (pm_cap >= (size_t)count * stride || grow_buf((void **)&pm, &pm_cap, (size_t)count * stride * 2));
    if (colour) {
        uint32_t off = fvf_stride(vertex_shader & (D3DFVF_POSITION_MASK | D3DFVF_NORMAL | D3DFVF_PSIZE));
        memcpy(pm, verts, (size_t)count * stride);
        for (UINT i = 0; i < count; i++) {
            uint32_t c, a;
            memcpy(&c, pm + i * stride + off, 4);
            if (textured) {   // times the texel, channel by channel
                uint32_t m = 0;
                for (int sh = 0; sh < 32; sh += 8) m |= ((c >> sh & 255) * (texel >> sh & 255) / 255) << sh;
                c = m;
            }
            a = c >> 24;
            c = c & 0xFF000000u | ((c >> 16 & 255) * a / 255) << 16 | ((c >> 8 & 255) * a / 255) << 8 | (c & 255) * a / 255;
            memcpy(pm + i * stride + off, &c, 4);
        }
        verts = pm;
    }
    if (colour && textured) {   // the colour is all in the vertices now (rs_valid = 0 in hud_end puts the stage back)
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, colour ? D3DBLEND_DESTALPHA : D3DBLEND_ZERO);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
    dev_draw(pt, minv, nverts, prims, idx, fix_texcoords(verts, stride, count), stride, 0, 0);
    hud_end();
}

void hud_end(void)
{
    hud_draw = 0;
    if (hud_aop_stage >= 0) IDirect3DDevice9_SetTextureStageState(dev, hud_aop_stage, D3DTSS_ALPHAOP, hud_aop);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderTarget(dev, 0, scene_rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, cur_z ? scene_ds : 0);
    rec_target(scene_rt, cur_z ? scene_ds : 0);
    apply_viewport();
    rs_valid = 0;
}

// picture = modern: in-engine scenes draw black bars over the top and bottom of the picture, as one pre-transformed
// quad list drawn over everything (depth test "always"): a quad the full width of the screen, from the top of the art
// (y 16) or down to its bottom (y 464), up to 160 px high. Notes where the picture between them is.
void letterbox_check(const uint8_t *verts, uint32_t stride, const uint16_t *idx, uint32_t n, int32_t base)
{
    if ((vertex_shader & 1) || (vertex_shader & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || cur_rt != &backbuffer_hdr) return;
    if (RS[XRS_ZENABLE] && RS[XRS_ZFUNC] != 0x207) return;
    for (uint32_t q = 0; q + 3 < n; q += 4) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int k = 0; k < 4; k++) {
            const float *p = (const float *)(verts + ((idx ? idx[q + k] + base : (int32_t)(q + k)) * (int32_t)stride));
            if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
        }
        if (x0 > 1 || x1 < 639 || y1 - y0 < 8 || y1 - y0 > 160) continue;
        // Bars reach into the picture: the game also blacks out its border strips (y -8-16 and 464-480) every frame
        // of play, and taking those for bars held the view still all the time.
        if (y0 <= ART_Y0 + 1 && y1 > ART_Y0 + 4) lbox[0] = y1;
        else if (y1 >= ART_Y1 - 1 && y0 < ART_Y1 - 4) lbox[1] = y0;
    }
}

// picture = modern: a still the game shows like a movie, 16:9 and letterboxed in the 4:3 frame (the letter scenes'
// 1024x512 sheets, 640x360 drawn at y 76-436): one textured quad the full width of the screen, not the art's height.
// Notes where it is, so the frame can fill the window with it as a movie does.
void wide_picture_check(const uint8_t *verts, uint32_t stride, const uint16_t *idx, uint32_t n, int32_t base)
{
    if ((vertex_shader & 1) || (vertex_shader & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || cur_rt != &backbuffer_hdr) return;
    const XPix *x = bound_tex[0];
    if (!x || tex_info(x->Format, x->Size).w < 512) return;
    for (uint32_t q = 0; q + 3 < n; q += 4) {
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int k = 0; k < 4; k++) {
            const float *p = (const float *)(verts + ((idx ? idx[q + k] + base : (int32_t)(q + k)) * (int32_t)stride));
            if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0]; if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
        }
        if (x0 <= 1 && x1 >= 639 && y1 - y0 >= 200 && y0 >= ART_Y0 + 8 && y1 <= ART_Y1 - 8) {
            wide_pic[0] = y0; wide_pic[1] = y1;
            // An opaque picture covers the 2D pieces drawn before it, but the HUD layer is laid over the whole frame:
            // the message box the game draws under an in-game movie (links04, room 508) showed as a black box on it.
            if (hud_drawn && hud_surf && !RS[XRS_ALPHABLENDENABLE]) {
                IDirect3DDevice9_SetRenderTarget(dev, 0, hud_surf);
                IDirect3DDevice9_SetDepthStencilSurface(dev, 0);
                rec_target(hud_surf, 0);
                dev_clear(0, 0, D3DCLEAR_TARGET, 0, 1, 0);
                IDirect3DDevice9_SetRenderTarget(dev, 0, scene_rt);
                IDirect3DDevice9_SetDepthStencilSurface(dev, cur_z ? scene_ds : 0);
                rec_target(scene_rt, cur_z ? scene_ds : 0);
                apply_viewport();
                rs_valid = 0;
            }
            return;
        }
        static float last[4];   // debug: a big picture that is not taken, once per place (the letter's first second was 4:3)
        if (x1 - x0 >= 400 && (last[0] != x0 || last[1] != y0 || last[2] != x1 || last[3] != y1)) {
            xlog("[d3d] frame %u: picture %.0f,%.0f-%.0f,%.0f not a wide still", frame_no, x0, y0, x1, y1);
            last[0] = x0, last[1] = y0, last[2] = x1, last[3] = y1;
        }
    }
}

// The letter scenes' subtitles are drawn into the picture, so a wide still enlarged them with it (by 448/336 on 16:9):
// far too big. In a frame that draws a wide still, the textured 2D quads after it that are not full-width bars (the
// text) are drawn smaller by that much, about the middle of the picture: on screen they come out the size they are
// in 4:3, still where the game scrolls them.
static int wide_still(const RECT *cr, float *top);
const uint8_t *still_text(const uint8_t *verts, uint32_t n, uint32_t stride)
{
    static uint8_t out[64 * 32];
    if (wide_pic[1] <= wide_pic[0] || n > 64 || stride > 32 || stats.vs || stats.world) return 0;
    if ((vertex_shader & 1) || (vertex_shader & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || !(vertex_shader & D3DFVF_TEXCOUNT_MASK) || !bound_tex[0]) return 0;
    RECT cr;
    GetClientRect(hwnd, &cr);
    float top;
    if (!wide_still(&cr, &top)) return 0;
    float x0 = 1e9f, x1 = -1e9f;
    for (uint32_t i = 0; i < n; i++) { float v = ((const float *)(verts + i * stride))[0]; if (v < x0) x0 = v; if (v > x1) x1 = v; }
    if (x0 <= 1 && x1 >= 639) return 0;   // the picture itself, or a bar
    float k = (ART_X1 - ART_X0) * cr.bottom / cr.right / (ART_Y1 - ART_Y0), cx = 320, cy = (wide_pic[0] + wide_pic[1]) / 2;
    memcpy(out, verts, n * stride);
    for (uint32_t i = 0; i < n; i++) {
        float *p = (float *)(out + i * stride);
        p[0] = cx + (p[0] - cx) * k, p[1] = cy + (p[1] - cy) * k;
    }
    return out;
}

// "NOW LOADING..." between rooms: on a PC most loads are over in a few frames, and the text flashed by as a flicker.
// It's held back for the first 300 ms of a load, so it shows only on a load that takes a while. It sits at the
// bottom (rows 171-223) of a 128x256 sheet whose top is "Died".
#define LOADING_SHEET_HASH 0xf2ccd7435c0c0b7dull   // content_hash of its texels, as the texture pack names it
int hold_loading_text(const uint8_t *v, uint32_t stride)
{
    static uint32_t sheet, last_frame;
    static DWORD since;
    const XPix *x = bound_tex[0];
    if (!x || !x->Data || x->Size || XFMT_ULOG(x->Format) != 7 || XFMT_VLOG(x->Format) != 8) return 0;
    if (x->Data != sheet) {
        static struct { uint32_t data, frame; } seen[16];   // checked recently and not it
        static int next;
        for (int i = 0; i < 16; i++) if (seen[i].data == x->Data && frame_no - seen[i].frame < 600) return 0;
        if (content_hash(x, 0) != LOADING_SHEET_HASH) { seen[next].data = x->Data; seen[next].frame = frame_no; next = (next + 1) & 15; return 0; }
        sheet = x->Data;
    }
    uint32_t fvf = vertex_shader;
    if ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT < 1) return 0;
    if (((const float *)(v + fvf_stride(fvf & ~D3DFVF_TEXCOUNT_MASK)))[1] < 160.0f / 256) return 0;   // "Died"
    if (frame_no - last_frame > 2) since = GetTickCount();
    last_frame = frame_no;
    return GetTickCount() - since < 300;
}

// HD pack: the save/load screen's orb cluster laid out as the in-game HUD's. The game draws it from its own pieces: a
// frame sprite of the save sheet (d712191e, palette e90936d1, texels 174-236 x 197-245, 62x48 on screen) with the
// blue orb (28 px) and the green orbs (6-10 px, in an arc down the left, off-centre in their sockets) on top, from the
// HUD sheet. When a pack has the HUD sheet's frame (080376b1, palette 3c7b7762), that frame is drawn instead, at its
// in-game size (79x68) where the bars start, and the orbs go into its sockets at their in-game sizes. The pack's file
// holds a copy of the frame without the arm to the Enhance gauge at texels 0-78 x 184-251 (empty in the game's sheet),
// and that copy is the one drawn. A pack without that file changes nothing. Geometry from F12 captures of the load screen and of play.
#define SAVE_SHEET_HASH 0xd712191ef9ac158aull
#define HUD_FRAME_PAL 0x3c7b7762u   // the HUD sheet's palette in the orb frame's colours (the pack's copy for the load screen)
static uint64_t sheet_of(const XPix *x)   // content_hash of a paletted texture, remembered by address
{
    static struct { uint32_t data, frame; uint64_t h; } c[16];
    static int next;
    if (!x || !x->Data || x->Size || XFMT_CODE(x->Format) != 0x0b) return 0;
    for (int i = 0; i < 16; i++) if (c[i].data == x->Data && frame_no - c[i].frame < 600) return c[i].h;
    // ponytail: a different sheet uploaded at the same address within 10 s keeps the old answer
    uint64_t h = content_hash(x, 0);
    c[next].data = x->Data; c[next].frame = frame_no; c[next].h = h; next = (next + 1) & 15;
    return h;
}
static uint32_t stage0_pal_hash(void)   // FNV-1a 32 of stage 0's palette, as texture names carry it
{
    const uint32_t *pal = stage_palette(0);
    return pal ? pack_pal_hash(pal) : 0;
}
static IDirect3DTexture9 *hud_frame_tex(void)   // the pack's HUD sheet in its frame's colours, or 0
{
    static IDirect3DTexture9 *t;
    static int tried;
    if (tried++) return t;
    char sheet[64];
    pack_name(sheet, 0, sizeof sheet, 256, 256, 0x0b, HUD_SHEET_HASH, 1, HUD_FRAME_PAL);
    uint32_t w, h, *px = texpack_load(sheet, &w, &h);
    if (!px) return 0;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(tex_create(w, h, 1, D3DFMT_A8R8G8B8, &t))
        && SUCCEEDED(tex_lock(t, 0, &lr, 0))) {
        for (uint32_t y = 0; y < h; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, px + (size_t)y * w, w * 4);
        tex_unlock(t, 0);
        xlog("[d3d] save screen orbs: the pack's HUD frame (%ux%u)", w, h);
    }
    free(px);
    return t;
}
const uint8_t *orb_layout(const uint8_t *verts, uint32_t n, uint32_t stride, IDirect3DTexture9 **tex)
{
    static uint32_t seen = ~0u;   // frame the save screen's orb frame was drawn in, and where
    static float fx, fy;
    static uint8_t out[64 * 28];
    if ((vertex_shader & 1) || (vertex_shader & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || !(vertex_shader & D3DFVF_TEXCOUNT_MASK)
        || n % 4 || n > 64 || stride > 28 || !texpack_replacing()) return 0;
    uint64_t sh = sheet_of(bound_tex[0]);
    if (sh != SAVE_SHEET_HASH && !(sh == HUD_SHEET_HASH && seen == frame_no)) return 0;
    uint32_t uo = fvf_stride(vertex_shader & ~D3DFVF_TEXCOUNT_MASK), pal = stage0_pal_hash();
    float x0 = 1e9f, y0 = 1e9f, u0 = 1e9f, v0 = 1e9f;
    for (uint32_t i = 0; i < n; i++) {
        const float *p = (const float *)(verts + i * stride), *t = (const float *)(verts + i * stride + uo);
        x0 = fminf(x0, p[0]); y0 = fminf(y0, p[1]); u0 = fminf(u0, t[0] * 256); v0 = fminf(v0, t[1] * 256);
    }
    memcpy(out, verts, n * stride);
    if (sh == SAVE_SHEET_HASH) {   // the frame: the HUD's instead, its arm at the bars (12 px left, 2 up of the old)
        if (pal != 0xe90936d1u || n != 4 || fabsf(u0 - 174) > 0.6f || fabsf(v0 - 197) > 0.6f || !(*tex = hud_frame_tex())) return 0;
        seen = frame_no; fx = x0; fy = y0;
        for (uint32_t i = 0; i < 4; i++) {
            float *p = (float *)(out + i * stride), *t = (float *)(out + i * stride + uo);
            int r = p[0] > x0 + 1, b = p[1] > y0 + 1;
            p[0] = fx - 12 + (r ? 79 : 0); p[1] = fy - 2 + (b ? 68 : 0);
            t[0] = (r ? 77.8f : 0.8f) / 256; t[1] = (b ? 250.8f : 184.8f) / 256;   // inside its edge: no bleed
        }
        return out;
    }
    if (fabsf(u0 - 101.5f) > 0.6f) return 0;   // the orb sprite, in any of its colours (the big one changes with the element)
    static const float old_c[5][2] = {{13, 7}, {8, 16}, {6, 27}, {10, 39}, {17, 45}};      // green orbs' centres, then
    static const float new_o[5][2] = {{12, 7}, {3, 27}, {12, 46}, {31, 54}, {50, 46}};    // their sockets (from the
    float ox = fx - 12, oy = fy - 2;                                                       // new frame's corner)
    for (uint32_t q = 0; q < n; q += 4) {
        float qx0 = 1e9f, qy0 = 1e9f, qx1 = -1e9f, qy1 = -1e9f;
        for (int k = 0; k < 4; k++) {
            const float *p = (const float *)(out + (q + k) * stride);
            qx0 = fminf(qx0, p[0]); qy0 = fminf(qy0, p[1]); qx1 = fmaxf(qx1, p[0]); qy1 = fmaxf(qy1, p[1]);
        }
        if (qx0 < fx - 2 || qy0 < fy - 2 || qx1 > fx + 64 || qy1 > fy + 50) continue;   // not in the orb frame
        float nx, ny, sz;
        if (qx1 - qx0 > 20) { nx = ox + 24; ny = oy + 19; sz = 24; }                    // the blue orb
        else {
            float cx = (qx0 + qx1) / 2 - fx, cy = (qy0 + qy1) / 2 - fy, best = 1e9f;
            int s = 0;
            for (int j = 0; j < 5; j++) { float d = hypotf(cx - old_c[j][0], cy - old_c[j][1]); if (d < best) best = d, s = j; }
            nx = ox + new_o[s][0]; ny = oy + new_o[s][1]; sz = 9;
        }
        for (int k = 0; k < 4; k++) {
            float *p = (float *)(out + (q + k) * stride);
            p[0] = nx + (p[0] > qx0 + 0.5f ? sz : 0); p[1] = ny + (p[1] > qy0 + 0.5f ? sz : 0);
        }
    }
    return out;
}

// The title's "BUTTON" (title menu sheet 39cf6d57, texel rows 32-64) takes in the sheet's row 63, the top edge of the
// box below it: a line under the word, in the game's own drawing too, and plain at any scale above 1x. Its last row
// is left out (and the quad a pixel shorter, so nothing is stretched).
#define TITLE_SHEET_HASH 0x39cf6d57f5b6c821ull
const uint8_t *title_trim(const uint8_t *verts, uint32_t n, uint32_t stride)
{
    static uint8_t out[64 * 28];
    if ((vertex_shader & 1) || (vertex_shader & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW || !(vertex_shader & D3DFVF_TEXCOUNT_MASK)
        || n % 4 || n > 64 || stride > 28 || sheet_of(bound_tex[0]) != TITLE_SHEET_HASH) return 0;
    uint32_t uo = fvf_stride(vertex_shader & ~D3DFVF_TEXCOUNT_MASK), hit = 0;
    memcpy(out, verts, n * stride);
    for (uint32_t q = 0; q < n; q += 4) {
        float v0 = 1e9f, v1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (uint32_t i = q; i < q + 4; i++) {
            float v = ((const float *)(out + i * stride + uo))[1] * 512, y = ((const float *)(out + i * stride))[1];
            v0 = fminf(v0, v); v1 = fmaxf(v1, v); y0 = fminf(y0, y); y1 = fmaxf(y1, y);
        }
        if (fabsf(v0 - 32) > 0.1f || fabsf(v1 - 64) > 0.1f) continue;
        for (uint32_t i = q; i < q + 4; i++) {
            float *p = (float *)(out + i * stride), *t = (float *)(out + i * stride + uo);
            if (t[1] * 512 > 63.9f) { t[1] = 63.0f / 512; p[1] -= (y1 - y0) / 32; }
        }
        hit = 1;
    }
    return hit ? out : 0;
}

// picture = modern: at the end of each Present, whether the next frame is drawn wide, and where the slice sits:
// in play, Samanosuke's feet about three quarters of the way down it, eased towards while the camera holds still, at
// once when it moves or cuts.
// Whether the frame just drawn is black: nothing in it brighter than 4/255, read back from the scene shrunk to 32 x 24.
// (A dark room fading out reads 8 on average while still visible.)
IDirect3DSurface9 *dark_tiny;   // a render target: reset_device drops it
static int frame_dark(void)
{
    static IDirect3DSurface9 *sys;
    if (!dark_tiny && FAILED(IDirect3DDevice9_CreateRenderTarget(dev, 32, 24, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &dark_tiny, 0))) dark_tiny = 0;
    if (!sys && FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 32, 24, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, 0))) sys = 0;
    if (!dark_tiny || !sys) return 1;
    IDirect3DSurface9 *frame = 0;
    IDirect3DTexture9_GetSurfaceLevel(frame_tex, 0, &frame);
    IDirect3DDevice9_StretchRect(dev, scene_rt, 0, frame, 0, D3DTEXF_NONE);   // resolves MSAA
    IDirect3DDevice9_StretchRect(dev, frame, 0, dark_tiny, 0, D3DTEXF_LINEAR);
    IDirect3DSurface9_Release(frame);
    D3DLOCKED_RECT lr;
    int top = 0;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, dark_tiny, sys)) || FAILED(IDirect3DSurface9_LockRect(sys, &lr, 0, D3DLOCK_READONLY))) return 1;
    for (int y = 0; y < 24; y++)
        for (int x = 0; x < 32; x++) {
            uint32_t c = ((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[x];
            int m = (int)(c & 0xff), g = (int)(c >> 8 & 0xff), b = (int)(c >> 16 & 0xff);
            if (g > m) m = g; if (b > m) m = b;
            if (m > top) top = m;
        }
    IDirect3DSurface9_UnlockRect(sys);
    return top <= 4;
}

// A 16:9 still (wide_picture_check) in the frame just drawn, with no 3D in it: whether it fills this window, and if so
// the slice's top, framed on the picture. (Its height scaled as a movie's: of its 640 px width, the art's 597 show.)
static int wide_still(const RECT *cr, float *top)
{
    float pic0 = wide_pic[0], pic1 = wide_pic[1];
    if (settings.picture != PICTURE_MODERN || stats.vs || stats.world || pic1 <= pic0 || cr->bottom <= 0 || cr->right * 3 <= cr->bottom * 4
        || (pic1 - pic0) * (ART_X1 - ART_X0) / 640 > (ART_X1 - ART_X0) * cr->bottom / cr->right + 1) return 0;
    float sh = (ART_X1 - ART_X0) * cr->bottom / cr->right, t = (pic0 + pic1 - sh) / 2;
    *top = t < ART_Y0 ? ART_Y0 : t > ART_Y1 - sh ? ART_Y1 - sh : t;
    return 1;
}
static int still_only;   // the frame being drawn is wide only for a still (wide_next_frame)

// Decided when the frame is shown, not a frame ahead: the first frame of a still showed 4:3 before the view widened,
// and the first frame after one showed wide.
void wide_still_now(void)
{
    RECT cr;
    float top;
    GetClientRect(hwnd, &cr);
    if (wide_still(&cr, &top)) {
        if (!wide) still_only = 1;
        wide = 1, wide_aspect = (float)cr.right / cr.bottom, pan_y = top;
    } else if (still_only) {   // the still is gone: back to 4:3, unless 3D was drawn (laid out wide; it stays wide)
        if (!(stats.vs || stats.world)) wide = 0, pan_y = -1;
        still_only = 0;
    }
}

void wide_next_frame(void)
{
    {   // the game's mode word (G_STATE) as it changes: which screen is up, for picture = modern's rules
        static uint32_t last = ~0u;
        uint32_t m = *(volatile uint32_t *)G_STATE;
        if (m != last) xlog("[d3d] frame %u: mode %08x", frame_no, m), last = m;
    }
    RECT cr;
    GetClientRect(hwnd, &cr);
    int hud = hud_drawn;
    hud_drawn = 0;
    // Wide while the game draws its 3D world (play, the pause menu over it, in-engine scenes); not the title screen
    // or the full-screen menus (equip, map, files), which are 2D only. In play, up to two frames without 3D
    // (a cut) stay wide. (Going by "in play" alone kept the equip screen wide for its first frames, laid out wrong.)
    // Opening a menu, the game fades out a still of the 3D picture, 2D only: that stays wide until the screen is dark
    // (or a second has passed, or a movie starts), so the picture doesn't jump to the 4:3 menu framing mid-fade.
    // A frame in play that draws the HUD (or a message box) but no 3D, as the close-up of a Magic Mirror with its
    // message, stays wide too: it went 4:3 a second in. A room's painted background in play counts as 3D (its depth
    // is uploaded with it): a close-up with no 3D in it, as of a locked door, went 4:3 until its message box came up.
    // A full-screen 2D screen (screen2d_names: the decode puzzle, Options) is never wide, whatever is drawn under it.
    static int flat, hold;
    flat = stats.vs || stats.world || (stats.depth && *(volatile uint8_t *)G_STATE == 2) ? 0 : flat + 1;
    screen2d_on = screen2d_frame == frame_no;
    uint8_t gm = *(volatile uint8_t *)G_STATE;
    fullpic_on = fullpic_frame == frame_no && !(stats.vs || stats.world || stats.depth) && (gm == 2 || gm == 0x0c);
    if (screen2d_on) hold = 0;
    int w3d = !screen2d_on && (!flat || (view_playing && flat <= 2) || (hold && hud && (*(volatile uint8_t *)G_STATE) == 2));
    if (w3d) hold = 1;
    else if (hold && (flat > 60 || frame_no - movie_seen < 2 || settings.picture != PICTURE_MODERN || frame_dark())) hold = 0;
    // A movie whose own picture is wider than 4:3 (most are 16:9, letterboxed in the 4:3 frame) fills the window's
    // width when that doesn't cut it: the slice, centred, is the picture's height or more. Taller ones (links01,
    // linksu06, links0c) stayed 4:3; trial (2026-10-24): they fill the width too, centred, the top and bottom cut.
    // ONI_MOVIE_43=1 keeps them 4:3.
    static int keep43 = -1;
    if (keep43 < 0) keep43 = env_int("ONI_MOVIE_43", 0) == 1;
    int mv = frame_no - movie_seen < 2 && movie_w && cr.bottom > 0
             && (!keep43 || movie_h * (ART_X1 - ART_X0) / movie_w <= (ART_X1 - ART_X0) * cr.bottom / cr.right + 1);
    // So does a 16:9 still drawn like one (wide_still), framed on the picture.
    float top;
    int ps = !w3d && !mv && wide_still(&cr, &top);
    wide_pic[0] = wide_pic[1] = 0;
    static int ps_was;
    if (ps != ps_was) xlog("[d3d] frame %u: wide still %s", frame_no, ps ? "on" : "off"), ps_was = ps;
    float shot_v;
    wide = settings.picture == PICTURE_MODERN && (w3d || hold || mv || ps) && cr.bottom > 0 && cr.right * 3 > cr.bottom * 4
        && !(w3d && wide_shot(&shot_v) == SHOT_WHOLE);
    still_only = wide && ps && !w3d && !hold && !mv;
    if (!wide || (mv && !w3d)) pan_y = -1;   // a movie: centred
    if (!wide) return;
    wide_aspect = (float)cr.right / cr.bottom;
    if (ps) pan_y = top;
}

// picture = modern: for the frame just drawn, with its own camera, as the 2019 remaster pans (read from its code; see
// docs/widescreen.md, "The remaster's pan rule"). Once per game step his feet's height in the picture (+1 top, -1
// bottom) is smoothed 10% of the way, or taken at once at a camera cut. While that is between the middle and three
// quarters down, the slice stays centred; above it the slice follows him 1:1, below it twice as fast, reaching the
// picture's top or bottom edge as he does. Shots the remaster frames by hand (wide_shots) follow those instead.
void wide_pan(void)
{
    float feet = 0, sh = (ART_X1 - ART_X0) / wide_aspect, lo = ART_Y0, hi = ART_Y1 - sh;
    int cut = 0;
    view_playing = game_view(&feet, &cut);
    // An in-engine scene (letterbox bars drawn): the slice holds still on the middle of the picture between the bars,
    // as the remaster frames its scenes, instead of following him and sliding the bars into view. Bars missing for a
    // frame or two (at a cut) don't end it. It eases in, or moves at once at a camera cut.
    static float bars[2];
    static int bars_gap = 99;
    if (lbox[0] > 0 && lbox[1] > lbox[0]) { bars[0] = lbox[0]; bars[1] = lbox[1]; bars_gap = 0; } else bars_gap++;
    lbox[0] = lbox[1] = 0;
    // No 3D this frame (a cut's gap, the still the game fades out when a menu opens): the slice stays. But a painted
    // close-up in play with nothing 3D in it (a locked door's lock, before the key goes in), held 3 frames, is framed
    // on its own picture, centred: it kept the room view's framing, then slid once the key (3D) appeared in it.
    static int still_view;
    int closeup = wide && !(stats.vs || stats.world) && stats.depth && *(volatile uint8_t *)G_STATE == 2;
    still_view = closeup ? still_view + 1 : 0;
    if (still_view == 3) pan_y = (lo + hi) / 2;
    if (!wide || !(stats.vs || stats.world)) return;
    if (bars_gap <= 2) {
        float t = (bars[0] + bars[1] - sh) / 2;
        t = t < lo ? lo : t > hi ? hi : t;
        pan_y = pan_y < 0 || cut || fabsf(t - pan_y) < 0.5f ? t : pan_y + (t - pan_y) * 0.15f;
        return;
    }
    float v = 0, dead = -0.5f;   // dead: bottom of the dead zone, whose top is the middle
    int shot = wide_shot(&v), held = shot == SHOT_HOLD || shot == SHOT_HOLD_SNAP;
    // Paused or a scene: the slice stays. So it does while he is behind the camera, unless the shot is held anyway.
    if (!view_playing || (isnan(feet) && !held)) { if (pan_y < 0) pan_y = (lo + hi) / 2; return; }
    float half = (ART_Y1 - ART_Y0) / 2, live = held ? v : ((ART_Y0 + ART_Y1) / 2 - feet) / half;
    if (shot == SHOT_RAISE) live += v;
    if (shot == SHOT_STRETCH) live = (live + 1) / 0.7f * 2 - 1;
    if (shot == SHOT_STRETCH || shot == SHOT_NO_DEAD) dead = 0;
    if (shot == SHOT_HOLD_SNAP) cut = 1;
    // A camera cut snaps the height, in the cut's own frame. Any other jump counts only once it holds for a second
    // frame (then as a cut): single frames read him somewhere else entirely (seen in room 101), and following those
    // flashed the other end of the picture.
    static float height, last = NAN, jumped = NAN;
    if (pan_y < 0 || isnan(last)) cut = 1;
    else if (!cut && fabsf(live - last) * half > 40) {
        if (isnan(jumped) || fabsf(live - jumped) * half > 40) { jumped = live; return; }
        cut = 1;
    }
    jumped = NAN; last = live;
    height = cut ? live : height * 0.9f + live * 0.1f;
    height = height < -1 ? -1 : height > 1 ? 1 : height;
    float pan = height > 0 ? height : height < dead ? (height - dead) / (1 + dead) : 0;   // +1: the slice at the top
    pan_y = (lo + hi) / 2 - pan * (hi - lo) / 2;
}

