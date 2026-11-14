# 150 — Audio playground

The audio package (`packages/audio`, docs/audio-design.md) to try by ear. This first part has
the basics a game or an app uses. Later parts add one section each: the synth and MIDI, the
microphone, effects on the buses, and the real-time view.

Run it from Rae devtools' Examples, or:

```sh
compiler/bin/rae run examples/150_audioPlayground/Main.rae
```

## What to try

- **Sounds:** Click, Bell and Whoosh are WAV files, Chime is AAC. They play on the effects bus.
  Press them fast, or several at once: each press is its own voice, so they overlap.
- **Alert:** plays on the alerts bus. While it plays, the music bus goes down 14 dB and comes back
  (ducking). Turn **Ducking** off in the Mix section to hear the difference.
- **Music bus:** an MP3 streamed from disk (not loaded whole). It is a test signal, not music: a
  short tone at the start of every second, 300 Hz at 0 s and 40 Hz higher each second, so after a
  seek the pitch tells where it is. Play and Pause share one button; **From the start** starts it
  again. Drag the position slider to seek; it follows the stream while you do not hold it.
- **Mix:** a volume per bus (music, effects, alerts; 100 % is 0 dB, 0 % is silent), and the pan
  of the stream and of every sound started after you move it.
- **Game sounds:** **Footstep (varied)** plays the same short sound with a little different
  pitch and level each time (up to 6 % and 3 dB either way), from the app's own random numbers
  with a fixed seed, so a run plays the same. **Machine gun (cap 3)** fires ten shots 60 ms
  apart, each 0.25 s long: the shot sound is capped at three voices, so as each new shot starts
  the oldest fades out in 10 ms, and the rest of the mix is never touched. The slider sets the
  playback rate of the orbiting buzz below, as a car engine's pitch follows its speed (start
  the orbit to hear it).
- **Placed in 3D:** a looping buzz placed in the world (`audio/AudioSpatial`, docs/audio-design.md
  §10.4) goes round you, the listener, who looks ahead. Use headphones: it moves from ear to ear
  as it passes (0° is ahead, 90° on your right). The slider sets its distance: it is full inside
  2 m, quieter further out, and silent past 25 m, where its voice is paused so it costs nothing;
  bring it back and it goes on. The readout shows its angle, pan and gain.
- **Steps from events:** a walker or a mech crosses a strip of six grounds: grass, gravel, road,
  sand, rock and shallow water. Each foot that lands is a step event (`app3d/StepEvents`, from
  the walker kit's `app3d/Footprints` and `app3d/MechWalker`) with the foot, the speed, the
  weight and the ground. `StepsSection`'s step sound system is the game's own code: it plays
  the sound of that ground, louder at speed, and a heavy stomp under the 12 t mech, each a
  little varied. The walker kit has no sound code.
- **Synth and MIDI** (the third column): the package's own synth (`audio/AudioSynth`).
  - The **keyboard**, C4 to C5, plays with the mouse (drag across the keys to slide from note
    to note) and with several fingers at once on a touch screen.
  - **Waveform** steps through soft (a sine and its octave, the synth's own tone), sine,
    triangle, square and saw; the four sliders set the envelope (attack, decay, sustain,
    release). The notes sounding take a change at once.
  - **Metronome** plays two bars of 4/4 at the tempo slider's speed: a click on every beat, the
    first of each bar an octave up and louder. Its notes are scheduled at exact samples
    (`playNoteAt`), their times from a TempoMap and a Transport (`audio/MusicalTime`); it is a
    technical signal, not music.
  - **MIDI**: every connected MIDI keyboard or controller plays the synth. The line under it
    names them, or says none is connected; one plugged in later is found within two seconds.
    The last note received is shown, and its key lights up.
- **Microphone: record and play back** (under the synth; `audio/AudioInput`).
  - **Allow** asks for the microphone. The engine opens with the input only once it is allowed
    (opening it unasked would bring up the system's question at launch), so allowing it later
    opens the engine again with the input and loads every sound again (the music and the orbit
    stop). A refusal says where to allow it: Settings > Privacy & Security > Microphone.
  - The **level** bar is what came in during the last frame (0 % is 60 dB down).
  - **Record 3 s** records a take while five clicks play, half a second apart. The take is cut
    by the round trip (output plus input latency), so its frame i lines up with what played at
    its start sample plus i. The clicks the microphone heard are found in the take and
    compared with when they were played: what is left is the air between speaker and
    microphone and any latency the platform does not report. On a MacBook Pro's own speakers
    and microphone that is 7-8 ms (the round trip is 3890 frames there, most of it the
    speakers' and the microphone's own processing, which Core Audio reports as their streams'
    latency).
  - **Play back** plays the take and the same clicks from one sample, the take started that
    measured offset earlier, so the recorded clicks land on the played ones.
  - **Save WAV** writes the take as a 32-bit float WAV in the app's documents (the Mac:
    `~/Documents`; an iPhone: the app's own Documents, seen in the Files app).
- **Effects on the buses** (the fourth column; `audio/BusEffects`): an EQ band, a compressor
  and a reverb on the music and the effects bus (**Bus** switches which one the column shows).
  Each turns on and off with a 5 ms crossfade and its sliders change it while it plays, without
  a click: the engine is started with the effect stages (`settings.busEffects`), so nothing is
  rebuilt. Off, a stage passes the sound unchanged, to the bit.
  - **Measure each on and off** renders one second of the music stream offline (its own
    engine, no device), with every effect off and with each of the three on alone at the
    bus's settings, and shows how the level changed: overall, at the stream's 300 Hz tone, at
    3 kHz, and in the gaps between the tones (where only a reverb's tail sounds). With the
    defaults: the EQ (+9 dB at 300 Hz) +8.9 dB at the tone and -0.1 dB at 3 kHz; the
    compressor (-30 dB, 4:1) about -11 dB; the reverb +0 dB at the tone and +9.5 dB in the
    gaps. Listening to them is a person's check.
- **Status:** the device the engine plays on, its sample rate and period, the latency the device
  and the engine add, the frames of silence played because samples were late (underruns), and
  how many sounds played and ended.

## How it is built

- The layout is `assets/Ui.raescene` (with its own `assets/Theme.raescene`); the font is the
  shared one in `lib/assets/walker/`.
- `Playground.rae` holds the app's state, with the audio engine as its resource, and loads the
  sounds once. Each section is its own module (`SoundsSection`, `MusicSection`, `MixSection`,
  `GameSection`, `PlacedSection`, `StepsSection` with its state in `StepsDemo`, `SynthSection` with its state in
  `SynthDemo`, `InputSection` with its state in `InputDemo`, `EffectsSection`, `StatusSection`), run once a frame from `Main.rae`.
- The sound files are made by `makeSounds.py`: technical test signals from simple math (tones,
  decays, filtered noise), never composed music, so nothing is under anyone's copyright. The chime
  is turned into AAC with macOS `afconvert`, the stream into MP3 with `lame`. They are mono at
  22 050 Hz and about 160 KB in all, small enough to commit, so the example runs from a fresh
  checkout; the script is only needed to change them.

## For agents: headless runs

| variable | what it does |
|---|---|
| `RAE_SDL_HEADLESS_MS=N` | a headless run of N ms; the engine renders offline (no device) |
| `RAE_AUDIO_DEVICE=offline` | the offline engine in a windowed run |
| `RAE_AUDIO_PLAYGROUND_DEMO=1` | presses the controls by itself (two sounds, the stream, an alert and the chime, a seek to 16 s, pan left, a varied step, a capped burst, the orbit at 6 m with its engine rate raised to 150 % and then past its far distance, a walker and a mech crossing the strip of grounds, the metronome at 240 beats a minute, and three simulated fingers on the keyboard, one sliding to another key, and the effects measured with the reverb then turned on) and logs a summary line at the end; with the offline engine it ends after 320 frames (5.3 s of audio), so its result does not depend on the machine's speed |
| `RAE_AUDIO_PLAYGROUND_SECONDS=N` | ends a run on the real device after N seconds |
| `RAE_AUDIO_PLAYGROUND_RECORD=1` | with a device and the microphone allowed: records a take at frame 60, plays it back and saves it, and logs a line `[audioPlayground] input: ...` at the end (the clicks heard and their offset, the saved path) |
| `RAE_REALTIME_CHECK=1` | fails the run if the audio thread allocated, locked or logged |

The visual example gate (`RAE_EXAMPLE_FILTER=150_audioPlayground make -C compiler test-examples`)
runs the headless demo and checks its summary line and the screenshot.
