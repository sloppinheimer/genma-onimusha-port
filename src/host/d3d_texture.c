// Textures, split out of d3d.c: the cache of D3D9 textures keyed by Xbox header address, the Xbox texture formats,
// decoding Xbox memory into a D3D9 texture, and the HD pack's names for a texture (texpack.c finds the files).
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

// ---- host object cache, keyed by Xbox header address -------------------------------------------------
struct tex_entry hosts[HOST_SLOTS];
// picture = modern: the decode puzzle (DOCUMENT / DECODE / BACK) is a full-screen 2D screen drawn in play (mode 2) over
// the 3D scene, so it went wide and its picture was cropped (too large). A frame that draws its button sheet
// (image\etc\e_mn005.ldt, picture 0) gets the in-game menu's treatment instead (menu_fill). So do the Options screen
// and the screens it opens, which the game shows in mode 0 like the title itself: Options by its sheet of dots, arrows
// and sliders (image\etc\syousai.ldt), Controls, Brightness and Screen Adjust by their backgrounds. A screen may name
// rows (art y, 0-448) whose own edge must not be carried outwards: Brightness's colour bars run to the picture's edge
// and smeared into grey and black blotches, so the sides there take the stone from above and below instead.
// The documents (books, letters found in play) are their page, shared by all of them, with each one's text over it:
// drawn over the room's 3D, they went wide and the page and text were cut off top and bottom.
// The puzzle box in room 310 (numbered tiles over a wooden board) draws its tiles in 3D over its own board picture,
// so it went wide and the board was cut off top and bottom; its board picture makes it such a screen too.
const struct screen2d_name screen2d_names[] = {{"256x128_0b_f28a975946f98e05"},
    {"256x256_0b_ad256928423d9f2d"}, {"1024x512_0c_99eb69032bb50c6a"}, {"1024x512_0c_14913ccc739c17e1", 76, 303},
    {"1024x512_0c_1f332d51c35a000c"}, {"1024x512_0c_2bdd29c1227de8b8"}, {"1024x512_0c_b1a718637f8688c4"}};
float screen2d_band[2];   // the rows of the screen last drawn (0 0 = none)
uint32_t screen2d_frame = ~0u;   // last frame that drew it
int screen2d_on;                 // the frame just decided (wide_next_frame) is such a screen
// A full-screen 2D picture in play (mode 2) or Oni Spirits (0x0c) with nothing 3D and no room depth: the stage result
// screens (one picture per stage), the Oni Spirits menu and its results. Pillarboxed until they got menu_fill's sides.
uint32_t fullpic_frame = ~0u;    // last frame that drew a DXT1 picture over the whole art
int fullpic_on;                  // the frame just decided is such a screen
uint32_t frame_no = 1;
uint32_t movie_seen;   // frame_no of the last movie frame uploaded
uint32_t movie_w, movie_h;   // its size: the movie's own picture (640x360 for most), centred in the 4:3 frame


// The entry for key, or 0.
struct tex_entry *host_find(const void *key)
{
    uint32_t h = ((uint32_t)key >> 2) * 2654435761u % HOST_SLOTS;
    for (int i = 0; i < HOST_SLOTS && hosts[h].key; i++, h = (h + 1) % HOST_SLOTS)
        if (hosts[h].key == key) return &hosts[h];
    return 0;
}

// The entry for key, made in the first free or forgotten slot on its chain if missing.
// ponytail: tombstones only become free slots again by reuse; rehash the table if long probes ever show up.
struct tex_entry *host_for(const void *key)
{
    struct tex_entry *h = host_find(key);
    if (h) return h;
    uint32_t i = ((uint32_t)key >> 2) * 2654435761u % HOST_SLOTS;
    for (int n = 0; n < HOST_SLOTS; n++, i = (i + 1) % HOST_SLOTS)
        if (!hosts[i].key || hosts[i].key == HOST_TOMB) { hosts[i].key = key; return &hosts[i]; }
    xfatal("[d3d] host cache full");
}

static void host_drop(struct tex_entry *h)
{
    if (h->tex) IDirect3DTexture9_Release(h->tex);
    if (h->big) IDirect3DTexture9_Release(h->big);
    memset(h, 0, sizeof *h);
    h->key = HOST_TOMB;
}
void host_forget(const void *key) { struct tex_entry *h = host_find(key); if (h) host_drop(h); }

// Paletted entries are keyed by texture data and palette (apply_stage_state), so no single Release ends them, and
// palette fades make one per frame. Run every 600 frames: drops those not drawn in that time.
void host_sweep(void)
{
    for (int i = 0; i < HOST_SLOTS; i++)
        if (((uint32_t)hosts[i].key & 3) == 2 && frame_no - hosts[i].hash_frame > 600) host_drop(&hosts[i]);
}

// ---- Xbox-visible memory -----------------------------------------------------------------------------
void *xalloc(uint32_t n) { return VirtualAlloc(0, n ? n : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
void *xheader(uint32_t n) { return calloc(1, n); }

// ---- formats -----------------------------------------------------------------------------------------
const struct xfmt formats[] = {
    {0x00, F_L8, 1, 0}, {0x01, F_AL8, 1, 0}, {0x02, F_1555, 2, 0}, {0x03, F_X555, 2, 0}, {0x04, F_4444, 2, 0},
    {0x05, F_565, 2, 0}, {0x06, F_8888, 4, 0}, {0x07, F_X888, 4, 0}, {0x0b, F_P8, 1, 0}, {0x0c, F_DXT1, 4, 0},
    {0x0e, F_DXT3, 8, 0}, {0x0f, F_DXT5, 8, 0}, {0x10, F_1555, 2, 1}, {0x11, F_565, 2, 1}, {0x12, F_8888, 4, 1},
    {0x13, F_L8, 1, 1}, {0x19, F_A8, 1, 0}, {0x1a, F_A8L8, 2, 0}, {0x1b, F_AL8, 1, 1}, {0x1c, F_X555, 2, 1},
    {0x1d, F_4444, 2, 1}, {0x1e, F_X888, 4, 1}, {0x1f, F_A8, 1, 1}, {0x20, F_A8L8, 2, 1}, {0x24, F_YUY2, 2, 1},
    {0x2a, F_DEPTH, 4, 0}, {0x2b, F_DEPTH, 4, 0}, {0x2c, F_DEPTH, 2, 0}, {0x2d, F_DEPTH, 2, 0},
    {0x2e, F_DEPTH, 4, 1}, {0x2f, F_DEPTH, 4, 1}, {0x30, F_DEPTH, 2, 1}, {0x31, F_DEPTH, 2, 1},
    {0x38, F_5551, 2, 0}, {0x39, F_4444R, 2, 0}, {0x3a, F_ABGR, 4, 0}, {0x3b, F_BGRA, 4, 0}, {0x3c, F_RGBA, 4, 0},
    {0x3d, F_5551, 2, 1}, {0x3e, F_4444R, 2, 1}, {0x3f, F_ABGR, 4, 1}, {0x40, F_BGRA, 4, 1}, {0x41, F_RGBA, 4, 1},
};
// bpp: bytes per texel; not used for DXT (sized by 4x4 block, see level_bytes).
int fmt_index(uint32_t code)
{
    for (int i = 0; i < (int)(sizeof formats / sizeof *formats); i++) if (formats[i].code == code) return i;
    return -1;
}

// Bytes in one mip level of an Xbox texture.
uint32_t level_bytes(int kind, int bpp, uint32_t w, uint32_t h)
{
    if (is_dxt(kind)) return ((w + 3) / 4) * ((h + 3) / 4) * dxt_block(kind);
    return w * h * bpp;
}

uint32_t ilog2(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }

// Xbox swizzle: texel (x, y) lives at the bit-interleave of x and y (Morton order, limited by the shorter side).
void swizzle_masks(uint32_t w, uint32_t h, uint32_t *mx, uint32_t *my)
{
    *mx = *my = 0;
    for (uint32_t i = 1, bit = 1; i < w || i < h; i <<= 1) {
        if (i < w) { *mx |= bit; bit <<= 1; }
        if (i < h) { *my |= bit; bit <<= 1; }
    }
}
uint32_t pdep(uint32_t v, uint32_t mask)
{
    uint32_t r = 0;
    for (uint32_t b = 1; mask; b <<= 1) { uint32_t low = mask & (~mask + 1); if (v & b) r |= low; mask &= mask - 1; }
    return r;
}

static uint32_t expand5(uint32_t v) { return (v << 3) | (v >> 2); }
static uint32_t expand6(uint32_t v) { return (v << 2) | (v >> 4); }
static uint32_t expand4(uint32_t v) { return v * 17; }

uint32_t texel_argb(int kind, const uint8_t *p, const uint32_t *pal)
{
    uint32_t v;
    switch (kind) {
    case F_L8: return 0xFF000000u | p[0] * 0x010101u;
    case F_AL8: return p[0] * 0x01010101u;
    case F_A8: return (uint32_t)p[0] << 24;
    case F_A8L8: v = *(uint16_t *)p; return ((v >> 8) << 24) | (v & 0xFF) * 0x010101u;
    case F_P8: return pal ? pal[p[0]] : 0xFFFF00FFu;
    case F_1555: v = *(uint16_t *)p; return ((v & 0x8000) ? 0xFF000000u : 0) | expand5((v >> 10) & 31) << 16 | expand5((v >> 5) & 31) << 8 | expand5(v & 31);
    case F_X555: v = *(uint16_t *)p; return 0xFF000000u | expand5((v >> 10) & 31) << 16 | expand5((v >> 5) & 31) << 8 | expand5(v & 31);
    case F_565: v = *(uint16_t *)p; return 0xFF000000u | expand5(v >> 11) << 16 | expand6((v >> 5) & 63) << 8 | expand5(v & 31);
    case F_4444: v = *(uint16_t *)p; return expand4(v >> 12) << 24 | expand4((v >> 8) & 15) << 16 | expand4((v >> 4) & 15) << 8 | expand4(v & 15);
    case F_5551: v = *(uint16_t *)p; return ((v & 1) ? 0xFF000000u : 0) | expand5(v >> 11) << 16 | expand5((v >> 6) & 31) << 8 | expand5((v >> 1) & 31);
    case F_4444R: v = *(uint16_t *)p; return expand4(v & 15) << 24 | expand4(v >> 12) << 16 | expand4((v >> 8) & 15) << 8 | expand4((v >> 4) & 15);
    case F_8888: return *(uint32_t *)p;
    case F_X888: return *(uint32_t *)p | 0xFF000000u;
    case F_ABGR: v = *(uint32_t *)p; return (v & 0xFF00FF00u) | (v & 0xFF) << 16 | (v >> 16 & 0xFF);
    case F_BGRA: return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    case F_RGBA: return p[0] << 24 | p[3] << 16 | p[2] << 8 | p[1];
    }
    return 0xFFFF00FFu;
}

static uint32_t clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static uint32_t yuv(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    return 0xFF000000u | clamp255((298 * c + 409 * e + 128) >> 8) << 16 | clamp255((298 * c - 100 * d - 208 * e + 128) >> 8) << 8 | clamp255((298 * c + 516 * d + 128) >> 8);
}

// ---- textures: Xbox memory -> D3D9 texture ---------------------------------------------------------

struct xtex_info tex_info(uint32_t format, uint32_t size)
{
    struct xtex_info t = {0};
    int fi = fmt_index(XFMT_CODE(format));
    if (fi < 0) return t;
    t.kind = formats[fi].kind; t.bpp = formats[fi].bpp; t.linear = formats[fi].linear;
    if (is_dxt(t.kind)) t.bpp = 0;
    if (size) {   // linear: explicit size and pitch
        t.w = XSIZE_W(size); t.h = XSIZE_H(size); t.pitch = XSIZE_PITCH(size); t.levels = 1;
    } else {
        t.w = 1u << XFMT_ULOG(format); t.h = 1u << XFMT_VLOG(format); t.levels = XFMT_LEVELS(format);
        if (!t.levels) t.levels = 1;
        t.pitch = is_dxt(t.kind) ? (t.w + 3) / 4 * dxt_block(t.kind) : t.w * t.bpp;   // DXT: a row of blocks
    }
    t.ok = 1;
    return t;
}

uint32_t texture_bytes(const struct xtex_info *t)
{
    if (t->linear) return t->pitch * t->h;
    uint32_t n = 0;
    for (uint32_t lv = 0, w = t->w, h = t->h; lv < t->levels; lv++, w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1)
        n += level_bytes(t->kind, t->bpp, w, h);
    return n;
}

// How many of the `want` bytes at p are committed and readable. Some textures (seen after the intro movie)
// claim more data than the memory behind them holds.
uint32_t readable_bytes(const void *p, uint32_t want)
{
    uint32_t got = 0;
    MEMORY_BASIC_INFORMATION m;
    while (got < want && VirtualQuery((const uint8_t *)p + got, &m, sizeof m)) {
        if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !m.Protect) break;
        got += (uint32_t)((uint8_t *)m.BaseAddress + m.RegionSize - ((const uint8_t *)p + got));
    }
    if (got < want) {
        static int warned;
        if (warned++ < 8) xlog("[d3d] texture data %p: only %u of %u bytes readable", p, got, want);
        return got;
    }
    return want;
}

uint64_t content_hash(const XPix *x, const uint32_t *pal)
{
    struct xtex_info t = tex_info(x->Format, x->Size);
    if (!t.ok) return 0;
    const void *p = (const void *)x->Data;
    uint64_t h = pack_texel_hash(p, readable_bytes(p, texture_bytes(&t)));
    for (int i = 0; pal && i < 256; i++) h = (h ^ pal[i]) * 1099511628211ull;
    return h;
}
uint64_t pack_texel_hash(const void *texels, uint32_t bytes)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p = texels;
    for (uint32_t i = 0; i + 8 <= bytes; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 1099511628211ull; }
    return h;
}
uint32_t pack_pal_hash(const uint32_t *pal)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 256; i++) h = (h ^ pal[i]) * 16777619u;
    return h;
}
void pack_name(char *name, char *wild, size_t n, uint32_t w, uint32_t h, uint32_t code, uint64_t texels, int paletted, uint32_t pal)
{
    int k = snprintf(name, n, "%ux%u_%02x_%016llx", w, h, code, (unsigned long long)texels);
    if (wild) memcpy(wild, name, k + 1);
    if (paletted) { snprintf(name + k, n - k, "_%08x", pal); if (wild) snprintf(wild + k, n - k, "_$"); }
}

// ---- HD texture packs (texpack.c) --------------------------------------------------------------------
// A texture's pack name: size, Xbox format code and a hash of its texels (every mip level), e.g. 256x256_06_<16 hex>;
// a paletted one adds its palette's hash (_<8 hex>), and a pack file with _$ there instead matches every palette.
// The same between runs. Returns a key for those contents.
uint64_t tex_name(const XPix *x, const uint32_t *pal, const struct xtex_info *t, char *out, char *wild, size_t n)
{
    uint64_t th = content_hash(x, 0);
    uint32_t ph = pal ? pack_pal_hash(pal) : 2166136261u;   // the FNV start when none: the key stays as it was
    pack_name(out, wild, n, t->w, t->h, XFMT_CODE(x->Format), th, pal != 0, ph);
    return th * 31 + ph;
}

static uint32_t dxt_mix(uint32_t a, uint32_t b, int wa, int wb)
{
    uint32_t v = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) v |= ((a >> s & 255) * wa + (b >> s & 255) * wb) / (wa + wb) << s;
    return v;
}

// DXT1/3/5 blocks (rows of blocks pitch bytes apart) -> w x h ARGB, for the dump.
void dxt_decode(int kind, const uint8_t *src, int pitch, uint32_t w, uint32_t h, uint32_t *out)
{
    for (uint32_t by = 0; by < (h + 3) / 4; by++) {
        const uint8_t *b = src + by * pitch;
        for (uint32_t bx = 0; bx < (w + 3) / 4; bx++, b += dxt_block(kind)) {
            const uint8_t *c = kind == F_DXT1 ? b : b + 8;
            uint32_t c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8, col[4], alpha[8];
            col[0] = texel_argb(F_565, c, 0); col[1] = texel_argb(F_565, c + 2, 0);
            if (kind != F_DXT1 || c0 > c1) { col[2] = dxt_mix(col[0], col[1], 2, 1); col[3] = dxt_mix(col[0], col[1], 1, 2); }
            else { col[2] = dxt_mix(col[0], col[1], 1, 1); col[3] = 0; }
            if (kind == F_DXT5) {
                alpha[0] = b[0]; alpha[1] = b[1];
                for (int j = 2; j < 8; j++)
                    alpha[j] = b[0] > b[1] ? ((8 - j) * b[0] + (j - 1) * b[1]) / 7 : j < 6 ? ((6 - j) * b[0] + (j - 1) * b[1]) / 5 : j == 6 ? 0 : 255;
            }
            uint64_t abits = 0;
            for (int j = 0; j < 6; j++) abits |= (uint64_t)b[2 + j] << (8 * j);
            uint32_t bits = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4, v = col[bits >> (2 * i) & 3];
                if (kind == F_DXT3) v = (v & 0xFFFFFF) | (b[i / 2] >> (4 * (i & 1)) & 15) * 17u << 24;
                if (kind == F_DXT5) v = (v & 0xFFFFFF) | alpha[abits >> (3 * i) & 7] << 24;
                if (x < w && y < h) out[y * w + x] = v;
            }
        }
    }
}


static IDirect3DTexture9 *uploaded(struct tex_entry *h, const XPix *x)
{
    h->data = x->Data; h->format = x->Format; h->size = x->Size;
    h->dirty = 0;
    h->uploaded_frame = frame_no;
    h->uploads++;
    return h->tex;
}

// One w x h level of a linear or swizzled (not DXT or YUY2) texture -> ARGB rows pitch bytes apart at out. A linear
// one's rows are t->pitch apart in src, at every level.
void decode_level(const struct xtex_info *t, const uint8_t *src, uint32_t w, uint32_t h, const uint32_t *pal,
                         uint8_t *out, int pitch)
{
    uint32_t mx = 0, my = 0;
    if (!t->linear) swizzle_masks(w, h, &mx, &my);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *d = (uint32_t *)(out + y * pitch), oy = t->linear ? 0 : pdep(y, my);
        if (t->linear) for (uint32_t xx = 0; xx < w; xx++) d[xx] = texel_argb(t->kind, src + y * t->pitch + xx * t->bpp, pal);
        else for (uint32_t xx = 0; xx < w; xx++) d[xx] = texel_argb(t->kind, src + (pdep(xx, mx) | oy) * t->bpp, pal);
    }
}

int cmp_plain;   // the F10 compare shot is uploading a texture as the game has it: no pack, no HD movie frame
IDirect3DTexture9 *upload_texture(const XPix *x, const uint32_t *pal, struct tex_entry *h)
{
    struct xtex_info t = tex_info(x->Format, x->Size);
    if (!t.ok || t.kind == F_DEPTH) return 0;
    h->pal = pal;
    if (XFMT_CUBEMAP(x->Format) || XFMT_DIMS(x->Format) == 3) {
        static int warned;
        if (!warned++) xlog("[d3d] TODO cube/volume textures (format %08x)", x->Format);
        xfeature(1, "texture: cube/volume texture not uploaded (format %02x)", XFMT_CODE(x->Format));
        return 0;
    }
    D3DFORMAT hf = t.kind == F_DXT1 ? D3DFMT_DXT1 : t.kind == F_DXT3 ? D3DFMT_DXT3 : t.kind == F_DXT5 ? D3DFMT_DXT5 : D3DFMT_A8R8G8B8;
    if (h->tex && (h->data != x->Data || h->format != x->Format || h->size != x->Size)) {
        IDirect3DTexture9_Release(h->tex);
        h->tex = 0;
    }
    // HD movies (movie.c): the game's decoded Bink frame -> the same frame of the HD file
    uint32_t mw, mh;
    if ((XFMT_CODE(x->Format) == 0x1e || t.kind == F_YUY2) && t.levels == 1 && !cmp_plain) {
        movie_sound_start(), movie_seen = frame_no;
        if (t.w != movie_w || t.h != movie_h) xlog("[movie] picture %ux%u", t.w, t.h);
        movie_w = t.w, movie_h = t.h;
    }
    const uint32_t *mf = (XFMT_CODE(x->Format) == 0x1e || t.kind == F_YUY2) && t.levels == 1 && !cmp_plain ? movie_frame(t.w, t.h, &mw, &mh) : 0;
    if (mf) {
        D3DSURFACE_DESC d;
        if (h->tex && (!h->replaced || FAILED(IDirect3DTexture9_GetLevelDesc(h->tex, 0, &d)) || d.Width != mw || d.Height != mh)) {
            IDirect3DTexture9_Release(h->tex);
            h->tex = 0;
        }
        if (!h->tex && FAILED(IDirect3DDevice9_CreateTexture(dev, mw, mh, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &h->tex, 0))) h->tex = 0;
        D3DLOCKED_RECT lr;
        int ok = h->tex && SUCCEEDED(IDirect3DTexture9_LockRect(h->tex, 0, &lr, 0, 0));
        if (ok) {
            for (uint32_t y = 0; y < mh; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, mf + (size_t)y * mw, mw * 4);
            IDirect3DTexture9_UnlockRect(h->tex, 0);
        }
        movie_frame_done();
        if (ok) { h->replaced = 1; h->pack_key = 0; return uploaded(h, x); }
    }
    // HD texture packs. Movie frames (YUY2, new every frame) are never dumped.
    char name[64] = "", wild[64] = "";
    int dump = texpack_dumping() && t.kind != F_YUY2 && !cmp_plain;
    uint64_t key = texpack_replacing() || dump ? tex_name(x, pal, &t, name, wild, sizeof name) : 0;
    h->screen2d = 0;
    if ((t.w == 256 && (t.h == 128 || t.h == 256) && XFMT_CODE(x->Format) == 0x0b) || (t.w == 1024 && t.h == 512 && XFMT_CODE(x->Format) == 0x0c)) {
        if (!key) { tex_name(x, pal, &t, name, wild, sizeof name); }
        for (size_t i = 0; i < sizeof screen2d_names / sizeof *screen2d_names; i++)
            if (!strncmp(name, screen2d_names[i].name, strlen(screen2d_names[i].name))) h->screen2d = (int)i + 1;
    }
    if (key && texpack_replacing() && !cmp_plain) {
        if (h->replaced && h->tex && h->pack_key == key) return uploaded(h, x);   // locked or re-checked, same contents
        LONGLONG t0 = qpc();
        int got = load_replacement(h, &t, name, wild);   // -1: being made off the game thread, the game's own meanwhile
        h->pack_wait = got < 0;
        if (got < 0) h->pack_wait_gen = pack_ready, got = 0;
        {   // pack textures that took long to make (a stutter's usual cause: JPG/PNG decoding on the game thread)
            double ms = qpc_ms(qpc() - t0);
            if (got && ms > 10) { xlog("[texpack] %s took %.1f ms", name, ms); stall_note("frame %u texture %s %.1f ms", frame_no, name, ms); }
            if (got) room_note(name);
        }
        // Not movie frames, and not while its HD version is still being made: that showed as "--" in textures_seen
        // for HD textures, the moment before theirs was ready. It's noted when it is (or when the pack has none).
        if (t.kind != F_YUY2 && XFMT_CODE(x->Format) != 0x1e && !h->pack_wait) texpack_seen(wild, got);
        if (got) {
            static int n;
            if (n++ < 50) xlog("[texpack] replaced %s", name);
            h->replaced = 1;
            h->pack_key = key;
            return uploaded(h, x);
        }
    }
    if (h->replaced) {   // contents changed: back to the game's own
        if (h->tex) IDirect3DTexture9_Release(h->tex);
        h->tex = 0;
        h->replaced = 0;
    }
    if (!h->tex) {
        static int n;
        if (n++ < 200) xlog("[d3d] texture %p: %ux%u levels %u fmt %02x%s data %08x", x, t.w, t.h, t.levels, XFMT_CODE(x->Format),
                            t.linear ? " linear" : "", x->Data);
    }
    if (!h->tex && FAILED(IDirect3DDevice9_CreateTexture(dev, t.w, t.h, t.levels, 0, hf, D3DPOOL_MANAGED, &h->tex, 0))) {
        xlog("[d3d] CreateTexture %ux%u x%u fmt %x failed", t.w, t.h, t.levels, XFMT_CODE(x->Format));
        return 0;
    }
    const uint8_t *src = (const uint8_t *)x->Data;
    uint8_t *padded = 0;
    uint32_t need = texture_bytes(&t), have = readable_bytes(src, need);
    if (have < need) {   // decode from a zero-padded copy rather than fault
        if (!(padded = calloc(1, need))) return 0;   // out of memory: drawn untextured this time, not a crash
        memcpy(padded, src, have);
        src = padded;
    }
    uint32_t w = t.w, hh = t.h;
    for (uint32_t lv = 0; lv < t.levels; lv++) {
        D3DLOCKED_RECT lr;
        if (FAILED(IDirect3DTexture9_LockRect(h->tex, lv, &lr, 0, 0))) break;
        if (is_dxt(t.kind)) {   // DXT is stored block-linear on Xbox too
            uint32_t row = ((w + 3) / 4) * dxt_block(t.kind);
            for (uint32_t y = 0; y < (hh + 3) / 4; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, src + y * row, row);
        } else if (t.kind == F_YUY2) {
            for (uint32_t y = 0; y < hh; y++) {
                const uint8_t *s = src + y * t.pitch;
                uint32_t *d = (uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch);
                for (uint32_t xx = 0; xx + 1 < w; xx += 2, s += 4) { d[xx] = yuv(s[0], s[1], s[3]); d[xx + 1] = yuv(s[2], s[1], s[3]); }
            }
        } else decode_level(&t, src, w, hh, pal, lr.pBits, lr.Pitch);
        IDirect3DTexture9_UnlockRect(h->tex, lv);
        src += level_bytes(t.kind, t.bpp, w, hh);
        if (w > 1) w >>= 1;
        if (hh > 1) hh >>= 1;
    }
    free(padded);
#ifdef ONI_DEV_TOOLS
    if (key && dump && !h->pack_wait) texdump_texture(h->tex, &t, name);   // not while its HD version is being made: the game's own shows only meanwhile
#endif
    return uploaded(h, x);
}

