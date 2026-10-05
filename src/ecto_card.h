/*
 * ecto_card — the parts of an ectocore.rocks SD card that break-alchemy
 * reads, as pure C (no FatFS, no libDaisy) so it is tested natively.
 *
 * This is a CLEAN-ROOM reader. The card format was described from the
 * exporter and firmware of schollz/_core (GPL-3.0) as facts -- layouts,
 * field order, units -- and no code was copied (docs/ectocore-sd-format.md
 * has the spec this file implements). Keep it that way: break-alchemy is
 * MIT, and a pasted function would make it GPL.
 *
 * Card layout (what is read):
 *   /bank1 .. /bank16/       (or /<one folder>/bankN/ -- the loader looks)
 *     S.0.wav                S = 0..15, 16-bit PCM, 44.1 kHz x oversampling,
 *                            padded with 0.5 s before and after the music
 *     S.0.wav.info           binary metadata, parsed here
 *   /settings/grimoire/runeR/effectE-on|off      R 1..7, E 1..16
 */
#ifndef ECTO_CARD_H
#define ECTO_CARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECTO_BANKS          16
#define ECTO_SLOTS          16
#define ECTO_MAX_SLICES     255
#define ECTO_MAX_TRANSIENTS 16      /* per lane, as the original keeps */
#define ECTO_RUNES          7
#define ECTO_EFFECTS        16

/* play_mode values in the flags word */
enum { ECTO_PLAY_THROUGH = 0, ECTO_PLAY_SLICE_STOP, ECTO_PLAY_SLICE_LOOP,
       ECTO_PLAY_SAMPLE_STOP, ECTO_PLAY_SAMPLE_LOOP };

/* Slice type: which drum the slice starts on. Bit 0 kick, bit 1 snare. */
enum { ECTO_SLICE_KICK = 1, ECTO_SLICE_SNARE = 2 };

typedef struct {
    uint32_t size;          /* PCM bytes of music, pads excluded */
    uint16_t bpm;           /* 30..300 */
    uint8_t  play_mode;
    uint8_t  one_shot;
    uint8_t  tempo_match;
    uint8_t  oversampling;  /* 1 or 2 (the flag is one bit) */
    uint8_t  stereo;        /* 0 mono, 1 stereo */
    uint8_t  version;       /* 0 or 1 */
    uint16_t splice_trigger;/* ticks at 192 ppqn; 96 = an eighth note */
    uint8_t  splice_variable;
    uint8_t  n_slices;
    int32_t  slice_start[ECTO_MAX_SLICES]; /* byte offsets into the music */
    int32_t  slice_stop[ECTO_MAX_SLICES];
    int8_t   slice_type[ECTO_MAX_SLICES];
    /* version 1: transient positions as SOURCE FRAME / 16, three lanes
     * (kick, snare, other), each truncated to ECTO_MAX_TRANSIENTS */
    uint8_t  n_trans[3];
    uint16_t trans[3][ECTO_MAX_TRANSIENTS];
} ecto_info_t;

/* Parse a .info file. Returns 0 on success, or a negative reason:
 *   -1 too short / length does not match the version's exact formula
 *   -2 version > 1      -3 splice_trigger < 2     -4 play_mode > 4
 *   -5 a slice stop at or before its start, or past size              */
int ecto_info_parse(const uint8_t *buf, size_t len, ecto_info_t *out);

/* Frames per channel of music, and the sample rate it was written at. */
uint32_t ecto_info_frames(const ecto_info_t *in);
uint32_t ecto_info_rate(const ecto_info_t *in);

/* A WAV header, located by walking RIFF chunks (the original assumes a
 * 44-byte header; a file re-saved by another tool breaks that). */
typedef struct {
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;
    uint32_t data_offset;   /* byte offset of the first PCM byte */
    uint32_t data_bytes;
} ecto_wav_t;

/* Parse from the first bytes of the file (512 is plenty for normal files).
 * 0 on success; -1 not RIFF/WAVE, -2 no fmt, -3 not 16-bit PCM, -4 no data
 * chunk within buf. */
int ecto_wav_parse(const uint8_t *buf, size_t len, ecto_wav_t *out);

/* Byte offset of the music in the file: data_offset + the 0.5 s pad. */
uint32_t ecto_music_offset(const ecto_wav_t *w, const ecto_info_t *in);

/* ---- musical length ------------------------------------------------------ */

/* Beats in the music, from its frame count and bpm, rounded, at least 1. */
int ecto_beats(uint32_t frames, uint32_t rate, int bpm);

/* ---- slices on a grid ------------------------------------------------------
 *
 * The playback engine (smack_core) cuts a loop into EQUAL slices of
 * 1, 2, 4 or 8 half-steps (a half-step is a 32nd note; a beat is 8). A card's
 * slices are arbitrary. ecto_grid picks the engine slice size whose count is
 * closest to the card's, then marks each grid slice with what the card says
 * happens inside it: bit 0 kick, bit 1 snare, bit 2 any transient. That is
 * what Trig Out fires from. Grid cells are capped at 512 (the engine's). */
#define ECTO_GRID_MAX 512
enum { ECTO_G_KICK = 1, ECTO_G_SNARE = 2, ECTO_G_TRANS = 4 };

typedef struct {
    int     loop_hs;        /* beats * 8 */
    int     slice_res_idx;  /* engine index: 0..3 = 1, 2, 4, 8 half-steps */
    int     n;              /* grid slices = loop_hs / slice_hs */
    uint8_t flags[ECTO_GRID_MAX];
} ecto_grid_t;

void ecto_grid(const ecto_info_t *in, int beats, ecto_grid_t *g);

/* ---- grimoire ----------------------------------------------------------------
 *
 * Seven runes, each a set of the original's sixteen effects. Ours are
 * smack_core effects, so each ecto effect maps to one or more of them --
 * approximations, listed in ecto_card.c. A rune becomes a bitmask over
 * smack_fx_t (bit f = effect f allowed). */
uint32_t ecto_rune_mask(uint16_t effects);   /* bit e-1 = ecto effect e on */

/* The runes used when the card has no settings/grimoire. */
extern const uint16_t ecto_default_runes[ECTO_RUNES];

/* Apply one settings file NAME (not its contents) to a grimoire. Accepts
 * "effect<E>-on" / "effect<E>-off" for rune R. Returns 1 if it was one. */
int ecto_grimoire_apply(uint16_t runes[ECTO_RUNES], int rune_1based, const char *name);

/* ---- amen ---------------------------------------------------------------------
 *
 * The AMEN knob, as an order over n grid slices (order[i] = which slice plays
 * in position i). amen 0..1:
 *   < 0.056       straight: order[i] = i
 *   ... < 0.944   a sequence of L random slices repeated across the loop,
 *                 L from {1,2,3,4,5,6,7,8,9,10,12,14,16,24,32}
 *   >= 0.944      a fresh random run of 3-slice phrases, every roll
 * jump -1..+1 (AMEN RANDOM, centre dead zone): each position jumps to a
 * random slice with probability |jump|; to the LEFT the order carries on
 * from where it jumped, to the RIGHT the next position snaps back to the
 * grid. rng is a caller-owned xorshift state. */
void ecto_amen_order(uint16_t *order, int n, float amen, float jump, uint32_t *rng);

/* The sequence length the knob is on, for the ring (0 = straight, -1 = full random). */
int ecto_amen_len(float amen);

#ifdef __cplusplus
}
#endif

#endif /* ECTO_CARD_H */
