// Linux: the HD movies' decoder (movie.c) on FFmpeg, which deps.sh builds with only the MP4 demuxer and the H.264
// decoder: a file's frames as BGRA, converted as the Windows path's nv12_bgra does (BT.709, limited range, as
// movie_up.py writes them). libavutil, libswscale, libavcodec and libavformat are loaded when a movie first needs them,
// from bin/ beside the executable; without them the game's own movies play.
#include <windows.h>
#include <dlfcn.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

void xlog(const char *fmt, ...);

#define FUNCS(X) \
    X(av_log_set_callback) X(av_strerror) X(av_frame_alloc) X(av_frame_free) X(av_frame_unref) \
    X(sws_getCachedContext) X(sws_setColorspaceDetails) X(sws_getCoefficients) X(sws_scale) X(sws_freeContext) \
    X(avcodec_alloc_context3) X(avcodec_parameters_to_context) X(avcodec_open2) X(avcodec_free_context) \
    X(avcodec_send_packet) X(avcodec_receive_frame) X(av_packet_alloc) X(av_packet_free) X(av_packet_unref) \
    X(avformat_open_input) X(avformat_close_input) X(av_find_best_stream) X(av_read_frame)
static struct {
#define X(f) __typeof__(f) *f;
    FUNCS(X)
#undef X
} F;

struct ff {
    AVFormatContext *fmt;
    AVCodecContext *c;
    AVPacket *pkt;
    AVFrame *fr;
    struct SwsContext *sws;
    int stream;
    int64_t ts0;                   // the first frame's timestamp (AV_NOPTS_VALUE: none yet); B-frames start it above 0
    AVRational tb;
    uint32_t w, h;
};

// FFmpeg's errors into the game's log (a damaged file's: the first few)
static void ff_log(void *avcl, int level, const char *fmt, va_list ap)
{
    static int n;
    char line[512];
    (void)avcl;
    if (level > AV_LOG_ERROR || n >= 20) return;
    n++;
    vsnprintf(line, sizeof line, fmt, ap);
    size_t k = strlen(line);
    while (k && line[k - 1] == '\n') line[--k] = 0;
    xlog("[movie] FFmpeg: %s", line);
}

static int load(void)
{
    static int state;   // 1 loaded, -1 not there
    if (state) return state > 0;
    // in this order: each finds the ones it needs already loaded (bin/ is searched for the executable's own only)
    static const char *const libs[] = {"libavutil.so." AV_STRINGIFY(LIBAVUTIL_VERSION_MAJOR),
        "libswscale.so." AV_STRINGIFY(LIBSWSCALE_VERSION_MAJOR), "libavcodec.so." AV_STRINGIFY(LIBAVCODEC_VERSION_MAJOR),
        "libavformat.so." AV_STRINGIFY(LIBAVFORMAT_VERSION_MAJOR)};
    void *h[4];
    state = -1;
    for (int i = 0; i < 4; i++)
        if (!(h[i] = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL))) { xlog("[movie] no HD movies (%s)", dlerror()); return 0; }
#define X(f) for (int i = 0; i < 4 && !F.f; i++) F.f = (__typeof__(f) *)dlsym(h[i], #f); \
             if (!F.f) { xlog("[movie] no HD movies (FFmpeg has no %s)", #f); return 0; }
    FUNCS(X)
#undef X
    F.av_log_set_callback(ff_log);
    state = 1;
    return 1;
}

void ff_close(struct ff *f)
{
    if (!f) return;
    F.sws_freeContext(f->sws);
    F.av_frame_free(&f->fr);
    F.av_packet_free(&f->pkt);
    F.avcodec_free_context(&f->c);
    F.avformat_close_input(&f->fmt);
    free(f);
}

// path (UTF-8): its picture size, and its length in frames at 30 fps (as the Windows path counts it, from the
// file's duration). 0: not playable (logged).
struct ff *ff_open(const char *path, uint32_t *w, uint32_t *h, int *frames)
{
    if (!load()) return 0;
    struct ff *f = calloc(1, sizeof *f);
    const AVCodec *codec = 0;
    int r = f ? F.avformat_open_input(&f->fmt, path, 0, 0) : AVERROR(ENOMEM);
    if (r >= 0) r = f->stream = F.av_find_best_stream(f->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (r >= 0) r = (f->c = F.avcodec_alloc_context3(codec)) ? 0 : AVERROR(ENOMEM);
    if (r >= 0) r = F.avcodec_parameters_to_context(f->c, f->fmt->streams[f->stream]->codecpar);
    if (r >= 0) {
        f->c->thread_count = 0;   // a thread per core, frames decoded side by side
        r = F.avcodec_open2(f->c, codec, 0);
    }
    if (r >= 0) r = (f->pkt = F.av_packet_alloc()) && (f->fr = F.av_frame_alloc()) ? 0 : AVERROR(ENOMEM);
    if (r < 0) {
        char e[128] = "out of memory";
        if (f) F.av_strerror(r, e, sizeof e);
        xlog("[movie] can't play %s (%s)", path, e);
        ff_close(f);
        return 0;
    }
    AVStream *st = f->fmt->streams[f->stream];
    f->tb = st->time_base;
    f->ts0 = AV_NOPTS_VALUE;
    *w = f->w = (uint32_t)f->c->width;
    *h = f->h = (uint32_t)f->c->height;
    int64_t d = f->fmt->duration > 0 ? f->fmt->duration
              : st->duration > 0 ? st->duration * AV_TIME_BASE * f->tb.num / f->tb.den : 0;   // AV_TIME_BASE units
    *frames = (int)((d * 30 + AV_TIME_BASE / 2) / AV_TIME_BASE);
    return f;
}

// The next frame in showing order: its number (30 fps from the first), converted into bgra (w * h) when it isn't
// below need, else -1 (skipped, not converted). -1 with *end set: the file's end, or an error.
int ff_frame(struct ff *f, uint32_t *bgra, int need, int *end)
{
    int r;
    *end = 0;
    while ((r = F.avcodec_receive_frame(f->c, f->fr)) == AVERROR(EAGAIN)) {
        if (F.av_read_frame(f->fmt, f->pkt) < 0) { F.avcodec_send_packet(f->c, 0); continue; }   // the end: the last frames
        if (f->pkt->stream_index == f->stream) F.avcodec_send_packet(f->c, f->pkt);   // a damaged one: FFmpeg logs it
        F.av_packet_unref(f->pkt);
    }
    if (r < 0) {
        if (r != AVERROR_EOF) { char e[128]; F.av_strerror(r, e, sizeof e); xlog("[movie] decode failed (%s)", e); }
        *end = 1;
        return -1;
    }
    AVFrame *fr = f->fr;
    int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
    if (f->ts0 == AV_NOPTS_VALUE) f->ts0 = ts;
    int idx = ts == AV_NOPTS_VALUE ? -1 : (int)(((ts - f->ts0) * 30 * f->tb.num + f->tb.den / 2) / f->tb.den);
    if (idx >= need) {
        struct SwsContext *s = F.sws_getCachedContext(f->sws, fr->width, fr->height, fr->format, (int)f->w, (int)f->h,
                                                       AV_PIX_FMT_BGRA, SWS_BILINEAR, 0, 0, 0);
        if (s && s != f->sws) {   // a new one: BT.709, limited range in, full range out
            const int *bt709 = F.sws_getCoefficients(SWS_CS_ITU709);
            F.sws_setColorspaceDetails(s, bt709, 0, bt709, 1, 0, 1 << 16, 1 << 16);
        }
        f->sws = s;
        uint8_t *dst[4] = {(uint8_t *)bgra};
        int pitch[4] = {(int)f->w * 4};
        if (!s || F.sws_scale(s, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, pitch) <= 0) idx = -1;
    } else idx = -1;
    F.av_frame_unref(fr);
    return idx;
}
