/* The browser's output with shared memory (a program's `web: { sharedMemory:
 * true }`; docs/audio-design.md §11 step 3), included by rae_audio_web.c in
 * the package's shared-memory browser library (RAE_AUDIO_WEB_SHARED, built
 * with -sWASM_WORKERS -sAUDIO_WORKLET). Platform reason: Emscripten's Web
 * Audio worklet calls. The engine runs inside the AudioWorklet, on its own
 * Wasm thread: each render quantum (128 frames) is rendered there with
 * rae_audio_render, the same entry the native device callback calls, and the
 * commands reach it through the SpscRing as natively. No queue, so the
 * latency is the quantum and what the context reports. */
#include <emscripten/webaudio.h>

/* The worklet thread's stack */
#define RAE_WEB_WORKLET_STACK (64 * 1024)
/* The largest quantum rendered at once (Chrome's is 128; a context may be
 * asked for more) */
#define RAE_WEB_MAX_QUANTUM 4096

typedef struct {
  EMSCRIPTEN_WEBAUDIO_T context;
  int64_t sample_rate;
  int64_t channels;
  int64_t period_frames;
  void* render_state;     /* the Rae state rae_audio_render renders with */
  float* block;           /* a quantum, interleaved (the worklet thread's) */
  uint8_t* stack;
  int64_t callbacks;      /* atomic: renders */
  int attached;           /* the worklet thread is attached to the runtime */
  int started;
} RaeWebAudioDevice;

/* One quantum, on the worklet thread: rendered interleaved in periods, then
 * spread into the output's planes */
static bool rae_web_shared_process(int input_count, const AudioSampleFrame* inputs, int output_count,
                                   AudioSampleFrame* outputs, int param_count, const AudioParamFrame* params,
                                   void* user) {
  (void)input_count; (void)inputs; (void)param_count; (void)params;
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)user;
  if (output_count < 1) return true;
  AudioSampleFrame* out = &outputs[0];
  int frames = out->samplesPerChannel;
  int planes = out->numberOfChannels;
  int channels = (int)audio->channels;
  if (frames > RAE_WEB_MAX_QUANTUM) frames = RAE_WEB_MAX_QUANTUM;
  if (!audio->attached) {
    rae_thread_attach();
    rae_ext_Realtime_markThread(true);
    audio->attached = 1;
  }
  for (int done = 0; done < frames;) {
    int count = frames - done;
    if (count > audio->period_frames) count = (int)audio->period_frames;
    float* into = audio->block + (size_t)done * (size_t)channels;
    if (audio->render_state) rae_audio_render(audio->render_state, into, count, channels);
    else memset(into, 0, (size_t)count * (size_t)channels * sizeof(float));
    done += count;
  }
  for (int c = 0; c < planes; c++) {
    int from = c < channels ? c : channels - 1;
    for (int i = 0; i < frames; i++) out->data[c * out->samplesPerChannel + i] = audio->block[i * channels + from];
  }
  __atomic_fetch_add(&audio->callbacks, 1, __ATOMIC_RELAXED);
  return true;
}

static void rae_web_shared_processor_made(EMSCRIPTEN_WEBAUDIO_T context, bool success, void* user) {
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)user;
  if (!success) {
    emscripten_console_error("[audio] the AudioWorklet processor could not be made");
    return;
  }
  int channel_counts[1] = { (int)audio->channels };
  EmscriptenAudioWorkletNodeCreateOptions options = {
    .numberOfInputs = 0,
    .numberOfOutputs = 1,
    .outputChannelCounts = channel_counts
  };
  EMSCRIPTEN_WEBAUDIO_T node = emscripten_create_wasm_audio_worklet_node(context, "rae-output", &options,
                                                                         rae_web_shared_process, audio);
  emscripten_audio_node_connect(node, context, 0, 0);
}

static void rae_web_shared_thread_started(EMSCRIPTEN_WEBAUDIO_T context, bool success, void* user) {
  if (!success) {
    emscripten_console_error("[audio] the AudioWorklet thread did not start (is the page cross-origin isolated?)");
    return;
  }
  WebAudioWorkletProcessorCreateOptions options = { .name = "rae-output" };
  emscripten_create_wasm_audio_worklet_processor_async(context, &options, rae_web_shared_processor_made, user);
}

EM_JS_DEPS(rae_audio_web_shared, "$emscriptenGetAudioObject");

/* The context resumed now and, as browsers start one only after a click or
 * a key, on the page's first one */
EM_JS(void, rae_web_shared_resume, (EMSCRIPTEN_WEBAUDIO_T handle), {
  const context = emscriptenGetAudioObject(handle);
  if (!context) return;
  const resume = () => { if (context.state !== 'running' && context.state !== 'closed') context.resume().catch(() => {}); };
  resume();
  if (typeof document !== 'undefined') {
    for (const name of ['pointerdown', 'keydown', 'touchend']) document.addEventListener(name, resume);
  }
});

EM_JS(void, rae_web_shared_suspend, (EMSCRIPTEN_WEBAUDIO_T handle), {
  const context = emscriptenGetAudioObject(handle);
  if (context) context.suspend().catch(() => {});
});

/* The context's own latency (baseLatency + outputLatency) in frames */
EM_JS(double, rae_web_shared_latency_frames, (EMSCRIPTEN_WEBAUDIO_T handle), {
  const context = emscriptenGetAudioObject(handle);
  if (!context) return 0;
  return Math.round(((context.baseLatency || 0) + (context.outputLatency || 0)) * context.sampleRate);
});

int64_t rae_audio_device_open(int64_t sample_rate, int64_t channels, int64_t period_frames, int64_t input_channels) {
  (void)period_frames;
  if (input_channels > 0 || channels <= 0) return 0;
  EmscriptenWebAudioCreateAttributes attributes = {
    .latencyHint = "interactive",
    .sampleRate = (uint32_t)sample_rate,
    .renderSizeHint = 0
  };
  EMSCRIPTEN_WEBAUDIO_T context = emscripten_create_audio_context(&attributes);
  if (!context) return 0;
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)calloc(1, sizeof(RaeWebAudioDevice));
  if (!audio) return 0;
  audio->context = context;
  audio->sample_rate = emscripten_audio_context_sample_rate(context);
  audio->channels = channels;
  /* the engine's period is the quantum: the mixer renders at most that */
  int quantum = emscripten_audio_context_quantum_size(context);
  audio->period_frames = quantum > 0 ? quantum : 128;
  audio->block = (float*)calloc((size_t)RAE_WEB_MAX_QUANTUM * (size_t)channels, sizeof(float));
  audio->stack = (uint8_t*)aligned_alloc(16, RAE_WEB_WORKLET_STACK);
  if (!audio->block || !audio->stack) {
    free(audio->block);
    free(audio->stack);
    free(audio);
    emscripten_destroy_audio_context(context);
    return 0;
  }
  return (int64_t)(intptr_t)audio;
}

int64_t rae_audio_device_sample_rate(int64_t device) {
  return device ? ((RaeWebAudioDevice*)(intptr_t)device)->sample_rate : 0;
}

int64_t rae_audio_device_input_channels(int64_t device) {
  (void)device;
  return 0;
}

int64_t rae_audio_device_channels(int64_t device) {
  return device ? ((RaeWebAudioDevice*)(intptr_t)device)->channels : 0;
}

int64_t rae_audio_device_period_frames(int64_t device) {
  return device ? ((RaeWebAudioDevice*)(intptr_t)device)->period_frames : 0;
}

int64_t rae_audio_device_backend_name(int64_t device, uint8_t* into, int64_t capacity) {
  if (!device) return 0;
  const char* name = "Web Audio (AudioWorklet, shared memory)";
  int64_t length = (int64_t)strlen(name);
  if (length > capacity) length = capacity;
  memcpy(into, name, (size_t)length);
  return length;
}

/* The quantum and the context's latency (the clock's latency, §6.1) */
int64_t rae_audio_device_buffer_frames(int64_t device) {
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)(intptr_t)device;
  if (!audio) return 0;
  return audio->period_frames + (int64_t)rae_web_shared_latency_frames(audio->context);
}

int64_t rae_audio_device_core_audio_frames(int64_t device, int64_t property) {
  (void)device; (void)property;
  return -1;
}

int64_t rae_audio_device_workgroup(int64_t device) {
  (void)device;
  return 0;
}

void rae_audio_workgroup_release(int64_t workgroup) { (void)workgroup; }

/* Stage A's ring: never used (the worklet renders) */
void rae_audio_device_attach(int64_t device, float* slots, int64_t* counters, int64_t capacity,
                             int64_t signal) {
  (void)device; (void)slots; (void)counters; (void)capacity; (void)signal;
}

void rae_audio_device_attach_render(int64_t device, void* state) {
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)(intptr_t)device;
  if (audio) audio->render_state = state;
}

/* The worklet thread starts and makes its node without waiting here */
int64_t rae_audio_device_start(int64_t device) {
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)(intptr_t)device;
  if (!audio) return 0;
  if (!audio->started) {
    audio->started = 1;
    emscripten_start_wasm_audio_worklet_thread_async(audio->context, audio->stack, RAE_WEB_WORKLET_STACK,
                                                     rae_web_shared_thread_started, audio);
  }
  rae_web_shared_resume(audio->context);
  return 1;
}

int64_t rae_audio_device_stop(int64_t device) {
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)(intptr_t)device;
  if (!audio) return 0;
  rae_web_shared_suspend(audio->context);
  return 1;
}

/* The context is closed; its worklet thread and stack go with it, so the
 * stack and block are not freed under a quantum that may still run */
void rae_audio_device_close(int64_t device) {
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)(intptr_t)device;
  if (!audio) return;
  audio->render_state = NULL;
  emscripten_destroy_audio_context(audio->context);
}

/* No queue, so no underrun the page can see: the browser plays silence when
 * a quantum is late, and does not say */
int64_t rae_audio_device_underrun_frames(int64_t device) {
  (void)device;
  return 0;
}

int64_t rae_audio_device_callbacks(int64_t device) {
  return device ? __atomic_load_n(&((RaeWebAudioDevice*)(intptr_t)device)->callbacks, __ATOMIC_RELAXED) : 0;
}
