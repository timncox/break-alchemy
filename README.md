# Break — Ectocore-style break mangling for the Alchemy Lab

Firmware for the [Hermetic Modular Alchemy Lab V2](https://hermeticmodular.com/modules/alchemy-lab).
It plays sliced breaks from an SD card prepared on **ectocore.rocks**
(16 banks × 16 samples, with slices, kick/snare marks and a grimoire) and
mangles them through **Smack**'s seeded per-slice glitch engine.

It is *inspired by* the infinitedigits Ectocore and reads its card format.
It contains none of its code: `schollz/_core` is GPL-3.0, and this is a
clean-room implementation from a format description
([docs/ectocore-sd-format.md](docs/ectocore-sd-format.md)). MIT.

## Panel

```
PLAY                         SHIFT (hold B3)
[P1 BREAK]   B1 [P2 AMEN]    [VOLUME]      [SEED]
[P3 SAMPLE]  B2 [P4 RANDOM]  [BANK]        [PITCH RANGE]
[P5 GRIMOIRE]B3 [P6 DJ FILT] [CLOCK RATIO] [TEMPO]
```

| Control | Does |
|---|---|
| BREAK | how many slices carry an effect (squared curve) |
| AMEN | CCW: straight · middle: loop a random sequence of 1–32 slices · full CW: random 3-slice phrases |
| AMEN RANDOM | centre: off · left: jump, and carry on from there · right: jump, then snap back to the grid |
| SAMPLE / BANK | pick among the samples / banks actually on the card |
| GRIMOIRE | rune 1–7: which effects BREAK can use (from the card's `settings/grimoire`, else built-ins) |
| DJ FILTER | LP left, HP right, open at centre |
| VOLUME | 100 % at noon, then soft saturation |
| CLOCK RATIO | what a J1 pulse is worth: /2 half note, =1 quarter, **x2 eighth (2 ppqn, default)** |
| B1 | tap: re-roll · hold: stop / play |
| B2 | tap: tap tempo · hold: punch (every slice retriggers) |
| Settings (B2+B3) | P1 brightness · **P2 Trig Out** (kick / snare / transient / random) · P3-P4 presets · **P5 Evolve** (fresh pattern each pass) · page 2 = SD firmware picker |

Jacks: **J1** clock in · **J2** reset · **J3** AMEN cv · **J4** BREAK cv · **J5** SAMPLE cv ·
**J6** AMEN RANDOM cv · **J7** clock out (eighths) · **J8** trig out · **J9/J10** audio out.

LEDs: B1 green playing · amber loading · red no card / load failed · grey stopped ·
white punching. B2 blue = locked to J1 · grey free-running (white flash per beat) · red = CPU > 80 %.

## The card

Export from ectocore.rocks and copy the folder's contents (or the folder
itself) to a FAT32 card: `bank1/0.0.wav` + `bank1/0.0.wav.info`, … and
`settings/grimoire/runeN/effectE-on`. `tools/make_test_card.py <dir>` writes
a small synthetic card in the same layout.

## Build, test, flash

```
make libdaisy            # once
make                     # build/break_alchemy.bin
make test                # native: card reader, Amen/runes, engine with a loaded break
make program-live        # flash over HostLink
```

Emulator (alchemy-lab, branch `emu-custom`): see `emu.mk`.
`make -C <alchemy-lab>/emu test-custom FW_DIR=$PWD` runs `emu-tests/*.emu` against `emu-card/`.

Home preset slot **4**.
