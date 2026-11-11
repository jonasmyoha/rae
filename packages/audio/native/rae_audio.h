/* The C glue of the audio package (docs/audio-design.md §3 L1, §4.2): the
 * sound device through miniaudio's device layer, and the few platform calls
 * the Rae engine needs. Handles travel as integers (0: none). The engine, the
 * mixer and every decision are Rae (audio/AudioEngine, audio/AudioMixer).
 *
 * Stage A of §4.2: until Rae can export a function to C, the device's callback
 * is C, and it only copies. Rae renders into a SpscRing(Float) of interleaved
 * samples (lib/collections/SpscRing.rae: its slots, and its two counters
 * `read` at index 0 and `written` at index 8); the callback takes one
 * device period from it, writes silence for what is missing and counts that as
 * an underrun, then posts the Rae Signal that wakes the render thread. It
 * never allocates, locks or waits. */
#ifndef RAE_AUDIO_H
#define RAE_AUDIO_H

#include <stdint.h>

/* A playback device with float samples at this rate, channel count and
 * period (0 for the device's own); 0 when none could be opened. */
int64_t rae_audio_device_open(int64_t sample_rate, int64_t channels, int64_t period_frames);
/* What the device really uses */
int64_t rae_audio_device_sample_rate(int64_t device);
int64_t rae_audio_device_channels(int64_t device);
int64_t rae_audio_device_period_frames(int64_t device);
/* The ring the callback reads and the Signal it posts; before start */
void rae_audio_device_attach(int64_t device, float* slots, int64_t* counters, int64_t capacity,
                             int64_t signal);
/* Stage B: the callback renders with audio/AudioDevice's audioDeviceRender
 * (`rae_audio_render`) and this state, instead of copying from a ring */
void rae_audio_device_attach_render(int64_t device, void* state);
int64_t rae_audio_device_start(int64_t device);
int64_t rae_audio_device_stop(int64_t device);
void rae_audio_device_close(int64_t device);
/* Frames of silence the callback played because the ring was short */
int64_t rae_audio_device_underrun_frames(int64_t device);
/* Device callbacks so far */
int64_t rae_audio_device_callbacks(int64_t device);

/* Ask the scheduler to treat the calling thread as real-time: a time
 * constraint of `period_ns` with `compute_ns` of work on Apple; nothing
 * elsewhere yet. 1 when the request was taken. */
int64_t rae_audio_thread_set_realtime(int64_t period_ns, int64_t compute_ns);

/* The host's audio session (docs/audio-design.md §9.1). On iOS this is
 * AVAudioSession; elsewhere every call answers 0 and does nothing. The Rae
 * policy (audio/AudioSession) decides; these only make the call.
 *   category: 0 ambient, 1 soloAmbient, 2 playback, 3 playAndRecord
 *   mode:     0 default, 1 spokenAudio
 *   options:  1 mixWithOthers, 2 duckOthers, 4 interruptSpokenAudioAndMixWithOthers,
 *             8 defaultToSpeaker, 16 allowBluetoothA2DP */
int64_t rae_audio_session_available(void);
int64_t rae_audio_session_configure(int64_t category, int64_t mode, int64_t options);
/* Activate, or deactivate telling other apps they may resume */
int64_t rae_audio_session_set_active(int64_t active);
/* Start turning the session's notifications into events (once) */
int64_t rae_audio_session_observe(void);
/* The next session event as kind * 16 + value, or -1 for none:
 *   1 interruption began; 2 interruption ended (value 1: should resume);
 *   3 the route's old device went away (headphones out); 4 a new route
 *   (value: the reason); 5 the media services were reset.
 * Events are written on the main thread (the observers run on the main
 * queue) and read there, by audio/AudioSession. */
int64_t rae_audio_session_poll(void);
/* Add an event as if the system had sent it: the headless fake mode, and tests */
void rae_audio_session_inject(int64_t kind, int64_t value);

/* What the session says now, as one line of text written into `into` (at most
 * `capacity` bytes, no 0): its category, mode, options, the output route,
 * whether another app's audio plays, whether the system hints that secondary
 * audio should be silenced, the hardware rate and the IO buffer. The length,
 * or 0 off iOS. For logs and probes only; no decision reads it. */
int64_t rae_audio_session_describe(uint8_t* into, int64_t capacity);

/* Focus (docs/audio-design.md §9.2): INFocusStatusCenter, found at run time
 * (the Intents framework is loaded only here). The answer is
 * authorization * 10 + focused: authorization 0 not asked, 1 restricted,
 * 2 denied, 3 authorized; focused 0 no, 1 yes, 2 unknown. -1 when there is no
 * Focus API (off iOS, before iOS 15). */
int64_t rae_audio_focus_status(void);
/* Ask the person for access to the Focus status (iOS shows a prompt once);
 * 1 when the request was made. The app's Info.plist must hold
 * NSFocusStatusUsageDescription (gen-ios.sh: RAE_IOS_FOCUS_USAGE), or iOS
 * ends the app at the request; without it this asks nothing and answers 0.
 * Without access, iOS answers "not focused" whatever the Focus is. */
int64_t rae_audio_focus_request(void);

/* A decoder for MP3, FLAC, Ogg Vorbis or WAV (miniaudio's decoders and
 * stb_vorbis), at the file's own channels and rate, float samples. 0 when the
 * file is not one of them. length: frames, or -1 when unknown. read: up to
 * `frames` interleaved frames into `into`; the frames read (0 at the end). */
int64_t rae_audio_decoder_open(uint8_t* path_z);
int64_t rae_audio_decoder_channels(int64_t decoder);
int64_t rae_audio_decoder_sample_rate(int64_t decoder);
int64_t rae_audio_decoder_length(int64_t decoder);
int64_t rae_audio_decoder_read(int64_t decoder, float* into, int64_t frames);
int64_t rae_audio_decoder_seek(int64_t decoder, int64_t frame);
void rae_audio_decoder_close(int64_t decoder);

/* The size of a file in bytes, or -1 when it cannot be opened */
int64_t rae_audio_file_size(uint8_t* path_z);
/* Read up to `count` bytes of a file into `into`; the count read, or -1 */
int64_t rae_audio_read_file(uint8_t* path_z, uint8_t* into, int64_t count);

/* Write `count` bytes to a file (the capture WAV). 1 on success. */
int64_t rae_audio_write_file(uint8_t* path_z, uint8_t* bytes, int64_t count);

/* Now Playing (MPNowPlayingInfoCenter) and the remote commands
 * (MPRemoteCommandCenter), on macOS and iOS; elsewhere set / enable answer 0.
 * set: title, artist, album and an artwork image file (empty: left out),
 * duration (negative: left out) and position in seconds, rate 0 paused or 1
 * playing. describe: "title|artist|position|rate" of what is shown. enable:
 * the commands in `mask` (1 play, 2 pause, 4 toggle, 8 next, 16 previous,
 * 32 change position). poll: the next command as value * 16 + kind (kind as
 * the mask bit's index + 1; value the position in milliseconds for a change of
 * position), or -1. inject: put one in the queue (tests). run_loop_pump: run
 * the main run loop once without waiting (a program with no UI loop). */
int64_t rae_audio_now_playing_set(uint8_t* title_z, uint8_t* artist_z, uint8_t* album_z,
                                  uint8_t* artwork_path_z, double duration, double position, double rate);
int64_t rae_audio_now_playing_describe(uint8_t* into, int64_t capacity);
void rae_audio_now_playing_clear(void);
int64_t rae_audio_remote_enable(int64_t mask);
int64_t rae_audio_remote_poll(void);
void rae_audio_remote_inject(int64_t kind, int64_t value);
void rae_audio_run_loop_pump(void);

/* The output's latency for the clock (docs/audio-design.md §6.1).
 * buffer_frames: miniaudio's period times its periods. core_audio_frames:
 * macOS, the output device's latency (0), safety offset (1) or IO buffer size
 * (2) in frames, -1 elsewhere. session_output_latency_ns: iOS, the route's
 * output latency, -1 elsewhere. */
int64_t rae_audio_device_buffer_frames(int64_t device);
/* The platform audio system in use ("Core Audio", "AAudio", "WASAPI", ...), its length */
int64_t rae_audio_device_backend_name(int64_t device, uint8_t* into, int64_t capacity);
int64_t rae_audio_device_core_audio_frames(int64_t device, int64_t property);
int64_t rae_audio_session_output_latency_ns(void);

/* A file read in pieces (audio/AacDecoder reads an MP4's samples where its
 * sample table says): open answers a descriptor (>= 0) or -errno; read_at
 * reads up to `count` bytes at `offset` (pread), the count read or -errno. */
int64_t rae_audio_file_open(uint8_t* path_z);
int64_t rae_audio_file_read_at(int64_t file, uint8_t* into, int64_t count, int64_t offset);
void rae_audio_file_close(int64_t file);

/* AAC through the platform's decoder (AudioToolbox's AudioConverter on Apple;
 * elsewhere open answers 0, "not supported", until the FFmpeg package).
 * open: a converter from AAC at this rate and channel count to interleaved
 * Float, 0 when there is none. set_cookie: the track's ES descriptor (the esds
 * payload), 1 on success. decode: one AAC packet in, up to `max_frames` frames
 * out; the frames written, or a negative status; a packet of size 0 asks for
 * the frames still inside the decoder (the end of the stream). reset: forget
 * the stream (a seek). */
int64_t rae_audio_aac_open(int64_t sample_rate, int64_t channels);
int64_t rae_audio_aac_set_cookie(int64_t converter, uint8_t* cookie, int64_t size);
int64_t rae_audio_aac_decode(int64_t converter, uint8_t* packet, int64_t size, float* out,
                             int64_t max_frames, int64_t channels);
void rae_audio_aac_reset(int64_t converter);
void rae_audio_aac_close(int64_t converter);

#endif
