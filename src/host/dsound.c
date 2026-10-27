// Xbox DirectSound (XDK 3936) buffers on top of XAudio2.
//
// Each Xbox sound buffer becomes one XAudio2 source voice. PCM plays straight out of the buffer's memory, so
// streaming writes through Lock land in what is playing, as on Xbox. Xbox ADPCM is decoded to PCM at Play and
// SetCurrentPosition, so writes to a playing ADPCM buffer are heard only from the next of those. Play cursors come
// from XAudio2's sample counter. 3D sounds are panned (below); reverb and effects are not done.
#define COBJMACROS
#include "host.h"
#include <windows.h>
#include <xaudio2.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define WAVE_FORMAT_XBOX_ADPCM 0x0069
#define DSBPLAY_LOOPING 1
#define DSBSTATUS_PLAYING 1
#define DSBSTATUS_LOOPING 4

static IXAudio2 *xa;
static IXAudio2MasteringVoice *master;
static SRWLOCK lock = SRWLOCK_INIT;

struct xbuf {
    const void *vtbl;                 // first field like a COM object, in case code peeks at it
    uint32_t refs;
    uint16_t tag, channels, bits;
    uint32_t rate;
    uint8_t *data; uint32_t bytes; int own_data;
    int16_t *pcm; uint32_t pcm_samples; int pcm_dirty;   // decoded ADPCM
    uint32_t loop_start, loop_len;    // bytes, Xbox units
    int is3d, mode;                   // DirectSound 3D: a mono buffer created with DSBCAPS_CTRL3D; DS3DMODE_*
    float where[3];                   // its position in the world
    uint32_t pos;                     // play cursor (bytes) while stopped
    int playing, looping;
    uint64_t samples_at_submit; uint32_t submit_sample;
    IXAudio2SourceVoice *voice;
    float volume, ratio;
};

#define DSBCAPS_CTRL3D 0x10
#define LOCKED(body) do { AcquireSRWLockExclusive(&lock); body; ReleaseSRWLockExclusive(&lock); } while (0)

// ---- DirectSound 3D -----------------------------------------------------------------------------------
// The game places sounds in the world (IDirectSoundBuffer_SetPosition) and sets the listener to the camera at each
// camera cut (IDirectSound_SetAllParameters: position, front = view direction, top = (0,-1,0)). Units are mm
// (distance factor 0.001) and rolloff is tiny or 0, so the audible effect is mostly placement around the camera.
// The pan follows DirectSound 3D's left-handed convention (right = top x front), so the game's vectors are used
// unchanged: each mono 3D voice gets an equal-power stereo pan from the sound's angle around the camera (sounds
// behind fold to the sides) and DirectSound's rolloff law for loudness (min distance 1, the SDK default; the game
// never sets one). Doppler is off in the game; the Xbox's HRTF and speaker mix bins are not reproduced.
static struct { float pos[3], front[3], top[3], rolloff; } listener = {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}, 1};
static struct xbuf *bufs3d[512];   // ponytail: fixed table; buffers past it just stay centred
static int n3d;

static void track3d(struct xbuf *b, int add)
{
    if (add) { if (n3d < (int)(sizeof bufs3d / sizeof *bufs3d)) bufs3d[n3d++] = b; return; }
    for (int i = 0; i < n3d; i++) if (bufs3d[i] == b) { bufs3d[i] = bufs3d[--n3d]; return; }
}

static float fl(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static int normalize3(float *v)
{
    float l = sqrtf(dot3(v, v));
    if (l < 1e-6f) return 0;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return 1;
}

// Pan one 3D buffer for the current listener (lock held).
static void spatialize(struct xbuf *b)
{
    if (!b->is3d || !b->voice) return;
    float m[2] = {0.70710678f, 0.70710678f};   // centred, equal power
    if (b->mode != 2) {   // DS3DMODE_DISABLE: no 3D
        float front[3], top[3], rel[3];
        if (b->mode == 1) {   // DS3DMODE_HEADRELATIVE: the position is already relative to the listener
            front[0] = 0; front[1] = 0; front[2] = 1; top[0] = 0; top[1] = 1; top[2] = 0;
            memcpy(rel, b->where, sizeof rel);
        } else {
            memcpy(front, listener.front, sizeof front);
            memcpy(top, listener.top, sizeof top);
            for (int i = 0; i < 3; i++) rel[i] = b->where[i] - listener.pos[i];
            if (!normalize3(front)) { front[0] = 0; front[1] = 0; front[2] = 1; }
            float d = dot3(top, front);   // the game's top isn't perpendicular to its view direction
            for (int i = 0; i < 3; i++) top[i] -= d * front[i];
            if (!normalize3(top)) { top[0] = 0; top[1] = 1; top[2] = 0; }
        }
        float right[3] = {top[1] * front[2] - top[2] * front[1], top[2] * front[0] - top[0] * front[2], top[0] * front[1] - top[1] * front[0]};
        float x = dot3(rel, right), z = dot3(rel, front), dist = sqrtf(dot3(rel, rel));
        float pan = x * x + z * z > 1e-6f ? x / sqrtf(x * x + z * z) : 0;   // sin of the angle: -1 left .. 1 right
        float a = (pan + 1) * 0.78539816f;                                   // 0 .. pi/2
        float att = dist <= 1.0f ? 1.0f : 1.0f / (1.0f + listener.rolloff * (dist - 1.0f));   // DirectSound 3D rolloff
        m[0] = cosf(a) * att; m[1] = sinf(a) * att;
    }
    IXAudio2Voice_SetOutputMatrix((IXAudio2Voice *)b->voice, 0, 1, 2, m, 0);
}

static void spatialize_all(void) { for (int i = 0; i < n3d; i++) spatialize(bufs3d[i]); }

static const void *buffer_vtbl[32];   // zeroed: nothing should call through it

// ---- Xbox ADPCM (IMA ADPCM, 36-byte blocks per channel: 1 header sample + 64 nibbles = 65 samples) -----
static const int16_t steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int8_t index_step[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
enum { ADPCM_BLOCK = 36, ADPCM_SAMPLES = 65 };

static int16_t adpcm_sample(int code, int *pred, int *idx)
{
    int step = steps[*idx], d = step >> 3;
    if (code & 4) d += step;
    if (code & 2) d += step >> 1;
    if (code & 1) d += step >> 2;
    if (code & 8) d = -d;
    *pred += d;
    if (*pred > 32767) *pred = 32767; else if (*pred < -32768) *pred = -32768;
    *idx += index_step[code];
    if (*idx > 88) *idx = 88; else if (*idx < 0) *idx = 0;
    return (int16_t)*pred;
}

static uint32_t adpcm_decode(const uint8_t *in, uint32_t bytes, int ch, int16_t *out)
{
    uint32_t blocks = bytes / (ADPCM_BLOCK * ch), n = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        int pred[2], idx[2];
        for (int c = 0; c < ch; c++, in += 4) {
            pred[c] = (int16_t)(in[0] | in[1] << 8);
            idx[c] = in[2] > 88 ? 88 : in[2];
            out[n * ch + c] = (int16_t)pred[c];
        }
        n++;
        for (int group = 0; group < 8; group++) {
            for (int c = 0; c < ch; c++, in += 4) {
                uint32_t code = in[0] | in[1] << 8 | in[2] << 16 | (uint32_t)in[3] << 24;
                for (int j = 0; j < 8; j++, code >>= 4) out[(n + j) * ch + c] = adpcm_sample(code & 15, &pred[c], &idx[c]);
            }
            n += 8;
        }
    }
    return n;
}

// ---- cursor maths ---------------------------------------------------------------------------------
static uint32_t frame_bytes(const struct xbuf *b) { return b->channels * (b->bits / 8 ? b->bits / 8 : 2); }
static uint32_t bytes_to_samples(const struct xbuf *b, uint32_t v)
{
    if (b->tag == WAVE_FORMAT_XBOX_ADPCM) return v / (ADPCM_BLOCK * b->channels) * ADPCM_SAMPLES;
    return v / frame_bytes(b);
}
static uint32_t samples_to_bytes(const struct xbuf *b, uint32_t s)
{
    if (b->tag == WAVE_FORMAT_XBOX_ADPCM) return s / ADPCM_SAMPLES * ADPCM_BLOCK * b->channels;
    return s * frame_bytes(b);
}
static uint32_t total_samples(const struct xbuf *b) { return bytes_to_samples(b, b->bytes); }

static uint32_t current_sample(struct xbuf *b)
{
    if (!b->playing || !b->voice) return bytes_to_samples(b, b->pos);
    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState(b->voice, &st, 0);
    uint64_t s = b->submit_sample + (st.SamplesPlayed - b->samples_at_submit);
    uint32_t total = total_samples(b);
    if (!st.BuffersQueued) { b->playing = 0; b->pos = 0; return 0; }   // one-shot finished
    if (b->looping && total) {
        uint32_t ls = bytes_to_samples(b, b->loop_start), le = b->loop_len ? ls + bytes_to_samples(b, b->loop_len) : total;
        if (le > total) le = total;
        if (s >= le && le > ls) s = ls + (s - le) % (le - ls);
    }
    return (uint32_t)(total ? s % total : 0);
}

// ---- voice handling -------------------------------------------------------------------------------
static int ensure_voice(struct xbuf *b)
{
    if (b->voice) return 1;
    if (!xa) return 0;
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, b->channels, b->rate, 0, 0, 16, 0};
    if (b->tag == WAVE_FORMAT_PCM && b->bits == 8) f.wBitsPerSample = 8;
    f.nBlockAlign = f.nChannels * f.wBitsPerSample / 8;
    f.nAvgBytesPerSec = f.nBlockAlign * f.nSamplesPerSec;
    if (FAILED(IXAudio2_CreateSourceVoice(xa, &b->voice, &f, 0, 4.0f, 0, 0, 0))) {
        xlog("[dsound] CreateSourceVoice (%u ch, %u Hz, %u bit) failed", b->channels, b->rate, f.wBitsPerSample);
        return 0;
    }
    IXAudio2SourceVoice_SetVolume(b->voice, b->volume, 0);
    IXAudio2SourceVoice_SetFrequencyRatio(b->voice, b->ratio, 0);
    spatialize(b);
    return 1;
}

// Stop and Flush are asynchronous: the audio thread may still read the buffer after them. DestroyVoice waits for it,
// so it comes first whenever memory a voice may be playing is freed or replaced (ensure_voice makes a new one).
static void drop_voice(struct xbuf *b)
{
    if (b->voice) { IXAudio2SourceVoice_DestroyVoice(b->voice); b->voice = 0; }
}

static void submit(struct xbuf *b, uint32_t start_sample)
{
    if (!b->data || !b->bytes || !xa) return;
    const uint8_t *audio = b->data;
    uint32_t audio_bytes = b->bytes;
    if (b->tag == WAVE_FORMAT_XBOX_ADPCM) {
        if (!b->pcm || b->pcm_dirty) {
            int16_t *pcm = malloc((size_t)(b->bytes / ADPCM_BLOCK + 1) * ADPCM_SAMPLES * 2);
            if (!pcm) return;
            uint32_t n = adpcm_decode(b->data, b->bytes, b->channels, pcm);
            drop_voice(b);   // it may be playing the old decode
            free(b->pcm);
            b->pcm = pcm; b->pcm_samples = n; b->pcm_dirty = 0;
        }
        audio = (const uint8_t *)b->pcm;
        audio_bytes = b->pcm_samples * 2 * b->channels;
    }
    if (!ensure_voice(b)) return;
    uint32_t total = audio_bytes / ((b->tag == WAVE_FORMAT_PCM && b->bits == 8 ? 1 : 2) * b->channels);
    if (start_sample >= total) start_sample = 0;
    XAUDIO2_BUFFER xb = {0};
    xb.AudioBytes = audio_bytes;
    xb.pAudioData = audio;
    xb.PlayBegin = start_sample;
    if (b->looping) {
        xb.LoopBegin = bytes_to_samples(b, b->loop_start);
        xb.LoopLength = b->loop_len ? bytes_to_samples(b, b->loop_len) : 0;
        if (xb.LoopBegin >= total) xb.LoopBegin = 0;
        if (xb.LoopBegin + xb.LoopLength > total) xb.LoopLength = 0;
        xb.LoopCount = XAUDIO2_LOOP_INFINITE;
    } else {
        xb.Flags = XAUDIO2_END_OF_STREAM;
    }
    IXAudio2SourceVoice_Stop(b->voice, 0, 0);
    IXAudio2SourceVoice_FlushSourceBuffers(b->voice);
    XAUDIO2_VOICE_STATE st;
    IXAudio2SourceVoice_GetState(b->voice, &st, 0);
    if (FAILED(IXAudio2SourceVoice_SubmitSourceBuffer(b->voice, &xb, 0))) {
        xlog("[dsound] submit failed (begin %u loop %u+%u total %u)", xb.PlayBegin, xb.LoopBegin, xb.LoopLength, total);
        return;
    }
    b->samples_at_submit = st.SamplesPlayed;
    b->submit_sample = start_sample;
    IXAudio2SourceVoice_Start(b->voice, 0, 0);
    b->playing = 1;
}

static void stop(struct xbuf *b)
{
    if (!b->playing) return;
    b->pos = samples_to_bytes(b, current_sample(b));
    b->playing = 0;
    if (b->voice) { IXAudio2SourceVoice_Stop(b->voice, 0, 0); IXAudio2SourceVoice_FlushSourceBuffers(b->voice); }
}

static float db100_to_gain(int32_t v) { return v <= -10000 ? 0.0f : powf(10.0f, v / 2000.0f); }

// ---- HLE: IDirectSound ----------------------------------------------------------------------------
static uint32_t ds_object[16] = {(uint32_t)buffer_vtbl, 1};

static uint32_t __stdcall DirectSoundCreate(uint32_t guid, uint32_t ppDS, uint32_t unused)
{
    AcquireSRWLockExclusive(&lock);
    if (!xa) {
        CoInitializeEx(0, COINIT_MULTITHREADED);
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(IXAudio2_CreateMasteringVoice(xa, &master, 2, 48000, 0, 0, 0, AudioCategory_GameEffects))) {
            xlog("[dsound] XAudio2 init failed - running silent");
            if (xa) { IXAudio2_Release(xa); xa = 0; }   // no voices without a mastering voice
        } else {
            xlog("[dsound] XAudio2 ready");
            if (host_background()) IXAudio2MasteringVoice_SetVolume(master, 0.0f, 0);   // tours run muted
        }
    }
    ReleaseSRWLockExclusive(&lock);
    *(uint32_t **)ppDS = ds_object;
    return 0;
}
static uint32_t __stdcall ds_ok1(uint32_t a) { return 0; }
static uint32_t __stdcall ds_ok2(uint32_t a, uint32_t b) { return 0; }
static uint32_t __stdcall ds_ok3(uint32_t a, uint32_t b, uint32_t c) { return 0; }
static uint32_t __stdcall ds_ok0(void) { return 0; }


// SetAllParameters(pThis, const DS3DLISTENER *, dwApply): {dwSize, position, velocity, front, top, distance factor,
// rolloff, doppler}. Applied at once (the deferred/commit distinction doesn't matter for panning).
static uint32_t __stdcall IDirectSound_SetAllParameters(uint32_t pThis, uint32_t p, uint32_t apply)
{
    const float *l = (const float *)(p + 4);
    LOCKED(memcpy(listener.pos, l, 12); memcpy(listener.front, l + 6, 12); memcpy(listener.top, l + 9, 12);
           listener.rolloff = l[13]; spatialize_all(););
    return 0;
}
// I3DL2 reverb isn't reproduced. Where the game seen so far sets it, it's off (room level -10000); report any
// place that turns it on.
static uint32_t __stdcall IDirectSound_SetI3DL2Listener(uint32_t pThis, uint32_t p, uint32_t apply)
{
    int32_t room = *(const int32_t *)p;
    if (room > -10000) xfeature(1, "sound: I3DL2 reverb on (room level %d)", room);
    return 0;
}
static uint32_t __stdcall IDirectSound_SetRolloffFactor(uint32_t pThis, uint32_t v, uint32_t apply)
{
    LOCKED(listener.rolloff = fl(v); spatialize_all(););
    return 0;
}
static uint32_t __stdcall IDirectSoundBuffer_SetPosition(uint32_t pThis, uint32_t x, uint32_t y, uint32_t z, uint32_t apply)
{
    struct xbuf *b = (struct xbuf *)pThis;
    LOCKED(b->where[0] = fl(x); b->where[1] = fl(y); b->where[2] = fl(z); spatialize(b););
    return 0;
}
static uint32_t __stdcall IDirectSoundBuffer_SetMode(uint32_t pThis, uint32_t mode, uint32_t apply)
{
    struct xbuf *b = (struct xbuf *)pThis;
    LOCKED(b->mode = (int)mode; spatialize(b););
    return 0;
}

// DownloadEffectsImage(pThis, pvImageBuffer, dwImageSize, pImageLoc, ppImageDesc): the DSP effects image
// (reverb etc.) has no host equivalent yet. Hand back an empty effect map.
static uint32_t effect_desc[64];
static uint32_t __stdcall IDirectSound_DownloadEffectsImage(uint32_t pThis, uint32_t buf, uint32_t size, uint32_t loc, uint32_t ppDesc)
{
    if (ppDesc) *(uint32_t **)ppDesc = effect_desc;
    return 0;
}

// CreateSoundBuffer(pThis, DSBUFFERDESC *, ppBuffer, unused).
// DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, lpwfxFormat, lpMixBins, dwInputMixBin
static uint32_t __stdcall IDirectSound_CreateSoundBuffer(uint32_t pThis, uint32_t pdsbd, uint32_t ppBuffer, uint32_t unused)
{
    const uint32_t *d = (const uint32_t *)pdsbd;
    const WAVEFORMATEX *f = (const WAVEFORMATEX *)d[3];
    struct xbuf *b = calloc(1, sizeof *b);
    if (!b) { *(struct xbuf **)ppBuffer = 0; return 0x8007000Eu; }   // DSERR_OUTOFMEMORY, not a write through null
    b->vtbl = buffer_vtbl; b->refs = 1; b->volume = 1.0f; b->ratio = 1.0f;
    b->tag = f ? f->wFormatTag : WAVE_FORMAT_PCM;
    b->channels = f && f->nChannels ? f->nChannels : 1;
    b->rate = f && f->nSamplesPerSec ? f->nSamplesPerSec : 48000;
    b->bits = f ? f->wBitsPerSample : 16;
    if (b->tag != WAVE_FORMAT_PCM && b->tag != WAVE_FORMAT_XBOX_ADPCM) { xlog("[dsound] buffer format tag %x not supported", b->tag); xfeature(1, "sound: buffer format tag %x", b->tag); }
    if (d[2]) {
        b->bytes = d[2]; b->data = calloc(1, b->bytes); b->own_data = 1;
        if (!b->data) { free(b); *(struct xbuf **)ppBuffer = 0; return 0x8007000Eu; }
    }
    b->is3d = (d[1] & DSBCAPS_CTRL3D) && b->channels == 1;
    if (b->is3d) LOCKED(track3d(b, 1));
    *(struct xbuf **)ppBuffer = b;
    return 0;
}

// ---- HLE: IDirectSoundBuffer --------------------------------------------------------------------------
#define B ((struct xbuf *)pThis)

static uint32_t __stdcall IDirectSoundBuffer_SetBufferData(uint32_t pThis, uint32_t pv, uint32_t n)
{
    LOCKED(
        stop(B);
        drop_voice(B);   // the game may free its old data now
        if (B->own_data) { free(B->data); B->own_data = 0; }
        B->data = (uint8_t *)pv; B->bytes = n; B->pcm_dirty = 1; B->pos = 0;
    );
    return 0;
}
static uint32_t __stdcall IDirectSoundBuffer_Play(uint32_t pThis, uint32_t r1, uint32_t r2, uint32_t flags)
{
    LOCKED(
        int looping = (flags & DSBPLAY_LOOPING) != 0;
        uint32_t at = B->playing ? current_sample(B) : bytes_to_samples(B, B->pos);   // current_sample may stop it
        if (!B->playing || looping != B->looping) { B->looping = looping; submit(B, at); }   // looping is set at submit
    );
    return 0;
}
static uint32_t __stdcall IDirectSoundBuffer_Stop(uint32_t pThis) { LOCKED(stop(B)); return 0; }
static uint32_t __stdcall IDirectSoundBuffer_GetStatus(uint32_t pThis, uint32_t pStatus)
{
    LOCKED(
        current_sample(B);   // notices one-shot buffers that finished
        *(uint32_t *)pStatus = B->playing ? DSBSTATUS_PLAYING | (B->looping ? DSBSTATUS_LOOPING : 0) : 0;
    );
    return 0;
}
static uint32_t __stdcall IDirectSoundBuffer_GetCurrentPosition(uint32_t pThis, uint32_t pPlay, uint32_t pWrite)
{
    LOCKED(
        uint32_t p = samples_to_bytes(B, current_sample(B));
        if (pPlay) *(uint32_t *)pPlay = p;
        if (pWrite) *(uint32_t *)pWrite = B->bytes ? (p + 1024) % B->bytes : 0;   // ponytail: fixed 1 KB lead
    );
    return 0;
}
static uint32_t __stdcall IDirectSoundBuffer_SetCurrentPosition(uint32_t pThis, uint32_t pos)
{
    LOCKED(
        if (B->playing) submit(B, bytes_to_samples(B, pos)); else B->pos = pos;
    );
    return 0;
}
static uint32_t __stdcall IDirectSoundBuffer_SetLoopRegion(uint32_t pThis, uint32_t start, uint32_t len)
{
    LOCKED(B->loop_start = start; B->loop_len = len;);   // ponytail: takes effect at the next Play
    return 0;
}
static uint32_t __stdcall IDirectSoundBuffer_SetVolume(uint32_t pThis, uint32_t vol)
{
    LOCKED(B->volume = db100_to_gain((int32_t)vol); if (B->voice) IXAudio2SourceVoice_SetVolume(B->voice, B->volume, 0););
    return 0;
}
static uint32_t __stdcall IDirectSoundBuffer_SetFrequency(uint32_t pThis, uint32_t hz)
{
    LOCKED(
        B->ratio = hz ? (float)hz / B->rate : 1.0f;
        if (B->ratio > 4.0f) B->ratio = 4.0f;
        if (B->voice) IXAudio2SourceVoice_SetFrequencyRatio(B->voice, B->ratio, 0);
    );
    return 0;
}
// Lock(pThis, dwOffset, dwBytes, ppv1, pcb1, ppv2, pcb2, dwFlags): DSBLOCK_FROMWRITECURSOR 1, DSBLOCK_ENTIREBUFFER 2
static uint32_t __stdcall IDirectSoundBuffer_Lock(uint32_t pThis, uint32_t off, uint32_t n, uint32_t pp1, uint32_t pn1, uint32_t pp2, uint32_t pn2, uint32_t flags)
{
    LOCKED(
        if (flags & 2) { off = 0; n = B->bytes; }
        if (flags & 1) off = B->bytes ? (samples_to_bytes(B, current_sample(B)) + 1024) % B->bytes : 0;
        if (off >= B->bytes) off = 0;
        uint32_t first = n < B->bytes - off ? n : B->bytes - off;
        *(uint8_t **)pp1 = B->data + off; *(uint32_t *)pn1 = first;
        if (pp2) *(uint8_t **)pp2 = first < n ? B->data : 0;
        if (pn2) *(uint32_t *)pn2 = first < n ? n - first : 0;
        B->pcm_dirty = 1;
    );
    return 0;
}
static uint32_t __stdcall IDirectSoundBuffer_Unlock(uint32_t pThis, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return 0; }
static uint32_t __stdcall IDirectSoundBuffer_Release(uint32_t pThis)
{
    uint32_t left;
    LOCKED(
        left = --B->refs;
        if (!left) {
            if (B->is3d) track3d(B, 0);
            stop(B);
            drop_voice(B);
            if (B->own_data) free(B->data);
            free(B->pcm);
            free(B);
        }
    );
    return left;
}

static uint32_t __stdcall stream_pause(uint32_t pThis, uint32_t pause)
{
    static int n;
    if (n++ < 4) xlog("[dsound] TODO stream %p (vtable %08x) Pause(%u)", (void *)pThis, *(uint32_t *)pThis, pause);
    return 0;
}

// ---- a movie's sound from a .wav beside its .bik ------------------------------------------------------
// For a remade movie (e.g. the uncut Hecuba) whose .bik has no sound: today's Bink tools only write the newer Bink
// audio, which the game's 2001 Bink can't play. The .wav (PCM) starts with the movie's first frame (d3d_texture.c calls
// movie_sound_start on every movie frame upload) and stops when the game closes the .bik (skipping included).
static struct { void *file; uint8_t *data; IXAudio2SourceVoice *voice; XAUDIO2_BUFFER buf; int started; } msnd;

static void movie_sound_free(void)   // lock held
{
    if (msnd.voice) IXAudio2SourceVoice_DestroyVoice(msnd.voice);
    free(msnd.data);
    memset(&msnd, 0, sizeof msnd);
}

void movie_sound_open(const wchar_t *wav, void *file)
{
    uint32_t n = 0;
    uint8_t *d = read_file(wav, 256u << 20, &n);
    if (d && n <= 44) { free(d); d = 0; }
    WAVEFORMATEX fmt = {0};
    const uint8_t *pcm = 0;
    uint32_t bytes = 0;
    for (uint32_t i = 12; d && !memcmp(d, "RIFF", 4) && i + 8 <= n;) {
        uint32_t len = *(uint32_t *)(d + i + 4);
        if (!memcmp(d + i, "fmt ", 4) && len >= 16 && n - i - 8 >= 16) memcpy(&fmt, d + i + 8, 16);
        if (!memcmp(d + i, "data", 4)) { pcm = d + i + 8; bytes = len <= n - i - 8 ? len : n - i - 8; }
        uint64_t next = (uint64_t)i + 8 + ((len + 1ull) & ~1ull);   // 64-bit: a chunk length past the end ends the walk
        if (next > n) break;                                       // instead of wrapping i round (a file that hung it)
        i = (uint32_t)next;
    }
    AcquireSRWLockExclusive(&lock);
    movie_sound_free();
    if (!xa || !pcm || fmt.wFormatTag != WAVE_FORMAT_PCM || FAILED(IXAudio2_CreateSourceVoice(xa, &msnd.voice, &fmt, 0, 2.0f, 0, 0, 0))) {
        xlog("[dsound] movie sound %ls: not a PCM .wav, or no audio", wav);
        msnd.voice = 0;
        free(d);
    } else {
        msnd.data = d;
        msnd.file = file;
        msnd.buf.AudioBytes = bytes; msnd.buf.pAudioData = pcm; msnd.buf.Flags = XAUDIO2_END_OF_STREAM;
        IXAudio2SourceVoice_SubmitSourceBuffer(msnd.voice, &msnd.buf, 0);
        xlog("[dsound] movie sound %ls (%u Hz, %u ch, %.1f s)", wav, fmt.nSamplesPerSec, fmt.nChannels, bytes / (double)fmt.nAvgBytesPerSec);
    }
    ReleaseSRWLockExclusive(&lock);
}

void movie_sound_start(void)
{
    if (!msnd.voice || msnd.started) return;
    AcquireSRWLockExclusive(&lock);
    if (msnd.voice && !msnd.started) { IXAudio2SourceVoice_Start(msnd.voice, 0, 0); msnd.started = 1; }
    ReleaseSRWLockExclusive(&lock);
}

void movie_sound_close(void *file)
{
    if (!msnd.voice || file != msnd.file) return;
    AcquireSRWLockExclusive(&lock);
    if (file == msnd.file) movie_sound_free();
    ReleaseSRWLockExclusive(&lock);
}

const struct hle_impl dsound_impls[] = {
    {"DirectSoundCreate", DirectSoundCreate}, {"DirectSoundDoWork", ds_ok0}, {"DirectSoundUseFullHRTF", ds_ok0},
    {"IDirectSound_SetDistanceFactor", ds_ok3}, {"IDirectSound_SetDopplerFactor", ds_ok3}, {"IDirectSound_SetRolloffFactor", IDirectSound_SetRolloffFactor},
    {"IDirectSound_SetI3DL2Listener", IDirectSound_SetI3DL2Listener}, {"IDirectSound_SetAllParameters", IDirectSound_SetAllParameters}, {"IDirectSound_CommitDeferredSettings", ds_ok1},
    {"IDirectSound_EnableHeadphones", ds_ok2}, {"IDirectSound_Release", ds_ok1},
    {"IDirectSound_DownloadEffectsImage", IDirectSound_DownloadEffectsImage},
    {"IDirectSound_CreateSoundBuffer", IDirectSound_CreateSoundBuffer},
    {"IDirectSoundBuffer_SetBufferData", IDirectSoundBuffer_SetBufferData}, {"IDirectSoundBuffer_Play", IDirectSoundBuffer_Play},
    {"IDirectSoundBuffer_Stop", IDirectSoundBuffer_Stop}, {"IDirectSoundBuffer_GetStatus", IDirectSoundBuffer_GetStatus},
    {"IDirectSoundBuffer_GetCurrentPosition", IDirectSoundBuffer_GetCurrentPosition},
    {"IDirectSoundBuffer_SetCurrentPosition", IDirectSoundBuffer_SetCurrentPosition},
    {"IDirectSoundBuffer_SetLoopRegion", IDirectSoundBuffer_SetLoopRegion}, {"IDirectSoundBuffer_SetVolume", IDirectSoundBuffer_SetVolume},
    {"IDirectSoundBuffer_SetFrequency", IDirectSoundBuffer_SetFrequency}, {"IDirectSoundBuffer_Lock", IDirectSoundBuffer_Lock},
    {"IDirectSoundBuffer_Unlock", IDirectSoundBuffer_Unlock}, {"IDirectSoundBuffer_Release", IDirectSoundBuffer_Release},
    {"IDirectSoundBuffer_SetHeadroom", ds_ok2}, {"IDirectSoundBuffer_SetMixBins", ds_ok2}, {"IDirectSoundBuffer_SetMixBinVolumes_12", ds_ok3},
    {"IDirectSoundBuffer_SetMode", IDirectSoundBuffer_SetMode}, {"IDirectSoundBuffer_SetI3DL2Source", ds_ok3},
    {"IDirectSoundBuffer_SetPosition", IDirectSoundBuffer_SetPosition},
    {"IDirectSoundStream_Pause", stream_pause}, {"CDirectSoundStream_SetVolume", ds_ok2},
    {0, 0},
};
