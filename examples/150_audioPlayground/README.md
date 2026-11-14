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
- **Status:** the device the engine plays on, its sample rate and period, the latency the device
  and the engine add, the frames of silence played because samples were late (underruns), and
  how many sounds played and ended.

## How it is built

- The layout is `assets/Ui.raescene` (with its own `assets/Theme.raescene`); the font is the
  shared one in `lib/assets/walker/`.
- `Playground.rae` holds the app's state, with the audio engine as its resource, and loads the
  sounds once. Each section is its own module (`SoundsSection`, `MusicSection`, `MixSection`,
  `StatusSection`), run once a frame from `Main.rae`.
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
| `RAE_AUDIO_PLAYGROUND_DEMO=1` | presses the controls by itself (two sounds, the stream, an alert and the chime, a seek to 16 s, pan left) and logs a summary line at the end; with the offline engine it ends after 120 frames (2 s of audio), so its result does not depend on the machine's speed |
| `RAE_AUDIO_PLAYGROUND_SECONDS=N` | ends a run on the real device after N seconds |
| `RAE_REALTIME_CHECK=1` | fails the run if the audio thread allocated, locked or logged |

The visual example gate (`RAE_EXAMPLE_FILTER=150_audioPlayground make -C compiler test-examples`)
runs the headless demo and checks its summary line and the screenshot.
