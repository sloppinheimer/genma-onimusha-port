// HD texture packs on the GPU side (texpack.c reads the files): the replacement made for a game texture, the cache
// of the last ones made, the room read-ahead threads, and the stall and room lists kept next to the exe.
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
#include <ctype.h>
#include "d3d_internal.h"
#include <emmintrin.h>

// The pack's replacement for this texture into h->tex: A8R8G8B8, with mip levels box-filtered from it if the original
// has mips. Any size that is the original's times a whole number: texture coordinates are 0..1 (fix_texcoords divides
// linear textures' texel coordinates by the Xbox size, not this one), so they land on the same places.
// The last ones made are kept, up to PACK_KEEP bytes: the game reloads a shot's background at every camera cut (a scene
// cutting back and forth reloads the same few), and reading a 4096 px JPG and uploading it took 55-60 ms on the game
// thread, a stutter at each cut. Kept, it's shared at once; a room's backgrounds are also read ahead when the game
// opens its .bgs (bg_prefetch, below). ponytail: least recently used by linear search.
#define PACK_KEEP (768u << 20)
// Slots: room 007 reads ahead 27 camera pictures and 88 more textures (about 500 MB). With 96 slots the textures read
// last pushed out the camera pictures read first, and each camera cut in a fight remade its picture on the game
// thread (70 ms; stalls.txt 2026-10-24). The memory limit is what holds it back now.
#define PACK_SLOTS 256
// Each keeps what the file was: its size (checked against the game's texture on every use), whether it was made with
// mips, and whether its alpha is coverage (soft_alpha), so a kept or read-ahead texture is drawn as a decoded one.
struct pack_info { uint32_t w, h; int mips, soft; };
static struct { char name[64]; IDirect3DTexture9 *tex; uint32_t bytes; uint64_t used; struct pack_info info; int shared; } pack_kept[PACK_SLOTS];
static uint32_t pack_kept_bytes;
// Textures room_textures.txt lists in 5 rooms or more (the in-game menus, the pause screen, item pictures) are kept
// before anything else: used once in a while but everywhere, least-recently-used dropped them within a few rooms, and
// the menu's background was a 65-90 ms pause each launch (stalls.txt 2026-10-24). Filled by start_ahead_thread.
static char pack_shared[32][64];
static int pack_shared_n;
static int is_shared(const char *name)
{
    for (int i = 0; i < pack_shared_n; i++) if (!strcmp(pack_shared[i], name)) return 1;
    return 0;
}
static uint64_t pack_tick;
static SRWLOCK pack_lock = SRWLOCK_INIT;   // the prefetch thread adds to it too
static IDirect3DTexture9 *pack_find(const char *name, struct pack_info *info)   // a reference of the caller's own, or 0
{
    IDirect3DTexture9 *t = 0;
    AcquireSRWLockExclusive(&pack_lock);
    for (int i = 0; i < PACK_SLOTS && !t; i++)
        if (pack_kept[i].tex && !strcmp(pack_kept[i].name, name)) {
            pack_kept[i].used = ++pack_tick; t = pack_kept[i].tex; IDirect3DTexture9_AddRef(t);
            if (info) *info = pack_kept[i].info;
        }
    ReleaseSRWLockExclusive(&pack_lock);
    return t;
}
// 1 kept, 0 not (one of that name is kept already, or nothing could make room), -1 never: too big for the cache.
static int pack_keep_(const char *name, IDirect3DTexture9 *tex, uint32_t bytes, const struct pack_info *info);
static int pack_keep(const char *name, IDirect3DTexture9 *tex, uint32_t bytes, const struct pack_info *info)
{
    AcquireSRWLockExclusive(&pack_lock);
    int r = pack_keep_(name, tex, bytes, info);
    ReleaseSRWLockExclusive(&pack_lock);
    return r;
}
static int pack_keep_(const char *name, IDirect3DTexture9 *tex, uint32_t bytes, const struct pack_info *info)
{
    if (bytes > PACK_KEEP / 4 || strlen(name) >= sizeof pack_kept[0].name) return -1;
    for (int i = 0; i < PACK_SLOTS; i++)
        if (pack_kept[i].tex && !strcmp(pack_kept[i].name, name)) {
            if (pack_kept[i].info.mips || !info->mips) return 0;   // made twice at once
            pack_kept_bytes -= pack_kept[i].bytes;   // read ahead without the mips the game's texture has: this one replaces it
            IDirect3DTexture9_Release(pack_kept[i].tex);
            pack_kept[i].tex = 0;
            break;
        }
    for (;;) {
        int lru = -1, slot = -1;
        for (int i = 0; i < PACK_SLOTS; i++) {
            if (!pack_kept[i].tex) { if (slot < 0) slot = i; continue; }
            // the least recently used, a shared one only if nothing else is left
            if (lru < 0 || (pack_kept[lru].shared > pack_kept[i].shared) ||
                (pack_kept[lru].shared == pack_kept[i].shared && pack_kept[i].used < pack_kept[lru].used)) lru = i;
        }
        if (slot >= 0 && pack_kept_bytes + bytes <= PACK_KEEP) {
            strcpy(pack_kept[slot].name, name);
            pack_kept[slot].tex = tex; IDirect3DTexture9_AddRef(tex);
            pack_kept[slot].bytes = bytes; pack_kept[slot].used = ++pack_tick; pack_kept[slot].info = *info;
            pack_kept[slot].shared = is_shared(name);
            pack_kept_bytes += bytes;
            return 1;
        }
        if (lru < 0) return 0;
        pack_kept_bytes -= pack_kept[lru].bytes;
        IDirect3DTexture9_Release(pack_kept[lru].tex);
        pack_kept[lru].tex = 0;
    }
}

// logs\stalls.txt: the stall lines of the log (a game step over 45 ms, slow pack textures), adding up over
// launches (onimusha.log starts over each time), each launch headed by its date. Delete it to start over; past 1 MB it
// becomes stalls.old.txt (replacing the one before) and a new one starts, so it never grows without end.
char last_room[16] = "-";   // the room whose files the game opened last (room_open)

// logs\room_textures.txt: "ROOMnnn <pack name>" for each pack texture made while in that room, once each,
// adding up over launches. The input for reading a room's textures ahead (as bg_prefetch does its backgrounds).
// ponytail: a room is the last .rdt/.bgs the game opened, so the first textures of the next room can land on the one
// before when the game loads them ahead of its file; fine for a read-ahead list.
static FILE *rooms_file;
static uint64_t rooms_keys[1 << 15];
static uint32_t rooms_n;
static int room_key_add(const char *room, const char *name)   // 1: new
{
    return keyset_add(rooms_keys, 1 << 15, &rooms_n, fnv64_str(fnv64_str(FNV64_START, room), name));
}
void room_note(const char *name)
{
    static int tried;
    if (!tried++) {
        wchar_t path[MAX_PATH];
        host_path(path, L"logs\\room_textures.txt");
        FILE *old = _wfopen(path, L"r");
        char line[160], r[32], t[100];
        if (old) { while (fgets(line, sizeof line, old)) if (line[0] != ';' && sscanf(line, "%31s %99s", r, t) == 2) room_key_add(r, t); fclose(old); }
        if ((rooms_file = _wfopen(path, L"a")) && !old) fputs("; HD pack textures made in each room, once each, adding up over launches (d3d_pack.c room_note)\n", rooms_file);
    }
    const char *room = last_room[0] != '-' ? last_room : "START";   // before any room: the logos, the title
    if (rooms_file && room_key_add(room, name)) { fprintf(rooms_file, "%s %s\n", room, name); fflush(rooms_file); }
}

void room_open(const wchar_t *path)
{
    const wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    snprintf(last_room, sizeof last_room, "%.*ls", (int)(wcslen(base) > 4 ? wcslen(base) - 4 : wcslen(base)), base);
    for (char *c = last_room; *c; c++) *c = (char)toupper((uint8_t)*c);
}
void stall_note(const char *fmt, ...)
{
    static FILE *f;
    static int tried;
    if (!tried++) {
        wchar_t path[MAX_PATH];
        host_path(path, L"logs\\stalls.txt");
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > (1u << 20))) {
            wchar_t old[MAX_PATH];
            host_path(old, L"logs\\stalls.old.txt");
            MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
        }
        if ((f = _wfopen(path, L"a"))) { SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "; launch %04d-%02d-%02d %02d:%02d\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute); }
    }
    if (!f) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fflush(f);
}

// ---- pack textures as made: DXT1 for opaque backgrounds ----------------------------------------------------------
// One 4x4 block of ARGB (rows `pitch` texels apart; x/y clamp to w x h) as DXT1: endpoints the block's colour range,
// inset by 1/16, each texel the nearest of the four along it. Quick and close to the Xbox's own (its backgrounds are
// DXT1). ponytail: range fit, no endpoint search; fine for photographic art, a cluster fit would do edges better.
static void dxt1_block(const uint32_t *px, uint32_t pitch, uint32_t bx, uint32_t by, uint32_t w, uint32_t h, uint8_t *out)
{
    int c[16][3], lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++) {
        uint32_t x = bx + (i & 3), y = by + (i >> 2);
        if (x >= w) x = w - 1;
        if (y >= h) y = h - 1;
        uint32_t v = px[(size_t)y * pitch + x];
        c[i][0] = v >> 16 & 255; c[i][1] = v >> 8 & 255; c[i][2] = v & 255;
        for (int k = 0; k < 3; k++) { if (c[i][k] < lo[k]) lo[k] = c[i][k]; if (c[i][k] > hi[k]) hi[k] = c[i][k]; }
    }
    for (int k = 0; k < 3; k++) { int in = (hi[k] - lo[k]) / 16; lo[k] += in; hi[k] -= in; }
    uint16_t c0 = (uint16_t)((hi[0] * 31 + 127) / 255 << 11 | (hi[1] * 63 + 127) / 255 << 5 | (hi[2] * 31 + 127) / 255);
    uint16_t c1 = (uint16_t)((lo[0] * 31 + 127) / 255 << 11 | (lo[1] * 63 + 127) / 255 << 5 | (lo[2] * 31 + 127) / 255);
    uint32_t idx = 0;
    if (c0 != c1) {
        int ax[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}, len = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2];
        static const uint8_t pick[6] = {1, 3, 3, 2, 2, 0};   // t in sixths, from lo (1) to hi (0); 3 = 1/3, 2 = 2/3 of the way
        for (int i = 0; i < 16; i++) {
            int d = (c[i][0] - lo[0]) * ax[0] + (c[i][1] - lo[1]) * ax[1] + (c[i][2] - lo[2]) * ax[2];
            int t = len ? d * 6 / len : 0;
            idx |= (uint32_t)pick[t < 0 ? 0 : t > 5 ? 5 : t] << (2 * i);
        }
        if (c0 < c1) {   // four-colour mode needs c0 > c1: swap the ends, and 0<->1, 2<->3 in every index
            uint16_t tmp = c0; c0 = c1; c1 = tmp;
            idx ^= 0x55555555u;
        }
    }
    out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8); out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
    memcpy(out + 4, &idx, 4);
}

// A whole 4x4 block (bx + 4 <= w, by + 4 <= h) as dxt1_block makes it, with SSE2: the same arithmetic on all 16 texels
// at once (range by byte min/max, each texel's place along it by 16-bit multiply-adds), so the same bytes; checked
// against it on an HD background and on random texels. A 4096x2048 background took 134 ms one block at a time on a
// Steam Deck, 41 ms this way (53 / 20 ms on a desktop).
static void dxt1_block_sse2(const uint32_t *px, uint32_t pitch, uint32_t bx, uint32_t by, uint8_t *out)
{
    const uint32_t *p = px + (size_t)by * pitch + bx;
    __m128i row[4] = {_mm_loadu_si128((const __m128i *)p), _mm_loadu_si128((const __m128i *)(p + pitch)),
                      _mm_loadu_si128((const __m128i *)(p + 2 * pitch)), _mm_loadu_si128((const __m128i *)(p + 3 * pitch))};
    __m128i mn = _mm_min_epu8(_mm_min_epu8(row[0], row[1]), _mm_min_epu8(row[2], row[3]));
    __m128i mx = _mm_max_epu8(_mm_max_epu8(row[0], row[1]), _mm_max_epu8(row[2], row[3]));
    mn = _mm_min_epu8(mn, _mm_shuffle_epi32(mn, _MM_SHUFFLE(1, 0, 3, 2)));
    mn = _mm_min_epu8(mn, _mm_shuffle_epi32(mn, _MM_SHUFFLE(2, 3, 0, 1)));
    mx = _mm_max_epu8(mx, _mm_shuffle_epi32(mx, _MM_SHUFFLE(1, 0, 3, 2)));
    mx = _mm_max_epu8(mx, _mm_shuffle_epi32(mx, _MM_SHUFFLE(2, 3, 0, 1)));
    uint32_t lov = (uint32_t)_mm_cvtsi128_si32(mn), hiv = (uint32_t)_mm_cvtsi128_si32(mx);
    int lo[3] = {(int)(lov >> 16 & 255), (int)(lov >> 8 & 255), (int)(lov & 255)};
    int hi[3] = {(int)(hiv >> 16 & 255), (int)(hiv >> 8 & 255), (int)(hiv & 255)};
    for (int k = 0; k < 3; k++) { int in = (hi[k] - lo[k]) / 16; lo[k] += in; hi[k] -= in; }
    uint16_t c0 = (uint16_t)((hi[0] * 31 + 127) / 255 << 11 | (hi[1] * 63 + 127) / 255 << 5 | (hi[2] * 31 + 127) / 255);
    uint16_t c1 = (uint16_t)((lo[0] * 31 + 127) / 255 << 11 | (lo[1] * 63 + 127) / 255 << 5 | (lo[2] * 31 + 127) / 255);
    uint32_t idx = 0;
    if (c0 != c1) {
        int ax[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}, len = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2];
        static const uint8_t pick[6] = {1, 3, 3, 2, 2, 0};
        // texels in memory B, G, R, A: as 16-bit lanes, two texels a register; the texel's offset from lo times the
        // axis, A's lane times 0
        __m128i zero = _mm_setzero_si128();
        __m128i lo16 = _mm_set_epi16(0, (short)lo[0], (short)lo[1], (short)lo[2], 0, (short)lo[0], (short)lo[1], (short)lo[2]);
        __m128i ax16 = _mm_set_epi16(0, (short)ax[0], (short)ax[1], (short)ax[2], 0, (short)ax[0], (short)ax[1], (short)ax[2]);
        // t = d * 6 / len clamped to 0..5 (d * 6 / len rounds toward zero, so a negative d is 0): how many of
        // len .. 5 len 6d reaches, d6 > k len - 1 for each
        __m128i lim[5];
        for (int k = 0; k < 5; k++) lim[k] = _mm_set1_epi32((k + 1) * len - 1);
        for (int r = 0; r < 4; r++) {
            __m128i m0 = _mm_madd_epi16(_mm_sub_epi16(_mm_unpacklo_epi8(row[r], zero), lo16), ax16);   // texels 0, 1
            __m128i m1 = _mm_madd_epi16(_mm_sub_epi16(_mm_unpackhi_epi8(row[r], zero), lo16), ax16);   // texels 2, 3
            m0 = _mm_add_epi32(m0, _mm_srli_epi64(m0, 32));   // lanes 0 and 2: d of each texel
            m1 = _mm_add_epi32(m1, _mm_srli_epi64(m1, 32));
            __m128i d = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(m0), _mm_castsi128_ps(m1), _MM_SHUFFLE(2, 0, 2, 0)));
            __m128i d6 = _mm_add_epi32(_mm_slli_epi32(d, 2), _mm_slli_epi32(d, 1));
            __m128i t = _mm_setzero_si128();
            for (int k = 0; k < 5; k++) t = _mm_sub_epi32(t, _mm_cmpgt_epi32(d6, lim[k]));   // each true lane is -1
            int ts[4];
            _mm_storeu_si128((__m128i *)ts, t);
            for (int j = 0; j < 4; j++) idx |= (uint32_t)pick[len ? ts[j] : 0] << (2 * (r * 4 + j));
        }
        if (c0 < c1) {
            uint16_t tmp = c0; c0 = c1; c1 = tmp;
            idx ^= 0x55555555u;
        }
    }
    out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8); out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
    memcpy(out + 4, &idx, 4);
}

// The pack picture px (w x h ARGB; overwritten by its mip levels) as a texture, its memory in *bytes: DXT1 when `dxt`
// (an opaque background: an eighth of the memory, so a room's animated backgrounds stay kept), else A8R8G8B8; every
// mip level box-filtered when `mips`.
// A cut-out picture (alpha all but on or off: grass, leaves, railings) loses its thin parts in the smaller mip levels:
// averaged with the clear around them they drop under the game's alpha test, and blades seen at a distance or edge-on
// break into dots. Each smaller level's alpha is scaled so as many texels pass half its full alpha as did at full size
// (alpha-tested mipmaps keeping their coverage). Pictures with real soft alpha (fire, glows) are left as they are.
struct cutout { int on; uint32_t full; double cover; };
static void cutout_measure_full(const uint32_t *px, uint32_t w, uint32_t h, struct cutout *c)
{
    size_t n = (size_t)w * h, mid = 0, pass = 0;
    uint32_t full = 0;
    for (size_t i = 0; i < n; i++) if (px[i] >> 24 > full) full = px[i] >> 24;
    for (size_t i = 0; i < n; i++) {
        uint32_t a = px[i] >> 24;
        if (a * 10 > full && a * 10 < full * 9) mid++;
        if (a * 2 >= full) pass++;
    }
    c->full = full; c->cover = n ? (double)pass / n : 0;
    c->on = full >= 0x40 && mid * 50 < n && pass * 50 > n && pass * 50 < n * 49;   // mostly on/off, some of each
}
static void cutout_keep(uint32_t *px, uint32_t w, uint32_t h, const struct cutout *c)
{
    size_t n = (size_t)w * h;
    double lo = 1, hi = 8;
    for (int it = 0; it < 16; it++) {   // the smallest scale that reaches the full-size share
        double s = (lo + hi) / 2;
        size_t pass = 0;
        for (size_t i = 0; i < n; i++) if ((px[i] >> 24) * s * 2 >= c->full) pass++;
        if ((double)pass / n < c->cover) lo = s; else hi = s;
    }
    for (size_t i = 0; i < n; i++) {
        double a = (px[i] >> 24) * hi;
        px[i] = (px[i] & 0xFFFFFF) | (uint32_t)(a > c->full ? c->full : a + 0.5) << 24;
    }
}

static IDirect3DTexture9 *pack_texture(uint32_t *px, uint32_t w, uint32_t h, int mips, int dxt, uint32_t *bytes)
{
    IDirect3DTexture9 *t = 0;
    struct cutout cut = {0};
    if (mips && !dxt) cutout_measure_full(px, w, h, &cut);
    if (FAILED(tex_create(w, h, mips ? 0 : 1, dxt ? D3DFMT_DXT1 : D3DFMT_A8R8G8B8, &t))) return 0;
    *bytes = dxt ? w * h / 2 : w * h * 4;
    if (mips) *bytes = *bytes / 3 * 4;
    for (DWORD lv = 0, n = IDirect3DTexture9_GetLevelCount(t); lv < n; lv++) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(tex_lock(t, lv, &lr, 0))) {
            if (dxt)
                for (uint32_t by = 0; by < h; by += 4)
                    for (uint32_t bx = 0; bx < w; bx += 4) {
                        uint8_t *out = (uint8_t *)lr.pBits + (by / 4) * lr.Pitch + (bx / 4) * 8;
                        if (bx + 4 <= w && by + 4 <= h) dxt1_block_sse2(px, w, bx, by, out);
                        else dxt1_block(px, w, bx, by, w, h, out);
                    }
            else
                for (uint32_t y = 0; y < h; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, px + (size_t)y * w, w * 4);
            tex_unlock(t, lv);
        }
        uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;   // next level, in place (writes stay behind reads)
        for (uint32_t y = 0; lv + 1 < n && y < nh; y++) {
            for (uint32_t xx = 0; xx < nw; xx++) {
                const uint32_t *a = px + (y * 2 % h) * w, *b = px + ((y * 2 + 1) % h) * w;
                uint32_t x0 = xx * 2 % w, x1 = (xx * 2 + 1) % w, q[4] = {a[x0], a[x1], b[x0], b[x1]};
                // colour weighted by alpha: a see-through texel's colour (often the cut-out's background) doesn't
                // bleed into the edge of what's drawn
                uint32_t al = 0, v;
                for (int i = 0; i < 4; i++) al += q[i] >> 24;
                v = (al + 2) / 4 << 24;
                for (int s = 0; s < 24; s += 8) {
                    uint32_t c = 0;
                    for (int i = 0; i < 4; i++) c += (q[i] >> s & 255) * (al ? q[i] >> 24 : 1);
                    v |= (al ? (c + al / 2) / al : (c + 2) / 4) << s;
                }
                px[y * nw + xx] = v;
            }
        }
        w = nw; h = nh;
        if (lv + 1 < n && cut.on) cutout_keep(px, w, h, &cut);
    }
    return t;
}
static int all_opaque(const uint32_t *px, size_t n) { for (size_t i = 0; i < n; i++) if (px[i] >> 24 != 0xFF) return 0; return 1; }

// Palette alpha in a pack file runs 0-0x80, as the game's own; anything above marks a smooth-edged redraw
static int soft_alpha(const uint32_t *px, size_t n) { for (size_t i = 0; i < n; i++) if (px[i] >> 24 > 0x80) return 1; return 0; }
static int whole_multiple(const char *name, uint32_t w, uint32_t h, const struct xtex_info *t)
{
    // up to 16x: a bigger file (a 16384 x 16384 picture for a 16 x 16 texture) is a gigabyte to decode, for nothing
    if (!(w % t->w || h % t->h || w / t->w != h / t->h || w / t->w > 16)) return 1;
    static int warned;
    if (warned++ < 20) xlog("[texpack] %s is %ux%u: not the original %ux%u times a whole number, not used", name, w, h, t->w, t->h);
    return 0;
}

// The pack file for t (name, or its _$ wild form) made into a texture: info and bytes for the kept cache, *got the
// name it was found under. 0 when there's no file or it isn't the original's size times a whole number (*bad = 1:
// not worth trying again), or the texture couldn't be made (*bad = 0: out of memory, say, maybe not next time).
static IDirect3DTexture9 *make_replacement(const struct xtex_info *t, const char *name, const char *wild, struct pack_info *info,
                                           uint32_t *bytes, const char **got, int *bad)
{
    *bad = 1;
    uint32_t w, hh, *px = texpack_load_either(name, wild, &w, &hh, got);
    if (!px) return 0;
    if (!whole_multiple(name, w, hh, t)) { free(px); return 0; }
    *bad = 0;
    // Mip levels also where the game's own has none: an HD picture is 4x or more the original, so on a model at any
    // distance it is drawn smaller than it is, and without mips that samples scattered texels (grass blades turned to
    // dotted lines). Not for backgrounds (DXT1, drawn at screen size).
    *info = (struct pack_info){w, hh, t->levels > 1 || t->kind != F_DXT1, soft_alpha(px, (size_t)w * hh)};
    IDirect3DTexture9 *tex = pack_texture(px, w, hh, info->mips, t->kind == F_DXT1 && all_opaque(px, (size_t)w * hh), bytes);
    free(px);
    if (!tex) xlog("[texpack] %s (%ux%u) couldn't be made into a texture (out of memory?): tried again later", *got, w, hh);
    return tex;
}

// Pack textures other than room backgrounds are made off the game thread, two at a time: on a miss the game's own
// texture is drawn meanwhile (a frame or a few) and the entry tries again when pack_ready moves (apply_stage_state).
// Backgrounds (opaque DXT1, _0c_) stay on the game thread: a camera cut showing the low-resolution picture first would
// be seen, and the read-ahead has them made before the cut anyway.
volatile LONG pack_ready;
static char pack_queued[8][64];   // being made (under pack_lock)
static uint64_t pack_failed[1 << 12];   // names whose file didn't make a texture (wrong size): not tried again
static uint32_t npack_failed;
struct pack_job { char name[64], wild[64]; struct xtex_info t; };
static DWORD WINAPI pack_job_run(void *p)
{
    struct pack_job *job = p;
    CoInitializeEx(0, COINIT_MULTITHREADED);   // texpack_load reads through WIC
    struct pack_info info;
    uint32_t bytes = 0;
    const char *got;
    LONGLONG t0 = qpc();
    int bad, kept = 0;
    IDirect3DTexture9 *tex = make_replacement(&job->t, job->name, job->wild, &info, &bytes, &got, &bad);
    double ms = qpc_ms(qpc() - t0);
    // Made but too big to keep (over a quarter of the cache): it was released here, the entry asked again when
    // pack_ready moved, and the picture was decoded over and over for as long as it was on screen.
    if (tex) { kept = pack_keep(got, tex, bytes, &info); IDirect3DTexture9_Release(tex); }
    if (kept < 0) xlog("[texpack] %s is too big for the texture cache (%u MB): not used", got, bytes >> 20);
    if (ms > 50) xlog("[texpack] %s made off the game thread in %.1f ms", job->name, ms);
    AcquireSRWLockExclusive(&pack_lock);
    for (int i = 0; i < 8; i++) if (!strcmp(pack_queued[i], job->name)) pack_queued[i][0] = 0;
    // only a file that can't work: one that failed for lack of memory is tried again
    if ((!tex && bad) || kept < 0) keyset_add(pack_failed, 1 << 12, &npack_failed, fnv64_str(FNV64_START, job->name));
    ReleaseSRWLockExclusive(&pack_lock);
    InterlockedIncrement(&pack_ready);   // also when it failed: the waiting entries go back to the game's own texture
    free(job);
    CoUninitialize();
    return 0;
}
// 1: being made now (queued, or already); 0: no such pack file, or two are being made (tried again when one is done)
static int pack_queue(const struct xtex_info *t, const char *name, const char *wild)
{
    if (!texpack_has(name) && !(strcmp(name, wild) && texpack_has(wild))) return 0;
    int at = -1, busy = 0;
    uint64_t key = fnv64_str(FNV64_START, name) | 1;
    AcquireSRWLockExclusive(&pack_lock);
    for (uint32_t i = (uint32_t)key & ((1 << 12) - 1); pack_failed[i]; i = (i + 1) & ((1 << 12) - 1))
        if (pack_failed[i] == key) { ReleaseSRWLockExclusive(&pack_lock); return 0; }
    for (int i = 0; i < 8; i++) {
        if (!strcmp(pack_queued[i], name)) { ReleaseSRWLockExclusive(&pack_lock); return 1; }
        if (pack_queued[i][0]) busy++; else if (at < 0) at = i;
    }
    struct pack_job *job = busy < 2 && at >= 0 ? calloc(1, sizeof *job) : 0;
    if (job) {
        snprintf(job->name, sizeof job->name, "%s", name); snprintf(job->wild, sizeof job->wild, "%s", wild); job->t = *t;
        snprintf(pack_queued[at], sizeof pack_queued[at], "%s", name);
    }
    ReleaseSRWLockExclusive(&pack_lock);
    if (!job) return 1;   // waits for one of the two to finish
    HANDLE th = CreateThread(0, 0, pack_job_run, job, 0, 0);
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); return 1; }
    AcquireSRWLockExclusive(&pack_lock); pack_queued[at][0] = 0; ReleaseSRWLockExclusive(&pack_lock);
    free(job);
    return 0;
}

int load_replacement(struct tex_entry *h, const struct xtex_info *t, const char *name, const char *wild)
{
    struct pack_info info;
    IDirect3DTexture9 *kept = pack_find(name, &info);
    if (!kept && strcmp(name, wild)) kept = pack_find(wild, &info);
    // a kept one without mips where it should have them (read ahead, guessed from the name) is made again below;
    // extra mips where it has none are harmless
    if (kept && (info.mips || (t->levels <= 1 && t->kind == F_DXT1))) {
        if (!whole_multiple(name, info.w, info.h, t)) { IDirect3DTexture9_Release(kept); return 0; }
        if (h->tex) IDirect3DTexture9_Release(h->tex);
        h->tex = kept;
        h->soft_alpha = info.soft;
        return 1;
    }
    if (kept) IDirect3DTexture9_Release(kept);
    if (!strstr(name, "_0c_")) return pack_queue(t, name, wild) ? -1 : 0;   // made off the game thread
    const char *got;
    uint32_t bytes = 0;
    int bad;
    IDirect3DTexture9 *tex = make_replacement(t, name, wild, &info, &bytes, &got, &bad);
    if (!tex) return 0;
    if (h->tex) IDirect3DTexture9_Release(h->tex);
    h->tex = tex;
    h->soft_alpha = info.soft;
    pack_keep(got, h->tex, bytes, &info);
    return 1;
}

// A room's textures read ahead: when the game opens image\bgs\roomNNN.bgs (kernel.c), two threads make the pack's
// versions into the kept textures, so the camera cuts and animated backgrounds that show them don't wait for the
// JPG/PNG: first each camera's picture, named as tex_name does (one XPR0 per camera, a DXT1 texture: FNV-1a 64 over
// its texels, every mip level, as tools/hdpack/gamefiles.py bg_names; the first BG_AHEAD), then every texture
// room_textures.txt lists for the room (room_note: animated background frames, characters, props), in the order first
// seen. A newer room stops an older room's threads.
#define BG_AHEAD 24
static volatile LONG bg_gen;
struct bg_job { wchar_t path[MAX_PATH]; char room[16]; LONG gen, next, refs; };
static char (*room_list)[2][64];   // room_textures.txt as read at the first room: {room, name}
static int room_list_n;
// Both lists are read: the one recorded on this machine and one a pack ships (the highest enabled mod that has a
// room_textures.txt), so a fresh install reads ahead on first visits too. A texture in both is made once.
static void room_list_load(void)
{
    static int tried;
    if (tried++) return;
    wchar_t path[2][MAX_PATH];
    host_path(path[0], L"logs\\room_textures.txt");
    int lists = 1 + mod_file(L"room_textures.txt", path[1], MAX_PATH);
    int cap = 0;
    for (int k = 0; k < lists; k++) {
        FILE *f = _wfopen(path[k], L"r");
        char line[160], r[64], t[64];
        while (f && fgets(line, sizeof line, f)) {
            if (line[0] == ';' || sscanf(line, "%63s %63s", r, t) != 2) continue;
            if (room_list_n == cap) { void *m = realloc(room_list, (cap = cap ? cap * 2 : 1024) * sizeof *room_list); if (!m) break; room_list = m; }
            strcpy(room_list[room_list_n][0], r); strcpy(room_list[room_list_n][1], t); room_list_n++;
        }
        if (f) fclose(f);
    }
}
// The pack's texture `name` (or its _$ version) into the kept textures, unless there already. 1: made.
static int pack_ahead(const char *name)
{
    char wild[64];
    int u = texpack_wild_name(name, wild, sizeof wild);
    IDirect3DTexture9 *t = pack_find(name, 0);
    if (!t && u) t = pack_find(wild, 0);
    if (t) { IDirect3DTexture9_Release(t); return 0; }
    const char *got;
    uint32_t w, h, *px = texpack_load_either(name, wild, &w, &h, &got);
    if (!px) return 0;
    int dxt = strstr(name, "_0c_") && all_opaque(px, (size_t)w * h);
    struct pack_info info = {w, h, !strstr(name, "_0c_"), soft_alpha(px, (size_t)w * h)};   // 3D textures with mips: a guess
    uint32_t bytes = 0;                                                                      // load_replacement checks
    t = pack_texture(px, w, h, info.mips, dxt, &bytes);
    free(px);
    if (!t) return 0;
    pack_keep(got, t, bytes, &info);
    IDirect3DTexture9_Release(t);
    return 1;
}
static DWORD WINAPI bg_list_thread(void *p)
{
    struct bg_job *job = p;
    CoInitializeEx(0, COINIT_MULTITHREADED);
    int made = 0;
    for (LONG i; job->gen == bg_gen && (i = InterlockedIncrement(&job->next) - 1) < room_list_n; )
        if (!strcmp(room_list[i][0], job->room)) made += pack_ahead(room_list[i][1]);
    if (made) xlog("[texpack] read ahead %d listed textures of %s", made, job->room);
    if (!InterlockedDecrement(&job->refs)) free(job);
    CoUninitialize();
    return 0;
}
// The pack name of the XPR0 at d + o (a room's camera picture: DXT1, one level), or 0 if it isn't one.
static int bg_name(const uint8_t *d, DWORD size, DWORD o, char *name, size_t cap)
{
    if ((uint64_t)o + 32 > size || memcmp(d + o, "XPR0", 4)) return 0;
    uint32_t hsize, common, data, fmt;
    memcpy(&hsize, d + o + 8, 4); memcpy(&common, d + o + 12, 4); memcpy(&data, d + o + 16, 4); memcpy(&fmt, d + o + 24, 4);
    uint32_t code = fmt >> 8 & 255, w = 1u << (fmt >> 20 & 15), h = 1u << (fmt >> 24 & 15), levels = fmt >> 16 & 15;
    if (common == 0xffffffffu || code != 0x0c || levels > 1) return 0;   // backgrounds: DXT1, one level
    uint32_t n = ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if ((uint64_t)o + hsize + data + n > size) return 0;
    pack_name(name, 0, cap, w, h, code, pack_texel_hash(d + o + hsize + data, n), 0, 0);   // as tex_name names it
    return 1;
}

// Doors: a room's script (its .rdt's section 8) holds a command per door, "[id] 31 00 00", eight 16-bit values (the
// door's area, where you arrive, which way you face), then the stage, room and camera it leads to (PLAN.md, room 212).
// While the room's own textures are read ahead, the picture of each door's arrival camera is too: stepping through,
// the next room's first camera was the slowest pack load left (100-230 ms, stalls.txt 2026-10-23). The camera is the
// target .bgs's n-th XPR0 (one per camera). A wrong guess (a door whose camera number means something else: some lead
// to "room 600" with numbers past its 8 cameras) only reads a picture ahead for nothing, or none.
// ponytail: a byte search for the door command; a stray match only costs a wasted read-ahead.
// Each target camera's pack name is kept once found: the .bgs files are up to ~18 MB and a room has up to 16 doors, so
// reading them all at every room change competed with the game's own room loading. The thread also reads at background
// I/O priority while it does this.
static struct { char tgt[8]; uint8_t cam; char name[64]; } door_names[512];
static int ndoor_names;
static SRWLOCK door_lock = SRWLOCK_INIT;
static int door_name(const char *tgt, uint8_t cam, char *name, size_t cap)   // 1: known (name filled, "" for none)
{
    int hit = 0;
    AcquireSRWLockShared(&door_lock);
    for (int i = 0; i < ndoor_names && !hit; i++)
        if (door_names[i].cam == cam && !strcmp(door_names[i].tgt, tgt)) { snprintf(name, cap, "%s", door_names[i].name); hit = 1; }
    ReleaseSRWLockShared(&door_lock);
    return hit;
}
static void door_name_keep(const char *tgt, uint8_t cam, const char *name)
{
    AcquireSRWLockExclusive(&door_lock);
    if (ndoor_names < (int)(sizeof door_names / sizeof *door_names)) {
        snprintf(door_names[ndoor_names].tgt, sizeof door_names[0].tgt, "%s", tgt);
        door_names[ndoor_names].cam = cam;
        snprintf(door_names[ndoor_names].name, sizeof door_names[0].name, "%s", name);
        ndoor_names++;
    }
    ReleaseSRWLockExclusive(&door_lock);
}
static void door_ahead_(const char *room, LONG gen);
static void door_ahead(const char *room, LONG gen)
{
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   // low I/O priority as well as CPU
    door_ahead_(room, gen);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}
static void door_ahead_(const char *room, LONG gen)
{
    if (strlen(room) != 7 || _strnicmp(room, "ROOM", 4)) return;
    wchar_t path[MAX_PATH], rel[64];
    swprintf(rel, 64, L"image\\rdt\\room%hs.rdt", room + 4);
    game_file(path, rel);
    uint32_t size = 0, made = 0, doors = 0;
    uint8_t *d = read_file(path, 64u << 20, &size);
    uint32_t n = 0, off = 0, len = 0;
    if (d && size >= 8 + 12 * 9) { memcpy(&n, d + 4, 4); memcpy(&off, d + 8 + 12 * 8 + 4, 4); memcpy(&len, d + 8 + 12 * 8 + 8, 4); }
    if (!d || n < 9 || (uint64_t)off + len > size) { free(d); return; }
    char seen[16][8];
    int nseen = 0;
    for (uint32_t p = off + 1; p + 22 <= off + len && gen == bg_gen && nseen < 16; p++) {
        if (d[p] != 0x31 || d[p + 1] || d[p + 2]) continue;
        uint32_t st = d[p + 19], rm = d[p + 20], cam = d[p + 21];
        if (st > 6 || rm >= 0x40) continue;
        char tgt[8];
        snprintf(tgt, sizeof tgt, "%u%02u", st, rm);   // rooms are decimal: roomSRR (G_ROOM)
        int dup = !_stricmp(tgt, room + 4);
        for (int i = 0; i < nseen && !dup; i++) dup = !strcmp(seen[i], tgt) && seen[i][7] == (char)cam;
        if (dup) continue;
        memcpy(seen[nseen], tgt, 7); seen[nseen][7] = (char)cam; nseen++;
        doors++;
        char name[64] = "";
        if (door_name(tgt, (uint8_t)cam, name, sizeof name)) { if (name[0]) made += pack_ahead(name); continue; }
        swprintf(rel, 64, L"image\\bgs\\room%hs.bgs", tgt);
        game_file(path, rel);
        uint32_t bsize = 0;
        uint8_t *b = read_file(path, 256u << 20, &bsize);
        uint32_t k = 0;
        for (DWORD o = 0; b && o + 32 <= bsize; o++) {
            if (memcmp(b + o, "XPR0", 4)) continue;
            if (k++ != cam) continue;
            if (!bg_name(b, bsize, o, name, sizeof name)) name[0] = 0;
            break;
        }
        if (b) door_name_keep(tgt, (uint8_t)cam, name);   // "" too: that camera has no picture to read ahead
        if (name[0]) made += pack_ahead(name);
        free(b);
    }
    free(d);
    if (doors) xlog("[texpack] read ahead %u door cameras of %s (%u doors)%s", made, room, doors, gen == bg_gen ? "" : ", stopped: another room");
}

// The camera pictures are made by BG_WORKERS threads at once (bg_thread and its helpers), taking the next name in
// turn. One after another, a Steam Deck took 150-450 ms each (decoding a 4096x2048 JPG, making it DXT1): room 007's
// were still being made when its cutscene cut to them, and each one missed was made on the game thread, a stutter.
#define BG_WORKERS 3
struct bg_work { char (*names)[64]; LONG n, gen; volatile LONG next, made; };
static void bg_work_run(struct bg_work *w)
{
    for (LONG i; w->gen == bg_gen && w->made < BG_AHEAD && (i = InterlockedIncrement(&w->next) - 1) < w->n; )
        if (pack_ahead(w->names[i])) InterlockedIncrement(&w->made);
}
static DWORD WINAPI bg_helper(void *p)
{
    CoInitializeEx(0, COINIT_MULTITHREADED);   // texpack_load reads through WIC
    bg_work_run(p);
    CoUninitialize();
    return 0;
}

static DWORD WINAPI bg_thread(void *p)
{
    struct bg_job *job = p;
    CoInitializeEx(0, COINIT_MULTITHREADED);   // texpack_load reads through WIC
    HANDLE f = CreateFileW(job->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    DWORD size = f != INVALID_HANDLE_VALUE ? GetFileSize(f, 0) : 0, got = 0;
    uint8_t *d = size && size < (256u << 20) ? malloc(size) : 0;
    if (!d || !ReadFile(f, d, size, &got, 0) || got != size) size = 0;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    // the room's camera pictures not kept yet, in file order (on this thread's stack: the helpers use them only while
    // it waits for them); then made by BG_WORKERS threads, the first BG_AHEAD
    char nm[64][64];
    int n = 0, kept = 0;
    for (DWORD o = 0; d && o + 32 <= size && n < 64 && job->gen == bg_gen; o++) {
        char name[64];
        if (!bg_name(d, size, o, name, sizeof name)) continue;
        IDirect3DTexture9 *t = pack_find(name, 0);
        if (t) { IDirect3DTexture9_Release(t); kept++; continue; }
        memcpy(nm[n++], name, sizeof name);
    }
    struct bg_work w = {nm, n, job->gen, 0, 0};
    HANDLE helper[BG_WORKERS - 1];
    int helpers = 0;
    for (int i = 0; i < BG_WORKERS - 1 && n > 1; i++)
        if ((helper[helpers] = CreateThread(0, 0, bg_helper, &w, 0, 0))) SetThreadPriority(helper[helpers++], THREAD_PRIORITY_BELOW_NORMAL);
    bg_work_run(&w);
    for (int i = 0; i < helpers; i++) { WaitForSingleObject(helper[i], INFINITE); CloseHandle(helper[i]); }
    int made = w.made;
    xlog("[texpack] read ahead %d backgrounds of %ls (%d already there)%s", made, wcsrchr(job->path, L'\\') ? wcsrchr(job->path, L'\\') + 1 : job->path,
         kept, job->gen == bg_gen ? "" : ", stopped: another room");
    free(d);
    char room[16];
    LONG gen = job->gen;
    memcpy(room, job->room, sizeof room);
    bg_list_thread(job);   // then help with the room's list (frees the job when the other thread is done too)
    door_ahead(room, gen);   // then the neighbouring rooms' arrival cameras
    CoUninitialize();
    return 0;
}

void bg_prefetch(const wchar_t *path)
{
    static wchar_t last[MAX_PATH];
    const wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (!dev || !texpack_replacing() || !_wcsicmp(last, base)) return;   // the game opens a room's file more than once
    wcsncpy(last, base, MAX_PATH - 1);
    room_list_load();
    struct bg_job *job = calloc(1, sizeof *job);
    if (!job) return;
    wcsncpy(job->path, path, MAX_PATH - 1); job->path[MAX_PATH - 1] = 0;
    snprintf(job->room, sizeof job->room, "%.*ls", (int)(wcslen(base) > 4 ? wcslen(base) - 4 : wcslen(base)), base);
    for (char *c = job->room; *c; c++) *c = (char)toupper((uint8_t)*c);
    job->gen = InterlockedIncrement(&bg_gen);
    job->refs = 2;
    HANDLE th = CreateThread(0, 0, bg_thread, job, 0, 0), th2 = th ? CreateThread(0, 0, bg_list_thread, job, 0, 0) : 0;
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); } else { free(job); return; }
    if (th2) { SetThreadPriority(th2, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th2); } else InterlockedDecrement(&job->refs);
}

// An animated background's frame sheets: xmovNNN*.xpr is one XPR0 holding 2048x1024 DXT1 sheets (17 for the ogre's
// attack in room 007), and the game moves to the next sheet every few frames. Made one after another as they came up,
// each took ~200 ms (stalls.txt, 2026-10-23): a scene running at 5 fps. Opened as the room is entered, so two threads
// make them all into the kept textures first. Not stopped by a newer room: a file is at most a few dozen sheets.
struct xmov_job { uint8_t *d; DWORD size; LONG next, refs, made; wchar_t name[64], path[MAX_PATH]; };
static DWORD WINAPI xmov_thread(void *p)
{
    struct xmov_job *job = p;
    CoInitializeEx(0, COINIT_MULTITHREADED);
    uint32_t hsize;
    memcpy(&hsize, job->d + 8, 4);
    if (hsize > job->size) hsize = job->size;   // a short or damaged file: the header table can't run past its end
    for (LONG k; (k = InterlockedIncrement(&job->next) - 1) < 64 && 12 + 20 * (uint32_t)k + 20 <= hsize; ) {
        uint32_t e[5];   // common, data, lock, format, size
        memcpy(e, job->d + 12 + 20 * k, sizeof e);
        if (e[0] == 0xffffffffu) break;
        uint32_t code = e[3] >> 8 & 255, w = 1u << (e[3] >> 20 & 15), h = 1u << (e[3] >> 24 & 15), levels = e[3] >> 16 & 15, n = 0;
        if (code != 0x0c) continue;
        for (uint32_t i = 0, lw = w, lh = h; i < (levels ? levels : 1); i++, lw = lw > 1 ? lw / 2 : 1, lh = lh > 1 ? lh / 2 : 1)
            n += ((lw + 3) / 4) * ((lh + 3) / 4) * 8;   // every mip level, as tex_name hashes them
        if ((uint64_t)hsize + e[1] + n > job->size) continue;
        char name[64];
        pack_name(name, 0, sizeof name, w, h, code, pack_texel_hash(job->d + hsize + e[1], n), 0, 0);
        if (pack_ahead(name)) InterlockedIncrement(&job->made);
    }
    if (!InterlockedDecrement(&job->refs)) {
        if (job->made) xlog("[texpack] read ahead %ld frame sheets of %ls", job->made, job->name);
        free(job->d); free(job);
    }
    CoUninitialize();
    return 0;
}

// The file is read by the first worker, not in the game's open call: up to 128 MB (the ogre's is 17.8) read before
// NtCreateFile returned was 100-200 ms of the game's thread on a hard disk. It then starts the second worker.
static DWORD WINAPI xmov_load(void *p)
{
    struct xmov_job *job = p;
    HANDLE f = CreateFileW(job->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    DWORD size = f != INVALID_HANDLE_VALUE ? GetFileSize(f, 0) : 0, got = 0;
    int ok = size > 32 && size < (128u << 20) && (job->d = malloc(size)) && ReadFile(f, job->d, size, &got, 0) && got == size
             && !memcmp(job->d, "XPR0", 4);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) { free(job->d); free(job); return 0; }
    job->size = size; job->refs = 2;
    HANDLE th = CreateThread(0, 0, xmov_thread, job, 0, 0);
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); }
    else job->refs = 1;
    return xmov_thread(job);
}
void xmov_prefetch(const wchar_t *path)
{
    if (!dev || !texpack_replacing()) return;
    const wchar_t *base = wcsrchr(path, L'\\');
    struct xmov_job *job = calloc(1, sizeof *job);
    if (!job || wcslen(path) >= MAX_PATH) { free(job); return; }
    wcscpy(job->path, path);
    wcsncpy(job->name, base ? base + 1 : path, 63);
    HANDLE th = CreateThread(0, 0, xmov_load, job, 0, 0);
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); }
    else free(job);
}

// At launch: the pack textures room_textures.txt lists before any room (START: the logos, the title, its menus) and
// those it lists in 3 rooms or more (the in-game menus, the pause screen: the game opens them anywhere), made into the
// kept textures by a thread meanwhile. Each was a 40-90 ms pause the first time its screen opened, every launch.
// At most START_AHEAD of them, and none once a room opens: the kept textures held 96 then, and more evicted each other and
// then the room's own read-ahead (the list has 137 such names; review of 2672ca8).
#define START_AHEAD 48
static int by_rooms(const void *a, const void *b) { return ((const int *)b)[1] - ((const int *)a)[1]; }
static DWORD WINAPI start_ahead_thread(void *p)
{
    (void)p;
    CoInitializeEx(0, COINIT_MULTITHREADED);
    int made = 0, want = 0, n = 0;
    int (*cand)[3] = malloc(sizeof *cand * (room_list_n + 1));   // {first line, rooms, START}, one per texture
    for (int i = 0; cand && i < room_list_n; i++) {
        int earlier = 0, rooms = 0, start = 0;
        for (int j = 0; j < i && !earlier; j++) earlier = !strcmp(room_list[j][1], room_list[i][1]);
        if (earlier) continue;   // each texture at its first line only
        for (int j = i; j < room_list_n; j++) {   // the rooms it's listed in (both lists may name the same one)
            if (strcmp(room_list[j][1], room_list[i][1])) continue;
            if (!strcmp(room_list[j][0], "START")) { start = 1; continue; }
            int again = 0;
            for (int k = i; k < j && !again; k++) again = !strcmp(room_list[k][1], room_list[i][1]) && !strcmp(room_list[k][0], room_list[j][0]);
            rooms += !again;
        }
        cand[n][0] = i; cand[n][1] = rooms; cand[n][2] = start; n++;
    }
    if (cand) qsort(cand, n, sizeof *cand, by_rooms);   // the most widely used first (stable enough: ties keep no order)
    AcquireSRWLockExclusive(&pack_lock);
    for (int c = 0; c < n && pack_shared_n < 32 && cand[c][1] >= 5; c++)
        if (strlen(room_list[cand[c][0]][1]) < sizeof pack_shared[0]) strcpy(pack_shared[pack_shared_n++], room_list[cand[c][0]][1]);
    for (int i = 0; i < PACK_SLOTS; i++) if (pack_kept[i].tex) pack_kept[i].shared = is_shared(pack_kept[i].name);
    ReleaseSRWLockExclusive(&pack_lock);
    for (int pass = 0; pass < 2; pass++)   // the START ones first: the title is up within half a second
    for (int c = 0; c < n && want < START_AHEAD && !bg_gen; c++) {
        if (pass ? cand[c][2] || cand[c][1] < 3 : !cand[c][2]) continue;
        want++;
        made += pack_ahead(room_list[cand[c][0]][1]);
    }
    free(cand);
    xlog("[texpack] read ahead %d of %d textures used before any room or in many rooms%s; %d kept for good (5 rooms or more)",
         made, want, bg_gen ? " (stopped: a room opened)" : "", pack_shared_n);
    CoUninitialize();
    return 0;
}
void start_prefetch(void)
{
    static int done;
    if (done || !dev) return;
    done = 1;
    if (!texpack_replacing()) return;
    room_list_load();
    HANDLE th = CreateThread(0, 0, start_ahead_thread, 0, 0, 0);
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); }
}
