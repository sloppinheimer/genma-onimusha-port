// Frame interpolation (settings: frame_interpolation), split out of d3d.c: each game step's draws are recorded,
// the real frame is held, and in-betweens are replayed with character and camera movement blended.
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

// ---- frame interpolation (settings: frame_interpolation) ---------------------------------------------
// The game advances one fixed step per frame at 59.94 Hz (see wait_vblank). To show twice as many frames, each
// frame's drawing is recorded (device state, vertex and index data, vertex-shader constants) and the real frame is
// held back one slot: through the slot the recording is replayed settings.interp - 1 times, each with every
// vertex-shader constant (camera, object and bone matrices) blended that far from the matching draw of the previous
// frame, and shown; the real frame follows at the end of the slot. Costs one frame (~17 ms) of extra latency. Draws
// without constants (2D layers, effects the game builds on the CPU) replay as they are in the newer frame.
enum { OP_DRAW, OP_CLEAR, OP_TARGET, OP_COPY };
struct op {
    int kind;
    IDirect3DStateBlock9 *sb;                          // OP_DRAW: all device state except the render target
    D3DPRIMITIVETYPE pt;
    UINT minv, nverts, prims, stride, indexed;
    size_t verts, idx, consts;                         // offsets into the frame's data; consts == NO_CONSTS for none
    uint32_t key[4];                                   // draws match across frames by key and occurrence
    DWORD count, flags, color, stencil;                // OP_CLEAR
    float z;
    D3DVIEWPORT9 vp;
    size_t rects;
    IDirect3DSurface9 *rt, *ds;                        // OP_TARGET
    IDirect3DSurface9 *src, *dst;                      // OP_COPY
    RECT sr, dr;
    int whole;
    int volume;                                        // OP_DRAW: a stencil shadow volume (see apply_render_state)
};
#define NO_CONSTS ((size_t)-1)
struct frame_rec frames[3];                    // ring: being recorded, last finished, the one before
int rec_cur, rec_done = -1, rec_prev = -1;
int recording;                                  // settings.frame_interpolation or the wheel open, and not ONI_FAST
int draw_is_volume;                             // the draw being made is a shadow volume (apply_render_state)
uint32_t rec_serial = 1;                        // counts recorded frames (texture rewrite detection)
LONGLONG prof[4];   // debug timing: capture, replay, present, hold (QPC ticks)
// Stall breakdown: where each frame's time went, in QPC ticks, summed between Presents (the stall line splits a stall
// into these): our Present, our drawing, texture uploads, in-between frames, the frame-pacing wait.
LONGLONG st_present, st_draw, st_tex, st_inb, st_wait;
static IDirect3DStateBlock9 *sb_pool[2048];
int sb_used;

// Grows a buffer to n bytes: 0 (and the old one kept, *cap unchanged) when there's no memory for it.
int grow_buf(void **p, size_t *cap, size_t n)
{
    void *q = realloc(*p, n);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}
static size_t rec_put(const void *p, size_t n)
{
    struct frame_rec *f = &frames[rec_cur];
    size_t at = f->used, need = at + ((n + 15) & ~(size_t)15);
    // Out of memory (a 32-bit process near its limit): the frame is marked broken, so it's never replayed, rather
    // than written through a null pointer on the game thread.
    if (need > f->size) {
        uint8_t *d = realloc(f->data, need * 2);
        if (!d) { f->broken = 1; f->used = 0; return 0; }
        f->data = d; f->size = need * 2;
    }
    memcpy(f->data + at, p, n);
    f->used = need;
    return at;
}

static struct op *rec_op(int kind)
{
    struct frame_rec *f = &frames[rec_cur];
    static struct op spare;   // out of memory: the op goes nowhere and the frame isn't replayed (as rec_put)
    struct op *o = &spare;
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 256;
        struct op *ops = realloc(f->ops, cap * sizeof *ops);
        if (ops) f->ops = ops, f->cap = cap;
        else f->broken = 1;
    }
    if (f->n < f->cap) o = &f->ops[f->n++];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    o->consts = NO_CONSTS;
    return o;
}

static UINT verts_for(D3DPRIMITIVETYPE pt, UINT prims)
{
    switch (pt) {
    case D3DPT_POINTLIST: return prims;
    case D3DPT_LINELIST: return prims * 2;
    case D3DPT_LINESTRIP: return prims + 1;
    case D3DPT_TRIANGLELIST: return prims * 3;
    default: return prims + 2;   // strips and fans
    }
}

// Every game draw goes through here. consts: the 192 vertex-shader constants it was drawn with (0 for none).
void dev_draw(D3DPRIMITIVETYPE pt, UINT minv, UINT nverts, UINT prims, const uint16_t *idx, const void *verts, UINT stride,
                     const float *consts, const uint32_t key[4])
{
    if (idx) IDirect3DDevice9_DrawIndexedPrimitiveUP(dev, pt, minv, nverts, prims, idx, D3DFMT_INDEX16, verts, stride);
    else IDirect3DDevice9_DrawPrimitiveUP(dev, pt, prims, verts, stride);
    if (!recording) return;
    if (sb_used == (int)(sizeof sb_pool / sizeof *sb_pool)) { frames[rec_cur].broken = 1; return; }
    LONGLONG t0 = qpc();
    if (sb_pool[sb_used]) IDirect3DStateBlock9_Capture(sb_pool[sb_used]);
    else if (FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &sb_pool[sb_used]))) { sb_pool[sb_used] = 0; frames[rec_cur].broken = 1; return; }
    prof[0] += qpc() - t0;
    struct op *o = rec_op(OP_DRAW);
    o->sb = sb_pool[sb_used++];
    o->pt = pt; o->minv = minv; o->nverts = nverts; o->prims = prims; o->stride = stride; o->indexed = idx != 0;
    o->verts = rec_put(verts, (idx ? minv + nverts : verts_for(pt, prims)) * stride);
    if (idx) o->idx = rec_put(idx, verts_for(pt, prims) * 2);
    if (consts) o->consts = rec_put(consts, 192 * 16);
    if (key) memcpy(o->key, key, sizeof o->key);
    o->volume = draw_is_volume;
}

void dev_clear(DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    IDirect3DDevice9_Clear(dev, count, rects, flags, color, z, stencil);
    if (!recording) return;
    struct op *o = rec_op(OP_CLEAR);
    o->count = count; o->flags = flags; o->color = color; o->z = z; o->stencil = stencil;
    IDirect3DDevice9_GetViewport(dev, &o->vp);   // a clear without rects clears the viewport
    if (count) o->rects = rec_put(rects, count * sizeof *rects);
}

void rec_target(IDirect3DSurface9 *rt, IDirect3DSurface9 *ds)
{
    if (!recording) return;
    struct op *o = rec_op(OP_TARGET);
    o->rt = rt; o->ds = ds;
}

void dev_copy(IDirect3DSurface9 *src, const RECT *sr, IDirect3DSurface9 *dst, const RECT *dr)
{
    IDirect3DDevice9_StretchRect(dev, src, sr, dst, dr, D3DTEXF_NONE);
    if (!recording) return;
    struct op *o = rec_op(OP_COPY);
    o->src = src; o->dst = dst; o->whole = !sr;
    if (sr) { o->sr = *sr; o->dr = *dr; }
}

// ---- frame interpolation: holding the real frame and replaying in-betweens ----------------------------
static IDirect3DTexture9 *held_tex;       // the real frame, shown at the end of its slot
static IDirect3DStateBlock9 *end_sb;      // device state at Present, restored after a replay
static IDirect3DSurface9 *end_rt, *end_ds;
static IDirect3DSurface9 *copy_backup[4];   // the game's copy targets, kept from an in-between's replay (show_inbetween)

// Present with interpolation on: finish this frame's recording and hold the picture back.
void hold_frame(void)
{
    // Movies (Bink) present without waiting for the vertical blank, so nothing would show the held frame: show it now.
    if (held) show_held();
    IDirect3DSurface9 *f = resolve_frame(), *h = 0;
    D3DSURFACE_DESC d;
    IDirect3DSurface9_GetDesc(f, &d);
    if (!held_tex && FAILED(IDirect3DDevice9_CreateTexture(dev, d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
                                                           &held_tex, 0))) held_tex = 0;
    held_wide = wide; held_hud = frame_hud;
    if (held_tex) {
        IDirect3DTexture9_GetSurfaceLevel(held_tex, 0, &h);
        IDirect3DDevice9_StretchRect(dev, f, 0, h, 0, D3DTEXF_NONE);
        IDirect3DSurface9_Release(h);
        held = 1;
    } else { pres_wide = wide; pres_hud = frame_hud; present_surface(f); }
    IDirect3DSurface9_Release(f);
    if (end_sb) IDirect3DStateBlock9_Capture(end_sb);
    else if (FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &end_sb))) end_sb = 0;
    if (end_rt) IDirect3DSurface9_Release(end_rt);
    if (end_ds) IDirect3DSurface9_Release(end_ds);
    end_rt = end_ds = 0;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &end_rt);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &end_ds);
    rec_prev = rec_done; rec_done = rec_cur; rec_cur = (rec_cur + 1) % 3;
    struct frame_rec *n = &frames[rec_cur];
    n->n = 0; n->used = 0; n->broken = 0; n->depth_hash = 0;
    sb_used = 0;
    rec_serial++;
}

// Device reset: recordings point at released surfaces, and state blocks and held_tex must go before Reset.
// Interpolation starts again with the next frame.
void interp_drop(void)
{
    held = 0;
    rec_done = rec_prev = -1;
    frames[rec_cur].broken = 1;
    for (int i = 0; i < (int)(sizeof sb_pool / sizeof *sb_pool); i++)
        if (sb_pool[i]) { IDirect3DStateBlock9_Release(sb_pool[i]); sb_pool[i] = 0; }
    sb_used = 0;
    if (end_sb) { IDirect3DStateBlock9_Release(end_sb); end_sb = 0; }
    if (end_rt) { IDirect3DSurface9_Release(end_rt); end_rt = 0; }
    if (end_ds) { IDirect3DSurface9_Release(end_ds); end_ds = 0; }
    if (held_tex) { IDirect3DTexture9_Release(held_tex); held_tex = 0; }
    for (int k = 0; k < 4; k++) if (copy_backup[k]) { IDirect3DSurface9_Release(copy_backup[k]); copy_backup[k] = 0; }
    if (hud_surf) { IDirect3DSurface9_Release(hud_surf); hud_surf = 0; }
    if (hud_tex) { IDirect3DTexture9_Release(hud_tex); hud_tex = 0; }
    hud_drawn = 0;
}

void show_held(void)
{
    if (!held) return;
    held = 0;
    IDirect3DSurface9 *h = 0;
    IDirect3DTexture9_GetSurfaceLevel(held_tex, 0, &h);
    pres_wide = held_wide; pres_hud = held_hud;
    if (!overlays_late(pres_wide)) draw_wheel(h), draw_subtitle(h), draw_popup(h);   // now, not when the frame was rendered: the in-betweens before it already show a later wheel
    present_surface(h);
    IDirect3DSurface9_Release(h);
}

static const struct op *sort_ops;   // the frame by_key sorts draws of
static int by_key(const void *x, const void *y)
{
    int i = *(const int *)x, j = *(const int *)y, c = memcmp(sort_ops[i].key, sort_ops[j].key, sizeof sort_ops->key);
    return c ? c : i - j;
}

// Replays the last finished frame with its vertex-shader constants blended from the frame before (t = 0: the older
// frame, 1: the newer one) and shows it. Returns 0 (nothing drawn) at a camera cut, when too few draws match, or when
// the frame can't be replayed (a texture was rewritten while in use, as menus do with text).
int show_inbetween(float t)
{
    inb_skipped++;   // undone below once shown
    if (rec_done < 0 || rec_prev < 0 || !end_sb) return 0;   // end_sb: no way to put the device back after a replay
    struct frame_rec *f = &frames[rec_done], *p = &frames[rec_prev];
    if (f->broken || p->broken || f->depth_hash != p->depth_hash) return 0;
    static int *match, *fk, *pk;
    static int match_cap;
    int cap = f->n > p->n ? f->n : p->n;
    if (match_cap < cap) {   // out of memory: no in-between this time
        int c2 = cap * 2, ok = 1;
        int *a = realloc(match, c2 * sizeof *match); if (a) match = a; else ok = 0;
        a = realloc(fk, c2 * sizeof *fk); if (a) fk = a; else ok = 0;
        a = realloc(pk, c2 * sizeof *pk); if (a) pk = a; else ok = 0;
        if (!ok) return 0;
        match_cap = c2;
    }
    // The k-th draw with a key matches the k-th draw with that key in the older frame: both frames' keyed draws sorted
    // by key, then order, and walked side by side.
    int with = 0, matched = 0, np = 0;
    for (int i = 0; i < f->n; i++) { match[i] = -1; if (f->ops[i].kind == OP_DRAW && f->ops[i].consts != NO_CONSTS) fk[with++] = i; }
    for (int j = 0; j < p->n; j++) if (p->ops[j].kind == OP_DRAW && p->ops[j].consts != NO_CONSTS) pk[np++] = j;
    sort_ops = f->ops; qsort(fk, with, sizeof *fk, by_key);
    sort_ops = p->ops; qsort(pk, np, sizeof *pk, by_key);
    for (int a = 0, b = 0; a < with && b < np;) {
        int c = memcmp(f->ops[fk[a]].key, p->ops[pk[b]].key, sizeof f->ops->key);
        if (c < 0) a++;
        else if (c > 0) b++;
        else { match[fk[a++]] = pk[b++]; matched++; }
    }
    if (!with || matched * 10 < with * 7) return 0;   // nothing 3D to blend, or under 70% matched: a different scene
    // The replay copies into the game's own textures as the real frame did (the copy of the finished frame it draws
    // back squeezed). Put them back afterwards: the frame before a camera cut only redraws that copy, and showed the
    // last in-between instead of its own frame, a jump of a couple of pixels.
    IDirect3DSurface9 *dsts[4];
    int nd = 0;
    for (int i = 0; i < f->n; i++) {
        if (f->ops[i].kind != OP_COPY) continue;
        int seen = 0;
        for (int k = 0; k < nd; k++) seen |= dsts[k] == f->ops[i].dst;
        if (seen) continue;
        if (nd < 4) dsts[nd++] = f->ops[i].dst;
        else { static int warned; if (!warned++) xlog("[d3d] in-between: more than 4 copy targets in a frame, the rest not put back"); }
    }
    for (int k = 0; k < nd; k++) {
        D3DSURFACE_DESC d, b;
        IDirect3DSurface9_GetDesc(dsts[k], &d);
        if (copy_backup[k]) IDirect3DSurface9_GetDesc(copy_backup[k], &b);
        if (copy_backup[k] && (b.Width != d.Width || b.Height != d.Height || b.Format != d.Format)) { IDirect3DSurface9_Release(copy_backup[k]); copy_backup[k] = 0; }
        if (!copy_backup[k]) IDirect3DDevice9_CreateRenderTarget(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &copy_backup[k], 0);
        if (copy_backup[k]) IDirect3DDevice9_StretchRect(dev, dsts[k], 0, copy_backup[k], 0, D3DTEXF_NONE);
    }
    float c[192][4];
    LONGLONG t0 = qpc();
    for (int i = 0; i < f->n; i++) {
        struct op *o = &f->ops[i];
        switch (o->kind) {
        case OP_DRAW: {
            IDirect3DStateBlock9_Apply(o->sb);
            if (match[i] >= 0) {
                const float *a = (const float *)(p->data + p->ops[match[i]].consts), *b = (const float *)(f->data + o->consts);
                for (int k = 0; k < 192 * 4; k++) (&c[0][0])[k] = a[k] + (b[k] - a[k]) * t;
                IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, &c[0][0], 192);
            }
            const uint8_t *v = f->data + o->verts;
            // The game builds shadow volumes on the CPU in world space, so the blended constants only move the camera:
            // the volume would keep the newer frame's pose while the bodies it shadows are drawn in between, and the
            // shadow edges would jump once per game frame (on the characters themselves most of all). When both frames
            // built the volume the same way (same strip, same vertex count), blend its vertices too.
            // ponytail: every dword is blended as a float, right for position-only vertices; a packed colour would not be.
            const struct op *po = match[i] >= 0 ? &p->ops[match[i]] : 0;
            if (o->volume && po && po->volume && po->nverts == o->nverts && po->minv == o->minv && po->stride == o->stride
                && po->indexed == o->indexed && po->prims == o->prims && (!o->indexed || !memcmp(p->data + po->idx, f->data + o->idx, verts_for(o->pt, o->prims) * 2))) {
                size_t nf = (size_t)(o->indexed ? o->minv + o->nverts : verts_for(o->pt, o->prims)) * o->stride / 4;
                static float *lv;
                static size_t lv_cap;
                if (lv_cap < nf) { float *q = realloc(lv, nf * 2 * sizeof *lv); if (q) lv = q, lv_cap = nf * 2; }   // cap in floats
                if (lv_cap >= nf) {   // else (out of memory) this draw is shown as it is, not blended
                    const float *a = (const float *)(p->data + po->verts), *b = (const float *)v;
                    for (size_t k = 0; k < nf; k++) lv[k] = a[k] + (b[k] - a[k]) * t;
                    v = (const uint8_t *)lv;
                }
            }
            if (o->indexed)
                IDirect3DDevice9_DrawIndexedPrimitiveUP(dev, o->pt, o->minv, o->nverts, o->prims, f->data + o->idx, D3DFMT_INDEX16, v, o->stride);
            else
                IDirect3DDevice9_DrawPrimitiveUP(dev, o->pt, o->prims, v, o->stride);
            break;
        }
        case OP_CLEAR:
            IDirect3DDevice9_SetViewport(dev, &o->vp);
            IDirect3DDevice9_Clear(dev, o->count, o->count ? (const D3DRECT *)(f->data + o->rects) : 0, o->flags, o->color, o->z, o->stencil);
            break;
        case OP_TARGET:
            IDirect3DDevice9_SetRenderTarget(dev, 0, o->rt);
            IDirect3DDevice9_SetDepthStencilSurface(dev, o->ds);
            break;
        case OP_COPY:
            IDirect3DDevice9_StretchRect(dev, o->src, o->whole ? 0 : &o->sr, o->dst, o->whole ? 0 : &o->dr, D3DTEXF_NONE);
            break;
        }
    }
    prof[1] += qpc() - t0;
    pres_wide = held_wide; pres_hud = held_hud;
    if (!overlays_late(pres_wide)) draw_wheel(scene_rt), draw_subtitle(scene_rt), draw_popup(scene_rt);   // mod overlay: drawn on top of every shown frame, not part of the recording
    if (mid_shot) {
        char file[32], name[MAX_PATH + 64];
        snprintf(file, sizeof file, "mid_%u.bmp", mid_shot);
        capture_path(name, sizeof name, "screenshots", file);
        unique_path(name, sizeof name);
        save_frame(name);
        mid_shot = 0;
    }
    t0 = qpc();
    present_frame();
    prof[2] += qpc() - t0;
    for (int k = 0; k < nd; k++) if (copy_backup[k]) IDirect3DDevice9_StretchRect(dev, copy_backup[k], 0, dsts[k], 0, D3DTEXF_NONE);
    inb_skipped--; inb_shown++;
    // Put the device back exactly as the game left it at Present.
    IDirect3DDevice9_SetRenderTarget(dev, 0, end_rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, end_ds);
    IDirect3DStateBlock9_Apply(end_sb);
    rs_valid = 0;
    return 1;
}

// ---- F10: compare shot ---------------------------------------------------------------------------------
// One frame, as the HD pack and the port's options draw it and as the original game would: F10 has the next frame
// drawn 4:3 (picture = modern's HUD layer and crop off for it, so both halves frame the same) and recorded; after it is
// held for showing, its recording is replayed with every pack texture swapped for the game's own (uploaded again from
// Xbox memory) and every upscaled copy (hud_filter = xbr, enhanced_text, background_filter = enhanced) for the texture
// it was made from, then point-sampled down to 640x480. Same frame, so enemies, effects and the camera match. Saved in
// captures\compare: <frame>_hd.png (render resolution), <frame>_original.png (640x480), <frame>_side.png (the two
// side by side, the original stretched to match, at most 1440 high, each captioned). The shown frame isn't touched; the one 4:3 frame
// flashes on screen in picture = modern. Debug: ONI_COMPARE=<frame> takes it from a script.
int cmp_state;   // 1: F10 pressed, the next frame is the compare frame; 2: it is being drawn
static uint32_t *read_rt(IDirect3DSurface9 *s, UINT w, UINT h)   // a render target (not multisampled) as BGRA, opaque
{
    IDirect3DSurface9 *sys = 0;
    D3DLOCKED_RECT lr;
    uint32_t *px = 0;
    if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, 0))
        && SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, s, sys)) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, 0, D3DLOCK_READONLY))) {
        px = malloc((size_t)w * h * 4);
        for (UINT y = 0; px && y < h; y++) memcpy(px + (size_t)y * w, (uint8_t *)lr.pBits + y * lr.Pitch, w * 4);
        for (size_t i = 0; px && i < (size_t)w * h; i++) px[i] |= 0xFF000000u;
        IDirect3DSurface9_UnlockRect(sys);
    }
    if (sys) IDirect3DSurface9_Release(sys);
    return px;
}
static uint32_t *scaled_rt(IDirect3DSurface9 *src, UINT w, UINT h, D3DTEXTUREFILTERTYPE f)   // src resized, read back
{
    IDirect3DSurface9 *rt = 0;
    uint32_t *px = 0;
    if (SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(dev, w, h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, 0))
        && SUCCEEDED(IDirect3DDevice9_StretchRect(dev, src, 0, rt, 0, f))) px = read_rt(rt, w, h);
    if (rt) IDirect3DSurface9_Release(rt);
    return px;
}
// The side-by-side's captions, bottom left of each half: white with a black outline, drawn with GDI into a copy.
static void cmp_label(uint32_t *px, UINT w, UINT h, UINT x0, const wchar_t *text)
{
    BITMAPINFO bi = {{sizeof bi.bmiHeader, (LONG)w, -(LONG)h, 1, 32}};
    void *bits = 0;
    HDC dc = CreateCompatibleDC(0);
    HBITMAP bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, 0, 0) : 0;
    if (!bm) { if (dc) DeleteDC(dc); return; }
    memcpy(bits, px, (size_t)w * h * 4);
    HGDIOBJ old_bm = SelectObject(dc, bm);
    int size = (int)h / 26;
    HFONT f = CreateFontW(-size, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HGDIOBJ old_f = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    int x = (int)x0 + size / 2, y = (int)h - size * 2, o = size / 14 + 1;
    SetTextColor(dc, RGB(0, 0, 0));
    for (int dy = -o; dy <= o; dy++)
        for (int dx = -o; dx <= o; dx++) if (dx || dy) TextOutW(dc, x + dx, y + dy, text, (int)wcslen(text));
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, x, y, text, (int)wcslen(text));
    GdiFlush();
    memcpy(px, bits, (size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) px[i] |= 0xFF000000u;   // GDI leaves alpha 0 where it drew
    SelectObject(dc, old_f); SelectObject(dc, old_bm);
    DeleteObject(f); DeleteObject(bm); DeleteDC(dc);
}
static void cmp_save(uint32_t at, const char *what, const uint32_t *px, UINT w, UINT h)
{
    char file[48], name[MAX_PATH + 64];
    wchar_t wn[MAX_PATH + 64];
    snprintf(file, sizeof file, "%u_%s.png", at, what);
    capture_path(name, sizeof name, "compare", file);
    if (px && utf8_wide(name, wn, MAX_PATH + 64)) write_png(wn, px, w, h, w * 4), xlog("[cmp] saved %s (%ux%u)", name, w, h);
    else xlog("[cmp] %s: couldn't read the frame back", file);
}
void compare_shot(void)
{
    uint32_t at = frame_no;
    if (rec_done < 0 || !end_sb || !held || !held_tex || frames[rec_done].broken) { xlog("[cmp] F10: the frame couldn't be recorded, no shot"); return; }
    struct frame_rec *f = &frames[rec_done];
    D3DSURFACE_DESC d;
    IDirect3DTexture9_GetLevelDesc(held_tex, 0, &d);
    UINT W = d.Width, H = d.Height, sh = H > 1440 ? 1440 : H, sw = (UINT)((uint64_t)W * sh / H);
    IDirect3DSurface9 *hs = 0;
    IDirect3DTexture9_GetSurfaceLevel(held_tex, 0, &hs);
    uint32_t *hd = read_rt(hs, W, H), *hd_side = scaled_rt(hs, sw, sh, D3DTEXF_LINEAR);
    IDirect3DSurface9_Release(hs);
    // What to bind instead, for the textures this frame drew with: the game's own for a pack's (made now) and for
    // our upscaled copies. Our sampling tweaks for those (smooth font and soft pack sheets) go back to nearest.
    static struct { IDirect3DTexture9 *from, *to; int point, own; } sw_[1024];
    int n = 0;
    for (int i = 0; i < HOST_SLOTS && n < 1022; i++) {
        struct tex_entry *h = &hosts[i];
        if (!h->key || h->key == HOST_TOMB || !h->tex || h->gpu || h->used_rec != rec_serial - 1) continue;
        IDirect3DTexture9 *own = 0;
        if (h->replaced) {
            XPix x = {0, h->data, 0, h->format, h->size};
            struct tex_entry t;
            memset(&t, 0, sizeof t);
            cmp_plain = 1;
            own = upload_texture(&x, h->pal, &t);
            cmp_plain = 0;
            if (own) sw_[n].from = h->tex, sw_[n].to = own, sw_[n].point = h->soft_alpha, sw_[n].own = 1, n++;
        }
        if (h->big) sw_[n].from = h->big, sw_[n].to = own ? own : h->tex, sw_[n].point = h->text, sw_[n].own = 0, n++;
    }
    // Replayed into the game's targets as an in-between is (show_inbetween), its copy targets kept and put back.
    IDirect3DSurface9 *dsts[4], *keep[4] = {0};
    int nd = 0;
    for (int i = 0; i < f->n && nd < 4; i++) {
        if (f->ops[i].kind != OP_COPY) continue;
        int seen = 0;
        for (int k = 0; k < nd; k++) seen |= dsts[k] == f->ops[i].dst;
        if (seen) continue;
        dsts[nd] = f->ops[i].dst;
        IDirect3DSurface9_GetDesc(dsts[nd], &d);
        if (SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &keep[nd], 0)))
            IDirect3DDevice9_StretchRect(dev, dsts[nd], 0, keep[nd], 0, D3DTEXF_NONE);
        nd++;
    }
    for (int i = 0; i < f->n; i++) {
        struct op *o = &f->ops[i];
        switch (o->kind) {
        case OP_DRAW:
            IDirect3DStateBlock9_Apply(o->sb);
            for (int st = 0; st < 4; st++) {
                IDirect3DBaseTexture9 *bt = 0;
                IDirect3DDevice9_GetTexture(dev, st, &bt);
                if (!bt) continue;
                for (int k = 0; k < n; k++) {
                    if ((IDirect3DBaseTexture9 *)sw_[k].from != bt) continue;
                    IDirect3DDevice9_SetTexture(dev, st, (IDirect3DBaseTexture9 *)sw_[k].to);
                    if (sw_[k].point) {
                        IDirect3DDevice9_SetSamplerState(dev, st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                        IDirect3DDevice9_SetSamplerState(dev, st, D3DSAMP_MINFILTER, D3DTEXF_POINT);
                    }
                    break;
                }
                IDirect3DBaseTexture9_Release(bt);
            }
            if (o->indexed)
                IDirect3DDevice9_DrawIndexedPrimitiveUP(dev, o->pt, o->minv, o->nverts, o->prims, f->data + o->idx, D3DFMT_INDEX16, f->data + o->verts, o->stride);
            else
                IDirect3DDevice9_DrawPrimitiveUP(dev, o->pt, o->prims, f->data + o->verts, o->stride);
            break;
        case OP_CLEAR:
            IDirect3DDevice9_SetViewport(dev, &o->vp);
            IDirect3DDevice9_Clear(dev, o->count, o->count ? (const D3DRECT *)(f->data + o->rects) : 0, o->flags, o->color, o->z, o->stencil);
            break;
        case OP_TARGET:
            IDirect3DDevice9_SetRenderTarget(dev, 0, o->rt);
            IDirect3DDevice9_SetDepthStencilSurface(dev, o->ds);
            break;
        case OP_COPY:
            IDirect3DDevice9_StretchRect(dev, o->src, o->whole ? 0 : &o->sr, o->dst, o->whole ? 0 : &o->dr, D3DTEXF_NONE);
            break;
        }
    }
    IDirect3DSurface9 *fr = resolve_frame(), *lo = 0;
    uint32_t *orig = 0, *orig_side = 0;
    if (SUCCEEDED(IDirect3DDevice9_CreateRenderTarget(dev, bb_w, bb_h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &lo, 0))
        && SUCCEEDED(IDirect3DDevice9_StretchRect(dev, fr, 0, lo, 0, D3DTEXF_POINT))) {
        orig = read_rt(lo, bb_w, bb_h);
        orig_side = scaled_rt(lo, sw, sh, D3DTEXF_LINEAR);
    }
    if (lo) IDirect3DSurface9_Release(lo);
    if (fr) IDirect3DSurface9_Release(fr);
    for (int k = 0; k < nd; k++) if (keep[k]) IDirect3DDevice9_StretchRect(dev, keep[k], 0, dsts[k], 0, D3DTEXF_NONE), IDirect3DSurface9_Release(keep[k]);
    for (int k = 0; k < n; k++) if (sw_[k].own) IDirect3DTexture9_Release(sw_[k].to);
    IDirect3DDevice9_SetRenderTarget(dev, 0, end_rt);
    IDirect3DDevice9_SetDepthStencilSurface(dev, end_ds);
    IDirect3DStateBlock9_Apply(end_sb);
    rs_valid = 0;
    uint32_t *side = orig_side && hd_side ? malloc((size_t)sw * 2 * sh * 4) : 0;
    for (UINT y = 0; side && y < sh; y++) {
        memcpy(side + (size_t)y * sw * 2, orig_side + (size_t)y * sw, sw * 4);
        memcpy(side + (size_t)y * sw * 2 + sw, hd_side + (size_t)y * sw, sw * 4);
    }
    if (side) cmp_label(side, sw * 2, sh, 0, L"Original Xbox (640x480)"), cmp_label(side, sw * 2, sh, sw, L"Port + HD pack");
    xlog("[cmp] F10: frame %u, %d textures swapped back, room %s", at, n, last_room);
    cmp_save(at, "hd", hd, W, H);
    cmp_save(at, "original", orig, bb_w, bb_h);
    cmp_save(at, "side", side, sw * 2, sh);
    free(hd); free(hd_side); free(orig); free(orig_side); free(side);
}
