// settings: enhanced_text. The game's menu and message font is one 256x256 sheet of 16x16 cells (image\etc\MOJIST_E.LDT,
// or kernel.c's MOJIST_FG copy in French/German), drawn by cell with the game's own width table. text_sheet redraws
// every cell with Kabuto Sans (tools/font; built into this DLL, host.rc) at k times the size, each letter placed
// where the original's ink is, so the game's text layout doesn't change.
//
// The result is an index image, not colours: fill 1, the anti-aliased edge between fill and outline 2-8, outline 9,
// transparent 0, the indices the original uses. d3d_post.c colours it with whatever palette the game draws the sheet with,
// so coloured and fading text keep working.
#include "host.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SS 4   // supersampling: GDI draws paths without anti-aliasing
#define TEXT_K_MAX 13
// Look, in game pixels: the fill is the font's shape thinned by THIN on every side, and the outline reaches EDGE past
// the font's shape (so EDGE + THIN past the fill), with mitred corners like the original's square-ish outline.
#define THIN 0.3f
#define EDGE 0.7f

// What each cell holds. code: the game's character code (its cell is code + 0x20); n: cells it spans.
enum { BASELINE, CENTRE, FIT };   // letters sit on the sheet's baseline; symbols centre on the original's ink; FIT fills it
struct item { uint8_t code, n, mode; const wchar_t *s; };

static const wchar_t symbols60[] =
    L"\u00b7\u2013-\u2192\u2190\u2191\u2193\u25cb\u00d7\u25b3\u25a1\ue000\ue001\ue002\ue003\u203e"   // 0x60-0x6f
    L"\uff3c\u203c\u2049\u00f7\u2026\u23cf\u3002\u300c\u300d\u3001\u300e\u300f\u25bc\u25b7\u00ae";   // 0x70-0x7e
// The accented letters: codes 0x82-0xb0 in the French/German copy (kernel.c european_font), in MOJIST_P's order.
static const wchar_t *const accents[47] = {
    L"\u00c4", L"\u00e4", L"\u00d6", L"\u00f6", L"\u00dc", L"\u00fc", L"\u00df", L"\u00c0", L"\u00e0", L"\u00c2", L"\u00e2", L"\u00c8",
    L"\u00e8", L"\u00c9", L"\u00e9", L"\u00ca", L"\u00ea", L"\u00ce", L"\u00ee", L"\u00cf", L"\u00ef", L"\u00d4", L"\u00f4", L"\u00d9",
    L"\u00f9", L"\u00c7", L"\u00e7", L"\u00d1", L"\u00f1", L"\u00cb", L"\u00eb", L"\u00c1", L"\u00e1", L"\u00cd", L"\u00ed", L"\u00d3",
    L"\u00f3", L"\u00da", L"\u00fa", L"\u00cc", L"\u00ec", L"\u00d2", L"\u00f2", L"N\u00ba", L"n\u00ba", L"\u00db", L"\u00fb"};

static int items(int european, struct item *out)
{
    static wchar_t one[0x60][2], sym[0x1f][2];
    int n = 0;
    for (int c = 1; c < 0x60; c++) {   // ASCII from '!', with the sheet's curly quotes
        one[c][0] = c == 0x02 ? 0x201d : c == 0x07 ? 0x2019 : c == 0x40 ? 0x2018 : c == 0x5f ? 0x201c : (wchar_t)(c + 0x20);
        out[n++] = (struct item){(uint8_t)c, 1, BASELINE, one[c]};
    }
    for (int c = 0x60; c < 0x7f; c++) { sym[c - 0x60][0] = symbols60[c - 0x60]; out[n++] = (struct item){(uint8_t)c, 1, CENTRE, sym[c - 0x60]}; }
    out[n++] = (struct item){0x80, 2, CENTRE, L"\u221e"};
    if (european) for (int i = 0; i < 47; i++) out[n++] = (struct item){(uint8_t)(0x82 + i), 1, FIT, accents[i]};
    else out[n++] = (struct item){0x82, 3, CENTRE, L"\ue004"};   // LvMAX (its codes are the accents in French/German)
    return n;
}

// The English sheet from the disc: 4-bit indices, one per byte. 0 if it can't be read.
static const uint8_t *original_sheet(void)
{
    static uint8_t sheet[65536], b[0x10000];
    static int state;   // 0 not read yet, 1 ok, -1 failed
    if (state) return state > 0 ? sheet : 0;
    state = -1;
    wchar_t path[MAX_PATH];
    game_file(path, L"image\\etc\\MOJIST_E.LDT");   // a mod's copy if it has one, as the game reads it
    DWORD got = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f != INVALID_HANDLE_VALUE) { ReadFile(f, b, sizeof b, &got, 0); CloseHandle(f); }
    uint32_t off = got >= 16 ? *(uint32_t *)(b + 12) : 0;   // LINK: the first entry {flag, offset, size} is the sheet
    uint64_t px = 0;                                         // 64-bit so offsets from the file can't wrap the checks
    if (off && (uint64_t)off + 0x30 <= got && !memcmp(b + off, "OIM ", 4) && b[off + 0x10] == 8 && b[off + 0x11] == 8 && b[off + 0x12] == 0x14)
        px = (uint64_t)off + *(uint32_t *)(b + off + 0x14);
    if (!px || px + 0x8000 > got) { xlog("[text] couldn't read the font sheet from %ls: enhanced_text off", path); return 0; }
    for (int i = 0; i < 0x8000; i++) { sheet[i * 2] = b[px + i] & 15; sheet[i * 2 + 1] = b[px + i] >> 4; }   // left texel: low nibble
    state = 1;
    return sheet;
}

// Kabuto Sans, from this DLL's resources (host.rc), for GDI in this process only (as FR_PRIVATE would be). 1: loaded.
int kabuto_font(void)
{
    static int state;
    if (state) return state > 0;
    HMODULE me = 0;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)kabuto_font, &me);
    HRSRC r = FindResourceW(me, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(me, r) : 0;
    DWORD fonts = 0;
    state = g && AddFontMemResourceEx(LockResource(g), SizeofResource(me, r), 0, &fonts) && fonts ? 1 : -1;
    if (state < 0) xlog("[text] couldn't load Kabuto Sans from the DLL: enhanced_text off");
    return state > 0;
}

// Ink bounding box {x0, y0, x1, y1} (x1, y1 exclusive) of a w x h block at (x0, y0) of a 256-wide index image; 0 if empty.
static int ink_box(const uint8_t *img, int x0, int y0, int w, int h, int box[4])
{
    box[0] = box[1] = 1 << 30; box[2] = box[3] = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (img[(y0 + y) * 256 + x0 + x]) {
                if (x < box[0]) box[0] = x;
                if (y < box[1]) box[1] = y;
                if (x > box[2]) box[2] = x;
                if (y > box[3]) box[3] = y;
            }
    box[2]++; box[3]++;
    return box[2] > 0;
}

// A GDI drawing surface: 32-bit DIB, white on black.
struct canvas { HDC dc; HBITMAP bm; HGDIOBJ old; uint32_t *px; int w, h; };
static int canvas_make(struct canvas *c, int w, int h)
{
    BITMAPINFO bi = {{sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB}};
    if (!(c->dc = CreateCompatibleDC(0)) || !(c->bm = CreateDIBSection(c->dc, &bi, DIB_RGB_COLORS, (void **)&c->px, 0, 0))) return 0;
    c->old = SelectObject(c->dc, c->bm);
    c->w = w; c->h = h;
    SetGraphicsMode(c->dc, GM_ADVANCED);
    SetBkMode(c->dc, TRANSPARENT);
    SetTextAlign(c->dc, TA_BASELINE | TA_LEFT);
    return 1;
}
static void canvas_free(struct canvas *c)
{
    if (c->bm) { SelectObject(c->dc, c->old); DeleteObject(c->bm); }
    if (c->dc) DeleteDC(c->dc);
    memset(c, 0, sizeof *c);
}

// The glyph's path, its baseline origin mapped by x' = sx x + dx, y' = sy y + dy, then everything below the baseline
// pulled up by `desc` (1: not), so descenders fit the cell without shrinking the rest of the letter.
static void glyph_path(struct canvas *c, HFONT font, const wchar_t *s, float sx, float sy, float dx, float dy, float desc)
{
    XFORM m = {sx, 0, 0, sy, dx, dy}, id = {1, 0, 0, 1, 0, 0};
    SelectObject(c->dc, font);
    SetWorldTransform(c->dc, &m);
    BeginPath(c->dc);
    TextOutW(c->dc, 0, 0, s, (int)wcslen(s));
    EndPath(c->dc);
    SetWorldTransform(c->dc, &id);   // the path is in device units now, so pens aren't scaled with it
    if (desc >= 1) return;
    static POINT pt[4096];
    static BYTE ty[4096];
    int n = GetPath(c->dc, pt, ty, 4096);
    if (n <= 0) return;
    for (int i = 0; i < n; i++) if (pt[i].y > dy) pt[i].y = (LONG)(dy + (pt[i].y - dy) * desc + 0.5f);
    BeginPath(c->dc);
    PolyDraw(c->dc, pt, ty, n);
    EndPath(c->dc);
}

// Draws the glyph (see glyph_path). outline: widened by `pen` canvas pixels on every side (mitred corners); else
// thinned by `pen` on every side.
static void draw(struct canvas *c, HFONT font, const wchar_t *s, float sx, float sy, float dx, float dy, float desc, int outline, int pen)
{
    PatBlt(c->dc, 0, 0, c->w, c->h, BLACKNESS);
    HGDIOBJ of = SelectObject(c->dc, font), ob = SelectObject(c->dc, GetStockObject(WHITE_BRUSH));
    glyph_path(c, font, s, sx, sy, dx, dy, desc);
    LOGBRUSH lb = {BS_SOLID, outline ? RGB(255, 255, 255) : RGB(0, 0, 0), 0};
    HPEN p = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_SQUARE | PS_JOIN_MITER, pen * 2, &lb, 0, 0);
    SetMiterLimit(c->dc, 2.0f, 0);
    HGDIOBJ op = SelectObject(c->dc, p);
    if (outline) {
        StrokeAndFillPath(c->dc);
    } else {
        FillPath(c->dc);   // consumes the path: make it again to stroke the edge away in black
        glyph_path(c, font, s, sx, sy, dx, dy, desc);
        StrokePath(c->dc);
    }
    SelectObject(c->dc, op);
    DeleteObject(p);
    SelectObject(c->dc, ob);
    SelectObject(c->dc, of);
    GdiFlush();
}

static int canvas_box(const struct canvas *c, int box[4])
{
    box[0] = box[1] = 1 << 30; box[2] = box[3] = -1;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++)
            if (c->px[y * c->w + x] & 0x80) {
                if (x < box[0]) box[0] = x;
                if (y < box[1]) box[1] = y;
                if (x > box[2]) box[2] = x;
                if (y > box[3]) box[3] = y;
            }
    box[2]++; box[3]++;
    return box[2] > 0;
}

static uint8_t *make_sheet(const uint8_t *orig, int european, int k)
{
    int W = 256 * k, px = k * SS;   // px: canvas pixels per game pixel
    uint8_t *out = malloc((size_t)W * W);
    float *edge = malloc(sizeof(float) * 48 * k * 16 * k), *fill = malloc(sizeof(float) * 48 * k * 16 * k);   // 3 cells
    HFONT font = CreateFontW(-(int)(15.5f * px + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, 0,
                             NONANTIALIASED_QUALITY, 0, L"Kabuto Sans");
    struct canvas big = {0}, cell = {0};   // big: for measuring (64 x 48 game pixels, origin at (16, 32)); cell: up to 3 cells
    if (!out || !edge || !fill || !font || !canvas_make(&big, 64 * px, 48 * px) || !canvas_make(&cell, 48 * px, 16 * px)) {
        xlog("[text] couldn't set up drawing: enhanced_text off");
        free(out); out = 0;
        goto done;
    }
    for (int y = 0; y < W; y++)   // start from the original enlarged: cells without a replacement (the small font) stay
        for (int x = 0; x < W; x++) out[y * W + x] = orig[(y / k) * 256 + x / k];
    static struct item list[256];
    int n = items(european, list), drawn = 0;
    for (int i = 0; i < n; i++) {
        const struct item *it = &list[i];
        int cx = (it->code + 0x20) % 16 * 16, cy = (it->code + 0x20) / 16 * 16, ob[4], nb[4], cw = 16 * it->n * k;
        if (!ink_box(orig, cx, cy, 16 * it->n, 16, ob)) continue;   // nothing there to replace
        int edge_pen = (int)(EDGE * px + 0.5f), thin_pen = (int)(THIN * px + 0.5f);
        draw(&big, font, it->s, 1, 1, 16.0f * px, 32.0f * px, 1, 1, edge_pen);   // the outlined glyph at its own size
        if (!canvas_box(&big, nb)) continue;
        drawn++;
        float nl = nb[0] - 16.0f * px, nt = nb[1] - 32.0f * px;                  // ours, from the origin
        float nw = (float)(nb[2] - nb[0]), nh = (float)(nb[3] - nb[1]), sx = 1, sy = 1, dx, dy;
        // What the game draws of a glyph: from half a texel in on the left and top to the right and bottom of its
        // {offset, width} box (the original's ink: letters fill it exactly), cell rows 0.5-16. Nothing of ours may
        // reach past that, or it's cut off. Letters keep one size on the sheet's baseline (13 px); one wider than the
        // box is narrowed, and descenders deeper than the 3 px left below the baseline are pulled up (only the part
        // below it). Symbols shrink to fit and centre on the original's ink.
        float bl = (ob[0] > 0 ? ob[0] : 0.5f) * px + 1, br = (float)(ob[2] * px - 1);   // allowed, canvas pixels (1 spare each side)
        float bt = (ob[1] > 0 ? ob[1] : 0.5f) * px + 1, bb = 16.0f * px - 1;
        float aw = br - bl, desc = 1;
        if (it->mode == FIT) {
            float ah = (ob[3] < 16 ? ob[3] * px : bb) - bt;
            sx = aw / nw < 1 ? aw / nw : 1; sy = ah / nh;
            dy = bt - nt * sy;                                                    // top to the original's top
        } else if (it->mode == BASELINE) {
            float base = 13.0f * px, below = nt + nh;                             // reached below the baseline
            if (base + below > bb && below > edge_pen) desc = (bb - base - edge_pen) / (below - edge_pen);
            if (desc < 0.3f) desc = 0.3f;
            if (base + nt < bt) sy = (base - bt) / -nt;                           // above the top: squash
            if (nw > aw) sx = aw / nw;
            dy = base;
        } else {
            float ah = (ob[3] < 16 ? ob[3] * px : bb) - bt;
            sx = sy = 1;
            if (nw > aw) sx = sy = aw / nw;
            if (nh * sy > ah) sx = sy = ah / nh;
            dy = (ob[1] + ob[3]) * 0.5f * px - (nt + nh * 0.5f) * sy;
            if (dy + nt * sy < bt) dy = bt - nt * sy;
            if (dy + (nt + nh) * sy > bb) dy = bb - (nt + nh) * sy;
        }
        dx = (bl + br) * 0.5f - (nl + nw * 0.5f) * sx;                            // centred in the drawn box
        for (int pass = 0; pass < 2; pass++) {   // outline coverage, then fill coverage, averaged down to k per game pixel
            float *dst = pass ? fill : edge;
            draw(&cell, font, it->s, sx, sy, dx, dy, desc, pass == 0, pass == 0 ? edge_pen : thin_pen);
            for (int y = 0; y < 16 * k; y++)
                for (int x = 0; x < cw; x++) {
                    int sum = 0;
                    for (int v = 0; v < SS; v++)
                        for (int u = 0; u < SS; u++) sum += (cell.px[(y * SS + v) * cell.w + x * SS + u] & 0xff) > 127;
                    dst[y * cw + x] = sum / (float)(SS * SS);
                }
        }
        for (int y = 0; y < 16 * k; y++)
            for (int x = 0; x < cw; x++) {
                float e = edge[y * cw + x], f = fill[y * cw + x];
                int v = e < 0.5f ? 0 : 1 + (int)((1 - f) * 8 + 0.5f);
                out[(cy * k + y) * W + cx * k + x] = (uint8_t)v;
            }
    }
    if (!drawn) {   // no glyph came out (the Linux build doesn't draw paths): the sheet would only be the original enlarged
        xlog("[text] Kabuto Sans's outlines can't be drawn here: enhanced_text off");
        free(out); out = 0;
    }
done:
    canvas_free(&big);
    canvas_free(&cell);
    if (font) DeleteObject(font);
    free(edge);
    free(fill);
    return out;
}

// A redrawn copy of the English sheet itself (the game's own letters, cleaned up at high resolution), packed by
// tools/font/pack_sheet.py as text_sheet_e.bin: from an enabled mod's font\ folder (the HD pack; it's made from the game's
// own font, so the port doesn't ship it), else beside the exe. When it's there the English sheet comes from it instead
// of Kabuto Sans. Each pixel: fill coverage << 4 | fill + outline coverage, 4 bits each, master_m pixels per texel.
static uint8_t *master;
static int master_m;
static void load_master(void)
{
    static int tried;
    if (tried++) return;
    wchar_t path[MAX_PATH];   // from the HD pack (it's the game's own letters, so it isn't shipped with the port)
    if (!mod_file(L"font\\text_sheet_e.bin", path, MAX_PATH)) host_path(path, L"text_sheet_e.bin");
    uint32_t n = 0;
    uint8_t *b = read_file(path, 64u << 20, &n);
    if (!b) return;
    if (n <= 8) { free(b); b = 0; }
    int size = b ? *(uint16_t *)(b + 4) : 0, m = b ? *(uint16_t *)(b + 6) : 0;
    if (!b || memcmp(b, "OTXS", 4) || m < 2 || m > 32 || size != 256 * m) { xlog("[text] %ls isn't a packed font sheet", path); free(b); return; }
    uint8_t *px = malloc((size_t)size * size);
    size_t at = 0, total = (size_t)size * size;
    for (DWORD i = 8; px && i + 1 < n && at < total; i += 2)
        for (int r = b[i]; r-- && at < total;) px[at++] = b[i + 1];
    free(b);
    if (!px || at != total) { xlog("[text] %ls is cut short", path); free(px); return; }
    master = px; master_m = m;
    xlog("[text] redrawn sheet %ls (%dx)", path, m);
}

// The master at k pixels per texel, as text.c's index image: each pixel the average of the master pixels it covers.
// Its outline coverage goes to `coverage` for blended, anti-aliased edges, so any pixel the outline touches gets a colour.
static uint8_t *coverage;
const uint8_t *text_coverage(void) { return coverage; }
static uint8_t *sheet_from_master(int k)
{
    int W = 256 * k, S = 256 * master_m;
    uint8_t *out = malloc((size_t)W * W);
    int *lo = malloc(sizeof(int) * (W + 1));
    free(coverage);
    coverage = malloc((size_t)W * W);
    if (!out || !lo || !coverage) { free(out); free(lo); free(coverage); coverage = 0; return 0; }
    for (int i = 0; i <= W; i++) lo[i] = (int)((long long)i * S / W);
    for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++) {
            int y1 = lo[y + 1] > lo[y] ? lo[y + 1] : lo[y] + 1, x1 = lo[x + 1] > lo[x] ? lo[x + 1] : lo[x] + 1, fill = 0, edge = 0, n = 0;
            for (int v = lo[y]; v < y1; v++)
                for (int u = lo[x]; u < x1; u++, n++) { uint8_t p = master[(size_t)v * S + u]; fill += p >> 4; edge += p & 15; }
            float fc = fill / (15.0f * n), ec = edge / (15.0f * n);
            out[y * W + x] = ec <= 0 ? 0 : (uint8_t)(1 + (int)((1 - fc) * 8 + 0.5f));
            coverage[y * W + x] = (uint8_t)(ec * 255 + 0.5f);
        }
    free(lo);
    return out;
}

const uint8_t *text_sheet(const uint8_t *idx, int k)
{
    static uint8_t *sheet;   // the last one made
    static int sheet_kind = -1, sheet_scale;
    static int logged;
    const uint8_t *orig = original_sheet();
    if (!orig || k < 2 || k > TEXT_K_MAX) return 0;
    // The plain letters and symbols (rows 32-159) are the same in the English and the French/German sheets.
    int same = 0;
    for (int i = 32 * 256; i < 160 * 256; i++) same += idx[i] == orig[i];
    if (same < 128 * 256) {
        if (same > 128 * 256 / 2 && logged++ < 4) xlog("[text] a 256x256 paletted sheet matches the font in %d of %d texels: left as it is", same, 128 * 256);
        return 0;
    }
    int european = settings.text_lang >= LANG_FRENCH;
    if (sheet_kind == european && sheet_scale == k) return sheet;   // 0 if making it failed: not retried
    load_master();
    // ponytail: the redrawn sheet has no accented letters yet, so French/German keep Kabuto Sans throughout.
    if (!european && master) {
        free(sheet);
        DWORD t0 = GetTickCount();
        sheet = sheet_from_master(k);
        sheet_kind = european; sheet_scale = k;
        if (sheet) xlog("[text] font sheet from the redrawn sheet at %dx in %u ms", k, GetTickCount() - t0);
        return sheet;
    }
    if (!kabuto_font()) return 0;
    free(sheet);
    free(coverage);
    coverage = 0;
    // The accent rows below 160 are kernel.c's rearrangement in French/German: take them from the texture itself.
    static uint8_t src[65536];
    memcpy(src, orig, sizeof src);
    if (european) memcpy(src + 160 * 256, idx + 160 * 256, 96 * 256);
    DWORD t0 = GetTickCount();
    sheet = make_sheet(src, european, k);
    sheet_kind = european; sheet_scale = k;
    if (sheet) xlog("[text] font sheet redrawn with Kabuto Sans at %dx (%s) in %u ms", k, european ? "French/German" : "English", GetTickCount() - t0);
    char dump[4] = "";   // debug: ONI_DUMP_TEXT=1 saves it as text_sheet.pgm next to the exe (index x 28: grey levels)
    debug_env("ONI_DUMP_TEXT", dump, sizeof dump);
    if (sheet && dump[0] == '1') {
        wchar_t path[MAX_PATH];
        host_path(path, L"text_sheet.pgm");
        FILE *f = _wfopen(path, L"wb");
        if (f) {
            fprintf(f, "P5 %d %d 255\n", 256 * k, 256 * k);
            for (int i = 0; i < 256 * k * 256 * k; i++) fputc(sheet[i] * 28, f);
            fclose(f);
        }
    }
    return sheet;
}
