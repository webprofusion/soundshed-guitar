#!/usr/bin/env python3
"""Synthesise the three guitar DI clips the factory pack is measured against.

The clips are made here rather than recorded so that no third-party audio is added to the
repository and anyone can regenerate them bit for bit. Each is a plucked-string model
(Karplus-Strong with a damping filter and a pick-position comb) through a pickup-like
low-pass, at the level of a guitar plugged straight into an interface. All three have the same RMS.

    clean-chords.wav   open chords, strummed and left to ring
    palm-mute.wav      low-string eighth notes, heavily damped
    lead.wav           single notes high on the neck, with vibrato

Usage: python3 make_di.py <output directory>
"""

import math
import struct
import sys
import wave
from pathlib import Path

RATE = 48000
# Every clip is brought to the same RMS, so a preset's level on one can be compared with its
# level on another. About -30 dBFS, which puts a strummed chord's peaks near -12 dBFS: a hot
# humbucker at unity interface gain.
RMS = 10.0 ** (-30.0 / 20.0)


class Lcg:
    """A fixed generator, so the clips never change between runs or Python versions."""

    def __init__(self, seed):
        self.state = seed

    def bipolar(self):
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return (self.state >> 8) / 8388608.0 - 1.0


def pluck(freq, seconds, damping, rng, pick=0.13, vibrato=0.0):
    """One plucked string. `damping` in 0..1 is how fast it dies: 0 rings, 1 is palm muted."""
    count = int(seconds * RATE)
    period = RATE / freq
    size = int(period) + 2
    line = [rng.bipolar() for _ in range(size)]

    # A pick is not a noise burst: round the excitation off, as a plectrum's width does.
    for _ in range(3):
        smoothed = 0.0

        for i in range(size):
            smoothed += 0.25 * (line[i] - smoothed)
            line[i] = smoothed

    # Plucking at a point removes the harmonics with a node there.
    offset = max(1, int(period * pick))
    line = [line[i] - line[(i - offset) % size] for i in range(size)]

    loss = 0.9985 - 0.03 * damping
    bright = 0.6 - 0.4 * damping  # one-pole in the loop: upper harmonics die first, sooner when muted
    out = [0.0] * count
    state = 0.0
    position = 0.0

    for n in range(count):
        length = period * (1.0 + vibrato * 0.006 * math.sin(2.0 * math.pi * 5.5 * n / RATE) * min(1.0, n / (0.25 * RATE)))
        read = position - length
        index = math.floor(read)
        frac = read - index
        a = line[index % size]
        b = line[(index + 1) % size]
        sample = a + (b - a) * frac
        state += bright * (sample - state)
        line[int(position) % size] = state * loss
        out[n] = sample
        position += 1.0

    return out


def mix_into(target, source, start, gain=1.0):
    begin = int(start * RATE)

    for i, value in enumerate(source):
        if begin + i >= len(target):
            break

        target[begin + i] += value * gain


def pickup(signal):
    """Second-order low-pass near 3.5 kHz with a small resonance, and a DC block."""
    w = 2.0 * math.pi * 3500.0 / RATE
    alpha = math.sin(w) / (2.0 * 1.3)
    cosw = math.cos(w)
    b0 = (1.0 - cosw) / 2.0 / (1.0 + alpha)
    b1 = (1.0 - cosw) / (1.0 + alpha)
    a1 = -2.0 * cosw / (1.0 + alpha)
    a2 = (1.0 - alpha) / (1.0 + alpha)
    out = []
    x1 = x2 = y1 = y2 = 0.0
    dc_in = dc_out = 0.0

    for x in signal:
        y = b0 * x + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, x, y1, y
        dc_out = y - dc_in + 0.999 * dc_out
        dc_in = y
        out.append(dc_out)

    return out


def write(path, signal):
    rms = math.sqrt(sum(v * v for v in signal) / len(signal)) or 1.0
    scale = RMS / rms

    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(3)
        handle.setframerate(RATE)
        frames = bytearray()

        for value in signal:
            frames += struct.pack("<i", int(round(value * scale * 8388607.0)))[:3]

        handle.writeframes(bytes(frames))


E2, A2, D3, G3, B3, E4 = 82.41, 110.0, 146.83, 196.0, 246.94, 329.63


def fret(open_hz, n):
    return open_hz * 2.0 ** (n / 12.0)


def clean_chords():
    rng = Lcg(1)
    seconds = 8.0
    out = [0.0] * int(seconds * RATE)
    chords = [
        [fret(E2, 0), fret(A2, 2), fret(D3, 2), fret(G3, 1), fret(B3, 0), fret(E4, 0)],  # E
        [fret(A2, 0), fret(D3, 2), fret(G3, 2), fret(B3, 2), fret(E4, 0)],  # A
        [fret(D3, 0), fret(G3, 2), fret(B3, 3), fret(E4, 2)],  # D
        [fret(E2, 3), fret(A2, 2), fret(D3, 0), fret(G3, 0), fret(B3, 0), fret(E4, 3)],  # G
    ]

    for index, chord in enumerate(chords):
        for string, freq in enumerate(chord):
            mix_into(out, pluck(freq, 2.0, 0.05, rng), index * 2.0 + string * 0.018, 0.8)

    return pickup(out)


def palm_mute():
    rng = Lcg(2)
    seconds = 8.0
    out = [0.0] * int(seconds * RATE)
    step = 60.0 / 140.0 / 2.0  # eighth notes at 140 bpm
    pattern = [0, 0, 0, 3, 0, 0, 5, 0]
    time = 0.0
    index = 0

    while time < seconds - 0.3:
        note = pattern[index % len(pattern)]
        accent = 1.0 if index % 4 == 0 else 0.8
        # The root and its fifth, as a muted power chord.
        mix_into(out, pluck(fret(E2, note), 0.3, 0.85, rng, pick=0.08), time, accent)
        mix_into(out, pluck(fret(A2, note + 2), 0.3, 0.85, rng, pick=0.08), time + 0.004, accent * 0.8)
        time += step
        index += 1

    return pickup(out)


def lead():
    rng = Lcg(3)
    seconds = 8.0
    out = [0.0] * int(seconds * RATE)
    # (string, fret, start, length)
    notes = [
        (G3, 9, 0.0, 0.5), (G3, 11, 0.5, 0.5), (B3, 9, 1.0, 0.5), (B3, 12, 1.5, 1.4),
        (E4, 9, 3.0, 0.4), (E4, 12, 3.4, 0.4), (E4, 14, 3.8, 1.6),
        (B3, 12, 5.5, 0.35), (B3, 10, 5.85, 0.35), (G3, 11, 6.2, 0.35), (G3, 9, 6.55, 1.4),
    ]

    for string, n, start, length in notes:
        mix_into(out, pluck(fret(string, n), length, 0.1, rng, vibrato=1.0 if length > 1.0 else 0.0), start)

    return pickup(out)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)

    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)

    for name, maker in (("clean-chords", clean_chords), ("palm-mute", palm_mute), ("lead", lead)):
        write(directory / f"{name}.wav", maker())
        print(f"wrote {directory / name}.wav")


if __name__ == "__main__":
    main()
