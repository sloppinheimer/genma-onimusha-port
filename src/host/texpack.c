// HD texture packs, Dolphin/PCSX2-style. d3d_texture.c names every texture by a hash of its Xbox contents (tex_name there);
// texpack_load finds a file with that name in an enabled mod's textures\ folder (any subfolder, .png, .dds or .jpg;
// JPEG has no alpha, so only for opaque pictures such as backgrounds). All go through Windows' own WIC.
#define COBJMACROS
#include "host.h"
#include "mods_list.h"
#ifdef _WIN32
#include <wincodec.h>
#endif
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

struct entry { char name[64]; wchar_t path[MAX_PATH]; int order; };
static struct entry *index_;
static int nindex, cap;

static int entry_cmp(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    return c ? c : x->order - y->order;   // same name in two mods: the higher one in the list first
}

static void scan(const wchar_t *dir)
{
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(p, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (d.cFileName[0] == L'.') continue;
        swprintf(p, MAX_PATH, L"%ls\\%ls", dir, d.cFileName);
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { scan(p); continue; }
        wchar_t *ext = wcsrchr(d.cFileName, L'.');
        if (!ext || (_wcsicmp(ext, L".png") && _wcsicmp(ext, L".dds") && _wcsicmp(ext, L".jpg") && _wcsicmp(ext, L".jpeg"))
            || ext - d.cFileName >= 64) continue;
        if (nindex == cap) {
            struct entry *more = realloc(index_, (cap ? cap * 2 : 256) * sizeof *index_);
            if (!more) break;
            index_ = more; cap = cap ? cap * 2 : 256;
        }
        struct entry *e = &index_[nindex];
        int n = (int)(ext - d.cFileName);
        for (int i = 0; i < n; i++) e->name[i] = (char)towlower(d.cFileName[i]);
        e->name[n] = 0;
        wcscpy(e->path, p);
        e->order = nindex++;
    } while (FindNextFileW(h, &d));
    FindClose(h);
}

void texpack_init(const wchar_t *root)
{
    int n;
    const struct mod_item *all = host_mods(&n);
    for (int i = 0; i < n; i++) {
        if (!all[i].on) continue;
        wchar_t p[MAX_PATH];
        swprintf(p, MAX_PATH, L"%ls\\mods\\%ls\\textures", root, all[i].name);
        int before = nindex;
        scan(p);
        if (nindex > before) xlog("[texpack] mods\\%ls: %d replacement textures", all[i].name, nindex - before);
    }
    if (nindex) qsort(index_, nindex, sizeof *index_, entry_cmp);
#ifdef ONI_DEV_TOOLS
    texdump_init(root);
#endif
}

int texpack_replacing(void) { return nindex != 0; }

// Coverage: with a pack on, each texture the game draws is listed once in logs\textures_seen.txt ("HD" or
// "--" and its name, palette part as _$), and the log gets a running count. The list adds up over runs: it is read
// back at the start and only new textures (or a "--" that now has an HD version, as a later "HD" line) are appended;
// delete the file to start over. The -- lines are what the pack still lacks (UI and screen pictures are left out of
// the pack on purpose, so they show as -- too).
// ponytail: keyed by picture, not palette; fine for counting.
static FILE *seen_file;
static uint64_t seen_keys[1 << 15];
static uint8_t seen_hd_at[1 << 15];
static int seen_n, seen_hd;
static int seen_slot(const char *wild, int *fresh)   // the table slot of a name, -1 when the table is nearly full
{
    uint64_t k = fnv64_str(FNV64_START, wild);   // as keyset_add, but the slot is wanted too
    uint32_t i = keyset_slot(seen_keys, 1 << 15, k, fresh);
    if (*fresh) { if (seen_n >= (1 << 15) - 1024) return -1; seen_keys[i] = k | 1; seen_n++; }
    return (int)i;
}
static void seen_mark(int i, int replaced) { if (replaced && !seen_hd_at[i]) seen_hd_at[i] = 1, seen_hd++; }
void texpack_seen(const char *wild, int replaced)
{
    static int tried;
    int fresh;
    if (!tried++) {
        wchar_t p[MAX_PATH];
        host_path(p, L"logs\\textures_seen.txt");
        FILE *old = _wfopen(p, L"r");
        char line[128], name[100], tag[4];
        if (old) {
            while (fgets(line, sizeof line, old))
                if (sscanf(line, "%3s %99s", tag, name) == 2 && line[0] != ';') {
                    int i = seen_slot(name, &fresh);
                    if (i >= 0) seen_mark(i, !strcmp(tag, "HD"));
                }
            fclose(old);
        }
        if ((seen_file = _wfopen(p, L"a")) && !old)
            fputs("; Textures drawn, once each, adding up over runs (delete to start over): HD = the pack replaced it, -- = it didn't (texpack.c)\n", seen_file);
        if (seen_n) xlog("[texpack] coverage list: %d textures from earlier runs, %d with an HD version", seen_n, seen_hd);
    }
    int i = seen_slot(wild, &fresh);
    if (i < 0) return;
    int upgrade = !fresh && replaced && !seen_hd_at[i];
    if (!fresh && !upgrade) return;
    seen_mark(i, replaced);
    if (seen_file) { fprintf(seen_file, "%s %s\n", replaced ? "HD" : texpack_ignored(wild) ? "ok" : "--", wild); fflush(seen_file); }
    if (fresh && seen_n % 100 == 0) xlog("[texpack] coverage: %d of %d textures seen had an HD version (%d%%)", seen_hd, seen_n, 100 * seen_hd / seen_n);
}

#ifdef _WIN32
static IWICImagingFactory *wic_f;
static BOOL CALLBACK wic_start(PINIT_ONCE once, void *p, void **ctx)
{
    (void)once; (void)p; (void)ctx;
    CoInitializeEx(0, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE (already set up by someone) is fine too
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic_f))) {
        wic_f = 0;
        xlog("[texpack] couldn't start WIC: no texture loading or dumping");
    }
    return TRUE;
}
static IWICImagingFactory *wic(void)   // once, whichever thread asks first (the game's, or a read-ahead thread)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, wic_start, 0, 0);
    return wic_f;
}

#endif

static int name_cmp(const void *key, const void *e) { return strcmp(key, ((const struct entry *)e)->name); }

int texpack_has(const char *name) { return nindex && bsearch(name, index_, nindex, sizeof *index_, name_cmp); }

uint32_t *texpack_load_either(const char *name, const char *wild, uint32_t *w, uint32_t *h, const char **got)
{
    uint32_t *px = texpack_load(*got = name, w, h);
    if (!px && strcmp(name, wild)) px = texpack_load(*got = wild, w, h);
    return px;
}
int texpack_wild_name(const char *name, char *wild, size_t n)
{
    snprintf(wild, n, "%s", name);
    char *u = 0;   // size_format_texels_palette: the 3rd underscore starts the palette part
    int k = 0;
    for (char *c = wild; *c; c++) if (*c == '_' && ++k == 3) u = c;
    if (u && (size_t)(u - wild) + 3 <= n) strcpy(u, "_$");
    return u != 0;
}

uint32_t *texpack_load(const char *name, uint32_t *w, uint32_t *h)
{
    struct entry *e;
    if (!nindex || !(e = bsearch(name, index_, nindex, sizeof *index_, name_cmp))) return 0;
    while (e > index_ && !strcmp(e[-1].name, name)) e--;
    uint32_t *px = 0;
    UINT W = 0, H = 0;
#ifdef _WIN32
    IWICImagingFactory *f = wic();
    IWICBitmapDecoder *dec = 0;
    IWICBitmapFrameDecode *fr = 0;
    IWICBitmapSource *bgra = 0;
    if (f && SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, e->path, 0, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))
        && SUCCEEDED(WICConvertBitmapSource(&GUID_WICPixelFormat32bppBGRA, (IWICBitmapSource *)fr, &bgra))
        && SUCCEEDED(IWICBitmapSource_GetSize(bgra, &W, &H)) && W && H && W <= 16384 && H <= 16384 && (px = malloc((size_t)W * H * 4))
        && FAILED(IWICBitmapSource_CopyPixels(bgra, 0, W * 4, W * H * 4, (BYTE *)px))) { free(px); px = 0; }
    if (!px) xlog("[texpack] couldn't read %ls", e->path);
    if (bgra) IWICBitmapSource_Release(bgra);
    if (fr) IWICBitmapFrameDecode_Release(fr);
    if (dec) IWICBitmapDecoder_Release(dec);
#else
    px = compat_image_load(e->path, &W, &H);
    if (!px) xlog("[texpack] couldn't read %ls", e->path);
#endif
    // Alpha of 1 or 2 is transparent: upscalers leave a faint background there (98 pack sheets), and the game's alpha
    // test (alpha != 0) drew it solid, a black box around the ending's narration. Blended, it was invisible anyway.
    for (size_t i = 0; px && i < (size_t)W * H; i++) if (px[i] >> 24 <= 2) px[i] &= 0x00FFFFFFu;
    *w = W; *h = H;
    return px;
}

#ifdef _WIN32
void write_png(const wchar_t *p, const void *bgra, uint32_t w, uint32_t h, uint32_t pitch)
{
    IWICImagingFactory *f = wic();
    IWICStream *s = 0;
    IWICBitmapEncoder *enc = 0;
    IWICBitmapFrameEncode *fr = 0;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    int ok = f && SUCCEEDED(IWICImagingFactory_CreateStream(f, &s)) && SUCCEEDED(IWICStream_InitializeFromFilename(s, p, GENERIC_WRITE))
          && SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng, 0, &enc))
          && SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)s, WICBitmapEncoderNoCache))
          && SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, 0)) && SUCCEEDED(IWICBitmapFrameEncode_Initialize(fr, 0))
          && SUCCEEDED(IWICBitmapFrameEncode_SetSize(fr, w, h)) && SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(fr, &fmt))
          && IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA)
          && SUCCEEDED(IWICBitmapFrameEncode_WritePixels(fr, h, pitch, pitch * h, (BYTE *)bgra))
          && SUCCEEDED(IWICBitmapFrameEncode_Commit(fr)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc));
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (s) IWICStream_Release(s);
    if (!ok) { DeleteFileW(p); xlog("[texpack] couldn't write %ls", p); }
}
#else
void write_png(const wchar_t *p, const void *bgra, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (!compat_png_write(p, bgra, w, h, pitch)) xlog("[texpack] couldn't write %ls", p);
}
#endif
