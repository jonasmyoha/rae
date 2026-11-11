/* The audio package's C glue (rae_audio.h). Platform reason: opening and
 * running the sound device (miniaudio's device layer over Core Audio, WASAPI,
 * ALSA/PulseAudio/JACK, AAudio, Web Audio), the thread's real-time policy,
 * reading and writing files, and the platform's AAC decoder (AudioToolbox). The one place with more than one call is the device
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

/* The output's buffering in frames, for the clock (docs/audio-design.md §6.1):
 * miniaudio's period times its periods, on every platform */
int64_t rae_audio_device_buffer_frames(int64_t device) {
  if (!device) return 0;
  RaeAudioDevice* audio = (RaeAudioDevice*)(intptr_t)device;
  return (int64_t)audio->device.playback.internalPeriodSizeInFrames * (int64_t)audio->device.playback.internalPeriods;
}

/* macOS: one Core Audio property of the output device in frames (0 its
 * latency, 1 its safety offset, 2 its IO buffer size), read with the
 * AudioObjectGetPropertyData miniaudio loaded; -1 elsewhere or on failure */
int64_t rae_audio_device_core_audio_frames(int64_t device, int64_t property) {
#if defined(MA_APPLE_DESKTOP)
  if (!device) return -1;
  RaeAudioDevice* audio = (RaeAudioDevice*)(intptr_t)device;
  typedef OSStatus (*GetPropertyData)(AudioObjectID, const AudioObjectPropertyAddress*, UInt32, const void*,
                                      UInt32*, void*);
  GetPropertyData get = (GetPropertyData)audio->context.coreaudio.AudioObjectGetPropertyData;
  if (!get) return -1;
  AudioObjectPropertySelector selectors[3] = {kAudioDevicePropertyLatency, kAudioDevicePropertySafetyOffset,
                                              kAudioDevicePropertyBufferFrameSize};
  if (property < 0 || property > 2) return -1;
  AudioObjectPropertyAddress address = {selectors[property], kAudioObjectPropertyScopeOutput,
                                        kAudioObjectPropertyElementMain};
  UInt32 value = 0;
  UInt32 size = sizeof(value);
  if (get((AudioObjectID)audio->device.coreaudio.deviceObjectIDPlayback, &address, 0, NULL, &size, &value) != 0)
    return -1;
  return (int64_t)value;
#else
  (void)device; (void)property;
  return -1;
#endif
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

/* AVAudioSession's output latency in nanoseconds (the route's: the speaker,
 * Bluetooth headphones, ...) */
int64_t rae_audio_session_output_latency_ns(void) {
  @autoreleasepool {
    return (int64_t)([[AVAudioSession sharedInstance] outputLatency] * 1e9);
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
    /* the app went to the background / came back (UIKit's notifications,
     * named by their values so the glue needs no UIKit symbol) */
    [center addObserverForName:@"UIApplicationDidEnterBackgroundNotification" object:nil queue:main
                    usingBlock:^(NSNotification* note) {
      (void)note;
      rae_audio_session_inject(6, 0);
    }];
    [center addObserverForName:@"UIApplicationWillEnterForegroundNotification" object:nil queue:main
                    usingBlock:^(NSNotification* note) {
      (void)note;
      rae_audio_session_inject(7, 0);
    }];
  }
  return 1;
}
#else
int64_t rae_audio_session_output_latency_ns(void) {
  return -1;
}

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

/* ---- a file read in pieces (open is variadic in C, pread one call) -------- */
#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int64_t rae_audio_file_open(uint8_t* path_z) {
  int file = open((const char*)path_z, O_RDONLY);
  return file >= 0 ? (int64_t)file : -(int64_t)errno;
}

int64_t rae_audio_file_read_at(int64_t file, uint8_t* into, int64_t count, int64_t offset) {
  ssize_t got = pread((int)file, into, (size_t)count, (off_t)offset);
  return got >= 0 ? (int64_t)got : -(int64_t)errno;
}

void rae_audio_file_close(int64_t file) { close((int)file); }
#else
int64_t rae_audio_file_open(uint8_t* path_z) { (void)path_z; return -1; }
int64_t rae_audio_file_read_at(int64_t file, uint8_t* into, int64_t count, int64_t offset) {
  (void)file; (void)into; (void)count; (void)offset; return -1;
}
void rae_audio_file_close(int64_t file) { (void)file; }
#endif

/* ---- AAC through AudioToolbox's AudioConverter ----------------------------
 * Platform reason: AAC is the platform's to decode (docs/audio-design.md
 * §7.3, no permissive C decoder exists). AudioToolbox is loaded at run time,
 * like miniaudio loads Core Audio, so a program links no framework for it.
 * Each function makes one AudioConverter call; which packet goes in, the
 * priming to drop and the seeking are Rae (audio/AacDecoder). */
#if defined(__APPLE__)
#include <dlfcn.h>

typedef OSStatus (*RaeAudioConverterNew)(const AudioStreamBasicDescription*,
                                         const AudioStreamBasicDescription*, AudioConverterRef*);
typedef OSStatus (*RaeAudioConverterSetProperty)(AudioConverterRef, AudioConverterPropertyID, UInt32,
                                                 const void*);
typedef OSStatus (*RaeAudioConverterFill)(AudioConverterRef, AudioConverterComplexInputDataProc, void*,
                                          UInt32*, AudioBufferList*, AudioStreamPacketDescription*);
typedef OSStatus (*RaeAudioConverterReset)(AudioConverterRef);
typedef OSStatus (*RaeAudioConverterDispose)(AudioConverterRef);

static struct {
  RaeAudioConverterNew create;
  RaeAudioConverterSetProperty setProperty;
  RaeAudioConverterFill fill;
  RaeAudioConverterReset reset;
  RaeAudioConverterDispose dispose;
} g_rae_aac;
static pthread_once_t g_rae_aac_once = PTHREAD_ONCE_INIT;

static void rae_aac_load(void) {
  void* toolbox = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY);
  if (!toolbox) return;
  g_rae_aac.create = (RaeAudioConverterNew)dlsym(toolbox, "AudioConverterNew");
  g_rae_aac.setProperty = (RaeAudioConverterSetProperty)dlsym(toolbox, "AudioConverterSetProperty");
  g_rae_aac.fill = (RaeAudioConverterFill)dlsym(toolbox, "AudioConverterFillComplexBuffer");
  g_rae_aac.reset = (RaeAudioConverterReset)dlsym(toolbox, "AudioConverterReset");
  g_rae_aac.dispose = (RaeAudioConverterDispose)dlsym(toolbox, "AudioConverterDispose");
}

int64_t rae_audio_aac_open(int64_t sample_rate, int64_t channels) {
  pthread_once(&g_rae_aac_once, rae_aac_load);
  if (!g_rae_aac.create || !g_rae_aac.fill || channels <= 0) return 0;
  AudioStreamBasicDescription in = {0};
  in.mSampleRate = (Float64)sample_rate;
  in.mFormatID = kAudioFormatMPEG4AAC;
  in.mFramesPerPacket = 1024;
  in.mChannelsPerFrame = (UInt32)channels;
  AudioStreamBasicDescription out = {0};
  out.mSampleRate = (Float64)sample_rate;
  out.mFormatID = kAudioFormatLinearPCM;
  out.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  out.mFramesPerPacket = 1;
  out.mChannelsPerFrame = (UInt32)channels;
  out.mBitsPerChannel = 32;
  out.mBytesPerFrame = 4 * (UInt32)channels;
  out.mBytesPerPacket = out.mBytesPerFrame;
  AudioConverterRef converter = NULL;
  if (g_rae_aac.create(&in, &out, &converter) != noErr) return 0;
  return (int64_t)(intptr_t)converter;
}

int64_t rae_audio_aac_set_cookie(int64_t converter, uint8_t* cookie, int64_t size) {
  if (!converter || !g_rae_aac.setProperty) return 0;
  return g_rae_aac.setProperty((AudioConverterRef)(intptr_t)converter,
                               kAudioConverterDecompressionMagicCookie, (UInt32)size, cookie) == noErr;
}

/* The converter's input: the one packet of this call, then "no more for now"
 * (a status of our own, so the converter answers with what it has), or with a
 * packet of size 0 the end of the stream */
enum { RAE_AAC_WAIT = 0x72616521 };
typedef struct {
  uint8_t* data;
  UInt32 size;
  UInt32 channels;
  int given;
  AudioStreamPacketDescription description;
} RaeAacInput;

static OSStatus rae_aac_supply(AudioConverterRef converter, UInt32* packets, AudioBufferList* data,
                               AudioStreamPacketDescription** descriptions, void* user) {
  (void)converter;
  RaeAacInput* input = (RaeAacInput*)user;
  if (input->size == 0 || input->given) {
    *packets = 0;
    return input->size == 0 ? noErr : RAE_AAC_WAIT;
  }
  data->mNumberBuffers = 1;
  data->mBuffers[0].mData = input->data;
  data->mBuffers[0].mDataByteSize = input->size;
  data->mBuffers[0].mNumberChannels = input->channels;
  input->description.mStartOffset = 0;
  input->description.mVariableFramesInPacket = 0;
  input->description.mDataByteSize = input->size;
  if (descriptions) *descriptions = &input->description;
  *packets = 1;
  input->given = 1;
  return noErr;
}

int64_t rae_audio_aac_decode(int64_t converter, uint8_t* packet, int64_t size, float* out,
                             int64_t max_frames, int64_t channels) {
  if (!converter) return -1;
  RaeAacInput input = {packet, (UInt32)size, (UInt32)channels, 0, {0}};
  AudioBufferList list;
  list.mNumberBuffers = 1;
  list.mBuffers[0].mNumberChannels = (UInt32)channels;
  list.mBuffers[0].mDataByteSize = (UInt32)(max_frames * channels * 4);
  list.mBuffers[0].mData = out;
  UInt32 frames = (UInt32)max_frames;
  OSStatus status = g_rae_aac.fill((AudioConverterRef)(intptr_t)converter, rae_aac_supply, &input,
                                   &frames, &list, NULL);
  if (status != noErr && status != RAE_AAC_WAIT) return status < 0 ? (int64_t)status : -(int64_t)status;
  return (int64_t)frames;
}

void rae_audio_aac_reset(int64_t converter) {
  if (converter && g_rae_aac.reset) g_rae_aac.reset((AudioConverterRef)(intptr_t)converter);
}

void rae_audio_aac_close(int64_t converter) {
  if (converter && g_rae_aac.dispose) g_rae_aac.dispose((AudioConverterRef)(intptr_t)converter);
}
#else
int64_t rae_audio_aac_open(int64_t sample_rate, int64_t channels) {
  (void)sample_rate; (void)channels; return 0;
}
int64_t rae_audio_aac_set_cookie(int64_t converter, uint8_t* cookie, int64_t size) {
  (void)converter; (void)cookie; (void)size; return 0;
}
int64_t rae_audio_aac_decode(int64_t converter, uint8_t* packet, int64_t size, float* out,
                             int64_t max_frames, int64_t channels) {
  (void)converter; (void)packet; (void)size; (void)out; (void)max_frames; (void)channels; return -1;
}
void rae_audio_aac_reset(int64_t converter) { (void)converter; }
void rae_audio_aac_close(int64_t converter) { (void)converter; }
#endif

/* ---- Now Playing and the remote commands (docs/audio-design.md §9.1) -------
 * Platform reason: MPNowPlayingInfoCenter (what the lock screen, Control
 * Center and the media keys show) and MPRemoteCommandCenter (their buttons).
 * MediaPlayer is loaded at run time and its classes found by name, its key
 * constants by dlsym, so a program that never shows Now Playing links nothing
 * new. The commands go into a queue of their own, like the session's events:
 * the handlers run on the main queue, and the main thread reads them. Which
 * command does what (pause the stream, go to the next song) is the program's,
 * in Rae (audio/NowPlaying). */
#define RAE_AUDIO_REMOTE_CAPACITY 64
static int64_t g_rae_audio_remote[RAE_AUDIO_REMOTE_CAPACITY];
static int64_t g_rae_audio_remote_written;
static int64_t g_rae_audio_remote_read;

/* kind: 1 play, 2 pause, 3 toggle, 4 next, 5 previous, 6 change position;
 * value: the position in milliseconds for 6 */
void rae_audio_remote_inject(int64_t kind, int64_t value) {
  int64_t written = __atomic_load_n(&g_rae_audio_remote_written, __ATOMIC_RELAXED);
  int64_t read = __atomic_load_n(&g_rae_audio_remote_read, __ATOMIC_ACQUIRE);
  if (written - read >= RAE_AUDIO_REMOTE_CAPACITY) return;
  g_rae_audio_remote[written % RAE_AUDIO_REMOTE_CAPACITY] = value * 16 + kind;
  __atomic_store_n(&g_rae_audio_remote_written, written + 1, __ATOMIC_RELEASE);
}

int64_t rae_audio_remote_poll(void) {
  int64_t read = __atomic_load_n(&g_rae_audio_remote_read, __ATOMIC_RELAXED);
  int64_t written = __atomic_load_n(&g_rae_audio_remote_written, __ATOMIC_ACQUIRE);
  if (read == written) return -1;
  int64_t event = g_rae_audio_remote[read % RAE_AUDIO_REMOTE_CAPACITY];
  __atomic_store_n(&g_rae_audio_remote_read, read + 1, __ATOMIC_RELEASE);
  return event;
}

#if defined(__APPLE__) && defined(__OBJC__)
#include <dlfcn.h>
#include <objc/message.h>
#import <Foundation/Foundation.h>
#include <CoreFoundation/CoreFoundation.h>

static void* rae_audio_media_player(void) {
  static void* media = NULL;
  if (!media) media = dlopen("/System/Library/Frameworks/MediaPlayer.framework/MediaPlayer", RTLD_LAZY);
  return media;
}

/* A MediaPlayer key constant by name (MPMediaItemPropertyTitle, ...) */
static NSString* rae_audio_media_key(const char* name) {
  void* media = rae_audio_media_player();
  NSString* __unsafe_unretained* key = media ? (NSString* __unsafe_unretained*)dlsym(media, name) : NULL;
  return key ? *key : nil;
}

static id rae_audio_now_playing_center(void) {
  if (!rae_audio_media_player()) return nil;
  Class center = NSClassFromString(@"MPNowPlayingInfoCenter");
  return center ? ((id (*)(id, SEL))objc_msgSend)(center, sel_registerName("defaultCenter")) : nil;
}

/* An image from a file, as the platform's image type (UIImage / NSImage) */
static id rae_audio_image_from_file(NSString* path) {
#if defined(MA_APPLE_MOBILE)
  Class image = NSClassFromString(@"UIImage");
  return image ? ((id (*)(id, SEL, id))objc_msgSend)(image, sel_registerName("imageWithContentsOfFile:"), path)
               : nil;
#else
  static void* appKit = NULL;
  if (!appKit) appKit = dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY);
  Class image = NSClassFromString(@"NSImage");
  if (!image) return nil;
  id made = ((id (*)(id, SEL))objc_msgSend)(image, sel_registerName("alloc"));
  made = ((id (*)(id, SEL, id))objc_msgSend)(made, sel_registerName("initWithContentsOfFile:"), path);
  return made ? [made autorelease] : nil;
#endif
}

/* Show what plays: rate 0 paused, 1 playing; duration and position in
 * seconds (a negative duration is left out); empty strings are left out.
 * One call: setNowPlayingInfo (and playbackState on macOS, which shows that
 * instead of the rate). 1 when MediaPlayer is there. */
int64_t rae_audio_now_playing_set(uint8_t* title_z, uint8_t* artist_z, uint8_t* album_z,
                                  uint8_t* artwork_path_z, double duration, double position, double rate) {
  @autoreleasepool {
    id center = rae_audio_now_playing_center();
    if (!center) return 0;
    NSMutableDictionary* info = [NSMutableDictionary dictionary];
    NSString* title = [NSString stringWithUTF8String:(const char*)title_z];
    NSString* artist = [NSString stringWithUTF8String:(const char*)artist_z];
    NSString* album = [NSString stringWithUTF8String:(const char*)album_z];
    NSString* artworkPath = [NSString stringWithUTF8String:(const char*)artwork_path_z];
    NSString* key = nil;
    if (title.length && (key = rae_audio_media_key("MPMediaItemPropertyTitle"))) info[key] = title;
    if (artist.length && (key = rae_audio_media_key("MPMediaItemPropertyArtist"))) info[key] = artist;
    if (album.length && (key = rae_audio_media_key("MPMediaItemPropertyAlbumTitle"))) info[key] = album;
    if (duration >= 0 && (key = rae_audio_media_key("MPMediaItemPropertyPlaybackDuration"))) info[key] = @(duration);
    if ((key = rae_audio_media_key("MPNowPlayingInfoPropertyElapsedPlaybackTime"))) info[key] = @(position);
    if ((key = rae_audio_media_key("MPNowPlayingInfoPropertyPlaybackRate"))) info[key] = @(rate);
    if ((key = rae_audio_media_key("MPNowPlayingInfoPropertyDefaultPlaybackRate"))) info[key] = @(1.0);
    if ((key = rae_audio_media_key("MPNowPlayingInfoPropertyMediaType"))) info[key] = @(1); /* audio */
    id image = artworkPath.length ? rae_audio_image_from_file(artworkPath) : nil;
    Class artworkClass = NSClassFromString(@"MPMediaItemArtwork");
    if (image && artworkClass && (key = rae_audio_media_key("MPMediaItemPropertyArtwork"))) {
      CGSize size = ((CGSize (*)(id, SEL))objc_msgSend)(image, sel_registerName("size"));
      id (^handler)(CGSize) = ^id(CGSize asked) { (void)asked; return image; };
      id artwork = ((id (*)(id, SEL))objc_msgSend)(artworkClass, sel_registerName("alloc"));
      artwork = ((id (*)(id, SEL, CGSize, id))objc_msgSend)(artwork, sel_registerName("initWithBoundsSize:requestHandler:"),
                                                            size, handler);
      if (artwork) info[key] = [artwork autorelease];
    }
    ((void (*)(id, SEL, id))objc_msgSend)(center, sel_registerName("setNowPlayingInfo:"), info);
#if !defined(MA_APPLE_MOBILE)
    /* macOS shows the playback state, not the rate: 1 playing, 2 paused */
    ((void (*)(id, SEL, NSUInteger))objc_msgSend)(center, sel_registerName("setPlaybackState:"),
                                                 (NSUInteger)(rate > 0 ? 1 : 2));
#endif
    return 1;
  }
}

/* What Now Playing shows now, as text (for checks): "title|artist|position|rate" */
int64_t rae_audio_now_playing_describe(uint8_t* into, int64_t capacity) {
  @autoreleasepool {
    id center = rae_audio_now_playing_center();
    if (!center) return 0;
    NSDictionary* info = ((id (*)(id, SEL))objc_msgSend)(center, sel_registerName("nowPlayingInfo"));
    NSString* text = [NSString stringWithFormat:@"%@|%@|%.2f|%.1f",
        info[rae_audio_media_key("MPMediaItemPropertyTitle")] ?: @"",
        info[rae_audio_media_key("MPMediaItemPropertyArtist")] ?: @"",
        [info[rae_audio_media_key("MPNowPlayingInfoPropertyElapsedPlaybackTime")] doubleValue],
        [info[rae_audio_media_key("MPNowPlayingInfoPropertyPlaybackRate")] doubleValue]];
    const char* utf8 = [text UTF8String];
    int64_t length = (int64_t)strlen(utf8);
    if (length > capacity) length = capacity;
    memcpy(into, utf8, (size_t)length);
    return length;
  }
}

void rae_audio_now_playing_clear(void) {
  @autoreleasepool {
    id center = rae_audio_now_playing_center();
    if (center) ((void (*)(id, SEL, id))objc_msgSend)(center, sel_registerName("setNowPlayingInfo:"), nil);
  }
}

static void rae_audio_remote_target(id command, int64_t kind) {
  ((void (*)(id, SEL, BOOL))objc_msgSend)(command, sel_registerName("setEnabled:"), YES);
  NSInteger (^handler)(id) = ^NSInteger(id event) {
    int64_t value = 0;
    if (kind == 6) {
      double position = ((double (*)(id, SEL))objc_msgSend)(event, sel_registerName("positionTime"));
      value = (int64_t)(position * 1000.0 + 0.5);
    }
    rae_audio_remote_inject(kind, value);
    return 0; /* MPRemoteCommandHandlerStatusSuccess */
  };
  ((id (*)(id, SEL, id))objc_msgSend)(command, sel_registerName("addTargetWithHandler:"), handler);
}

static int g_rae_audio_remote_enabled;

/* Turn on the commands in `mask` (1 play, 2 pause, 4 toggle, 8 next,
 * 16 previous, 32 change position); once per program. 1 when MediaPlayer is
 * there. */
int64_t rae_audio_remote_enable(int64_t mask) {
  @autoreleasepool {
    if (!rae_audio_media_player()) return 0;
    Class centerClass = NSClassFromString(@"MPRemoteCommandCenter");
    if (!centerClass) return 0;
    if (g_rae_audio_remote_enabled) return 1;
    g_rae_audio_remote_enabled = 1;
    id center = ((id (*)(id, SEL))objc_msgSend)(centerClass, sel_registerName("sharedCommandCenter"));
    const char* names[6] = {"playCommand", "pauseCommand", "togglePlayPauseCommand", "nextTrackCommand",
                            "previousTrackCommand", "changePlaybackPositionCommand"};
    for (int64_t i = 0; i < 6; i++) {
      if (!(mask & (1 << i))) continue;
      id command = ((id (*)(id, SEL))objc_msgSend)(center, sel_registerName(names[i]));
      if (command) rae_audio_remote_target(command, i + 1);
    }
    return 1;
  }
}

/* Run the main thread's run loop once without waiting, so the remote
 * commands' handlers (on the main queue) run in a program with no UI loop
 * of its own (a command-line player, a check). An app's loop does this. */
void rae_audio_run_loop_pump(void) {
  CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
}
#else
int64_t rae_audio_now_playing_set(uint8_t* title_z, uint8_t* artist_z, uint8_t* album_z,
                                  uint8_t* artwork_path_z, double duration, double position, double rate) {
  (void)title_z; (void)artist_z; (void)album_z; (void)artwork_path_z; (void)duration; (void)position;
  (void)rate;
  return 0;
}
int64_t rae_audio_now_playing_describe(uint8_t* into, int64_t capacity) {
  (void)into; (void)capacity;
  return 0;
}
void rae_audio_now_playing_clear(void) {}
int64_t rae_audio_remote_enable(int64_t mask) {
  (void)mask;
  return 0;
}
void rae_audio_run_loop_pump(void) {}
#endif
