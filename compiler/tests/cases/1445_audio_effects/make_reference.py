# The reverb reference for fixture 1445: Jezar's public domain Freeverb,
# written here independently of the Rae code from its published structure,
# in float32 (every +, -, * rounded to float32, which double arithmetic then
# rounding gives exactly). Writes reverb_reference.wav (32-bit float, stereo,
# 44.1 kHz) for the test input, and prints the frame where the impulse
# response falls below -60 dB (the fixture expects it). Run once, from this
# folder: python3 make_reference.py
import struct

def f32(x):
    return struct.unpack('f', struct.pack('f', x))[0]

RATE = 44100
COMBS = [1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617]
ALLPASSES = [556, 441, 341, 225]
SPREAD = 23

class Comb:
    def __init__(self, size):
        self.buffer = [0.0] * size
        self.index = 0
        self.store = 0.0
    def process(self, value, feedback, damp1, damp2):
        output = self.buffer[self.index]
        self.store = f32(f32(output * damp2) + f32(self.store * damp1))
        self.buffer[self.index] = f32(value + f32(self.store * feedback))
        self.index = (self.index + 1) % len(self.buffer)
        return output

class Allpass:
    def __init__(self, size):
        self.buffer = [0.0] * size
        self.index = 0
    def process(self, value):
        delayed = self.buffer[self.index]
        output = f32(-value + delayed)
        self.buffer[self.index] = f32(value + f32(delayed * f32(0.5)))
        self.index = (self.index + 1) % len(self.buffer)
        return output

class Freeverb:
    def __init__(self, room, damping, wet, dry, width):
        self.combs_left = [Comb(t) for t in COMBS]
        self.combs_right = [Comb(t + SPREAD) for t in COMBS]
        self.allpasses_left = [Allpass(t) for t in ALLPASSES]
        self.allpasses_right = [Allpass(t + SPREAD) for t in ALLPASSES]
        wet_scaled = f32(f32(wet) * f32(3.0))
        self.feedback = f32(f32(f32(room) * f32(0.28)) + f32(0.7))
        self.damp1 = f32(f32(damping) * f32(0.4))
        self.damp2 = f32(f32(1.0) - self.damp1)
        self.wet1 = f32(wet_scaled * f32(f32(f32(width) / f32(2.0)) + f32(0.5)))
        self.wet2 = f32(wet_scaled * f32(f32(f32(1.0) - f32(width)) / f32(2.0)))
        self.dry = f32(f32(dry) * f32(2.0))
        self.gain = f32(0.015)
    def process(self, left, right):
        mono = f32(f32(left + right) * self.gain)
        out_left = 0.0
        out_right = 0.0
        for comb_left, comb_right in zip(self.combs_left, self.combs_right):
            out_left = f32(out_left + comb_left.process(mono, self.feedback, self.damp1, self.damp2))
            out_right = f32(out_right + comb_right.process(mono, self.feedback, self.damp1, self.damp2))
        for allpass_left, allpass_right in zip(self.allpasses_left, self.allpasses_right):
            out_left = allpass_left.process(out_left)
            out_right = allpass_right.process(out_right)
        result_left = f32(f32(f32(out_left * self.wet1) + f32(out_right * self.wet2)) + f32(left * self.dry))
        result_right = f32(f32(f32(out_right * self.wet1) + f32(out_left * self.wet2)) + f32(right * self.dry))
        return result_left, result_right

# The test input, as the Rae fixture makes it: an impulse (1 left, 0.5 right)
# at frame 0, then a noise burst at frames 100..1099 from a 31-bit LCG
def test_input(frames):
    seed = 12345
    values = []
    for frame in range(frames):
        left = 0.0
        right = 0.0
        if frame == 0:
            left, right = 1.0, 0.5
        elif 100 <= frame < 1100:
            seed = (seed * 1103515245 + 12345) % 2147483648
            left = f32(((seed // 65536) % 2001 - 1000) / 1000.0)
            seed = (seed * 1103515245 + 12345) % 2147483648
            right = f32(((seed // 65536) % 2001 - 1000) / 1000.0)
        values.append((left, right))
    return values

def write_wav(path, frames):
    data = b''.join(struct.pack('<ff', l, r) for l, r in frames)
    header = b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE'
    header += b'fmt ' + struct.pack('<IHHIIHH', 16, 3, 2, RATE, RATE * 8, 8, 32)
    header += b'data' + struct.pack('<I', len(data))
    with open(path, 'wb') as out:
        out.write(header + data)

# wet 1/3 rounded to float32, as the Rae fixture's 1.0 / 3.0
reverb = Freeverb(0.5, 0.5, f32(1.0 / 3.0), 0.0, 1.0)
write_wav('reverb_reference.wav', [reverb.process(l, r) for l, r in test_input(13230)])

# The tail: an impulse into a fresh reverb for 4 s; the last frame whose
# louder channel is above the peak's -60 dB
tail = Freeverb(0.5, 0.5, f32(1.0 / 3.0), 0.0, 1.0)
peak = 0.0
levels = []
for frame in range(4 * RATE):
    left, right = tail.process(1.0 if frame == 0 else 0.0, 1.0 if frame == 0 else 0.0)
    level = max(abs(left), abs(right))
    levels.append(level)
    peak = max(peak, level)
last = max(i for i, level in enumerate(levels) if level > peak * 0.001)
print('tail', last, 'frames', last / RATE, 's')
