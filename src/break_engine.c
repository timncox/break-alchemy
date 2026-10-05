/*
 * The vendored engine, compiled once, plus the break-alchemy shim around it.
 * See break_engine.h. Carries over smack_core_alchemy.c's job: the engine's
 * calloc/free go to the SDRAM bump allocator (versio_alloc.c), scoped to this
 * translation unit -- libDaisy, the SDK and everything else keep the real
 * ones. stdlib.h comes FIRST so the macros rewrite call sites only.
 *
 * Build this file; never list vendor/smack_core.c too (duplicate symbols).
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "versio_alloc.h"

#define calloc versio_calloc
#define free   versio_free

#include "vendor/smack_core.c"

#undef calloc
#undef free

#include "break_engine.h"
#include "ecto_card.h"

/* ---- pattern rules ---------------------------------------------------------- */

static volatile float    g_amen = 0.0f;
static volatile float    g_jump = 0.0f;
static volatile uint32_t g_rune = 0xFFFFFFFEu;   /* every effect, NONE excluded */

void brk_set_amen(float amen, float jump) { g_amen = amen; g_jump = jump; }
void brk_set_rune(uint32_t mask)          { g_rune = mask & ~1u; }

static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

/* Runs at the end of every roll_pattern(). The engine has already rolled
 * fx/order from its seed; this keeps the rolled DENSITY (which slices carry
 * an effect) and replaces anything outside the rune with an effect inside
 * it, then overwrites the order with the Amen sequence. Both draw from their
 * own hash of the engine's rng state, so a seed still means one pattern and
 * the engine's stream is untouched. */
static void brk_post_roll(struct smack *s)
{
    const uint32_t mask = g_rune;
    int allowed[32], na = 0;
    for (int f = 1; f < SMACK_FX_COUNT && f < 32; f++)
        if (mask >> f & 1u) allowed[na++] = f;

    for (int l = 0; l < 2; l++)
    {
        smack_lane_t *ln = &s->lane[l];
        if (l == 1 && !s->chan_mode) break;
        for (int i = 0; i < s->n_slices; i++)
        {
            const int f = ln->fx[i];
            if (f == SMACK_FX_NONE || ln->locked[i] || (mask >> f & 1u)) continue;
            if (!na) { ln->fx[i] = SMACK_FX_NONE; ln->fxp[i] = 0; continue; }
            const int nf = allowed[mix32(s->rng ^ (uint32_t)(i * 2654435761u) ^ (uint32_t)l) % (uint32_t)na];
            ln->fx[i]  = (uint8_t)nf;
            ln->fxp[i] = default_fxp(nf);
        }
        uint32_t r = mix32(s->rng ^ 0xA3E7u ^ (uint32_t)l);
        ecto_amen_order(ln->order, s->n_slices, g_amen, g_jump, &r);
    }
}

/* ---- slots + loading -------------------------------------------------------- */

#define SLOT_FRAMES ((uint32_t)(SMACK_RING_FRAMES / 2u))

static int g_slot = -1;

void brk_init(struct smack *s)
{
    s->rec_hold = 1;
    s->monitor  = 0;
    s->ab       = 1;
    smack_post_roll_hook = brk_post_roll;
}

uint32_t brk_slot_frames(void) { return SLOT_FRAMES; }

int16_t *brk_slot_ptr(struct smack *s, int slot)
{
    return s->ring + (size_t)(slot ? SLOT_FRAMES : 0u) * 2u;
}

int brk_playing_slot(const struct smack *s) { (void)s; return g_slot; }

void brk_commit(struct smack *s, int slot, uint32_t frames, int beats, int slice_res_idx)
{
    if (frames < 256u) return;
    if (frames > SLOT_FRAMES) frames = SLOT_FRAMES;
    if (beats < 1) beats = 1;
    if (beats > 64) beats = 64;           /* the engine's longest grid */
    if (slice_res_idx < 0 || slice_res_idx >= SLICE_RES_COUNT) slice_res_idx = 3;

    /* Keep the playhead's place in the bar: the old phase in clock ticks,
     * wrapped onto the new loop's length. */
    double ticks_in = 0.0;
    if (s->state == SMACK_LOOPING && s->loop_frames_per_tick > 0.0)
        ticks_in = s->play_pos / s->loop_frames_per_tick;

    const uint32_t ticks = (uint32_t)beats * 24u;    /* 3 per half-step, 8 per beat */
    s->loop_start           = slot ? SLOT_FRAMES : 0u;
    s->loop_len             = frames;
    s->loop_clock_ticks     = ticks;
    s->loop_frames_per_tick = (double)frames / (double)ticks;
    s->loop_available       = frames;
    s->loop_history_start   = s->loop_start;
    s->slice_res_idx        = slice_res_idx;
    s->play_pos = fmod(ticks_in, (double)ticks) * s->loop_frames_per_tick;
    if (s->play_pos >= (double)frames) s->play_pos = 0.0;
    s->state = SMACK_LOOPING;
    g_slot   = slot;
    roll_pattern(s);
}

void brk_reset(struct smack *s) { s->play_pos = 0.0; }

void brk_set_running(struct smack *s, int on)
{
    if (on && s->transport_paused) s->play_pos = 0.0;
    s->transport_paused = on ? 0 : 1;
}

/* ---- read-backs ------------------------------------------------------------- */

int brk_n_slices(const struct smack *s) { return s->state == SMACK_LOOPING ? s->n_slices : 0; }

int brk_cell(const struct smack *s)
{
    if (s->state != SMACK_LOOPING || s->slice_frames <= 0.0) return -1;
    int c = (int)(s->play_pos / s->slice_frames);
    if (c >= s->n_slices) c = s->n_slices - 1;
    return c;
}

int brk_source_cell(const struct smack *s)
{
    const int c = brk_cell(s);
    if (c < 0) return -1;
    if (s->punch_fx > 0 || !s->ab) return c;
    return s->lane[0].order[c];
}

int brk_cell_fx(const struct smack *s)
{
    const int c = brk_cell(s);
    if (c < 0) return 0;
    if (s->punch_fx > 0) return s->punch_fx;
    return s->ab ? s->lane[0].fx[c] : 0;
}

float brk_phase(const struct smack *s)
{
    if (s->state != SMACK_LOOPING || !s->loop_len) return 0.0f;
    return (float)(s->play_pos / (double)s->loop_len);
}

uint32_t brk_ticks(const struct smack *s) { return s->tick_total; }
