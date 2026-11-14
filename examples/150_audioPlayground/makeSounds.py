#!/usr/bin/env python3
# Makes the audio playground's sound files (examples/150_audioPlayground/assets/): technical test
# signals from simple math (tones, decays, filtered noise), never composed music. Run once from the
# repository root; the small files are committed, so the example does not need this to run:
#   python3 examples/150_audioPlayground/makeSounds.py
# Needs `lame` (the MP3 stream) and macOS `afconvert` (the AAC chime). Mono at 22 050 Hz keeps
# every file small; the engine plays a sound of any rate and resamples a stream.
import math
import os
import random
import struct
import subprocess
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")
RATE = 22050


def write_wav(name, frames, channels=1):
    """frames: a list of floats (mono) or of (left, right) pairs, each -1..1"""
    path = os.path.join(ASSETS, name)
    with wave.open(path, "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(RATE)
        data = bytearray()
        for frame in frames:
            values = frame if channels == 2 else (frame,)
            for value in values:
                data += struct.pack("<h", int(max(-1.0, min(1.0, value)) * 32000))
        out.writeframes(bytes(data))
    return path


def seconds(count):
    return int(count * RATE)


def click():
    # a short, bright tick: a 2.4 kHz tone with a 25 ms decay
    return [0.8 * math.sin(2 * math.pi * 2400 * i / RATE) * math.exp(-i / (0.025 * RATE))
            for i in range(seconds(0.12))]


def bell():
    # partials of a struck metal bar (inharmonic), the high ones fading first
    partials = [(523.0, 1.0, 0.45), (1046.0 * 1.003, 0.5, 0.3), (1570.0, 0.35, 0.2), (2620.0, 0.2, 0.12)]
    out = []
    for i in range(seconds(1.0)):
        t = i / RATE
        value = sum(a * math.exp(-t / d) * math.sin(2 * math.pi * f * t) for f, a, d in partials)
        out.append(0.35 * value * min(1.0, i / 40.0))
    return out


def whoosh():
    # noise through a one-pole low-pass whose cutoff sweeps up and back, with a swell
    rng = random.Random(150)
    out, low = [], 0.0
    total = seconds(0.6)
    for i in range(total):
        share = i / total
        cutoff = 300 + 4000 * math.sin(math.pi * share)
        k = 1.0 - math.exp(-2 * math.pi * cutoff / RATE)
        low += k * (rng.uniform(-1, 1) - low)
        out.append(0.9 * low * math.sin(math.pi * share))
    return out


def chime():
    # one 1 kHz tone with a 0.25 s decay (made into AAC, to test the AAC decoder)
    return [0.5 * math.sin(2 * math.pi * 1000 * i / RATE) * math.exp(-i / (0.25 * RATE)) * min(1.0, i / 30.0)
            for i in range(seconds(0.6))]


def alert():
    # two beeps, the kind an alarm or a message plays (it ducks the music)
    out = []
    for i in range(seconds(0.5)):
        t = i / RATE
        on = (t < 0.18) or (0.3 < t < 0.48)
        out.append(0.45 * math.sin(2 * math.pi * 1320 * t) if on else 0.0)
    return out


def buzz_loop():
    # The placed sound that goes round the listener: a 110 Hz buzz (harmonics 1 to 8 at 1/n, a
    # band-limited sawtooth), 0.5 s, exactly 55 cycles, so it loops without a seam. Bright
    # enough that its pan and distance are easy to hear.
    out = []
    for i in range(seconds(0.5)):
        t = i / RATE
        value = sum(math.sin(2 * math.pi * 110 * n * t) / n for n in range(1, 9))
        out.append(0.3 * value)
    return out


def step():
    # a footstep stand-in: low-passed noise with a 25 ms decay, 0.15 s (played varied, so no
    # two sound the same)
    rng = random.Random(151)
    out, low = [], 0.0
    for i in range(seconds(0.15)):
        k = 0.12
        low += k * (rng.uniform(-1, 1) - low)
        out.append(2.5 * low * math.exp(-i / (0.025 * RATE)) * min(1.0, i / 20.0))
    return out


def shot():
    # a gunshot stand-in: white noise with a 50 ms decay, 0.25 s, longer than the 60 ms between
    # shots of a burst, so a burst needs the voice cap
    rng = random.Random(152)
    return [0.8 * rng.uniform(-1, 1) * math.exp(-i / (0.05 * RATE)) * min(1.0, i / 10.0)
            for i in range(seconds(0.25))]


def noise_burst(seed, length, decay, smoothing, gain):
    # noise through a one-pole low-pass (`smoothing` 0..1: lower is duller), decaying
    rng = random.Random(seed)
    out, low = [], 0.0
    for i in range(seconds(length)):
        low += smoothing * (rng.uniform(-1, 1) - low)
        out.append(gain * low * math.exp(-i / (decay * RATE)) * min(1.0, i / 30.0))
    return out


def step_grass():
    # a soft, dull brush: low-passed noise, 0.14 s
    return noise_burst(161, 0.14, 0.035, 0.06, 4.0)


def step_gravel():
    # a crunch: a quick run of small random clicks under a short noise bed, 0.2 s
    rng = random.Random(162)
    out = noise_burst(163, 0.2, 0.05, 0.5, 0.25)
    for _ in range(26):
        at = int(rng.uniform(0, 0.12) * RATE)
        size = rng.uniform(0.2, 0.6)
        for j in range(40):
            if at + j < len(out):
                out[at + j] += size * rng.uniform(-1, 1) * math.exp(-j / 6.0)
    return out


def step_hard():
    # a hard tread (a road, rock): a 180 Hz knock with a 30 ms decay and a 3 ms click
    rng = random.Random(164)
    return [0.6 * math.sin(2 * math.pi * 180 * i / RATE) * math.exp(-i / (0.03 * RATE))
            + (0.5 * rng.uniform(-1, 1) * math.exp(-i / (0.003 * RATE)))
            for i in range(seconds(0.12))]


def step_sand():
    # a soft hiss that rises and falls, 0.2 s
    rng = random.Random(165)
    out, low = [], 0.0
    total = seconds(0.2)
    for i in range(total):
        low += 0.2 * (rng.uniform(-1, 1) - low)
        out.append(1.2 * low * math.sin(math.pi * i / total))
    return out


def step_water():
    # a splash: noise whose level wobbles at 30 Hz as it fades, 0.3 s
    rng = random.Random(166)
    out, low = [], 0.0
    for i in range(seconds(0.3)):
        t = i / RATE
        low += 0.35 * (rng.uniform(-1, 1) - low)
        wobble = 0.6 + 0.4 * math.sin(2 * math.pi * 30 * t)
        out.append(1.4 * low * wobble * math.exp(-t / 0.09) * min(1.0, i / 60.0))
    return out


def mech_stomp():
    # a machine's foot: a 45 Hz thud falling to 35 Hz, 0.4 s, with a low rumble of noise
    rng = random.Random(167)
    out, low, phase = [], 0.0, 0.0
    for i in range(seconds(0.4)):
        t = i / RATE
        phase += 2 * math.pi * (45 - 25 * t) / RATE
        low += 0.03 * (rng.uniform(-1, 1) - low)
        out.append((0.8 * math.sin(phase) + 2.0 * low) * math.exp(-t / 0.12) * min(1.0, i / 40.0))
    return out


def position_tones():
    # The stream's test signal, 30 s: at the start of every second a 0.15 s tone whose pitch
    # tells the second (300 Hz + 40 Hz per second: 300 Hz at 0 s, 1460 Hz at 29 s), silence
    # between. A seek, a pause and the position readout can be checked by ear against it.
    out = [0.0] * seconds(30.0)
    length = seconds(0.15)
    for second in range(30):
        frequency = 300.0 + 40.0 * second
        first = seconds(second)
        for i in range(length):
            edge = min(1.0, i / 100.0, (length - i) / 100.0)
            out[first + i] = 0.5 * edge * math.sin(2 * math.pi * frequency * i / RATE)
    return out


def main():
    os.makedirs(ASSETS, exist_ok=True)
    write_wav("click.wav", click())
    write_wav("bell.wav", bell())
    write_wav("whoosh.wav", whoosh())
    write_wav("alert.wav", alert())
    write_wav("buzzLoop.wav", buzz_loop())
    write_wav("step.wav", step())
    write_wav("shot.wav", shot())
    write_wav("stepGrass.wav", step_grass())
    write_wav("stepGravel.wav", step_gravel())
    write_wav("stepHard.wav", step_hard())
    write_wav("stepSand.wav", step_sand())
    write_wav("stepWater.wav", step_water())
    write_wav("mechStomp.wav", mech_stomp())
    chime_wav = write_wav("chime-source.wav", chime())
    subprocess.run(["afconvert", "-f", "m4af", "-d", "aac", "-b", "32000", chime_wav,
                    os.path.join(ASSETS, "chime.m4a")], check=True)
    os.remove(chime_wav)
    tones_wav = write_wav("positionTones-source.wav", position_tones())
    subprocess.run(["lame", "--quiet", "-m", "m", "-b", "16", tones_wav,
                    os.path.join(ASSETS, "positionTones.mp3")], check=True)
    os.remove(tones_wav)
    for name in sorted(os.listdir(ASSETS)):
        print(name, os.path.getsize(os.path.join(ASSETS, name)), "bytes")


main()
