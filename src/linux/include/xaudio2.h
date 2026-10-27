// Linux: the XAudio2 calls dsound.c makes, on FAudio (the open reimplementation of XAudio2: same structures, same
// calls under other names).
#pragma once
#include <FAudio.h>

typedef FAudio IXAudio2;
typedef FAudioVoice IXAudio2Voice;
typedef FAudioSourceVoice IXAudio2SourceVoice;
typedef FAudioMasteringVoice IXAudio2MasteringVoice;
typedef FAudioBuffer XAUDIO2_BUFFER;
typedef FAudioVoiceState XAUDIO2_VOICE_STATE;
typedef FAudioWaveFormatEx WAVEFORMATEX;
#define WAVE_FORMAT_PCM 1
#define XAUDIO2_DEFAULT_PROCESSOR FAUDIO_DEFAULT_PROCESSOR
#define XAUDIO2_END_OF_STREAM FAUDIO_END_OF_STREAM
#define XAUDIO2_LOOP_INFINITE FAUDIO_LOOP_INFINITE
#define AudioCategory_GameEffects 0

#define XAudio2Create(pp, flags, proc) FAudioCreate((pp), (flags), (proc))
#define IXAudio2_Release(x) FAudio_Release(x)
#define IXAudio2_CreateSourceVoice(x, pv, fmt, flags, ratio, cb, sends, fx) \
    FAudio_CreateSourceVoice((x), (pv), (const FAudioWaveFormatEx *)(fmt), (flags), (ratio), (cb), (sends), (fx))
// XAudio2 2.9: (..., flags, device id, effect chain, stream category); FAudio: (..., flags, device index, effect chain)
#define IXAudio2_CreateMasteringVoice(x, pv, ch, rate, flags, device, fx, category) \
    FAudio_CreateMasteringVoice((x), (pv), (ch), (rate), (flags), 0, (fx))
#define IXAudio2MasteringVoice_SetVolume(v, vol, op) FAudioVoice_SetVolume((FAudioVoice *)(v), (vol), (op))
#define IXAudio2Voice_SetOutputMatrix(v, dst, in, out, m, op) FAudioVoice_SetOutputMatrix((v), (dst), (in), (out), (m), (op))
#define IXAudio2SourceVoice_SetVolume(v, vol, op) FAudioVoice_SetVolume((FAudioVoice *)(v), (vol), (op))
#define IXAudio2SourceVoice_DestroyVoice(v) FAudioVoice_DestroyVoice((FAudioVoice *)(v))
#define IXAudio2SourceVoice_Start(v, flags, op) FAudioSourceVoice_Start((v), (flags), (op))
#define IXAudio2SourceVoice_Stop(v, flags, op) FAudioSourceVoice_Stop((v), (flags), (op))
#define IXAudio2SourceVoice_SubmitSourceBuffer(v, b, wma) FAudioSourceVoice_SubmitSourceBuffer((v), (b), (wma))
#define IXAudio2SourceVoice_FlushSourceBuffers(v) FAudioSourceVoice_FlushSourceBuffers(v)
#define IXAudio2SourceVoice_GetState(v, st, flags) FAudioSourceVoice_GetState((v), (st), (flags))
#define IXAudio2SourceVoice_SetFrequencyRatio(v, r, op) FAudioSourceVoice_SetFrequencyRatio((v), (r), (op))
