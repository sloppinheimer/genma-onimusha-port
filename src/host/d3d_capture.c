// Debug captures, split out of d3d.c: F12 (every draw of the next two frames logged, the frames saved),
// Shift+F12 (the textures a frame draws, as the game has them), ONI_SHOTS screenshots and where captures go.
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

// Debug: F12 in the game window logs every draw of the next two frames (and saves both frames).
int capture;

// Debug: Shift+F12 in the game window saves every texture the next frame draws with, as the game has it (its own picture in
// its own palette, not a pack's replacement), once each, to captures\textures\<frame>\<pack name>.png, with index.txt:
// per draw, the texture stage, pack name and whether a pack replaced it (HD) or not (--). So one Shift+F12 anywhere gives the
// originals of what's on screen, whatever mods are on.
int tex_capture;
uint32_t tex_capture_frame;
// Shift+F12's folder, captures\textures\<frame>: one of that name from an earlier session gets _2, _3... so its
// index.txt isn't appended to (it listed two different scenes).
const char *tex_capture_dir(void)
{
    static uint32_t made_for;
    static char dir[64];
    if (made_for != tex_capture_frame || !dir[0]) {
        made_for = tex_capture_frame;
        char path[MAX_PATH + 160];
        snprintf(dir, sizeof dir, "textures\\%u", tex_capture_frame);
        capture_path(path, sizeof path, "textures", dir + 9);   // makes captures\textures; is <frame> there already?
        for (int k = 2; uexists(path) && k < 1000; k++) { snprintf(dir, sizeof dir, "textures\\%u_%d", tex_capture_frame, k); capture_path(path, sizeof path, "textures", dir + 9); }
    }
    return dir;
}
static void capture_texture(const XPix *x, const uint32_t *pal, int st)
{
    struct xtex_info t = tex_info(x->Format, x->Size);
    if (!t.ok || t.kind == F_DEPTH || t.kind == F_YUY2 || XFMT_CUBEMAP(x->Format) || XFMT_DIMS(x->Format) == 3 || !x->Data) return;
    char name[64], wild[64];
    uint64_t key = tex_name(x, pal, &t, name, wild, sizeof name);
    static uint64_t done[4096];
    static uint32_t ndone, done_for;
    if (done_for != tex_capture_frame) { ndone = 0; done_for = tex_capture_frame; }
    struct tex_entry *h = host_find(cache_key(x, pal));
    char path[MAX_PATH + 160], png[80];
    const char *dir = tex_capture_dir();
    capture_path(path, sizeof path, dir, "index.txt");
    FILE *ix = ufopen(path, L"a");
    if (ix) { fprintf(ix, "f%u #%u s%d %s %s\n", frame_no, stats.drawn, st, name, h && h->replaced ? "HD" : "--"); fclose(ix); }
    for (uint32_t i = 0; i < ndone; i++) if (done[i] == key) return;
    if (ndone < 4096) done[ndone++] = key;
    const uint8_t *src = (const uint8_t *)x->Data;
    if (readable_bytes(src, texture_bytes(&t)) < texture_bytes(&t)) return;
    uint32_t *px = malloc((size_t)t.w * t.h * 4);
    if (!px) return;
    if (is_dxt(t.kind)) dxt_decode(t.kind, src, ((t.w + 3) / 4) * dxt_block(t.kind), t.w, t.h, px);
    else decode_level(&t, src, t.w, t.h, pal, (uint8_t *)px, t.w * 4);
    snprintf(png, sizeof png, "%s.png", name);
    capture_path(path, sizeof path, dir, png);
    wchar_t wp[MAX_PATH + 160];
    if (utf8_wide(path, wp, MAX_PATH + 160)) write_png(wp, px, t.w, t.h, t.w * 4);
    free(px);
}

void capture_textures(void)   // Shift+F12: each texture this draw uses
{
    for (int st = 0; st < 4; st++) {
        const XPix *x = bound_tex[st];
        int used = pixel_shader ? (RS[XRS_PSTEXTUREMODES] >> (st * 5) & 31) != 0 : TS(st, XTSS_COLOROP) > 1;
        if (!used || !x || !readable_bytes(x, 12)) continue;
        const uint32_t *cp = XFMT_CODE(x->Format) == 0x0b ? stage_palette(st) : 0;
        if (XFMT_CODE(x->Format) != 0x0b || cp) capture_texture(x, cp, st);
    }
}
void capture_draw(const char *kind, uint32_t xprim, uint32_t n, const uint8_t *verts, uint32_t stride)
{
    if (!capture) return;
    char tex[1024] = "";
    for (int st = 0, o = 0; st < 4; st++) {
        const XPix *x = bound_tex[st];
        int used = pixel_shader ? (RS[XRS_PSTEXTUREMODES] >> (st * 5) & 31) != 0 : TS(st, XTSS_COLOROP) > 1;
        if (!used) continue;
        struct xtex_info t = x ? tex_info(x->Format, x->Size) : (struct xtex_info){0};
        struct tex_entry *h = x ? host_find(x) : 0;
        o += snprintf(tex + o, sizeof tex - o, " s%d[%p %ux%u f%02x%s%s op%u a%x/%x aop%u tci%x%s]", st, x, t.w, t.h, x ? XFMT_CODE(x->Format) : 0,
                      t.linear ? " lin" : "", h && h->gpu ? " gpu" : "", TS(st, XTSS_COLOROP), TS(st, XTSS_COLORARG1), TS(st, XTSS_COLORARG2),
                      TS(st, XTSS_ALPHAOP), TS(st, XTSS_TEXCOORDINDEX), TS(st, XTSS_ALPHAKILL) ? " kill" : "");
        if (x && XFMT_CODE(x->Format) != 0x0b && t.w) {   // its pack name, to find a room's textures in the dump
            char nm[64], wd[64];
            tex_name(x, 0, &t, nm, wd, sizeof nm);
            o += snprintf(tex + o, sizeof tex - o, " %s", nm);
        }
        if (x && XFMT_CODE(x->Format) == 0x0b && stage_palette(st)) {
            uint32_t ph = pack_pal_hash(stage_palette(st));   // as the texture pack names it (tex_name)
            o += snprintf(tex + o, sizeof tex - o, " pal %08x %016llx @%08x hdr %p data %08x", ph, (unsigned long long)content_hash(x, 0), stage_pal[st]->Data, (void *)stage_pal[st], x->Data);
        }
    }
    xlog("[cap] f%u #%u %s prim %u n %u | blend %u %x/%x z %u/%u/%x atest %u (%x %02x) ps %x cull %x rt %p%s", frame_no, stats.drawn, kind, xprim, n,
         RS[XRS_ALPHABLENDENABLE], RS[XRS_SRCBLEND], RS[XRS_DESTBLEND], RS[XRS_ZENABLE], RS[XRS_ZWRITEENABLE], RS[XRS_ZFUNC],
         RS[XRS_ALPHATESTENABLE], RS[XRS_ALPHAFUNC], RS[XRS_ALPHAREF] & 0xFF, pixel_shader, RS[XRS_CULLMODE], cur_rt, tex);
    if (RS[XRS_STENCILENABLE] || RS[XRS_COLORWRITEENABLE] != 0x01010101)
        xlog("[cap]    stencil %u func %x ref %x mask %x/%x ops fail %x zfail %x pass %x | colour write %08x", RS[XRS_STENCILENABLE],
             RS[XRS_STENCILFUNC], RS[XRS_STENCILREF], RS[XRS_STENCILMASK], RS[XRS_STENCILWRITEMASK], RS[XRS_STENCILFAIL],
             RS[XRS_STENCILZFAIL], RS[XRS_STENCILPASS], RS[XRS_COLORWRITEENABLE]);
    if (verts && stride >= 16) {
        int screen = !(vertex_shader & 1) && (vertex_shader & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;   // 2D: every corner
        for (uint32_t i = 0; i < (n < (screen ? 64u : 4u) ? n : (screen ? 64u : 4u)); i++) {
            const float *v = (const float *)(verts + i * stride);
            xlog("[cap]    v%u %.3f %.3f %.4f %.4f | uv %.5f %.5f  stride %u%s%08x", i, v[0], v[1], v[2], v[3], v[stride / 4 - 2], v[stride / 4 - 1], stride,
                 (vertex_shader & D3DFVF_DIFFUSE) ? " colour " : " ", (vertex_shader & D3DFVF_DIFFUSE) ? ((const uint32_t *)v)[4] : 0);
        }
        xlog("[cap]    filter mag %u min %u mip %u addr %u/%u aref %02x afunc %x", TS(0, XTSS_MAGFILTER), TS(0, XTSS_MINFILTER), TS(0, XTSS_MIPFILTER),
             TS(0, XTSS_ADDRESSU), TS(0, XTSS_ADDRESSV), RS[XRS_ALPHAREF] & 0xFF, RS[XRS_ALPHAFUNC]);
    }
}

void write_bmp(const char *name, const D3DLOCKED_RECT *lr, int w, int h)   // name: UTF-8
{
    FILE *f = ufopen(name, L"wb");
    if (!f) return;
    BITMAPFILEHEADER fh = {0x4D42, sizeof fh + sizeof(BITMAPINFOHEADER) + w * h * 4, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER ih = {sizeof ih, w, -h, 1, 32};
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
    for (int y = 0; y < h; y++) fwrite((uint8_t *)lr->pBits + y * lr->Pitch, w * 4, 1, f);
    fclose(f);
    xlog("[d3d] saved %s", name);
}

static void save_frame_as(const char *name, int full, int png);
int player_shot;

// F12 in a release (player_shot): the picture as the window shows it (picture = modern's crop and HUD, a menu's
// filled sides), at the render resolution, as captures\screenshots\Genma Onimusha <date> <time>.png.
// Debug: ONI_SHOTS="120,600,1200" saves the backbuffer at those frames as shot_<frame>.bmp next to the exe.
void maybe_screenshot(void)
{
    if (player_shot) {
        player_shot = 0;
        SYSTEMTIME t;
        GetLocalTime(&t);
        char file[64], name[MAX_PATH + 64];
        snprintf(file, sizeof file, "Genma Onimusha %04u-%02u-%02u %02u-%02u-%02u.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        capture_path(name, sizeof name, "screenshots", file);
        unique_path(name, sizeof name);
        save_frame_as(name, 1, 1);
    }
#ifdef ONI_DEBUG
    static char spec[4096];
    static int init;
    if (!init++) env_str("ONI_SHOTS", spec, sizeof spec);
    char key[16];
    snprintf(key, sizeof key, "%u", frame_no);
    const char *p = spec;
    size_t kl = strlen(key);
    int hit = 0;
    while ((p = strstr(p, key))) { if ((p == spec || p[-1] == ',') && (p[kl] == 0 || p[kl] == ',')) { hit = 1; break; } p++; }
    int fresh = report_take_shot();   // a feature marked unsupported appeared this frame
    if (!hit && !capture && !fresh) return;
    char file[32], name[MAX_PATH + 64];
    snprintf(file, sizeof file, hit || capture ? "shot_%u.bmp" : "new_%u.bmp", frame_no);
    capture_path(name, sizeof name, "screenshots", file);
    unique_path(name, sizeof name);
    save_frame(name);
    if (hit) mid_shot = frame_no;   // with frame interpolation, also the in-between shown just before this frame
#endif
}

// F12 / ONI_SHOTS captures go to captures\screenshots and captures\depth in the install folder (made when first
// needed), not loose beside the exe: the folder filled up with hundreds of them. From the install folder, not the
// current one: started from bin\ itself, they went to bin\captures. As UTF-8, opened with ufopen / utf8_wide.
void capture_path(char *out, size_t cap, const char *kind, const char *file)
{
    static char base[MAX_PATH * 3];
    if (!base[0]) {
        wchar_t w[MAX_PATH];
        host_path(w, L"captures");
        CreateDirectoryW(w, 0);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, base, sizeof base, 0, 0);
    }
    snprintf(out, cap, "%s\\%s", base, kind);
    wchar_t wd[MAX_PATH + 160];
    if (utf8_wide(out, wd, MAX_PATH + 160)) CreateDirectoryW(wd, 0);
    snprintf(out, cap, "%s\\%s\\%s", base, kind, file);
}
void dump_path(char *out, size_t cap, const char *file) { capture_path(out, cap, "dumps", file); }

// Saved at the game's 640x480, downscaled from the render target, so captures compare across settings.
// Debug: ONI_SHOTS_FULL=1 saves at the render resolution instead.
void save_frame(const char *name)
{
    static int full = -1;
    if (full < 0) full = env_int("ONI_SHOTS_FULL", 0) == 1;
    save_frame_as(name, full, 0);
}
static void save_frame_as(const char *name, int full, int png)   // png: write_png, opaque; else write_bmp
{
    int w = full ? bb_w * scale : bb_w, h = full ? bb_h * scale : bb_h;
    RECT cr;
    GetClientRect(hwnd, &cr);
    int fill = !pres_wide && cr.bottom > 0 && menu_fill(cr.right, cr.bottom);
    if (pres_wide) { h = full ? bb_h * scale : bb_h; w = (int)(h * wide_aspect + 0.5f); }   // picture = modern: as the window shows it
    if (fill) w = (int)((float)h * cr.right / cr.bottom + 0.5f);
    IDirect3DSurface9 *bb = 0, *half = 0, *sys = 0;
    D3DLOCKED_RECT lr;
    bb = resolve_frame();
    IDirect3DDevice9_CreateRenderTarget(dev, w, h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &half, 0);
    if (half && pres_wide) compose_wide(bb, half, w, h);
    else if (half && fill) compose_fill(bb, half, w, h);
    else if (half) IDirect3DDevice9_StretchRect(dev, bb, 0, half, 0, D3DTEXF_LINEAR);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, w, h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, 0);
    if (half && sys && SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, half, sys)) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, 0, D3DLOCK_READONLY))) {
        if (!png) write_bmp(name, &lr, w, h);
        else {
            uint32_t *px = malloc((size_t)w * h * 4);
            wchar_t wn[MAX_PATH + 64];
            for (int y = 0; px && y < h; y++) {
                memcpy(px + (size_t)y * w, (uint8_t *)lr.pBits + y * lr.Pitch, w * 4);
                for (int x = 0; x < w; x++) px[(size_t)y * w + x] |= 0xFF000000u;   // X8R8G8B8: the top byte is undefined
            }
            if (px && utf8_wide(name, wn, MAX_PATH + 64)) write_png(wn, px, w, h, w * 4), xlog("[d3d] screenshot %s (%dx%d)", name, w, h);
            free(px);
        }
        IDirect3DSurface9_UnlockRect(sys);
    }
    if (sys) IDirect3DSurface9_Release(sys);
    if (half) IDirect3DSurface9_Release(half);
    if (bb) IDirect3DSurface9_Release(bb);
}
