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
