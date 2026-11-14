/* The browser's half of the audio package's C glue (rae_audio.h), included
 * by rae_audio_glue.c under Emscripten instead of miniaudio's device layer
 * (docs/audio-design.md §4.2, §11 step 3). Platform reason: Web Audio and
 * WebCodecs are JavaScript APIs, reached only through these EM_JS imports.
 *
 * The output: an AudioWorklet plays blocks from a small queue of its own and
 * asks the page for more through its MessagePort; the page's handler (main
 * thread) renders each block with the engine's stage-B entry,
 * rae_audio_render, and posts it back (transferred, not copied). So the
 * browser build needs no shared memory (no SharedArrayBuffer, no COOP/COEP
 * headers, no wasm workers), at the cost of the queue's latency. A page with
 * no AudioWorklet (plain http on a LAN address is not a secure context) gets
 * a ScriptProcessorNode that renders in its own callback.
 *
 * AAC: WebCodecs' AudioDecoder, one packet per call: decode, wait (EM_ASYNC_JS,
 * through Asyncify) until the decoder has taken it, then copy what came out;
 * the end of the stream flushes. */
#include <emscripten.h>

#if defined(RAE_AUDIO_WEB_SHARED)
/* a program with `web: { sharedMemory: true }`: the engine inside the worklet */
#include "rae_audio_web_shared.c"
#else
typedef struct {
  int id;                 /* the device's index on the JavaScript side */
  int64_t sample_rate;
  int64_t channels;
  int64_t period_frames;
  void* render_state;     /* the Rae state rae_audio_render renders with */
  float* block;           /* one period, interleaved, rendered and posted */
  int64_t callbacks;
} RaeWebAudioDevice;

/* Render one period into the device's block (the page's handler calls this
 * for each block the worklet asks for); its address in the heap */
EMSCRIPTEN_KEEPALIVE float* rae_audio_web_render(RaeWebAudioDevice* audio) {
  if (!audio->render_state) {
    memset(audio->block, 0, (size_t)(audio->period_frames * audio->channels) * sizeof(float));
  } else {
    rae_ext_Realtime_markThread(true);
    rae_audio_render(audio->render_state, audio->block, audio->period_frames, audio->channels);
    rae_ext_Realtime_markThread(false);
  }
  audio->callbacks++;
  return audio->block;
}

/* An AudioContext at `sample_rate` (the browser may choose another: the
 * answer is read back), not yet started; its index, or 0 when the page has
 * no Web Audio */
EM_JS(int, rae_web_audio_create, (int sample_rate), {
  const Context = globalThis.AudioContext || globalThis.webkitAudioContext;
  if (!Context) return 0;
  let context;
  try {
    context = new Context({ sampleRate: sample_rate, latencyHint: 'interactive' });
  } catch (error) {
    try { context = new Context({ latencyHint: 'interactive' }); } catch (again) { return 0; }
  }
  Module.raeAudioDevices = Module.raeAudioDevices || [null];
  Module.raeAudioDevices.push({ context: context, node: null, underrun: 0, played: 0, queueFrames: 0 });
  return Module.raeAudioDevices.length - 1;
});

EM_JS(int, rae_web_audio_rate, (int id), {
  const device = Module.raeAudioDevices[id];
  return device ? device.context.sampleRate : 0;
});

/* Start playing: the worklet (or the ScriptProcessorNode) is made once its
 * module has loaded, without waiting here; the context is resumed now and,
 * as browsers start a context only after a click or a key, again on the
 * page's first one */
EM_JS(void, rae_web_audio_start, (int id, RaeWebAudioDevice* audio, int channels, int period, int queue,
                                   int script), {
  const device = Module.raeAudioDevices[id];
  if (!device) return;
  device.stopped = false;
  const context = device.context;
  const resume = () => { if (!device.stopped && context.state !== 'running') context.resume().catch(() => {}); };
  resume();
  if (!device.listening && typeof document !== 'undefined') {
    device.listening = true;
    for (const name of ['pointerdown', 'keydown', 'touchend']) document.addEventListener(name, resume);
  }
  if (device.node) return;
  device.queueFrames = queue;
  const render = () => {
    const at = _rae_audio_web_render(audio) >> 2;
    return HEAPF32.slice(at, at + period * channels);
  };
  if (!script && context.audioWorklet && typeof AudioWorkletNode !== 'undefined') {
    const source = `
      class RaeOutput extends AudioWorkletProcessor {
        constructor(options) {
          super();
          const settings = options.processorOptions;
          this.channels = settings.channels;
          this.period = settings.period;
          this.target = settings.queue;
          this.blocks = [];
          this.offset = 0;
          this.queued = 0;
          this.asked = 0;
          this.started = false;
          this.underrun = 0;
          this.played = 0;
          this.port.onmessage = (event) => {
            const block = event.data.block;
            if (!block) return;
            this.blocks.push(block);
            this.queued += this.period;
            this.asked -= this.period;
            this.started = true;
          };
          this.ask();
        }
        ask() {
          const blocks = Math.floor((this.target - this.queued - this.asked) / this.period);
          if (blocks > 0) {
            this.asked += blocks * this.period;
            this.port.postMessage({ need: blocks });
          }
        }
        process(inputs, outputs) {
          const out = outputs[0];
          const frames = out[0].length;
          let missing = 0;
          for (let i = 0; i < frames; i++) {
            if (this.blocks.length === 0) {
              for (let c = 0; c < out.length; c++) out[c][i] = 0;
              if (this.started) missing++;
              continue;
            }
            const block = this.blocks[0];
            const base = this.offset * this.channels;
            for (let c = 0; c < out.length; c++) out[c][i] = block[base + Math.min(c, this.channels - 1)];
            this.offset++;
            if (this.offset >= this.period) {
              this.blocks.shift();
              this.offset = 0;
            }
            this.queued--;
          }
          this.played += frames;
          this.underrun += missing;
          this.ask();
          if (missing > 0 || this.played % 4096 < frames) {
            this.port.postMessage({ underrun: this.underrun, played: this.played });
          }
          return true;
        }
      }
      registerProcessor('rae-output', RaeOutput);
    `;
    const url = 'data:application/javascript,' + encodeURIComponent(source);
    context.audioWorklet.addModule(url).then(() => {
      if (device.closed) return;
      const node = new AudioWorkletNode(context, 'rae-output', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [channels],
        processorOptions: { channels: channels, period: period, queue: queue }
      });
      node.port.onmessage = (event) => {
        const message = event.data;
        if (message.need) {
          for (let i = 0; i < message.need; i++) {
            const block = render();
            node.port.postMessage({ block: block }, [block.buffer]);
          }
        }
        if (message.underrun !== undefined) {
          device.underrun = message.underrun;
          device.played = message.played;
        }
      };
      node.connect(context.destination);
      device.node = node;
      device.kind = 'AudioWorklet';
    }).catch((error) => console.error('[audio] the AudioWorklet did not load: ' + error.name + ': ' + error.message));
  } else {
    let size = 256;
    while (size < period * 2 && size < 16384) size *= 2;
    const node = context.createScriptProcessor(size, 0, channels);
    let block = null;
    let offset = period;
    node.onaudioprocess = (event) => {
      const out = event.outputBuffer;
      const planes = [];
      for (let c = 0; c < out.numberOfChannels; c++) planes.push(out.getChannelData(c));
      for (let i = 0; i < out.length; i++) {
        if (offset >= period) {
          block = render();
          offset = 0;
        }
        for (let c = 0; c < planes.length; c++) planes[c][i] = block[offset * channels + Math.min(c, channels - 1)];
        offset++;
      }
      device.played += out.length;
    };
    node.connect(context.destination);
    device.node = node;
    device.queueFrames = size;
    device.kind = 'ScriptProcessorNode';
  }
});

EM_JS(void, rae_web_audio_stop, (int id), {
  const device = Module.raeAudioDevices[id];
  if (!device) return;
  device.stopped = true;
  device.context.suspend().catch(() => {});
});

EM_JS(void, rae_web_audio_close, (int id), {
  const device = Module.raeAudioDevices[id];
  if (!device) return;
  device.closed = true;
  if (device.node) {
    if (device.node.port) device.node.port.onmessage = null;
    device.node.onaudioprocess = null;
    device.node.disconnect();
  }
  device.context.close().catch(() => {});
  Module.raeAudioDevices[id] = null;
});

EM_JS(double, rae_web_audio_underrun, (int id), {
  const device = Module.raeAudioDevices[id];
  return device ? device.underrun : 0;
});

/* What lies between a rendered frame and the speaker, in frames: the queue
 * the worklet keeps, the context's own processing (baseLatency) and the
 * output's (outputLatency, which Chrome knows once the context runs) */
EM_JS(double, rae_web_audio_latency_frames, (int id), {
  const device = Module.raeAudioDevices[id];
  if (!device) return 0;
  const context = device.context;
  const seconds = (context.baseLatency || 0) + (context.outputLatency || 0);
  return device.queueFrames + Math.round(seconds * context.sampleRate);
});

EM_JS(int, rae_web_audio_kind, (int id), {
  const device = Module.raeAudioDevices[id];
  if (!device || !device.kind) return 0;
  return device.kind === 'AudioWorklet' ? 1 : 2;
});

int64_t rae_audio_device_open(int64_t sample_rate, int64_t channels, int64_t period_frames, int64_t input_channels) {
  if (input_channels > 0 || channels <= 0 || period_frames <= 0) return 0;
  int id = rae_web_audio_create((int)sample_rate);
  if (!id) return 0;
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)calloc(1, sizeof(RaeWebAudioDevice));
  if (!audio) return 0;
  audio->id = id;
  audio->sample_rate = rae_web_audio_rate(id);
  audio->channels = channels;
  audio->period_frames = period_frames;
  audio->block = (float*)calloc((size_t)(period_frames * channels), sizeof(float));
  if (!audio->block) {
    rae_web_audio_close(id);
    free(audio);
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
  int kind = rae_web_audio_kind(((RaeWebAudioDevice*)(intptr_t)device)->id);
  const char* name = kind == 1 ? "Web Audio (AudioWorklet)"
                     : kind == 2 ? "Web Audio (ScriptProcessorNode)" : "Web Audio";
  int64_t length = (int64_t)strlen(name);
  if (length > capacity) length = capacity;
  memcpy(into, name, (size_t)length);
  return length;
}

/* The queue and the context's latency (the clock's latency, §6.1) */
int64_t rae_audio_device_buffer_frames(int64_t device) {
  return device ? (int64_t)rae_web_audio_latency_frames(((RaeWebAudioDevice*)(intptr_t)device)->id) : 0;
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

/* Stage A's ring: the browser has no render thread, so never used */
void rae_audio_device_attach(int64_t device, float* slots, int64_t* counters, int64_t capacity,
                             int64_t signal) {
  (void)device; (void)slots; (void)counters; (void)capacity; (void)signal;
}

void rae_audio_device_attach_render(int64_t device, void* state) {
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)(intptr_t)device;
  if (audio) audio->render_state = state;
}

/* The worklet keeps eight periods queued (43 ms at 256 frames and 48 kHz):
 * the main thread renders them between its other work, so the queue covers
 * a frame of the page's own work */
int64_t rae_audio_device_start(int64_t device) {
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)(intptr_t)device;
  if (!audio) return 0;
  int queue = (int)(8 * audio->period_frames);
  /* RAE_AUDIO_WEB_OUTPUT=script (a page's ?RAE_AUDIO_WEB_OUTPUT=script):
   * the ScriptProcessorNode even where an AudioWorklet exists, to test it */
  const char* output = getenv("RAE_AUDIO_WEB_OUTPUT");
  int script = output && strcmp(output, "script") == 0;
  rae_web_audio_start(audio->id, audio, (int)audio->channels, (int)audio->period_frames, queue, script);
  return 1;
}

int64_t rae_audio_device_stop(int64_t device) {
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)(intptr_t)device;
  if (!audio) return 0;
  rae_web_audio_stop(audio->id);
  return 1;
}

void rae_audio_device_close(int64_t device) {
  RaeWebAudioDevice* audio = (RaeWebAudioDevice*)(intptr_t)device;
  if (!audio) return;
  rae_web_audio_close(audio->id);
  free(audio->block);
  free(audio);
}

int64_t rae_audio_device_underrun_frames(int64_t device) {
  return device ? (int64_t)rae_web_audio_underrun(((RaeWebAudioDevice*)(intptr_t)device)->id) : 0;
}

int64_t rae_audio_device_callbacks(int64_t device) {
  return device ? ((RaeWebAudioDevice*)(intptr_t)device)->callbacks : 0;
}
#endif

/* ---- AAC through WebCodecs' AudioDecoder ---------------------------------- */
EM_JS(int, rae_web_aac_create, (int sample_rate, int channels), {
  if (typeof AudioDecoder === 'undefined') return 0;
  const state = { outputs: [], failed: false, time: 0, rate: sample_rate, channels: channels };
  try {
    state.decoder = new AudioDecoder({
      output: (data) => state.outputs.push(data),
      error: (error) => { state.failed = true; console.error('[audio] AAC:', error); }
    });
  } catch (error) {
    return 0;
  }
  Module.raeAacDecoders = Module.raeAacDecoders || [null];
  Module.raeAacDecoders.push(state);
  return Module.raeAacDecoders.length - 1;
});

/* `config`: the track's AudioSpecificConfig */
EM_JS(int, rae_web_aac_configure, (int id, const uint8_t* config, int size), {
  const state = Module.raeAacDecoders[id];
  if (!state) return 0;
  state.config = {
    codec: 'mp4a.40.2',
    sampleRate: state.rate,
    numberOfChannels: state.channels,
    description: HEAPU8.slice(config, config + size)
  };
  try {
    state.decoder.configure(state.config);
  } catch (error) {
    return 0;
  }
  return 1;
});

/* One packet (`size` 0: the end) in; the frames that came out, interleaved
 * Floats into `out`, at most `max_frames`; -1 when the decoder failed */
EM_ASYNC_JS(int, rae_web_aac_decode, (int id, const uint8_t* packet, int size, float* out, int max_frames,
                                      int channels), {
  const state = Module.raeAacDecoders[id];
  if (!state || state.failed || state.decoder.state !== 'configured') return -1;
  if (size > 0) {
    state.decoder.decode(new EncodedAudioChunk({
      type: 'key',
      timestamp: state.time,
      data: HEAPU8.slice(packet, packet + size)
    }));
    state.time += Math.round(1024 * 1000000 / state.rate);
  }
  if (size > 0) {
    /* no flush between packets: Chrome resets the decoder on a flush, and an
     * AAC frame overlaps the one before. Wait (zero-delay tasks) until the
     * decoder's queue is empty and its outputs have come, then take what is
     * there, maybe nothing yet (audio/AacDecoder counts frames, not packets) */
    if (!state.tick) {
      const channel = new MessageChannel();
      const waiting = [];
      channel.port1.onmessage = () => { const resume = waiting.shift(); if (resume) resume(); };
      state.tick = () => new Promise((resume) => { waiting.push(resume); channel.port2.postMessage(0); });
    }
    let quiet = 0;
    for (let round = 0; round < 10000 && quiet < 3; round++) {
      const before = state.outputs.length;
      await state.tick();
      if (state.failed) return -1;
      quiet = state.decoder.decodeQueueSize === 0 && state.outputs.length === before ? quiet + 1 : 0;
    }
  } else {
    try {
      await state.decoder.flush();
    } catch (error) {
      return -1;
    }
  }
  let frames = 0;
  for (const data of state.outputs) {
    const count = Math.min(data.numberOfFrames, max_frames - frames);
    const planes = [];
    for (let c = 0; c < data.numberOfChannels; c++) {
      const plane = new Float32Array(data.numberOfFrames);
      data.copyTo(plane, { planeIndex: c, format: 'f32-planar' });
      planes.push(plane);
    }
    const base = (out >> 2) + frames * channels;
    for (let i = 0; i < count; i++) {
      for (let c = 0; c < channels; c++) {
        const plane = planes[Math.min(c, planes.length - 1)];
        HEAPF32[base + i * channels + c] = plane ? plane[i] : 0;
      }
    }
    frames += count;
    data.close();
  }
  state.outputs = [];
  return state.failed ? -1 : frames;
});

EM_JS(void, rae_web_aac_reset, (int id), {
  const state = Module.raeAacDecoders[id];
  if (!state) return;
  for (const data of state.outputs) data.close();
  state.outputs = [];
  state.time = 0;
  if (state.decoder.state === 'closed' || !state.config) return;
  /* reset() drops the configuration: configure again */
  state.decoder.reset();
  state.decoder.configure(state.config);
});

EM_JS(void, rae_web_aac_close, (int id), {
  const state = Module.raeAacDecoders[id];
  if (!state) return;
  for (const data of state.outputs) data.close();
  if (state.decoder.state !== 'closed') state.decoder.close();
  Module.raeAacDecoders[id] = null;
});

int64_t rae_audio_aac_open(int64_t sample_rate, int64_t channels) {
  if (channels <= 0) return 0;
  return (int64_t)rae_web_aac_create((int)sample_rate, (int)channels);
}

/* On the web the cookie is the AudioSpecificConfig (audio/AacPlatform) */
int64_t rae_audio_aac_set_cookie(int64_t converter, uint8_t* cookie, int64_t size) {
  if (!converter) return 0;
  return (int64_t)rae_web_aac_configure((int)converter, cookie, (int)size);
}

int64_t rae_audio_aac_decode(int64_t converter, uint8_t* packet, int64_t size, float* out,
                             int64_t max_frames, int64_t channels) {
  if (!converter) return -1;
  return (int64_t)rae_web_aac_decode((int)converter, packet, (int)size, out, (int)max_frames, (int)channels);
}

void rae_audio_aac_reset(int64_t converter) {
  if (converter) rae_web_aac_reset((int)converter);
}

void rae_audio_aac_close(int64_t converter) {
  if (converter) rae_web_aac_close((int)converter);
}
