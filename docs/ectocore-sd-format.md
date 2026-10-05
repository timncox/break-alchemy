# Ectocore SD-card format (as exported by ectocore.rocks) — clean-room spec

Source read: github.com/schollz/_core @ 12d36c7 (2026-10-05, shallow clone). Exporter = Go server `core/src/pack/pack.go`, `core/src/pack/names.go`, `core/src/zeptocore/{zeptocore,zeptocorebinary}.go`. Reader = firmware `lib/bank_metadata.c`, `lib/sdcard_startup.h`, `lib/audio_callback.h`, `lib/break_knob.h`, `lib/ectocore.h`, `main.c`, `lib/globals.h`. Everything below is facts observed in that source unless marked "I'd guess".

## 1. Layout

The site delivers a ZIP whose single top-level folder (the pack name) contains everything; the user copies the folder's *contents* to the card root.

```
/bank1 .. /bank16/          (only banks with ≥1 file are created)
    S.0.wav                 primary audio, S = slot 0..15 (decimal, no zero pad)
    S.0.wav.info            binary metadata for that slot (required)
    S.name.json             UTF-8 JSON sidecar: {schema:1, name, originalFilename, audioSha256}; firmware ignores it
    S.2.wav, S.4.wav, S.6.wav   optional "normal-speed alternative" variants (same length/format as S.0)
/settings/                  global settings (firmware also scans card root first, for backwards compat)
    start_tempo, midi_channel, sample_cv_mapping   tiny text files
    <flag files>            zero-byte files whose NAME is the value (see §5)
    grimoire/rune1..rune7/effect1..effect16-{on|off}
```
Firmware-written (not from the exporter; safe to ignore or own): `save.bin`/`savefileN` (SaveFile struct state), `.core_seek/` (FAT cluster-chain seek-map cache — an optimisation for RP2040 seeking, purely derived, never required).

Filename/slot rules in firmware: probes `bank{b+1}/{slot}.0.wav.info` for slot 0..15; a slot is valid only if the .info parses AND `S.0.wav` exists with `fsize ≥ info.size + 44`. Gaps allowed (slots 0 and 2 → two selectable samples, physical ids kept). Max 16 banks × 16 samples.

## 2. Audio files (`S.0.wav`)

- Canonical RIFF/WAVE, PCM signed 16-bit little-endian, interleaved if stereo. Firmware **assumes a 44-byte header** (`WAV_HEADER 44`) and does not parse chunks — just seeks.
- Sample rate: 44 100 Hz × oversampling (exporter verifies `rate == 44100*oversampling`, 16-bit, channels = 1 or 2). Ectocore UI default oversampling = 1x → 44.1 kHz. Firmware engine runs at `SAMPLE_RATE 44100`.
- Channels: mono or stereo per file, taken from the source (the `stereoMono` field in the export JSON is not applied in `pack.go`). Flag stored in .info.
- Level: sox `norm gain -6` (normalised to −6 dBFS).
- **Wrap padding:** audio = [last 0.5 s of source] + [full source] + [first 0.5 s of source]. So playable data starts at byte `44 + 22050·oversampling·channels·2` (firmware literally uses `44 + (num_channels+1)·(oversampling+1)·44100`). All slice/phase offsets are relative to that point. The pads let interpolation/crossfades read past either end without wrapping logic.
- Variants `S.2/4/6.wav`: firmware probes `S.{2,4,..14}.wav` for the initially selected sample; if present, the saturate fx param picks a variant (`variant*2` in the filename). Same seek offsets via size ratio. Optional; the minimal reader can ignore them.

## 3. `S.0.wav.info` binary (little-endian, packed, no alignment)

| off | size | field |
|---|---|---|
| 0 | u32 | `size` = PCM byte count EXCLUDING the 44-byte header and both 0.5 s pads |
| 4 | u32 | `flags` bitfield (below) |
| 8 | u16 | `splice_info`: bits 0–14 `splice_trigger`, bit 15 `splice_variable` |
| 10 | u8 | `slice_num` N (1..255) |
| 11 | i32 × N | `slice_start[]` — byte offsets into PCM region, rounded then `/4*4` (4-byte aligned) |
| 11+4N | i32 × N | `slice_stop[]` — same units |
| 11+8N | i8 × N | `slice_type[]`: 0 none, 1 kick, 2 snare, 3 kick+snare |
| 11+9N | u16 × 3 | (version ≥1) counts c0,c1,c2 for transient lanes kick / snare / other(hh) |
| 17+9N | u16 × (c0+c1+c2) | transient positions, lane 0 then 1 then 2 |

`flags`: bits 0–8 `bpm` (firmware clamps to 30..300), 9–11 `play_mode`, 12 `one_shot`, 13 `tempo_match`, 14 `oversampling` (0=1x, 1=2x), 15 `num_channels` (0 mono, 1 stereo), 16–22 `version` (0 or 1; exporter writes 1), 23–31 reserved.

Exact length check: v0 file = `11 + 9N` bytes; v1 = `17 + 9N + 2·(c0+c1+c2)`. Firmware rejects: size 0 or > INT32_MAX, N=0, version >1, `splice_trigger < 2`, `play_mode > 4`, any start/stop > size, stop ≤ start. Retains only the first 16 transients per lane (consumes the rest). Exporter writes only non-zero transients.

- **Transient units:** `u16 = source_frame_index / 16` (frame at 44.1 kHz, relative to start of the un-padded audio; values >65535 after division are written as 0 → only the first ~23.8 s can carry transients). Firmware compares `transient*16` against `phase_bytes / 2 / channels + 440` (440-frame look-ahead ≈10 ms), firing Trig Out when the playhead crosses it (jump < 881 frames). Note frame units assume 1x oversampling.
- **play_mode:** 0 play-through (ignore boundaries), 1 stop at end of slice, 2 loop slice, 3 stop at end of sample, 4 loop sample. (A stale comment in zeptocore.go lists an older 0–3 numbering; the UI and firmware use 0–4.)
- **splice_trigger:** period in BPM-timer ticks; the timer runs at 192 ticks per quarter note. Default 96 = advance one slice every eighth note; exporter computes `round(192 / slicesPerBeat)`. A new slice is triggered when `tick % splice_trigger == 0`. Clock-out "sync to slice" mode also derives from it.
- **splice_variable:** when 1, slices advance sequentially rather than locking to `beat_total % N` (the stay-in-sync path is skipped).
- **tempo_match:** (I'd guess, not traced in firmware) playback rate follows global tempo relative to `bpm`; **one_shot:** single-shot (exporter sets play_mode=1, tempo_match=0, bpm=120 for oneshot drops).
- Slice auto-generation if no markers: `beats*2` equal slices (≥2 s files) or one onset-detected slice.

## 4. Grimoire (GLOBAL, not per bank)

7 runes × 16 booleans from `settings/grimoire/runeR/effectE-on|off` (R 1..7, E 1..16). Missing rune folder → firmware randomises that rune. Rune selected by the Break attenuator knob (`val*7/1024`). Website names the runes amalgam, alum, tree, azurite, hematite, sulphur, brimstone.

Effect index (E-1) is the *break-fx* index, not the internal FX enum. Per effect: params randomised on activation; duration and refractory in beats (random in [min,max]); probability scaling.

| E | name | activation params (firmware) | dur beats | refractory | prob% |
|---|---|---|---|---|---|
|1|distortion|fuzz on (drive set by break/volume knob)|2–4|4–16|50|
|2|loss|waveshaper p0,p1 rand 0–255|2–4|4–16|50|
|3|bitcrush|p0 rand 220–255, p1 rand 210–255|2–4|4–16|50|
|4|filter|cutoff rand 0–128, res rand 0–64|4–8|4–16|50|
|5|time stretch|on/off|4–32|16–32|50|
|6|delay|tempo-synced (30·44100/bpm, maybe ×2 faster, halved ≤10000 samples), feedback rand 1–4 (2 if long)|4–96|4–16|80|
|7|comb|p0 rand 0–120, p1 rand 0–20|2–6|4–16|50|
|8|beat repeat|length 30·44100/bpm samples, randomly ×1.5/×0.75/×2|1–4|4–8|40|
|9|reverb|room/damp/wet from fx params|8–36|8–32|40|
|10|autopan|LFO period 48×{2,4,8} ticks|4–8|2–6|50|
|11|pitch down|repitch, only if 120<bpm<180|8–32|8–32|40|
|12|pitch up|same, upward|8–32|8–32|30|
|13|reverse|on/off|1–8|1–8|90|
|14|retrigger|random divider {8,6,4,4,4,3,2,…,1}, optional vol ramp|4–8|4–8|50|
|15|retrigger + pitch|as 14 with pitch changes|4–12|8–16|30|
|16|tape stop|p0,p1 rand 0–128|8–16|32–64|10|

Special case: a rune with ONLY time-stretch on turns the Break knob into a direct real-time stretch control (0..4095) instead of a probability.

## 5. Global settings files (in `settings/`)

Text: `start_tempo` ("default" or integer 30–300 + "\n"), `midi_channel` ("1".."16"), `sample_cv_mapping` ("bank" or "1voct").
Zero-byte flags: `clock_stop_sync-on|off`, `clock_output_trig-on|off` (trig 50 ms vs gate), `clock_behavior_sync_slice-on|off`, `override_with_reset-none|sample|break|amen|clk`, `amen_cv-unipolar|bipolar`, `amen_behavior-jump|repeat|split`, `break_cv-unipolar|bipolar`, `sample_cv-unipolar|bipolar`, `knobx_select_sample-on|off`, `mash_mode_momentary-on|off`, `brightness-N` (0–100), optional `negativelatency-N`.

## 6. Knob semantics (Ectocore build)

- **Break knob:** setpoint v (0–1023) cubed-scaled `s = v³/1024²`. On each beat, per enabled-in-rune effect not active and not in refractory: activate if `rand(0..200) < s·prob%/1024`. s<15 forces all off. Active effects count down their beat duration, then enter refractory. Effects disabled in the current rune are turned off immediately. (TAP+Break = volume → saturation → fuzz.)
- **Amen knob** (no CV): v<57 → normal sequential; 57–965 → "Turing" sequence length from {1,2,3,4,5,6,7,8,9,10,12,14,16,24,32}; ≥966 → regenerate a random 64-entry slice array (with 1–50% chance of runs of 3 consecutive slices) and use length 8 with retrigger at phrase end. Playing slice = `seq[beat % len] % slice_num`. (TAP+Amen = DJ LP/HP filter.)
- **Amen-random (attenuator):** dead zone 512±24. Left: `stay_in_sync=false`, jump probability 0–100%. Right: `stay_in_sync=true`, jump probability 0–~90% (formula divides by 536, so it tops out near 90). Each beat, with that probability jump to a random slice. When `stay_in_sync` is true and splice_variable is off, the next beat re-derives position from `beat_total % slice_num` (i.e. "returns" to the grid); when false, it continues sequentially from wherever it jumped.
- **Amen CV:** knob/attenuator set a slice range [start,stop]; CV maps linearly across it. jump = only on CV change (>3), repeat = continuously, split = negative/positive halves map differently. Attenuator < noon adds random offset to CV.

## 7. Compatibility hazards

- No magic number/header in .info; only the 7-bit version field (0/1). Validate by exact length formula.
- 44-byte header assumption: if a user re-saves WAVs in another tool, extra chunks break the offset. Parse RIFF chunks properly in your reader and compute the data start; then add the 0.5 s pad.
- Oversampling field is 1 bit but the exporter allows "4x" (would be clamped to 2x flag while audio is 176.4 k → mismatch). Treat as 1x/2x only; I'd guess Ectocore users nearly always export 1x.
- Transients only within first ~23.8 s; zero means "unused".
- Seek maps (`.core_seek`) and `save.bin` are firmware-private and derived — not needed.
- Variants and `.name.json` are optional.
- The exporter's "native sample manager" and future firmware may change formats; the newer pack README says new packs "require firmware with granular-only Time Stretch support (up to 16×)" — this is a playback-engine requirement, not a file-format change.

## Verdict: FEASIBLE

Minimal subset for a Daisy reader: enumerate `bank1..16/`, for slot 0..15 read `S.0.wav.info` (11-byte header + 9N slice block + optional transient block, LE), open `S.0.wav`, locate PCM data, skip 0.5 s pad, treat slice offsets as byte offsets in 16-bit interleaved PCM at 44.1 kHz. Add `settings/grimoire/*` (7×16 bools) and the flag-file settings for full behaviour. With 64 MB SDRAM you can usually load a whole bank into RAM (≈6 min of 44.1 k stereo per 64 MB; samples have no length limit, so stream from SD as a fallback), which sidesteps the RP2040's seek-map machinery entirely.
