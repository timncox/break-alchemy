# Break — design notes

## Shape

Break is **smack-alchemy with a different source**. Smack records live input
into a 150 s ring and loops a captured window; Break fills that same ring
from the SD card and never records. Everything Smack does to a loop — equal
slices, a seeded roll of per-slice effects, re-ordering, punch, varispeed to
the clock — Break does to a break.

```
SD card ──card_loader (control thread, ~6 ms/frame)──► ring slot A | slot B  (SDRAM)
                                                          │ brk_commit (audio thread)
J1 clock ─► clock_adapter ─► MIDI ticks ─► smack_core ◄───┘
                                               │  post-roll hook: rune mask + Amen order
                                               ▼
                               volume/saturation ─► DJ filter ─► J9/J10
                               cell changes ─► J8 trig out · ticks ─► J7 clock out
```

## Files

| File | |
|---|---|
| `src/ecto_card.c/.h` | clean-room card reader: `.info`, WAV (RIFF chunks walked), grid, grimoire, Amen. Pure C, native tests. |
| `src/break_engine.c/.h` | the one TU that compiles `vendor/smack_core.c` (SDRAM calloc redirect, as smack_core_alchemy.c did); slots, commit, post-roll hook, read-backs |
| `src/card_loader.cpp/.h` | FatFS: find banks, stream a sample into the idle slot (AXI staging + SdDmaStagingPad, mono → stereo) |
| `src/break_alchemy.cpp` | the app: pages, buttons, jacks, LEDs, autosave |
| `src/vendor/smack_core.*` | the engine, from smack-alchemy, with **two** marked `BREAK-ALCHEMY PATCH` edits |

## The three engine edits

1. `rec_hold` — `record_ring_frame` returns at once. The recorder normally
   runs forever and only avoids the *playing* loop; it would overwrite the
   idle slot while the next sample loads.
2. `smack_post_roll_hook` — called at the end of every `roll_pattern`. The
   roll can come from anywhere (reroll, a density or seed change, a commit),
   so restricting effects to a rune or imposing the Amen order outside the
   engine would be undone by the next internal roll.

3. Keep pitch (`stretch`, `stretch_inc`) — in `render_lane`, the read
   position inside a slice moves at the source's own rate instead of the
   clock-scaled one, while the grid and its edge fades stay in clock time.
   Faster than the break: each slice is cut at the next grid line. Slower:
   it runs out and fades to a gap. Settings P6 picks it.

Upstream (`~/tim-os/smack`) doesn't have them; if Smack ever wants a "play a
file" mode they are the whole change.

## Two slots

The ring is split in half (75 s each at 48 k; samples stay at their own
44.1 k rate, which the engine's varispeed absorbs — inc = loop frames per
tick / clock frames per tick, so a 44.1 k break at its own BPM plays at
0.919×, i.e. correctly). The control thread loads into the slot that is not
playing, then sets `G_COMMIT_PENDING`; the audio callback calls `brk_commit`,
which only sets loop fields and re-rolls — no copying in the interrupt. The
playhead keeps its place in the bar across a swap (phase in clock ticks,
wrapped onto the new length).

Loops are cut to whole beats (≤ 64, the engine's longest grid) and, if a
sample is longer than a slot, halved until they fit.

## Ectocore ideas → Smack mechanics (approximations, on purpose)

| Ectocore | Here |
|---|---|
| arbitrary slice points | the engine's equal grid (1/2/4/8 half-steps), the count nearest the card's. Card slices → grid cells by nearest boundary, kept as kick/snare/transient flags |
| Break: per-beat probability, effects run N beats | `fx_density` = knob², per slice per roll; Evolve re-rolls each pass so effects come and go |
| 16 grimoire effects | each mapped to 1–2 smack effects (`ecto_card.c`, `k_ecto_to_smack`); pitch up/down both → PITCH/PSHIFT (sign rolled) |
| missing rune → randomised | missing rune → our fixed built-ins (`ecto_default_runes`) |
| Amen: per-beat Turing machine | an order over the grid, baked at roll time (same knob zones; jump left carries, right snaps back) |
| trig out at the transient's frame | at the start of the grid cell containing it (≤ one cell late/early) |
| clock in 2 ppqn | J1 with Clock Ratio x2 (default). The ratio no longer scales free-run (`free_ignores_ratio`) |

Not done: Amen/Break/Sample CV *modes* (jump/repeat/split, 1 V/oct sample),
MIDI, `S.2/4/6.wav` saturation variants, one-shot/play-mode semantics
(everything loops), tempo_match off, the settings flag files other than the
grimoire, SD streaming for samples longer than a slot.

## Verified / not

- Native (`make test`): parser incl. exact-length check and RIFF walk, grid
  flags, Amen orders, rune masks vs `smack_fx_t`, the engine playing a loaded
  break with the recorder held, rune-only effects, slot swap keeping the bar,
  stop/play, free-run tempo with ratio x2. The emu card is parsed too.
- Emulator (`emu-tests/`): boot → load → audio; J7 eighths at the break's
  tempo; J8 kick trigs; stop/play; sample + bank switching (proved by tempo);
  J1 lock at 120 and 240; BREAK/rune/Amen extremes stay finite and unclipped.
- Upstream vectors: the `.info` layouts schollz/_core's own reader test builds
  (test/bank_metadata/test_metadata.c @ 12d36c7) parse identically here.
- **Not verified:** a real ectocore.rocks export (the test card is generated
  from the spec by the same hands that wrote the reader); hardware (SD timing,
  CPU with heavy effects, the STM DAC written from the audio callback).
