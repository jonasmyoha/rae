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

/* The size of a file in bytes, or -1 when it cannot be opened */
int64_t rae_audio_file_size(uint8_t* path_z);
/* Read up to `count` bytes of a file into `into`; the count read, or -1 */
int64_t rae_audio_read_file(uint8_t* path_z, uint8_t* into, int64_t count);

/* Write `count` bytes to a file (the capture WAV). 1 on success. */
int64_t rae_audio_write_file(uint8_t* path_z, uint8_t* bytes, int64_t count);

#endif
