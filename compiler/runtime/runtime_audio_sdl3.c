/* ============================================================
 * runtime_audio_sdl3.c — minimal SFX + looping ambient (drives the game #46).
 *
 * The engine had NO audio path. This adds one on the SDL3 backend the SDL/wgpu
 * apps already link. SDL3 mixes every AudioStream bound to a device and converts
 * each stream's input format to the device format, so there is no hand-written
 * mixer or callback here — we just create a device, keep a small pool of streams
 * ("channels"), and put WAV bytes into a free one to play a sound.
 *
 * Guarded: the real implementation compiles under RAE_HAS_SDL3; otherwise no-op
 * stubs, so a non-SDL3 app still links. Every entry point is safe to call before
 * (or without) a working device — it just no-ops, so a headless/CI run with no
 * audio hardware runs fine and silent.
 * ============================================================ */
#ifdef RAE_HAS_SDL3
#include <SDL3/SDL.h>

#define RAE_AUDIO_MAX_CLIPS 32
#define RAE_AUDIO_CHANNELS  16

static SDL_AudioDeviceID g_ra_dev = 0;
static SDL_AudioSpec     g_ra_devspec;
static int               g_ra_ready = 0;

typedef struct { Uint8* buf; Uint32 len; SDL_AudioSpec spec; } RaeClip;
static RaeClip g_ra_clips[RAE_AUDIO_MAX_CLIPS];
static int     g_ra_clip_count = 0;

static SDL_AudioStream* g_ra_chan[RAE_AUDIO_CHANNELS];
static int              g_ra_loop_clip[RAE_AUDIO_CHANNELS]; /* -1 = one-shot channel */

void rae_ext_audio_init(void) {
    if (g_ra_ready) return;
    if (getenv("RAE_NO_AUDIO")) return;   /* opt-out for silent captures */
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[audio] SDL_INIT_AUDIO failed: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want; SDL_zero(want);
    want.format = SDL_AUDIO_F32; want.channels = 2; want.freq = 48000;
    g_ra_dev = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);
    if (!g_ra_dev) {
        fprintf(stderr, "[audio] open device failed: %s\n", SDL_GetError());
        return;
    }
    if (!SDL_GetAudioDeviceFormat(g_ra_dev, &g_ra_devspec, NULL)) g_ra_devspec = want;
    for (int i = 0; i < RAE_AUDIO_CHANNELS; i++) { g_ra_chan[i] = NULL; g_ra_loop_clip[i] = -1; }
    g_ra_ready = 1;
    fprintf(stderr, "[audio] ready — %d Hz, %d ch\n", g_ra_devspec.freq, g_ra_devspec.channels);
}

int64_t rae_ext_audio_load_wav(rae_String path) {
    if (!path.data) return -1;
    if (g_ra_clip_count >= RAE_AUDIO_MAX_CLIPS) return -1;
    SDL_AudioSpec spec; Uint8* buf = NULL; Uint32 len = 0;
    if (!SDL_LoadWAV((const char*)path.data, &spec, &buf, &len)) {
        fprintf(stderr, "[audio] load_wav %s failed: %s\n", (const char*)path.data, SDL_GetError());
        return -1;
    }
    int id = g_ra_clip_count++;
    g_ra_clips[id].buf = buf; g_ra_clips[id].len = len; g_ra_clips[id].spec = spec;
    return (int64_t)id;
}

/* Reuse or create the stream on channel `ch`, retargeting it to convert `src` -> device. */
static SDL_AudioStream* rae_audio_stream_for(int ch, const SDL_AudioSpec* src) {
    if (!g_ra_chan[ch]) {
        g_ra_chan[ch] = SDL_CreateAudioStream(src, &g_ra_devspec);
        if (g_ra_chan[ch]) SDL_BindAudioStream(g_ra_dev, g_ra_chan[ch]);
    } else {
        SDL_SetAudioStreamFormat(g_ra_chan[ch], src, &g_ra_devspec);
    }
    return g_ra_chan[ch];
}

void rae_ext_audio_play(int64_t clip, float volume) {
    if (!g_ra_ready || clip < 0 || clip >= g_ra_clip_count) return;
    int ch = -1;                                   /* first drained one-shot channel */
    for (int i = 0; i < RAE_AUDIO_CHANNELS; i++) {
        if (g_ra_loop_clip[i] >= 0) continue;
        if (!g_ra_chan[i] || SDL_GetAudioStreamQueued(g_ra_chan[i]) <= 0) { ch = i; break; }
    }
    if (ch < 0) return;                            /* all channels busy — drop this one-shot */
    SDL_AudioStream* s = rae_audio_stream_for(ch, &g_ra_clips[(int)clip].spec);
    if (!s) return;
    SDL_SetAudioStreamGain(s, volume);
    SDL_PutAudioStreamData(s, g_ra_clips[(int)clip].buf, (int)g_ra_clips[(int)clip].len);
}

/* Loop a clip on a dedicated channel; returns the channel id (or -1). audio_tick
 * keeps it topped up so it never gaps. Idempotent for the same clip. */
int64_t rae_ext_audio_loop(int64_t clip, float volume) {
    if (!g_ra_ready || clip < 0 || clip >= g_ra_clip_count) return -1;
    int ch = -1;
    for (int i = 0; i < RAE_AUDIO_CHANNELS; i++) if (g_ra_loop_clip[i] == (int)clip) { ch = i; break; }
    if (ch < 0) for (int i = 0; i < RAE_AUDIO_CHANNELS; i++)
        if (g_ra_loop_clip[i] < 0 && (!g_ra_chan[i] || SDL_GetAudioStreamQueued(g_ra_chan[i]) <= 0)) { ch = i; break; }
    if (ch < 0) return -1;
    SDL_AudioStream* s = rae_audio_stream_for(ch, &g_ra_clips[(int)clip].spec);
    if (!s) return -1;
    SDL_SetAudioStreamGain(s, volume);
    g_ra_loop_clip[ch] = (int)clip;
    SDL_PutAudioStreamData(s, g_ra_clips[(int)clip].buf, (int)g_ra_clips[(int)clip].len);
    return (int64_t)ch;
}

/* Keep every looping channel from draining. Call once per frame. */
void rae_ext_audio_tick(void) {
    if (!g_ra_ready) return;
    for (int i = 0; i < RAE_AUDIO_CHANNELS; i++) {
        int clip = g_ra_loop_clip[i];
        if (clip >= 0 && g_ra_chan[i] &&
            SDL_GetAudioStreamQueued(g_ra_chan[i]) < (int)g_ra_clips[clip].len) {
            SDL_PutAudioStreamData(g_ra_chan[i], g_ra_clips[clip].buf, (int)g_ra_clips[clip].len);
        }
    }
}

#else
void    rae_ext_audio_init(void) {}
int64_t rae_ext_audio_load_wav(rae_String path) { (void)path; return -1; }
void    rae_ext_audio_play(int64_t clip, float volume) { (void)clip; (void)volume; }
int64_t rae_ext_audio_loop(int64_t clip, float volume) { (void)clip; (void)volume; return -1; }
void    rae_ext_audio_tick(void) {}
#endif
