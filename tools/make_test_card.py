#!/usr/bin/env python3
"""
Write a small ectocore.rocks-layout card for the emulator and for checking a
real card by hand: synthetic drum breaks (kick / snare / hats), padded and
described exactly as the format spec says (docs/ectocore-sd-format.md).

    python3 tools/make_test_card.py emu-card

  bank1/0.0.wav  120 BPM, 4 beats, stereo, 16 slices, v1 .info with transients
  bank1/1.0.wav  170 BPM, 8 beats, mono,   32 slices, v0 .info
  bank1/2.0.wav  a WAV with a LIST chunk before data (the 44-byte trap)
  bank2/0.0.wav  100 BPM, 4 beats, stereo, 8 slices
  settings/grimoire/rune1/effect14-on   rune 1 = retrigger only
"""
import math
import os
import random
import struct
import sys

RATE = 44100


def drums(bpm, beats, seed):
    rnd = random.Random(seed)
    n = int(round(beats * 60.0 / bpm * RATE))
    beat = 60.0 / bpm * RATE
    out = [0.0] * n
    kicks, snares = [], []
    for b in range(beats):
        t0 = int(b * beat)
        if b % 2 == 0:
            kicks.append(t0)
            for i in range(min(int(0.25 * RATE), n - t0)):
                f = 50 + 120 * math.exp(-i / 1500.0)
                out[t0 + i] += 0.9 * math.exp(-i / 6000.0) * math.sin(2 * math.pi * f * i / RATE)
        else:
            snares.append(t0)
            for i in range(min(int(0.18 * RATE), n - t0)):
                out[t0 + i] += 0.6 * math.exp(-i / 3000.0) * (rnd.random() * 2 - 1)
        for h in (0, 0.5):                 # eighth-note hats
            t = int(t0 + h * beat)
            for i in range(min(int(0.03 * RATE), n - t)):
                out[t + i] += 0.2 * math.exp(-i / 300.0) * (rnd.random() * 2 - 1)
    return out, kicks, snares


def pcm(samples, channels):
    pad = int(0.5 * RATE)
    # the format: last 0.5 s, the music, first 0.5 s
    padded = samples[-pad:] + samples + samples[:pad]
    b = bytearray()
    for v in padded:
        s = max(-32768, min(32767, int(v * 16000)))
        b += struct.pack('<h', s) * channels
    return bytes(b), len(samples) * 2 * channels


def wav(path, data, channels, list_chunk=False):
    fmt = struct.pack('<HHIIHH', 1, channels, RATE, RATE * 2 * channels, 2 * channels, 16)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', 16) + fmt
    if list_chunk:
        info = b'INFOISFT\x05\x00\x00\x00test\x00\x00'
        body += b'LIST' + struct.pack('<I', len(info)) + info
    body += b'data' + struct.pack('<I', len(data)) + data
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)


def info(path, size, bpm, channels, n_slices, kicks, snares, version):
    flags = bpm | (0 << 9) | (1 << 13) | ((1 if channels == 2 else 0) << 15) | (version << 16)
    b = struct.pack('<IIHB', size, flags, 96, n_slices)
    step = (size // n_slices) & ~3
    starts = [step * i for i in range(n_slices)]
    stops = [step * (i + 1) for i in range(n_slices)]
    frames = size // 2 // channels
    types = []
    for i in range(n_slices):
        a, z = starts[i] // 2 // channels, stops[i] // 2 // channels
        t = 0
        if any(a <= k < z for k in kicks): t |= 1
        if any(a <= s < z for s in snares): t |= 2
        types.append(t)
    b += struct.pack('<%di' % n_slices, *starts)
    b += struct.pack('<%di' % n_slices, *stops)
    b += struct.pack('<%db' % n_slices, *types)
    if version == 1:
        kt = [k // 16 for k in kicks if k < frames][:16]
        st = [s // 16 for s in snares if s < frames][:16]
        b += struct.pack('<HHH', len(kt), len(st), 0)
        b += struct.pack('<%dH' % len(kt), *kt) + struct.pack('<%dH' % len(st), *st)
    with open(path, 'wb') as f:
        f.write(b)


def sample(dirp, slot, bpm, beats, channels, n_slices, version, seed, list_chunk=False):
    os.makedirs(dirp, exist_ok=True)
    s, k, sn = drums(bpm, beats, seed)
    data, size = pcm(s, channels)
    p = os.path.join(dirp, '%d.0.wav' % slot)
    wav(p, data, channels, list_chunk)
    info(p + '.info', size, bpm, channels, n_slices, k, sn, version)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else 'emu-card'
    sample(os.path.join(root, 'bank1'), 0, 120, 4, 2, 16, 1, 1)
    sample(os.path.join(root, 'bank1'), 1, 170, 8, 1, 32, 0, 2)
    sample(os.path.join(root, 'bank1'), 2, 120, 4, 2, 16, 0, 3, list_chunk=True)
    sample(os.path.join(root, 'bank2'), 0, 100, 4, 2, 8, 0, 4)
    g = os.path.join(root, 'settings', 'grimoire', 'rune1')
    os.makedirs(g, exist_ok=True)
    open(os.path.join(g, 'effect14-on'), 'w').close()
    print('wrote', root)


if __name__ == '__main__':
    main()
