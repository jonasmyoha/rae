/* The audio package's C glue (rae_audio.h). Platform reason: opening and
 * running the sound device (miniaudio's device layer over Core Audio, WASAPI,
 * ALSA/PulseAudio/JACK, AAudio, Web Audio), the thread's real-time policy, and
 * writing a file. The one place with more than one call is the device
 * callback, which must be C until Rae functions can be called from C
 * (docs/audio-design.md §4.2 stage A): it copies a period out of the Rae ring
 * and posts the Rae Signal, nothing more. */
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
/* The decoders: miniaudio's own MP3 and FLAC (dr_mp3, dr_flac) and WAV, and
 * Ogg Vorbis through stb_vorbis (native/miniaudio/stb_vorbis.c, public domain
 * or MIT, the copy miniaudio 0.11.26 ships in extras/), in the order
 * miniaudio's documentation gives */
#define STB_VORBIS_HEADER_ONLY
#include "miniaudio/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "miniaudio/stb_vorbis.c"

#include "rae_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

/* lib/Signal.rae's post, and the runtime's thread attach and real-time mark,
 * from the Rae runtime the program links */
void rae_ext_rae_signal_post(int64_t signal);
void rae_thread_attach(void);
void rae_ext_Realtime_markThread(bool realtime);

/* Stage B: audio/AudioDevice's `realtime cExport("rae_audio_render")`. A
 * program that uses audio/Audio defines it, and its definition replaces this
 * weak, empty one at link time; a program that only uses audio/Sound links
 * this file too (for its file calls) and never calls it. */
__attribute__((weak)) void rae_audio_render(void* state, float* output, int64_t frames, int64_t channels) {
  (void)state; (void)output; (void)frames; (void)channels;
}

typedef struct {
  ma_context context;
  ma_device device;
  float* slots;
  int64_t* counters;      /* [0] read (ours), [8] written (Rae's) */
  int64_t capacity;       /* in samples */
  int64_t signal;
  int64_t underrun_frames;
  int64_t callbacks;
  void* render_state;     /* stage B: the Rae state, or NULL for stage A */
} RaeAudioDevice;

/* The device's thread, attached to the runtime and marked real-time once */
static __thread int g_rae_audio_thread_ready;

static void rae_audio_callback(ma_device* device, void* output, const void* input, ma_uint32 frames) {
  (void)input;
  RaeAudioDevice* audio = (RaeAudioDevice*)device->pUserData;
  float* out = (float*)output;
  int64_t channels = (int64_t)device->playback.channels;
  if (audio->render_state) {
    /* Stage B: Rae renders straight into the device's buffer. The thread is
     * attached and marked on its first callback (a device may change it). */
    if (!g_rae_audio_thread_ready) {
      rae_thread_attach();
      rae_ext_Realtime_markThread(true);
      g_rae_audio_thread_ready = 1;
    }
    rae_audio_render(audio->render_state, out, (int64_t)frames, channels);
    __atomic_fetch_add(&audio->callbacks, 1, __ATOMIC_RELAXED);
    return;
  }
  int64_t wanted = (int64_t)frames * channels;
  int64_t taken = 0;
  if (audio->slots) {
    int64_t read = __atomic_load_n(&audio->counters[0], __ATOMIC_RELAXED);
    int64_t written = __atomic_load_n(&audio->counters[8], __ATOMIC_ACQUIRE);
    int64_t available = written - read;
    taken = available < wanted ? available : wanted;
    for (int64_t i = 0; i < taken; i++) out[i] = audio->slots[(read + i) % audio->capacity];
    __atomic_store_n(&audio->counters[0], read + taken, __ATOMIC_RELEASE);
  }
  if (taken < wanted) {
    memset(out + taken, 0, (size_t)(wanted - taken) * sizeof(float));
    __atomic_fetch_add(&audio->underrun_frames, (wanted - taken) / channels, __ATOMIC_RELAXED);
  }
  __atomic_fetch_add(&audio->callbacks, 1, __ATOMIC_RELAXED);
  if (audio->signal) rae_ext_rae_signal_post(audio->signal);
}

int64_t rae_audio_device_open(int64_t sample_rate, int64_t channels, int64_t period_frames) {
  RaeAudioDevice* audio = (RaeAudioDevice*)calloc(1, sizeof(RaeAudioDevice));
  if (!audio) return 0;
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = (ma_uint32)channels;
  config.sampleRate = (ma_uint32)sample_rate;
  config.periodSizeInFrames = (ma_uint32)period_frames;
  config.performanceProfile = ma_performance_profile_low_latency;
  config.noFixedSizedCallback = MA_TRUE;
  config.dataCallback = rae_audio_callback;
  config.pUserData = audio;
  /* On iOS the Rae policy owns the audio session (audio/AudioSession):
   * miniaudio leaves its category alone and never activates or deactivates
   * it. Elsewhere these settings change nothing. */
  ma_context_config context_config = ma_context_config_init();
  context_config.coreaudio.sessionCategory = ma_ios_session_category_none;
  context_config.coreaudio.noAudioSessionActivate = MA_TRUE;
  context_config.coreaudio.noAudioSessionDeactivate = MA_TRUE;
  if (ma_context_init(NULL, 0, &context_config, &audio->context) != MA_SUCCESS) {
    free(audio);
    return 0;
  }
  if (ma_device_init(&audio->context, &config, &audio->device) != MA_SUCCESS) {
    ma_context_uninit(&audio->context);
    free(audio);
    return 0;
  }
  return (int64_t)(intptr_t)audio;
}

int64_t rae_audio_device_sample_rate(int64_t device) {
  return device ? (int64_t)((RaeAudioDevice*)(intptr_t)device)->device.sampleRate : 0;
}

int64_t rae_audio_device_channels(int64_t device) {
  return device ? (int64_t)((RaeAudioDevice*)(intptr_t)device)->device.playback.channels : 0;
}

int64_t rae_audio_device_period_frames(int64_t device) {
  return device ? (int64_t)((RaeAudioDevice*)(intptr_t)device)->device.playback.internalPeriodSizeInFrames : 0;
}

void rae_audio_device_attach(int64_t device, float* slots, int64_t* counters, int64_t capacity,
                             int64_t signal) {
  RaeAudioDevice* audio = (RaeAudioDevice*)(intptr_t)device;
  if (!audio) return;
  audio->slots = slots;
  audio->counters = counters;
  audio->capacity = capacity;
  audio->signal = signal;
}

void rae_audio_device_attach_render(int64_t device, void* state) {
  RaeAudioDevice* audio = (RaeAudioDevice*)(intptr_t)device;
  if (audio) audio->render_state = state;
}

int64_t rae_audio_device_start(int64_t device) {
  return device && ma_device_start(&((RaeAudioDevice*)(intptr_t)device)->device) == MA_SUCCESS;
}

int64_t rae_audio_device_stop(int64_t device) {
  return device && ma_device_stop(&((RaeAudioDevice*)(intptr_t)device)->device) == MA_SUCCESS;
}

void rae_audio_device_close(int64_t device) {
  RaeAudioDevice* audio = (RaeAudioDevice*)(intptr_t)device;
  if (!audio) return;
  ma_device_uninit(&audio->device);
  ma_context_uninit(&audio->context);
  free(audio);
}

int64_t rae_audio_device_underrun_frames(int64_t device) {
  return device ? __atomic_load_n(&((RaeAudioDevice*)(intptr_t)device)->underrun_frames, __ATOMIC_RELAXED) : 0;
}

int64_t rae_audio_device_callbacks(int64_t device) {
  return device ? __atomic_load_n(&((RaeAudioDevice*)(intptr_t)device)->callbacks, __ATOMIC_RELAXED) : 0;
}

int64_t rae_audio_thread_set_realtime(int64_t period_ns, int64_t compute_ns) {
#if defined(__APPLE__)
  mach_timebase_info_data_t timebase;
  mach_timebase_info(&timebase);
  double ticks_per_ns = (double)timebase.denom / (double)timebase.numer;
  thread_time_constraint_policy_data_t policy;
  policy.period = (uint32_t)((double)period_ns * ticks_per_ns);
  policy.computation = (uint32_t)((double)compute_ns * ticks_per_ns);
  policy.constraint = policy.period;
  policy.preemptible = 1;
  return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                           (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
  (void)period_ns; (void)compute_ns;
  return 0;
#endif
}

int64_t rae_audio_write_file(uint8_t* path_z, uint8_t* bytes, int64_t count) {
  FILE* file = fopen((const char*)path_z, "wb");
  if (!file) return 0;
  size_t wrote = fwrite(bytes, 1, (size_t)count, file);
  return (fclose(file) == 0 && wrote == (size_t)count) ? 1 : 0;
}

int64_t rae_audio_file_size(uint8_t* path_z) {
  FILE* file = fopen((const char*)path_z, "rb");
  if (!file) return -1;
  int64_t size = -1;
  if (fseek(file, 0, SEEK_END) == 0) size = (int64_t)ftell(file);
  fclose(file);
  return size;
}

int64_t rae_audio_read_file(uint8_t* path_z, uint8_t* into, int64_t count) {
  FILE* file = fopen((const char*)path_z, "rb");
  if (!file) return -1;
  size_t got = fread(into, 1, (size_t)count, file);
  fclose(file);
  return (int64_t)got;
}

/* The session's events: one producer and one consumer, both the main thread
 * (the observers run on the main queue; inject is called there too). */
#define RAE_AUDIO_EVENT_CAPACITY 64
static int64_t g_rae_audio_events[RAE_AUDIO_EVENT_CAPACITY];
static int64_t g_rae_audio_events_written;
static int64_t g_rae_audio_events_read;

void rae_audio_session_inject(int64_t kind, int64_t value) {
  int64_t written = __atomic_load_n(&g_rae_audio_events_written, __ATOMIC_RELAXED);
  int64_t read = __atomic_load_n(&g_rae_audio_events_read, __ATOMIC_ACQUIRE);
  if (written - read >= RAE_AUDIO_EVENT_CAPACITY) return;
  g_rae_audio_events[written % RAE_AUDIO_EVENT_CAPACITY] = kind * 16 + value;
  __atomic_store_n(&g_rae_audio_events_written, written + 1, __ATOMIC_RELEASE);
}

int64_t rae_audio_session_poll(void) {
  int64_t read = __atomic_load_n(&g_rae_audio_events_read, __ATOMIC_RELAXED);
  int64_t written = __atomic_load_n(&g_rae_audio_events_written, __ATOMIC_ACQUIRE);
  if (read == written) return -1;
  int64_t event = g_rae_audio_events[read % RAE_AUDIO_EVENT_CAPACITY];
  __atomic_store_n(&g_rae_audio_events_read, read + 1, __ATOMIC_RELEASE);
  return event;
}

#if defined(MA_APPLE_MOBILE) && defined(__OBJC__)
#include <dlfcn.h>
#include <objc/message.h>
/* iOS: AVAudioSession. This file is compiled as Objective-C there (the
 * package installs it as src/rae_audio_glue.m for device builds), as
 * miniaudio's own iOS code needs. */
int64_t rae_audio_session_available(void) {
  return 1;
}

int64_t rae_audio_session_configure(int64_t category, int64_t mode, int64_t options) {
  @autoreleasepool {
    AVAudioSessionCategory name = AVAudioSessionCategoryAmbient;
    if (category == 1) name = AVAudioSessionCategorySoloAmbient;
    if (category == 2) name = AVAudioSessionCategoryPlayback;
    if (category == 3) name = AVAudioSessionCategoryPlayAndRecord;
    AVAudioSessionMode modeName = mode == 1 ? AVAudioSessionModeSpokenAudio : AVAudioSessionModeDefault;
    AVAudioSessionCategoryOptions flags = 0;
    if (options & 1) flags |= AVAudioSessionCategoryOptionMixWithOthers;
    if (options & 2) flags |= AVAudioSessionCategoryOptionDuckOthers;
    if (options & 4) flags |= AVAudioSessionCategoryOptionInterruptSpokenAudioAndMixWithOthers;
    if (options & 8) flags |= AVAudioSessionCategoryOptionDefaultToSpeaker;
    if (options & 16) flags |= AVAudioSessionCategoryOptionAllowBluetoothA2DP;
    return [[AVAudioSession sharedInstance] setCategory:name mode:modeName options:flags error:nil] ? 1 : 0;
  }
}

int64_t rae_audio_session_set_active(int64_t active) {
  @autoreleasepool {
    AVAudioSessionSetActiveOptions flags = active ? 0 : AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation;
    return [[AVAudioSession sharedInstance] setActive:(active ? YES : NO) withOptions:flags error:nil] ? 1 : 0;
  }
}

int64_t rae_audio_session_describe(uint8_t* into, int64_t capacity) {
  @autoreleasepool {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSMutableArray* outputs = [NSMutableArray array];
    for (AVAudioSessionPortDescription* port in session.currentRoute.outputs) {
      [outputs addObject:port.portType];
    }
    NSString* text = [NSString stringWithFormat:
        @"category %@ mode %@ options %lu route %@ otherAudioPlaying %d silenceSecondaryHint %d rate %.0f ioBuffer %.4f outputVolume %.2f",
        session.category, session.mode, (unsigned long)session.categoryOptions,
        [outputs componentsJoinedByString:@","], session.isOtherAudioPlaying ? 1 : 0,
        session.secondaryAudioShouldBeSilencedHint ? 1 : 0, session.sampleRate, session.IOBufferDuration,
        session.outputVolume];
    const char* utf8 = [text UTF8String];
    int64_t length = (int64_t)strlen(utf8);
    if (length > capacity) length = capacity;
    memcpy(into, utf8, (size_t)length);
    return length;
  }
}

/* INFocusStatusCenter through the Objective-C runtime, so a program that
 * never asks about Focus does not link Intents */
static id rae_audio_focus_center(void) {
  static void* intents = NULL;
  if (!intents) intents = dlopen("/System/Library/Frameworks/Intents.framework/Intents", RTLD_LAZY);
  Class center = NSClassFromString(@"INFocusStatusCenter");
  if (!center) return nil;
  return [center performSelector:NSSelectorFromString(@"defaultCenter")];
}

int64_t rae_audio_focus_status(void) {
  @autoreleasepool {
    id center = rae_audio_focus_center();
    if (!center) return -1;
    NSInteger authorization = (NSInteger)[[center valueForKey:@"authorizationStatus"] integerValue];
    id status = [center valueForKey:@"focusStatus"];
    id focused = status ? [status valueForKey:@"isFocused"] : nil;
    int64_t focusedValue = 2;
    if ([focused isKindOfClass:[NSNumber class]]) focusedValue = [focused boolValue] ? 1 : 0;
    return (int64_t)authorization * 10 + focusedValue;
  }
}

int64_t rae_audio_focus_request(void) {
  @autoreleasepool {
    /* Without the purpose string iOS ends the app at the request (TCC),
     * as an iPhone run of checks/audioSession showed: ask only with it */
    if (![[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSFocusStatusUsageDescription"]) return 0;
    id center = rae_audio_focus_center();
    if (!center) return 0;
    SEL request = NSSelectorFromString(@"requestAuthorizationWithCompletionHandler:");
    if (![center respondsToSelector:request]) return 0;
    void (^done)(NSInteger) = ^(NSInteger status) { (void)status; };
    ((void (*)(id, SEL, id))objc_msgSend)(center, request, done);
    return 1;
  }
}

static int g_rae_audio_observing;

int64_t rae_audio_session_observe(void) {
  if (g_rae_audio_observing) return 1;
  g_rae_audio_observing = 1;
  @autoreleasepool {
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    NSOperationQueue* main = [NSOperationQueue mainQueue];
    AVAudioSession* session = [AVAudioSession sharedInstance];
    [center addObserverForName:AVAudioSessionInterruptionNotification object:session queue:main
                    usingBlock:^(NSNotification* note) {
      NSUInteger type = [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
      if (type == AVAudioSessionInterruptionTypeBegan) {
        rae_audio_session_inject(1, 0);
      } else {
        NSUInteger options = [note.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
        rae_audio_session_inject(2, (options & AVAudioSessionInterruptionOptionShouldResume) ? 1 : 0);
      }
    }];
    [center addObserverForName:AVAudioSessionRouteChangeNotification object:session queue:main
                    usingBlock:^(NSNotification* note) {
      NSUInteger reason = [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue];
      if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable) {
        rae_audio_session_inject(3, 0);
      } else {
        rae_audio_session_inject(4, (int64_t)(reason & 15));
      }
    }];
    [center addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:session queue:main
                    usingBlock:^(NSNotification* note) {
      (void)note;
      rae_audio_session_inject(5, 0);
    }];
  }
  return 1;
}
#else
int64_t rae_audio_session_describe(uint8_t* into, int64_t capacity) {
  (void)into; (void)capacity;
  return 0;
}

int64_t rae_audio_focus_status(void) {
  return -1;
}

int64_t rae_audio_focus_request(void) {
  return 0;
}

int64_t rae_audio_session_available(void) {
  return 0;
}

int64_t rae_audio_session_configure(int64_t category, int64_t mode, int64_t options) {
  (void)category; (void)mode; (void)options;
  return 0;
}

int64_t rae_audio_session_set_active(int64_t active) {
  (void)active;
  return 0;
}

int64_t rae_audio_session_observe(void) {
  return 0;
}
#endif

/* ---- decoders (audio/Decoder): one miniaudio call each ------------------ */

int64_t rae_audio_decoder_open(uint8_t* path_z) {
  ma_decoder* decoder = (ma_decoder*)calloc(1, sizeof(ma_decoder));
  if (!decoder) return 0;
  /* the file's own channels and rate, float samples: the Rae resampler
   * converts the rate (audio/Resampler) */
  ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
  if (ma_decoder_init_file((const char*)path_z, &config, decoder) != MA_SUCCESS) {
    free(decoder);
    return 0;
  }
  return (int64_t)(intptr_t)decoder;
}

int64_t rae_audio_decoder_channels(int64_t decoder) {
  return decoder ? (int64_t)((ma_decoder*)(intptr_t)decoder)->outputChannels : 0;
}

int64_t rae_audio_decoder_sample_rate(int64_t decoder) {
  return decoder ? (int64_t)((ma_decoder*)(intptr_t)decoder)->outputSampleRate : 0;
}

int64_t rae_audio_decoder_length(int64_t decoder) {
  ma_uint64 frames = 0;
  if (!decoder || ma_decoder_get_length_in_pcm_frames((ma_decoder*)(intptr_t)decoder, &frames) != MA_SUCCESS) return -1;
  return (int64_t)frames;
}

int64_t rae_audio_decoder_read(int64_t decoder, float* into, int64_t frames) {
  ma_uint64 got = 0;
  if (!decoder) return 0;
  ma_result result = ma_decoder_read_pcm_frames((ma_decoder*)(intptr_t)decoder, into, (ma_uint64)frames, &got);
  if (result != MA_SUCCESS && result != MA_AT_END) return 0;
  return (int64_t)got;
}

int64_t rae_audio_decoder_seek(int64_t decoder, int64_t frame) {
  return decoder && ma_decoder_seek_to_pcm_frame((ma_decoder*)(intptr_t)decoder, (ma_uint64)frame) == MA_SUCCESS;
}

void rae_audio_decoder_close(int64_t decoder) {
  if (!decoder) return;
  ma_decoder_uninit((ma_decoder*)(intptr_t)decoder);
  free((ma_decoder*)(intptr_t)decoder);
}
