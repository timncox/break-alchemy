/*
 * ecto_card -- see ecto_card.h. Clean-room: written from the format spec in
 * docs/ectocore-sd-format.md, not from schollz/_core's source.
 */
#include "ecto_card.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---- .info ------------------------------------------------------------------ */

int ecto_info_parse(const uint8_t *buf, size_t len, ecto_info_t *o)
{
    if (!buf || !o || len < 11) return -1;
    memset(o, 0, sizeof *o);

    o->size = rd32(buf);
    const uint32_t f = rd32(buf + 4);
    o->bpm          = (uint16_t)(f & 0x1FFu);
    o->play_mode    = (uint8_t)((f >> 9) & 7u);
    o->one_shot     = (uint8_t)((f >> 12) & 1u);
    o->tempo_match  = (uint8_t)((f >> 13) & 1u);
    o->oversampling = ((f >> 14) & 1u) ? 2 : 1;
    o->stereo       = (uint8_t)((f >> 15) & 1u);
    o->version      = (uint8_t)((f >> 16) & 0x7Fu);
    const uint16_t sp = rd16(buf + 8);
    o->splice_trigger  = (uint16_t)(sp & 0x7FFFu);
    o->splice_variable = (uint8_t)(sp >> 15);
    const size_t n = buf[10];
    o->n_slices = (uint8_t)n;

    if (o->version > 1) return -2;

    /* No magic number: the exact length is the format check. */
    const size_t base = 11 + 9 * n;
    if (o->version == 0)
    {
        if (len != base) return -1;
    }
    else
    {
        if (len < base + 6) return -1;
        const size_t c0 = rd16(buf + base), c1 = rd16(buf + base + 2), c2 = rd16(buf + base + 4);
        if (len != base + 6 + 2 * (c0 + c1 + c2)) return -1;
        const size_t cnt[3] = {c0, c1, c2};
        const uint8_t *p = buf + base + 6;
        for (int lane = 0; lane < 3; lane++)
        {
            for (size_t i = 0; i < cnt[lane]; i++, p += 2)
                if (i < ECTO_MAX_TRANSIENTS) o->trans[lane][i] = rd16(p);
            o->n_trans[lane] = (uint8_t)(cnt[lane] < ECTO_MAX_TRANSIENTS ? cnt[lane] : ECTO_MAX_TRANSIENTS);
        }
    }

    if (o->splice_trigger < 2) return -3;
    if (o->play_mode > 4) return -4;
    if (o->bpm < 30) o->bpm = 30;
    if (o->bpm > 300) o->bpm = 300;

    for (size_t i = 0; i < n; i++)
    {
        o->slice_start[i] = (int32_t)rd32(buf + 11 + 4 * i);
        o->slice_stop[i]  = (int32_t)rd32(buf + 11 + 4 * n + 4 * i);
        o->slice_type[i]  = (int8_t)buf[11 + 8 * n + i];
        if (o->slice_start[i] < 0 || o->slice_stop[i] <= o->slice_start[i]
            || (uint32_t)o->slice_stop[i] > o->size)
            return -5;
    }
    return 0;
}

uint32_t ecto_info_rate(const ecto_info_t *in) { return 44100u * in->oversampling; }

uint32_t ecto_info_frames(const ecto_info_t *in)
{
    return in->size / 2u / (in->stereo ? 2u : 1u);
}

/* ---- WAV -------------------------------------------------------------------- */

int ecto_wav_parse(const uint8_t *buf, size_t len, ecto_wav_t *w)
{
    if (!buf || !w || len < 12) return -1;
    memset(w, 0, sizeof *w);
    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return -1;
    int    have_fmt = 0;
    size_t p        = 12;
    while (p + 8 <= len)
    {
        const uint32_t sz = rd32(buf + p + 4);
        if (!memcmp(buf + p, "fmt ", 4))
        {
            if (p + 8 + 16 > len) return -2;
            const uint16_t fmt = rd16(buf + p + 8);
            w->channels = rd16(buf + p + 10);
            w->rate     = rd32(buf + p + 12);
            w->bits     = rd16(buf + p + 22);
            /* 1 = PCM, 0xFFFE = extensible (still PCM at 16 bits here) */
            if ((fmt != 1 && fmt != 0xFFFEu) || w->bits != 16
                || w->channels < 1 || w->channels > 2)
                return -3;
            have_fmt = 1;
        }
        else if (!memcmp(buf + p, "data", 4))
        {
            if (!have_fmt) return -2;
            w->data_offset = (uint32_t)(p + 8);
            w->data_bytes  = sz;
            return 0;
        }
        p += 8 + (size_t)sz + (sz & 1u);   /* chunks are word-aligned */
    }
    return have_fmt ? -4 : -2;
}

uint32_t ecto_music_offset(const ecto_wav_t *w, const ecto_info_t *in)
{
    const uint32_t pad_frames = ecto_info_rate(in) / 2u;   /* 0.5 s */
    return w->data_offset + pad_frames * (uint32_t)w->channels * 2u;
}

/* ---- length + grid ---------------------------------------------------------- */

int ecto_beats(uint32_t frames, uint32_t rate, int bpm)
{
    if (!rate || bpm <= 0) return 1;
    const double b = (double)frames / (double)rate * (double)bpm / 60.0;
    int          r = (int)(b + 0.5);
    return r < 1 ? 1 : r;
}

static const int k_slice_hs[4] = {1, 2, 4, 8};

void ecto_grid(const ecto_info_t *in, int beats, ecto_grid_t *g)
{
    memset(g, 0, sizeof *g);
    if (beats > ECTO_GRID_MAX / 8) beats = ECTO_GRID_MAX / 8;   /* 64 beats */
    g->loop_hs = beats * 8;

    /* The card's slice count, or eighth notes when it has none. */
    const int want = in->n_slices ? in->n_slices : beats * 2;
    int best = 3, best_err = 1 << 30;
    for (int i = 3; i >= 0; i--)          /* coarse first: ties go coarse */
    {
        const int n = g->loop_hs / k_slice_hs[i];
        if (n < 1 || n > ECTO_GRID_MAX) continue;
        const int err = n > want ? n - want : want - n;
        if (err < best_err) { best_err = err; best = i; }
    }
    g->slice_res_idx = best;
    g->n = g->loop_hs / k_slice_hs[best];
    if (g->n < 1) g->n = 1;

    const uint32_t frames = ecto_info_frames(in);
    if (!frames) return;
    const uint32_t ch = in->stereo ? 2u : 1u;
    for (int k = 0; k < in->n_slices; k++)
    {
        const uint32_t fr  = (uint32_t)in->slice_start[k] / 2u / ch;
        /* nearest cell boundary: offsets are 4-byte aligned, so a slice on a
         * grid line can sit a frame early */
        int            idx = (int)(((uint64_t)fr * (uint64_t)g->n + frames / 2u) / frames);
        if (idx >= g->n) idx = g->n - 1;
        if (in->slice_type[k] & ECTO_SLICE_KICK)  g->flags[idx] |= ECTO_G_KICK;
        if (in->slice_type[k] & ECTO_SLICE_SNARE) g->flags[idx] |= ECTO_G_SNARE;
    }
    static const uint8_t lane_bits[3] = {ECTO_G_KICK | ECTO_G_TRANS,
                                         ECTO_G_SNARE | ECTO_G_TRANS, ECTO_G_TRANS};
    for (int lane = 0; lane < 3; lane++)
        for (int i = 0; i < in->n_trans[lane]; i++)
        {
            const uint32_t fr  = (uint32_t)in->trans[lane][i] * 16u;
            if (fr >= frames) continue;
            int idx = (int)(((uint64_t)fr * (uint64_t)g->n + frames / 2u) / frames);
            if (idx >= g->n) idx = g->n - 1;
            g->flags[idx] |= lane_bits[lane];
        }
}

/* ---- grimoire ----------------------------------------------------------------- */

/* smack_fx_t codes. Mirrored, not included: this file stays engine-free;
 * test_ecto_card.c asserts they match smack_core.h. */
enum {
    FX_RETRIG = 1, FX_REVERSE, FX_PITCH, FX_SPEED, FX_GATE, FX_BUZZ, FX_CRUSH,
    FX_REPEAT, FX_REVAFTER, FX_TAPESTOP, FX_TAPESTART, FX_SCRATCH, FX_ENV,
    FX_PAN, FX_FILTER, FX_VOWEL, FX_TONALDELAY, FX_FREEZE, FX_DELAY, FX_DIST,
    FX_PHASER, FX_VERB, FX_PSHIFT, FX_RINGMOD, FX_COMB, FX_SCATTER
};
#define B(f) (1u << (f))

/* Ecto effect (1..16) -> the smack effects that stand in for it. These are
 * APPROXIMATIONS: smack's effects are per-slice and seeded, the original's
 * run for a number of beats. Pitch up and down both map to PITCH/PSHIFT,
 * whose direction the roll picks. */
static const uint32_t k_ecto_to_smack[ECTO_EFFECTS] = {
    /*  1 distortion          */ B(FX_DIST),
    /*  2 loss                */ B(FX_CRUSH) | B(FX_RINGMOD),
    /*  3 bitcrush            */ B(FX_CRUSH),
    /*  4 filter              */ B(FX_FILTER) | B(FX_VOWEL),
    /*  5 time stretch        */ B(FX_SPEED) | B(FX_FREEZE),
    /*  6 delay               */ B(FX_DELAY) | B(FX_TONALDELAY),
    /*  7 comb                */ B(FX_COMB),
    /*  8 beat repeat         */ B(FX_REPEAT),
    /*  9 reverb              */ B(FX_VERB),
    /* 10 autopan             */ B(FX_PAN),
    /* 11 pitch down          */ B(FX_PITCH) | B(FX_PSHIFT),
    /* 12 pitch up            */ B(FX_PITCH) | B(FX_PSHIFT),
    /* 13 reverse             */ B(FX_REVERSE) | B(FX_REVAFTER),
    /* 14 retrigger           */ B(FX_RETRIG),
    /* 15 retrigger + pitch   */ B(FX_BUZZ) | B(FX_SCRATCH),
    /* 16 tape stop           */ B(FX_TAPESTOP) | B(FX_TAPESTART),
};

uint32_t ecto_rune_mask(uint16_t effects)
{
    uint32_t m = 0;
    for (int e = 0; e < ECTO_EFFECTS; e++)
        if (effects >> e & 1u) m |= k_ecto_to_smack[e];
    return m;
}

#define E(n) (uint16_t)(1u << ((n) - 1))
/* Our own seven, for a card without a grimoire. (The original randomises a
 * missing rune; a fixed set is easier to learn.) */
const uint16_t ecto_default_runes[ECTO_RUNES] = {
    /* 1 stutter */ E(14) | E(8) | E(13),
    /* 2 tape    */ E(16) | E(5) | E(11) | E(12),
    /* 3 dirt    */ E(1) | E(2) | E(3),
    /* 4 space   */ E(6) | E(9) | E(10) | E(7),
    /* 5 filter  */ E(4) | E(7) | E(10),
    /* 6 glitch  */ E(15) | E(14) | E(3) | E(13),
    /* 7 all     */ 0xFFFFu,
};

int ecto_grimoire_apply(uint16_t runes[ECTO_RUNES], int rune, const char *name)
{
    if (rune < 1 || rune > ECTO_RUNES || !name) return 0;
    if (strncmp(name, "effect", 6)) return 0;
    const char *p = name + 6;
    int e = 0;
    while (*p >= '0' && *p <= '9') e = e * 10 + (*p++ - '0');
    if (e < 1 || e > ECTO_EFFECTS || *p != '-') return 0;
    p++;
    const uint16_t bit = (uint16_t)(1u << (e - 1));
    if (!strcmp(p, "on"))       runes[rune - 1] |= bit;
    else if (!strcmp(p, "off")) runes[rune - 1] &= (uint16_t)~bit;
    else return 0;
    return 1;
}

/* ---- amen ------------------------------------------------------------------------ */

static const int k_amen_len[15] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 24, 32};
#define AMEN_LO   (57.0f / 1023.0f)
#define AMEN_HI   (966.0f / 1023.0f)
#define JUMP_DEAD (24.0f / 512.0f)

static uint32_t xs(uint32_t *s)
{
    uint32_t x = *s ? *s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}
static int below(uint32_t *s, int n) { return n > 0 ? (int)(xs(s) % (uint32_t)n) : 0; }

int ecto_amen_len(float amen)
{
    if (amen < AMEN_LO) return 0;
    if (amen >= AMEN_HI) return -1;
    int i = (int)((amen - AMEN_LO) / (AMEN_HI - AMEN_LO) * 15.0f);
    if (i > 14) i = 14;
    if (i < 0) i = 0;
    return k_amen_len[i];
}

void ecto_amen_order(uint16_t *order, int n, float amen, float jump, uint32_t *rng)
{
    if (n < 1) return;
    const int len = ecto_amen_len(amen);
    if (len == 0)
    {
        for (int i = 0; i < n; i++) order[i] = (uint16_t)i;
    }
    else if (len > 0)
    {
        uint16_t seq[32];
        for (int j = 0; j < len; j++) seq[j] = (uint16_t)below(rng, n);
        for (int i = 0; i < n; i++) order[i] = seq[i % len];
    }
    else
    {
        /* full random: phrases of three consecutive slices */
        for (int i = 0; i < n; i += 3)
        {
            const int s0 = below(rng, n);
            for (int k = 0; k < 3 && i + k < n; k++) order[i + k] = (uint16_t)((s0 + k) % n);
        }
    }

    /* AMEN RANDOM */
    float a = jump < 0 ? -jump : jump;
    if (a <= JUMP_DEAD) return;
    float p = (a - JUMP_DEAD) / (1.0f - JUMP_DEAD);
    if (jump > 0) p *= 0.9f;                  /* right: up to ~90 % */
    const uint32_t thresh = (uint32_t)(p * 16777216.0f);
    int carry = -1;                           /* left: the slice we jumped to */
    for (int i = 0; i < n; i++)
    {
        if ((xs(rng) >> 8) < thresh)
        {
            const int to = below(rng, n);
            order[i] = (uint16_t)to;
            if (jump < 0) carry = to;
        }
        else if (carry >= 0)
        {
            carry = (carry + 1) % n;
            order[i] = (uint16_t)carry;
        }
    }
}
