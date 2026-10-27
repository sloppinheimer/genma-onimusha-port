// Linux: the GDI calls the overlays and text.c make (compat_win.h). Bitmaps (DIB sections) are real memory, and text is
// drawn into them with stb_truetype (public domain / MIT, src/linux/stb): anti-aliased, in SetTextColor's colour, laid
// out as DrawTextW/A and TextOutW do for the flags the host passes. Fonts: what AddFontMemResourceEx was given (Kabuto
// Sans, built into the executable as host.rc builds it into the DLL, so text.c's FindResourceW finds it); any other face
// (Segoe UI) is the system's Noto Sans or DejaVu Sans of the nearest weight, Kabuto Sans if it has neither. Letters a
// font lacks come from the next one (then Noto Sans CJK, for Japanese), as Windows' font fallback does.
// Paths aren't drawn (TextOutW between BeginPath and EndPath adds nothing), so text.c's Kabuto Sans sheet comes out
// empty and enhanced_text keeps the game's own font. ponytail: fill and stroke glyph paths for text.c.
#include <windows.h>
#include <stdlib.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <d3dcompiler.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"

void xlog(const char *fmt, ...);

// ---- Kabuto Sans, as the DLL's resource 2 (RT_RCDATA) -------------------------------------------------------------
// ONI_KABUTO_TTF: tools/font/KabutoSans.ttf (linux.cmake)
__asm__(".pushsection .rodata\n.balign 16\n.globl compat_kabuto_ttf\n.hidden compat_kabuto_ttf\ncompat_kabuto_ttf:\n"
        ".incbin \"" ONI_KABUTO_TTF "\"\n.globl compat_kabuto_ttf_end\n.hidden compat_kabuto_ttf_end\ncompat_kabuto_ttf_end:\n"
        ".popsection");
extern const uint8_t compat_kabuto_ttf[] __attribute__((visibility("hidden"))), compat_kabuto_ttf_end[] __attribute__((visibility("hidden")));
struct resource { const void *data; DWORD size; };
static struct resource kabuto_res;

HRSRC FindResourceW(HMODULE m, const wchar_t *name, const wchar_t *type)
{
    (void)m;
    if (name != MAKEINTRESOURCEW(2) || type != RT_RCDATA) return 0;
    kabuto_res.data = compat_kabuto_ttf;
    kabuto_res.size = (DWORD)(compat_kabuto_ttf_end - compat_kabuto_ttf);
    return &kabuto_res;
}
HGLOBAL LoadResource(HMODULE m, HRSRC r) { (void)m; return r; }
void *LockResource(HGLOBAL g) { return g ? (void *)((struct resource *)g)->data : 0; }
DWORD SizeofResource(HMODULE m, HRSRC r) { (void)m; return r ? ((struct resource *)r)->size : 0; }

// ---- objects: bitmaps and fonts are ours (tagged), brushes and pens are only names --------------------------------
#define GDI_MAGIC 0x30494447u
enum { GDI_BITMAP = 1, GDI_FONT };
struct gdiobj { uint32_t magic; int kind, stock; };
struct bitmap { struct gdiobj o; int w, h, bpp, stride, flip; uint8_t *bits; };   // flip: rows bottom-up
// A font file: Windows' ascent and descent (OS/2 usWinAscent/usWinDescent, what GDI's tmAscent/tmDescent come from) and
// units per em.
struct face { stbtt_fontinfo info; int asc, desc, em; };
// px: the em in pixels; asc, desc: the line's, in pixels (from the first face). f[1..]: fallbacks for missing letters.
struct font { struct gdiobj o; struct face *f[4]; int nf, asc, desc, mono; float px; };
struct dc { struct bitmap *bm; struct font *font; COLORREF color; int bk_mode, path; UINT align; };

static struct bitmap stock_bitmap = {{GDI_MAGIC, GDI_BITMAP, 1}};   // what a new DC has selected: nothing to draw on
static struct font stock_font = {{GDI_MAGIC, GDI_FONT, 1}};

static struct gdiobj *gdi(HGDIOBJ h, int kind)
{
    struct gdiobj *o = h;
    return (uintptr_t)h >= 0x10000 && o->magic == GDI_MAGIC && (!kind || o->kind == kind) ? o : 0;
}

HDC CreateCompatibleDC(HDC dc)
{
    (void)dc;
    struct dc *d = calloc(1, sizeof *d);
    if (d) d->bm = &stock_bitmap, d->font = &stock_font, d->bk_mode = OPAQUE;
    return (HDC)d;
}
BOOL DeleteDC(HDC dc) { free(dc); return 1; }
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO *bi, UINT usage, void **bits, HANDLE section, DWORD offset)
{
    (void)dc; (void)usage; (void)section; (void)offset;
    if (bits) *bits = 0;
    struct bitmap *b = calloc(1, sizeof *b);
    if (!b) return 0;
    b->o = (struct gdiobj){GDI_MAGIC, GDI_BITMAP, 0};
    b->w = abs(bi->bmiHeader.biWidth); b->h = abs(bi->bmiHeader.biHeight); b->bpp = bi->bmiHeader.biBitCount;
    b->stride = (b->w * b->bpp + 31) / 32 * 4;   // rows DWORD-aligned, as Windows'
    b->flip = bi->bmiHeader.biHeight > 0;
    if (!(b->bits = calloc(1, (size_t)b->stride * b->h + 16))) { free(b); return 0; }
    if (bits) *bits = b->bits;
    return b;
}
HGDIOBJ SelectObject(HDC dc, HGDIOBJ o)
{
    struct dc *d = (struct dc *)dc;
    HGDIOBJ old = o;   // brushes and pens: nothing to keep
    if (!d) return 0;
    if (gdi(o, GDI_BITMAP)) old = d->bm, d->bm = o;
    else if (gdi(o, GDI_FONT)) old = d->font, d->font = o;
    return old;
}
BOOL DeleteObject(HGDIOBJ o)
{
    struct gdiobj *g = gdi(o, 0);
    if (!g || g->stock) return 1;
    g->magic = 0;
    if (g->kind == GDI_BITMAP) free(((struct bitmap *)g)->bits);
    free(g);   // a font's faces stay: they're shared and cached
    return 1;
}
HGDIOBJ GetStockObject(int i) { (void)i; return (HGDIOBJ)1; }

// ---- font files ---------------------------------------------------------------------------------------------------
static pthread_mutex_t font_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static struct face *face_make(const uint8_t *data, int index)
{
    int at = stbtt_GetFontOffsetForIndex(data, index), asc, desc, gap;
    struct face *f = at >= 0 ? calloc(1, sizeof *f) : 0;
    if (!f || !stbtt_InitFont(&f->info, data, at)) { free(f); return 0; }
    f->em = be16(data + f->info.head + 18);
    if (!f->em) f->em = 2048;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    f->asc = asc; f->desc = -desc;
    uint32_t os2 = stbtt__find_table((uint8_t *)data, at, "OS/2");
    if (os2 && be16(data + os2 + 74)) f->asc = be16(data + os2 + 74), f->desc = be16(data + os2 + 76);
    return f;
}

// A font file from disk, mapped (pages are read as glyphs need them), index: the font in a collection. 0 if absent.
static struct face *face_file(const char *path, int index)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    void *p = MAP_FAILED;
    if (fd >= 0 && !fstat(fd, &st) && st.st_size > 0) p = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (fd >= 0) close(fd);
    if (p == MAP_FAILED) return 0;
    struct face *f = face_make(p, index);
    if (!f) munmap(p, st.st_size);
    return f;
}

// Fonts AddFontMemResourceEx has been given, by family name (name ID 1).
static struct { char family[64]; struct face *f; } mem_fonts[8];
static int n_mem;

HANDLE AddFontMemResourceEx(void *data, DWORD size, void *res, DWORD *count)
{
    (void)res;
    if (count) *count = 0;
    uint8_t *copy = data && size ? malloc(size) : 0;   // its own copy, as Windows keeps
    if (!copy) return 0;
    memcpy(copy, data, size);
    int fonts = stbtt_GetNumberOfFonts(copy), added = 0;
    pthread_mutex_lock(&font_lock);
    for (int i = 0; i < fonts && n_mem < 8; i++) {
        struct face *f = face_make(copy, i);
        if (!f) continue;
        int len = 0, n = 0;
        const char *s = stbtt_GetFontNameString(&f->info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
        for (int k = 0; s && k + 1 < len && n + 1 < (int)sizeof mem_fonts[0].family; k += 2) mem_fonts[n_mem].family[n++] = s[k] ? '?' : s[k + 1];   // UTF-16BE
        mem_fonts[n_mem].family[n] = 0;
        mem_fonts[n_mem++].f = f;
        added++;
    }
    pthread_mutex_unlock(&font_lock);
    if (!added) { free(copy); return 0; }
    if (count) *count = added;
    return copy;
}

// The system's sans-serif for a weight: Noto Sans, else DejaVu Sans, from where SteamOS (Arch), Debian/Ubuntu and
// Fedora keep them; the nearest weight as CSS picks it (heavier first from 600). Kabuto Sans if there's neither.
static const char *const noto_dirs[] = {"/usr/share/fonts/noto", "/usr/share/fonts/truetype/noto", "/usr/share/fonts/google-noto"};
static const char *const dejavu_dirs[] = {"/usr/share/fonts/TTF", "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/dejavu-sans-fonts", "/usr/share/fonts/dejavu"};
static const char *const cjk_dirs[] = {"/usr/share/fonts/noto-cjk", "/usr/share/fonts/opentype/noto", "/usr/share/fonts/google-noto-cjk"};

static struct face *first_file(const char *const *dirs, int ndirs, const char *const *names, int nnames, int index, const char *what)
{
    char path[256];
    for (int n = 0; n < nnames; n++)
        for (int d = 0; d < ndirs; d++) {
            snprintf(path, sizeof path, "%s/%s", dirs[d], names[n]);
            if (access(path, R_OK)) continue;
            struct face *f = face_file(path, index);
            if (f) { xlog("[gdi] %s: %s", what, path); return f; }
        }
    return 0;
}

static struct face *kabuto(void)
{
    static struct face *f;
    if (!f) f = face_make(compat_kabuto_ttf, 0);
    return f;
}

static struct face *sans(int weight)
{
    static const char *const noto[4][4] = {{"NotoSans-Regular.ttf", "NotoSans-Medium.ttf"}, {"NotoSans-Medium.ttf", "NotoSans-Regular.ttf"},
        {"NotoSans-SemiBold.ttf", "NotoSans-Bold.ttf", "NotoSans-Medium.ttf", "NotoSans-Regular.ttf"},
        {"NotoSans-Bold.ttf", "NotoSans-SemiBold.ttf", "NotoSans-Medium.ttf", "NotoSans-Regular.ttf"}};
    static const char *const dejavu[2][2] = {{"DejaVuSans.ttf", "DejaVuSans-Bold.ttf"}, {"DejaVuSans-Bold.ttf", "DejaVuSans.ttf"}};
    static struct face *got[4];
    static int tried[4];
    int w = weight < 500 ? 0 : weight < 600 ? 1 : weight < 700 ? 2 : 3;
    if (!tried[w]++) {
        char what[48];
        snprintf(what, sizeof what, "sans-serif text at weight %d", weight ? weight : FW_NORMAL);
        got[w] = first_file(noto_dirs, 3, noto[w], noto[w][3] ? 4 : 2, 0, what);
        if (!got[w]) got[w] = first_file(dejavu_dirs, 4, dejavu[w >= 2], 2, 0, what);
        if (!got[w]) xlog("[gdi] %s: no Noto Sans or DejaVu Sans, Kabuto Sans instead", what);
    }
    return got[w];
}

static struct face *cjk(int weight)   // Noto Sans CJK JP (the first font in the collection)
{
    static const char *const names[2][2] = {{"NotoSansCJK-Regular.ttc", "NotoSansCJK-Bold.ttc"}, {"NotoSansCJK-Bold.ttc", "NotoSansCJK-Regular.ttc"}};
    static struct face *got[2];
    static int tried[2];
    int w = weight >= 600;
    if (!tried[w]++) got[w] = first_file(cjk_dirs, 3, names[w], 2, 0, w ? "Japanese text, bold" : "Japanese text");
    return got[w];
}

static int same_name(const wchar_t *a, const char *b)
{
    for (; *a && *b; a++, b++) if (towlower(*a) != tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static void font_add(struct font *f, struct face *x)
{
    for (int i = 0; i < f->nf; i++) if (f->f[i] == x) return;
    if (x && f->nf < 4) f->f[f->nf++] = x;
}

HFONT CreateFontW(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD under, DWORD strike, DWORD charset,
                  DWORD outprec, DWORD clipprec, DWORD quality, DWORD pitch, const wchar_t *face)
{
    (void)w; (void)esc; (void)orient; (void)italic; (void)under; (void)strike; (void)charset; (void)outprec; (void)clipprec; (void)pitch;
    struct font *f = calloc(1, sizeof *f);
    if (!f) return 0;
    f->o = (struct gdiobj){GDI_MAGIC, GDI_FONT, 0};
    f->mono = quality == NONANTIALIASED_QUALITY;
    pthread_mutex_lock(&font_lock);
    struct face *named = 0, *sys = sans(weight);
    for (int i = 0; face && i < n_mem && !named; i++) if (same_name(face, mem_fonts[i].family)) named = mem_fonts[i].f;
    font_add(f, named ? named : sys ? sys : kabuto());
    font_add(f, sys);
    font_add(f, cjk(weight));
    pthread_mutex_unlock(&font_lock);
    struct face *p = f->f[0];
    if (!p) return f;   // draws nothing
    f->px = h < 0 ? (float)-h : h > 0 ? (float)h * p->em / (p->asc + p->desc) : 12;   // < 0: the em's height; > 0: the line's
    f->asc = (int)(p->asc * f->px / p->em + 0.5f);
    f->desc = (int)(p->desc * f->px / p->em + 0.5f);
    return f;
}
HFONT CreateFontA(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD under, DWORD strike, DWORD charset,
                  DWORD outprec, DWORD clipprec, DWORD quality, DWORD pitch, const char *face)
{
    wchar_t name[64];
    int n = 0;
    for (; face && face[n] && n < 63; n++) name[n] = (unsigned char)face[n];
    name[n] = 0;
    return CreateFontW(h, w, esc, orient, weight, italic, under, strike, charset, outprec, clipprec, quality, pitch, face ? name : 0);
}

// ---- drawing ------------------------------------------------------------------------------------------------------
int SetBkMode(HDC dc, int mode) { struct dc *d = (struct dc *)dc; int old = d ? d->bk_mode : 0; if (d) d->bk_mode = mode; return old; }
COLORREF SetTextColor(HDC dc, COLORREF c) { struct dc *d = (struct dc *)dc; COLORREF old = d ? d->color : 0; if (d) d->color = c; return old; }
UINT SetTextAlign(HDC dc, UINT a) { struct dc *d = (struct dc *)dc; UINT old = d ? d->align : 0; if (d) d->align = a; return old; }
int SetGraphicsMode(HDC dc, int m) { (void)dc; (void)m; return 1; }
BOOL SetWorldTransform(HDC dc, const XFORM *x) { (void)dc; (void)x; return 1; }
BOOL SetMiterLimit(HDC dc, FLOAT l, FLOAT *old) { (void)dc; (void)l; if (old) *old = 10; return 1; }

static RECT bounds(const struct dc *d) { return (RECT){0, 0, d->bm->w, d->bm->h}; }
static int drawable(const struct dc *d) { return d->bm->bits && (d->bm->bpp == 24 || d->bm->bpp == 32); }
static uint8_t *pixel(const struct bitmap *b, int x, int y) { return b->bits + (size_t)(b->flip ? b->h - 1 - y : y) * b->stride + x * (b->bpp / 8); }

static void fill(struct dc *d, int x0, int y0, int x1, int y1, COLORREF c)
{
    RECT r = bounds(d);
    if (!drawable(d)) return;
    if (x0 < r.left) x0 = r.left;
    if (y0 < r.top) y0 = r.top;
    if (x1 > r.right) x1 = r.right;
    if (y1 > r.bottom) y1 = r.bottom;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint8_t *p = pixel(d->bm, x, y);
            p[0] = c >> 16 & 255; p[1] = c >> 8 & 255; p[2] = c & 255;
            if (d->bm->bpp == 32) p[3] = 0;
        }
}

static struct face *glyph(const struct font *f, uint32_t c, int *g)
{
    for (int i = 0; i < f->nf; i++) if ((*g = stbtt_FindGlyphIndex(&f->f[i]->info, (int)c))) return f->f[i];
    *g = 0;   // in none of them: the first's missing-letter box
    return f->nf ? f->f[0] : 0;
}
static float advance(const struct font *f, const struct face *x, int g)
{
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&x->info, g, &adv, &lsb);
    return adv * f->px / x->em;
}
static float char_width(const struct font *f, uint32_t c)
{
    int g;
    struct face *x = glyph(f, c, &g);
    return x ? advance(f, x, g) : 0;
}
static float measure(const struct font *f, const uint32_t *s, int n)
{
    float w = 0;
    for (int i = 0; i < n; i++) w += char_width(f, s[i]);
    return w;
}

// The letters from (x, top) (the line's top-left), anti-aliased in the text colour over what's there, inside `clip`.
static void draw_run(struct dc *d, const uint32_t *s, int n, float x, int top, const RECT *clip)
{
    const struct font *f = d->font;
    COLORREF c = d->color;
    int base = top + f->asc, R = c & 255, G = c >> 8 & 255, B = c >> 16 & 255;
    if (d->bk_mode == OPAQUE) fill(d, (int)x, top, (int)ceilf(x + measure(f, s, n)), top + f->asc + f->desc, RGB(255, 255, 255));
    for (int i = 0; i < n; i++) {
        int g, ix = (int)floorf(x), x0, y0, x1, y1;
        struct face *fc = glyph(f, s[i], &g);
        if (!fc) continue;
        float k = f->px / fc->em, sx = x - ix;
        x += advance(f, fc, g);
        stbtt_GetGlyphBitmapBoxSubpixel(&fc->info, g, k, k, sx, 0, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        uint8_t *cov = w > 0 && h > 0 ? malloc((size_t)w * h) : 0;
        if (!cov) continue;
        stbtt_MakeGlyphBitmapSubpixel(&fc->info, cov, w, h, w, k, k, sx, 0, g);
        for (int v = 0; v < h; v++) {
            int y = base + y0 + v;
            if (y < clip->top || y >= clip->bottom) continue;
            for (int u = 0; u < w; u++) {
                int px = ix + x0 + u, a = cov[v * w + u];
                if (px < clip->left || px >= clip->right || !a) continue;
                if (f->mono) { if (a < 128) continue; a = 255; }
                uint8_t *p = pixel(d->bm, px, y);
                p[0] = (uint8_t)((p[0] * (255 - a) + B * a + 127) / 255);
                p[1] = (uint8_t)((p[1] * (255 - a) + G * a + 127) / 255);
                p[2] = (uint8_t)((p[2] * (255 - a) + R * a + 127) / 255);
            }
        }
        free(cov);
    }
}

// The string as code points. A strings are Windows-1252, an English Windows' ANSI code page. For DrawText (`text`) the
// '&' prefixes come out (&& is an '&'; the underline isn't drawn) and so do the line ends it doesn't draw.
static uint32_t *decode(const char *a, const wchar_t *w, int n, UINT flags, int text, int *len)
{
    static const uint16_t cp1252[32] = {0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160,
        0x2039, 0x152, 0x8d, 0x17d, 0x8f, 0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161,
        0x203a, 0x153, 0x9d, 0x17e, 0x178};
    if (n < 0) n = a ? (int)strlen(a) : (int)wcslen(w);
    uint32_t *u = malloc(sizeof *u * (n + 1));
    int m = 0;
    for (int i = 0; u && i < n; i++) {
        uint32_t c = a ? (unsigned char)a[i] : (uint32_t)w[i];
        if (text && c == '&' && !(flags & DT_NOPREFIX)) {
            if (++i >= n) break;
            c = a ? (unsigned char)a[i] : (uint32_t)w[i];
        }
        if (a && c >= 0x80 && c < 0xa0) c = cp1252[c - 0x80];
        if (text && c == '\r') continue;
        if (text && c == '\n' && (flags & DT_SINGLELINE)) continue;
        u[m++] = c;
    }
    *len = m;
    return u;
}

static int is_cjk(uint32_t c) { return (c >= 0x2e80 && c < 0xa000) || (c >= 0xf900 && c < 0xfb00) || (c >= 0xff00 && c < 0xffa0); }

// Lines as DrawText makes them: at '\n', and with DT_WORDBREAK where the next word would pass `width`: at spaces (which
// the line drops) or beside CJK letters (which have none between words). A word wider than the line isn't broken.
struct line { int start, end; float w; };
static int layout(const struct font *f, const uint32_t *s, int n, float width, int wrap, struct line *out)
{
    int nl = 0, i = 0;
    while (i < n) {
        int start = i, cut = -1, resume = i, ink_end = i;   // ink_end: past the last letter that isn't a space
        float w = 0, cut_w = 0, ink_w = 0;
        for (; i < n && s[i] != '\n'; i++) {
            float a = char_width(f, s[i]);
            if (s[i] == ' ') {
                if (i > start && s[i - 1] != ' ') cut = i, cut_w = w;
                resume = i + 1;
            } else {
                if (i > start && s[i - 1] != ' ' && (is_cjk(s[i]) || is_cjk(s[i - 1]))) cut = i, cut_w = w, resume = i;
                if (wrap && cut >= 0 && w + a > width) break;
                ink_end = i + 1; ink_w = w + a;
            }
            w += a;
        }
        if (i < n && s[i] != '\n') { out[nl++] = (struct line){start, cut, cut_w}; i = resume; }   // wrapped
        else { out[nl++] = (struct line){start, ink_end, ink_w}; if (i < n) i++; }
    }
    return nl;
}

static int draw_text(HDC dc, uint32_t *s, int n, RECT *r, UINT flags)
{
    struct dc *d = (struct dc *)dc;
    struct line *lines = s && d && r ? malloc(sizeof *lines * (n + 1)) : 0;
    if (!lines) { free(s); return 0; }
    const struct font *f = d->font;
    int lh = f->asc + f->desc, single = flags & DT_SINGLELINE;
    int nl = f->nf ? layout(f, s, n, (float)(r->right - r->left), !single && (flags & DT_WORDBREAK), lines) : 0;
    int h = nl * lh, top = r->top;
    if (single && (flags & DT_VCENTER)) top = r->top + (r->bottom - r->top - lh) / 2;
    else if (single && (flags & DT_BOTTOM)) top = r->bottom - lh;
    if (flags & DT_CALCRECT) {
        float widest = 0;
        for (int i = 0; i < nl; i++) if (lines[i].w > widest) widest = lines[i].w;
        r->right = r->left + (int)ceilf(widest);
        r->bottom = r->top + h;
    } else if (drawable(d)) {
        RECT clip = bounds(d);
        if (!(flags & DT_NOCLIP)) {
            if (r->left > clip.left) clip.left = r->left;
            if (r->top > clip.top) clip.top = r->top;
            if (r->right < clip.right) clip.right = r->right;
            if (r->bottom < clip.bottom) clip.bottom = r->bottom;
        }
        for (int i = 0; i < nl; i++) {
            int w = (int)ceilf(lines[i].w);
            int x = flags & DT_CENTER ? r->left + (r->right - r->left - w) / 2 : flags & DT_RIGHT ? r->right - w : r->left;
            draw_run(d, s + lines[i].start, lines[i].end - lines[i].start, (float)x, top + i * lh, &clip);
        }
    }
    free(lines);
    free(s);
    return single && (flags & (DT_VCENTER | DT_BOTTOM)) && !(flags & DT_CALCRECT) ? top - r->top + h : h;
}
int DrawTextW(HDC dc, const wchar_t *s, int n, RECT *r, UINT f)
{
    int len = 0;
    uint32_t *u = s ? decode(0, s, n, f, 1, &len) : 0;
    return draw_text(dc, u, len, r, f);
}
int DrawTextA(HDC dc, const char *s, int n, RECT *r, UINT f)
{
    int len = 0;
    uint32_t *u = s ? decode(s, 0, n, f, 1, &len) : 0;
    return draw_text(dc, u, len, r, f);
}
BOOL TextOutW(HDC dc, int x, int y, const wchar_t *s, int n)
{
    struct dc *d = (struct dc *)dc;
    int len = 0;
    if (!d || !s || n < 0) return 0;
    if (d->path || !d->font->nf || !drawable(d)) return 1;   // paths aren't drawn (see the top)
    uint32_t *u = decode(0, s, n, 0, 0, &len);
    if (!u) return 0;
    const struct font *f = d->font;
    float w = d->align & (TA_CENTER | TA_RIGHT) ? measure(f, u, len) : 0, px = (float)x;
    if ((d->align & TA_CENTER) == TA_CENTER) px -= floorf(w / 2);
    else if (d->align & TA_RIGHT) px -= ceilf(w);
    int top = (d->align & TA_BASELINE) == TA_BASELINE ? y - f->asc : (d->align & TA_BOTTOM) ? y - f->asc - f->desc : y;
    RECT clip = bounds(d);
    draw_run(d, u, len, px, top, &clip);
    free(u);
    return 1;
}
BOOL GdiFlush(void) { return 1; }
BOOL PatBlt(HDC dc, int x, int y, int w, int h, DWORD rop)
{
    struct dc *d = (struct dc *)dc;
    if (d && (rop == BLACKNESS || rop == WHITENESS)) fill(d, x, y, x + w, y + h, rop == BLACKNESS ? 0 : RGB(255, 255, 255));
    return 1;
}
BOOL BeginPath(HDC dc) { if (dc) ((struct dc *)dc)->path = 1; return 1; }
BOOL EndPath(HDC dc) { if (dc) ((struct dc *)dc)->path = 0; return 1; }
BOOL FillPath(HDC dc) { (void)dc; return 1; }
BOOL StrokePath(HDC dc) { (void)dc; return 1; }
BOOL StrokeAndFillPath(HDC dc) { (void)dc; return 1; }
int GetPath(HDC dc, POINT *p, BYTE *types, int n) { (void)dc; (void)p; (void)types; (void)n; return 0; }
BOOL PolyDraw(HDC dc, const POINT *p, const BYTE *types, int n) { (void)dc; (void)p; (void)types; (void)n; return 1; }
HPEN ExtCreatePen(DWORD style, DWORD width, const LOGBRUSH *b, DWORD n, const DWORD *dashes) { (void)style; (void)width; (void)b; (void)n; (void)dashes; return (HPEN)1; }
