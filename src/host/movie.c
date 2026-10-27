// HD movies: when the game opens image\movie\<name>.bik and an enabled mod has movies\<name>.mp4 or <name>_*.mp4 with
// as many frames (H.264, the Bink's
// size times a whole number, 30 fps, no sound needed), the game still plays its Bink (sound, timing, subtitles,
// skipping are all the game's own) and d3d_texture.c swaps each decoded movie frame texture for the same frame of the HD
// file. Media Foundation (built into Windows; on Linux FFmpeg, src/linux/movie_ff.c) decodes it on its own thread a
// few frames ahead.
//
// Which frame: Bink's own frame counter, read from its state in game memory (HBINK: width, height, frame count,
// current frame, ...), found once per movie by those first three values from the .bik header. (A clock drifted a
// few frames against the game's Bink in tests; picture matching was unreliable in dark scenes.)
#define COBJMACROS
#include "host.h"
#include <windows.h>
#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#define SLOTS 4
#define BINK_OFFSET -1   // the frame in the game's texture is one behind Bink's counter (checked against the .bik)
struct slot { uint32_t *px; int idx; };   // idx: frame number, -1 free, -2 being filled

static struct {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    HANDLE thread, file;
#ifdef _WIN32
    IMFSourceReader *rd;
#else
    void *rd;
#endif
    uint32_t W, H;                 // decoded size
    struct slot s[SLOTS];
    int on, stop, eof, need;       // need: the lowest frame still wanted
    LONGLONG ts0;                  // the file's first timestamp (-1: none yet); H.264 B-frames start it above 0
    int shown, missed;
} mv = {SRWLOCK_INIT, CONDITION_VARIABLE_INIT};

// The .bik the game has open (any, with or without an HD file): its header, and Bink's state once found.
static struct {
    void *file;
    uint32_t bink[3];              // width, height, frame count
    volatile uint32_t *hbink;      // Bink's state in game memory; 0: not found (yet)
    int scanned;
    struct sub { int a, b; wchar_t *text; } *subs;   // subtitles: from frame a up to b
    int nsubs;
} bk;

// NV12 (BT.709, limited range, as movie_up.py writes it) -> BGRA, two rows at a time.
static void nv12_bgra(const uint8_t *y0, const uint8_t *uv0, LONG pitch, uint32_t *out, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y += 2) {
        const uint8_t *uv = uv0 + (LONG)(y / 2) * pitch;
        for (int r = 0; r < 2 && y + r < h; r++) {
            const uint8_t *yy = y0 + (LONG)(y + r) * pitch;
            uint32_t *o = out + (size_t)(y + r) * w;
            for (uint32_t x = 0; x < w; x++) {
                int u = uv[x & ~1u] - 128, v = uv[x | 1] - 128, c = (yy[x] - 16) * 75;
                int R = (c + 115 * v + 32) >> 6, G = (c - 14 * u - 34 * v + 32) >> 6, B = (c + 135 * u + 32) >> 6;
                R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G; B = B < 0 ? 0 : B > 255 ? 255 : B;
                o[x] = 0xFF000000u | R << 16 | G << 8 | B;
            }
        }
    }
}

// One decoded frame into s->px; returns its frame number, or -1 (skipped or failed; *end set at the end or on error).
// Frames before mv.need are skipped without converting them (after a jump, e.g. the game dropping frames).
#ifdef _WIN32
static int decode_one(struct slot *s, int *end)
{
    DWORD flags = 0;
    LONGLONG ts = 0;
    IMFSample *smp = 0;
    int idx = -1;
    HRESULT hr = IMFSourceReader_ReadSample(mv.rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, 0, &flags, &ts, &smp);
    if (FAILED(hr)) xlog("[movie] decode failed (%08lx)", hr);
    *end = FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM);
    if (smp && !*end && mv.ts0 < 0) mv.ts0 = ts;
    if (smp && !*end && (idx = (int)(((ts - mv.ts0) * 30 + 5000000) / 10000000)) >= mv.need) {   // 100 ns units, 30 fps
        IMFMediaBuffer *b = 0;
        IMF2DBuffer2 *b2 = 0;
        BYTE *p, *start;
        LONG pitch;
        DWORD len;
        // The decoder pads the height (to 16): the UV plane starts after all of the Y plane's rows.
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(smp, &b)) && SUCCEEDED(IMFMediaBuffer_QueryInterface(b, &IID_IMF2DBuffer2, (void **)&b2))
            && SUCCEEDED(IMF2DBuffer2_Lock2DSize(b2, MF2DBuffer_LockFlags_Read, &p, &pitch, &start, &len))) {
            uint32_t rows = pitch > 0 ? len / pitch * 2 / 3 : 0;
            if (rows >= mv.H) nv12_bgra(p, p + (size_t)rows * pitch, pitch, s->px, mv.W, mv.H); else idx = -1;
            IMF2DBuffer2_Unlock2D(b2);
        } else idx = -1;
        if (b2) IMF2DBuffer2_Release(b2);
        if (b) IMFMediaBuffer_Release(b);
    } else idx = -1;
    if (smp) IMFSample_Release(smp);
    return idx;
}
#else   // Linux: FFmpeg (src/linux/movie_ff.c)
struct ff;
struct ff *ff_open(const char *path, uint32_t *w, uint32_t *h, int *frames);
int ff_frame(struct ff *f, uint32_t *bgra, int need, int *end);
void ff_close(struct ff *f);
static int decode_one(struct slot *s, int *end) { return ff_frame(mv.rd, s->px, mv.need, end); }
#endif

static DWORD WINAPI decode_thread(void *arg)
{
    (void)arg;
    CoInitializeEx(0, COINIT_MULTITHREADED);
    for (int end = 0; !end;) {
        AcquireSRWLockExclusive(&mv.lock);
        struct slot *s = 0;
        while (!mv.stop && !s) {
            for (int i = 0; i < SLOTS && !s; i++) if (mv.s[i].idx == -1) s = &mv.s[i];
            if (!s) SleepConditionVariableSRW(&mv.cv, &mv.lock, INFINITE, 0);
        }
        if (mv.stop) { ReleaseSRWLockExclusive(&mv.lock); break; }
        s->idx = -2;
        ReleaseSRWLockExclusive(&mv.lock);
        int idx = decode_one(s, &end);
        AcquireSRWLockExclusive(&mv.lock);
        s->idx = idx;
        mv.eof = end;
        WakeAllConditionVariable(&mv.cv);
        ReleaseSRWLockExclusive(&mv.lock);
    }
    CoUninitialize();
    return 0;
}

// Bink's state: {width, height, frame count, current frame} in the game's memory (below 256 MB; seen at 0x2d95e60).
static volatile uint32_t *find_bink_in(uint8_t *a, uint8_t *end)
{
    MEMORY_BASIC_INFORMATION m;
    for (; a < end && VirtualQuery(a, &m, sizeof m); a = (uint8_t *)m.BaseAddress + m.RegionSize) {
        if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) continue;
        const uint32_t *p = m.BaseAddress, *e = (const uint32_t *)((uint8_t *)m.BaseAddress + m.RegionSize) - 4;
        for (; p < e; p++)
            if (p[0] == bk.bink[0] && p[1] == bk.bink[1] && p[2] == bk.bink[2] && p[3] <= p[2]) return (volatile uint32_t *)p;
    }
    return 0;
}
static volatile uint32_t *find_bink(void)
{
#ifdef _WIN32
    return find_bink_in((uint8_t *)0x10000, (uint8_t *)0x10000000);
#else   // Linux: the game's memory is compat's VirtualAlloc reservations, which mmap places high
    static uintptr_t r[8192][2];   // {base, size}; not on the stack (the game's are 64 KB)
    volatile uint32_t *p = 0;
    for (int i = 0, n = compat_reservations(r, 8192); i < n && !p; i++) p = find_bink_in((uint8_t *)r[i][0], (uint8_t *)(r[i][0] + r[i][1]));
    return p;
#endif
}

// Every .bik the game opens: remembered with its header (for the frame counter and the HD file's frame-count check).
void movie_bik(const wchar_t *bik, void *file)
{
    for (int i = 0; i < bk.nsubs; i++) free(bk.subs[i].text);
    free(bk.subs);
    memset(&bk, 0, sizeof bk);
    HANDLE bf = CreateFileW(bik, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    uint8_t hd[28] = {0};   // "BIKi", size, frames (+8), ..., width (+20), height (+24)
    DWORD got = 0;
    if (bf != INVALID_HANDLE_VALUE) { ReadFile(bf, hd, sizeof hd, &got, 0); CloseHandle(bf); }
    if (got < sizeof hd) return;
    memcpy(&bk.bink[2], hd + 8, 4);
    memcpy(&bk.bink[0], hd + 20, 8);
    bk.file = file;
}

// The frame of the open movie the game is showing now (Bink's counter - 1), or -1.
static int movie_now(void)
{
    if (!bk.file) return -1;
    if (!bk.hbink && bk.scanned++ < 3 && !(bk.hbink = find_bink()))   // Bink's state is there by its first frame
        xlog("[movie] Bink's frame counter not found (%ux%u, %u frames)", bk.bink[0], bk.bink[1], bk.bink[2]);
    volatile uint32_t *b = bk.hbink;
    if (!b || b[0] != bk.bink[0] || b[1] != bk.bink[1] || b[2] != bk.bink[2]) return -1;
    return (int)b[3] + BINK_OFFSET;
}

// Subtitles: a UTF-8 .srt ("1", "00:00:01,300 --> 00:00:03,100", text lines, blank), times turned into frames.
void movie_subs(const wchar_t *srt)
{
    uint32_t got = 0;
    char *d = (char *)read_file(srt, 1u << 20, &got);   // NUL-terminated
    if (!d) return;
    for (char *p = got ? d : 0; p && *p;) {
        int h1, m1, s1, ms1, h2, m2, s2, ms2, k = 0;
        char *arrow = strstr(p, "-->");
        if (!arrow) break;
        char *line = arrow; while (line > p && line[-1] != '\n') line--;
        if (sscanf(line, "%d:%d:%d,%d --> %d:%d:%d,%d%n", &h1, &m1, &s1, &ms1, &h2, &m2, &s2, &ms2, &k) < 8) { p = arrow + 3; continue; }
        char *t = strchr(line + k, '\n'); t = t ? t + 1 : line + k;
        if (*t == '\n' || (t[0] == '\r' && t[1] == '\n')) { p = t; continue; }   // no text: an empty cue, skipped (not the next one's lines)
        char *e = strstr(t, "\n\n"), *e2 = strstr(t, "\r\n\r\n");
        if (!e || (e2 && e2 < e)) e = e2;
        if (!e) e = t + strlen(t);
        while (e > t && (e[-1] == '\r' || e[-1] == '\n')) e--;
        int wn = MultiByteToWideChar(CP_UTF8, 0, t, (int)(e - t), 0, 0);
        if (wn <= 0) { p = e; continue; }   // a cue with no text: skipped, the ones after it still read
        struct sub *more = realloc(bk.subs, (bk.nsubs + 1) * sizeof *bk.subs);
        if (more) bk.subs = more;           // at once: realloc may have moved the list and freed the old one
        wchar_t *w = more ? malloc((wn + 1) * sizeof *w) : 0;
        if (!w) break;
        MultiByteToWideChar(CP_UTF8, 0, t, (int)(e - t), w, wn); w[wn] = 0;
        for (wchar_t *c = w; *c; c++) if (*c == L'\r') *c = L' ';
        bk.subs[bk.nsubs].a = (int)(((h1 * 60 + m1) * 60 + s1) * 30 + ms1 * 30 / 1000);
        bk.subs[bk.nsubs].b = (int)(((h2 * 60 + m2) * 60 + s2) * 30 + ms2 * 30 / 1000);
        bk.subs[bk.nsubs++].text = w;
        p = e;
    }
    free(d);
    xlog("[movie] subtitles %ls: %d lines", srt, bk.nsubs);
}

const wchar_t *movie_subtitle(void)
{
    if (!bk.nsubs) return 0;
    int now = movie_now();
    for (int i = 0; now >= 0 && i < bk.nsubs; i++) if (now >= bk.subs[i].a && now < bk.subs[i].b) return bk.subs[i].text;
    return 0;
}

#ifdef _WIN32
int movie_open(const wchar_t *path, void *file)
{
    LONGLONG t0 = qpc();
    movie_close(mv.file);
    static int started;
    if (!started++ && FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) xlog("[movie] couldn't start Media Foundation");
    IMFMediaType *t = 0;
    IMFSourceReader *rd = 0;
    UINT64 size = 0;
    // NV12 out: Media Foundation's own conversion to RGB took 33 ms a frame at 2560x1440 (nv12_bgra: 10 ms)
    HRESULT hr = bk.file == file ? MFCreateSourceReaderFromURL(path, 0, &rd) : E_FAIL;
    if (SUCCEEDED(hr)) hr = IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) hr = IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(t, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, t);
    if (t) { IMFMediaType_Release(t); t = 0; }
    if (SUCCEEDED(hr)) hr = IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t);
    if (SUCCEEDED(hr)) hr = IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &size);
    if (t) IMFMediaType_Release(t);
    PROPVARIANT dur;   // must be the same cut as the .bik: as many frames (within one)
    PropVariantInit(&dur);
    if (SUCCEEDED(hr)) hr = IMFSourceReader_GetPresentationAttribute(rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &dur);
    int frames = SUCCEEDED(hr) ? (int)((dur.uhVal.QuadPart * 30 + 5000000) / 10000000) : 0;
    if (SUCCEEDED(hr) && abs(frames - (int)bk.bink[2]) > 1) {
        xlog("[movie] %ls has %d frames, the .bik %u: not used", path, frames, bk.bink[2]);
        hr = S_FALSE;   // not an error: another file may be the right cut
    }
    mv.W = (uint32_t)(size >> 32); mv.H = (uint32_t)size;
    int ok = hr == S_OK && mv.W && mv.H && mv.W <= 8192 && mv.H <= 8192;   // W * H * 4 fits in 32 bits
    for (int i = 0; i < SLOTS; i++) {
        mv.s[i].idx = -1;
        if (ok && !(mv.s[i].px = malloc((size_t)mv.W * mv.H * 4))) { xlog("[movie] out of memory for %ux%u frames", mv.W, mv.H); ok = 0; }
    }
    if (!ok) {
        if (hr != S_FALSE) xlog("[movie] can't play %ls (%08lx)", path, hr);
        for (int i = 0; i < SLOTS; i++) { free(mv.s[i].px); mv.s[i].px = 0; }
        if (rd) IMFSourceReader_Release(rd);
        return 0;
    }
    mv.rd = rd;
    mv.stop = mv.eof = mv.need = mv.shown = mv.missed = 0;
    mv.ts0 = -1;
    mv.file = file;
    mv.thread = CreateThread(0, 0, decode_thread, 0, 0, 0);
    mv.on = 1;
    xlog("[movie] HD %ls, %ux%u (opened in %.1f ms)", path, mv.W, mv.H, qpc_ms(qpc() - t0));
    return 1;
}
#else   // Linux: FFmpeg, opened on a thread of its own (this is called on the game's, whose stacks are 64 KB)
static struct { char path[1024]; struct ff *f; uint32_t w, h; int frames; } fo;
static DWORD WINAPI open_thread(void *arg) { (void)arg; fo.f = ff_open(fo.path, &fo.w, &fo.h, &fo.frames); return 0; }

int movie_open(const wchar_t *path, void *file)
{
    LONGLONG t0 = qpc();
    movie_close(mv.file);
    HANDLE t = 0;
    fo.f = 0;
    if (bk.file == file && compat_path(path, fo.path, sizeof fo.path) && (t = CreateThread(0, 0, open_thread, 0, 0, 0))) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
    if (!fo.f) return 0;   // (ff_open logged why)
    if (abs(fo.frames - (int)bk.bink[2]) > 1) {   // must be the same cut as the .bik: as many frames (within one)
        xlog("[movie] %ls has %d frames, the .bik %u: not used", path, fo.frames, bk.bink[2]);
        ff_close(fo.f);
        return 0;   // not an error: another file may be the right cut
    }
    mv.W = fo.w; mv.H = fo.h;
    int ok = mv.W && mv.H && mv.W <= 8192 && mv.H <= 8192;   // W * H * 4 fits in 32 bits
    if (!ok) xlog("[movie] can't play %ls (%ux%u)", path, mv.W, mv.H);
    for (int i = 0; i < SLOTS; i++) {
        mv.s[i].idx = -1;
        if (ok && !(mv.s[i].px = malloc((size_t)mv.W * mv.H * 4))) { xlog("[movie] out of memory for %ux%u frames", mv.W, mv.H); ok = 0; }
    }
    if (!ok) {
        for (int i = 0; i < SLOTS; i++) { free(mv.s[i].px); mv.s[i].px = 0; }
        ff_close(fo.f);
        return 0;
    }
    mv.rd = fo.f;
    mv.stop = mv.eof = mv.need = mv.shown = mv.missed = 0;
    mv.file = file;
    mv.thread = CreateThread(0, 0, decode_thread, 0, 0, 0);
    mv.on = 1;
    xlog("[movie] HD %ls, %ux%u (opened in %.1f ms)", path, mv.W, mv.H, qpc_ms(qpc() - t0));
    return 1;
}
#endif

void movie_close(void *file)
{
    if (file && file == bk.file) { for (int i = 0; i < bk.nsubs; i++) free(bk.subs[i].text); free(bk.subs); memset(&bk, 0, sizeof bk); }
    if (!mv.on || file != mv.file) return;
    LONGLONG t0 = qpc();
    AcquireSRWLockExclusive(&mv.lock);
    mv.on = 0;
    mv.stop = 1;
    WakeAllConditionVariable(&mv.cv);
    ReleaseSRWLockExclusive(&mv.lock);
    WaitForSingleObject(mv.thread, INFINITE);
    CloseHandle(mv.thread);
#ifdef _WIN32
    IMFSourceReader_Release(mv.rd);
#else
    ff_close(mv.rd);
#endif
    mv.rd = 0;
    for (int i = 0; i < SLOTS; i++) { free(mv.s[i].px); mv.s[i].px = 0; }
    xlog("[movie] closed in %.1f ms: %d HD frames shown, %d of the game's own (HD frame not ready)",
         qpc_ms(qpc() - t0), mv.shown, mv.missed);
    mv.file = 0;
}

static int movie_size(uint32_t w, uint32_t h, uint32_t *W, uint32_t *H)
{
    if (!mv.on || !w || !h || mv.W % w) return 0;
    uint32_t k = mv.W / w;
    if (mv.H < k * h || mv.H >= k * h + 16) return 0;   // H.264 may pad the height to a multiple of 16
    *W = mv.W; *H = k * h;
    return 1;
}

const uint32_t *movie_frame(uint32_t w, uint32_t h, uint32_t *W, uint32_t *H)
{
    if (!movie_size(w, h, W, H)) return 0;
    int want = movie_now();
    if (want < 0) return 0;
    AcquireSRWLockExclusive(&mv.lock);
    mv.need = want;
    for (int i = 0; i < SLOTS; i++) if (mv.s[i].idx >= 0 && mv.s[i].idx < want) mv.s[i].idx = -1;
    WakeAllConditionVariable(&mv.cv);
    struct slot *s = 0;
    for (int tries = 0; !s && tries < 6; tries++) {   // up to ~50 ms for the decoder to get there
        for (int i = 0; i < SLOTS; i++) if (mv.s[i].idx == want) s = &mv.s[i];
        if (!s && !mv.eof) SleepConditionVariableSRW(&mv.cv, &mv.lock, 10, 0);
    }
    if (s) mv.shown++; else mv.missed++;
    if (!s) ReleaseSRWLockExclusive(&mv.lock);
    return s ? s->px : 0;   // still locked: movie_frame_done
}

void movie_frame_done(void) { ReleaseSRWLockExclusive(&mv.lock); }
