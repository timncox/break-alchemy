#!/usr/bin/env python3
"""
Turn your own drum breaks into a Break card -- no ectocore.rocks needed.

    python3 tools/make_card.py ~/Breaks /Volumes/CARD
    python3 tools/make_card.py amen.wav think.aif --bank 2 --out /Volumes/CARD

Any format ffmpeg reads (wav, aif, mp3, flac, m4a). For each file:

  tempo    from the name if it has one ("amen 136bpm.wav", "think_115.wav"),
           else worked out from the length: the beat count (4, 8, 16, 32,
           64) that puts it nearest 130 BPM inside 80-180. --bpm overrides.
  length   trimmed or padded to exactly that many beats, so it loops on the grid
  slices   eighth notes (a 4-bar break = 32 slices)
  hits     kick / snare / other onsets found by a simple low-vs-high energy
           test -- a guess, good enough to drive Trig Out; check by ear
  level    peaks normalised to -6 dBFS, 44.1 kHz 16-bit, mono or stereo kept

Writes bankN/S.0.wav + S.0.wav.info in the ectocore.rocks layout (see
docs/ectocore-sd-format.md). Files fill slots 0..15 of --bank, then the next
bank. Existing slots are overwritten only with --force.

Stdlib only, plus the ffmpeg binary.
"""
import argparse
import array
import math
import os
import re
import struct
import subprocess
import sys

RATE = 44100
AUDIO_EXT = {'.wav', '.aif', '.aiff', '.mp3', '.flac', '.m4a', '.ogg'}


def decode(path):
    """-> (array('h') interleaved, channels)"""
    probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'a:0', '-show_entries',
                            'stream=channels', '-of', 'csv=p=0', path], capture_output=True, text=True)
    ch = 2 if probe.stdout.strip() not in ('1',) else 1
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 's16le', '-acodec', 'pcm_s16le',
                          '-ac', str(ch), '-ar', str(RATE), '-'], capture_output=True, check=True).stdout
    a = array.array('h')
    a.frombytes(raw[: len(raw) // (2 * ch) * 2 * ch])
    if sys.byteorder != 'little':
        a.byteswap()
    return a, ch


def bpm_from_name(name):
    m = re.search(r'(\d{2,3}(?:\.\d+)?)\s*bpm', name, re.I) or re.search(r'(?:^|[ _\-])(\d{2,3})(?:[ _\-.]|$)', name)
    if m:
        b = float(m.group(1))
        if 60 <= b <= 200:
            return b
    return None


def bpm_from_length(frames):
    secs = frames / RATE
    best = None
    for beats in (4, 8, 16, 32, 64):
        b = 60.0 * beats / secs
        if 80 <= b <= 180 and (best is None or abs(b - 130) < abs(best[0] - 130)):
            best = (b, beats)
    return best[0] if best else 120.0


def to_beats(frames, bpm):
    return max(1, min(64, round(frames / RATE * bpm / 60.0)))


def onsets(mono):
    """-> list of (frame, kind) kind 0 kick, 1 snare, 2 other. A heuristic:
    kicks are jumps in low-band (<150 Hz) energy smoothed over ~23 ms, so a
    decaying kick is one hit; snares and hats are jumps in high-band energy,
    the loud ones snares."""
    hop = 256
    k = 1 - math.exp(-2 * math.pi * 150 / RATE)
    lp = prev = el = eh = 0.0
    lows, highs = [], []
    for i, x in enumerate(mono):
        lp += k * (x - lp)
        hp = x - prev
        prev = x
        el += lp * lp
        eh += hp * hp
        if (i + 1) % hop == 0:
            lows.append(el / hop); highs.append(eh / hop); el = eh = 0.0
    n = len(lows)
    if n < 8:
        return []
    sl = [sum(lows[max(0, i - 3): i + 1]) / 4 for i in range(n)]
    lL = [math.log10(1e-9 + v) for v in sl]
    lH = [math.log10(1e-9 + v) for v in highs]
    maxL, maxH = max(sl), max(highs)
    out = []
    lastk = lasth = -10 ** 9
    for i in range(n):
        base = min(lL[max(0, i - 6): i]) if i else -9.0
        nxt = lL[i + 1] - (min(lL[max(0, i - 5): i + 1])) if i + 1 < n else 0
        if (lL[i] - base > 0.6 and sl[i] > 0.3 * maxL and not nxt > lL[i] - base
                and (i - lastk) * hop > RATE * 0.1):
            out.append((max(0, i - 3) * hop, 0)); lastk = i
        hb = min(lH[max(0, i - 4): i]) if i else -9.0
        if lH[i] - hb > 0.6 and highs[i] > 0.01 * maxH and (i - lasth) * hop > RATE * 0.06:
            if i + 1 < n and lH[i + 1] > lH[i]:
                continue                                   # still rising: take the peak hop
            peak = max(highs[i: i + 3])
            out.append((i * hop, 1 if peak > 0.35 * maxH else 2)); lasth = i
    # a snare's body has low end too: a "kick" within ~35 ms of a snare is the snare
    # ...unless the low end is strong enough to be a kick under a cymbal
    snares = [f for f, kd in out if kd == 1]
    def lowlevel(f):
        i = min(n - 1, f // hop + 3)
        return max(sl[i: i + 4])
    out = [(f, kd) for f, kd in out
           if not (kd == 0 and lowlevel(f) < 0.6 * maxL
                   and any(abs(f - sf) < RATE * 0.035 for sf in snares))]
    out.sort()
    return out


def write_slot(bank_dir, slot, pcm, ch, bpm, beats, hits):
    frames = len(pcm) // ch
    pad = RATE // 2
    body = pcm.tobytes() if sys.byteorder == 'little' else _swapped(pcm)
    fb = 2 * ch
    loops = body * (pad // frames + 2)                       # cycle short breaks to fill the pads
    padded = loops[len(loops) - pad * fb:] + body + loops[: pad * fb]
    os.makedirs(bank_dir, exist_ok=True)
    wav = os.path.join(bank_dir, '%d.0.wav' % slot)
    fmt = struct.pack('<HHIIHH', 1, ch, RATE, RATE * fb, fb, 16)
    with open(wav, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(padded)) + b'WAVE' + b'fmt ' + struct.pack('<I', 16) + fmt
                + b'data' + struct.pack('<I', len(padded)) + padded)
    size = len(body)
    n = min(255, beats * 2)
    step = (size // n) & ~3
    starts = [step * i for i in range(n)]
    stops = [step * (i + 1) for i in range(n)]
    types = [0] * n
    for fr, kind in hits:
        idx = min(n - 1, int(fr * n / frames))
        if kind == 0: types[idx] |= 1
        if kind == 1: types[idx] |= 2
    lanes = [[fr // 16 for fr, k in hits if k == kind and fr // 16 < 65536][:16] for kind in (0, 1, 2)]
    flags = int(round(bpm)) | (1 << 13) | ((1 if ch == 2 else 0) << 15) | (1 << 16)
    b = struct.pack('<IIHB', size, flags, 96, n)
    b += struct.pack('<%di' % n, *starts) + struct.pack('<%di' % n, *stops) + struct.pack('<%db' % n, *types)
    b += struct.pack('<HHH', *[len(l) for l in lanes])
    for l in lanes:
        b += struct.pack('<%dH' % len(l), *l)
    with open(wav + '.info', 'wb') as f:
        f.write(b)
    return wav


def _swapped(a):
    c = array.array('h', a); c.byteswap(); return c.tobytes()


def convert(path, bpm_override):
    pcm, ch = decode(path)
    frames = len(pcm) // ch
    if frames < RATE // 4:
        raise ValueError('shorter than 0.25 s')
    name = os.path.splitext(os.path.basename(path))[0]
    bpm = bpm_override or bpm_from_name(name) or bpm_from_length(frames)
    beats = to_beats(frames, bpm)
    want = int(round(beats * 60.0 / bpm * RATE))
    if frames >= want:
        pcm = pcm[: want * ch]
    else:
        pcm.extend([0] * ((want - frames) * ch))
    peak = max(1, max(abs(v) for v in pcm))
    g = 0.5 * 32767 / peak                                # -6 dBFS
    pcm = array.array('h', (max(-32768, min(32767, int(v * g))) for v in pcm))
    mono = [(pcm[2 * i] + pcm[2 * i + 1]) / 65536.0 for i in range(want)] if ch == 2 else [v / 32768.0 for v in pcm]
    return pcm, ch, bpm, beats, onsets(mono)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+', help='audio files or folders; with no --out the LAST one is the card')
    ap.add_argument('--out', help='the card (its root)')
    ap.add_argument('--bank', type=int, default=1, help='first bank to fill (1-16)')
    ap.add_argument('--bpm', type=float, help='force one tempo for every file')
    ap.add_argument('--force', action='store_true', help='overwrite slots that already hold a break')
    a = ap.parse_args()
    ins = a.inputs
    out = a.out
    if not out:
        if len(ins) < 2:
            ap.error('give the card as the last argument or with --out')
        out = ins[-1]; ins = ins[:-1]
    files = []
    for p in ins:
        if os.path.isdir(p):
            for root, _, fs in sorted(os.walk(p)):
                files += [os.path.join(root, f) for f in sorted(fs) if os.path.splitext(f)[1].lower() in AUDIO_EXT]
        else:
            files.append(p)
    bank, slot = a.bank, 0
    for f in files:
        while True:
            if bank > 16:
                print('card full (16 banks x 16)'); return
            target = os.path.join(out, 'bank%d' % bank, '%d.0.wav.info' % slot)
            if a.force or not os.path.exists(target):
                break
            slot += 1
            if slot == 16: bank, slot = bank + 1, 0
        try:
            pcm, ch, bpm, beats, hits = convert(f, a.bpm)
        except Exception as e:
            print('skip %s: %s' % (os.path.basename(f), e)); continue
        write_slot(os.path.join(out, 'bank%d' % bank), slot, pcm, ch, bpm, beats, hits)
        kinds = [sum(1 for _, k in hits if k == x) for x in (0, 1)]
        print('bank%-2d slot %-2d  %-32.32s %5.1f BPM  %2d beats  %s  %d kicks %d snares'
              % (bank, slot, os.path.basename(f), bpm, beats, 'stereo' if ch == 2 else 'mono  ', *kinds))
        slot += 1
        if slot == 16: bank, slot = bank + 1, 0


if __name__ == '__main__':
    main()
