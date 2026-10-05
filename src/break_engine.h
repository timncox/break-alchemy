/*
 * break_engine -- smack_core driven by loaded samples instead of a live
 * recording, plus the Ectocore-style pattern rules (grimoire runes, Amen).
 *
 * break_engine.c is the ONE translation unit that compiles the vendored
 * engine (it replaces smack-alchemy's smack_core_alchemy.c, keeping its
 * calloc/free redirect), so it can reach the engine's internals: the ring,
 * the loop fields and the lanes. Nothing else should.
 *
 * Threading. brk_commit / brk_reset / brk_set_running / the read-backs run
 * on the AUDIO thread (they touch what smack_process reads). brk_set_amen /
 * brk_set_rune take effect at the next roll, whichever thread rolls.
 * Writing a slot's samples (brk_slot_ptr) is the control thread's job, and
 * safe only into the slot that is NOT playing.
 */
#ifndef BREAK_ENGINE_H
#define BREAK_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct smack;

/* Call once after smack_create(): holds the recorder off the ring and
 * installs the post-roll hook. */
void brk_init(struct smack *s);

/* Two sample slots, each half the ring. Frames are stereo int16 pairs. */
int16_t *brk_slot_ptr(struct smack *s, int slot);
uint32_t brk_slot_frames(void);

/* Make slot `slot` (frames long, `beats` beats, cut at the engine's slice
 * index slice_res_idx 0..3) the playing loop. The playhead keeps its place
 * in the bar. Then rolls. AUDIO THREAD. */
void brk_commit(struct smack *s, int slot, uint32_t frames, int beats, int slice_res_idx);

/* Which slot plays, -1 = none yet. */
int  brk_playing_slot(const struct smack *s);

/* Pattern rules read at every roll. amen 0..1, jump -1..1, rune = bitmask
 * over smack_fx_t (0 = no effects at all). */
void brk_set_amen(float amen, float jump);
void brk_set_rune(uint32_t mask);

/* Back to the top of the loop (J2 reset). AUDIO THREAD. */
void brk_reset(struct smack *s);
/* Stop / play. Stopped holds the playhead and outputs silence. AUDIO THREAD. */
void brk_set_running(struct smack *s, int on);

/* Read-backs for LEDs and the outputs. */
int      brk_n_slices(const struct smack *s);
int      brk_cell(const struct smack *s);        /* grid position playing, -1 = none */
int      brk_source_cell(const struct smack *s); /* the slice it plays (after Amen) */
int      brk_cell_fx(const struct smack *s);     /* effect on that position, 0 = none */
float    brk_phase(const struct smack *s);       /* 0..1 through the loop */
uint32_t brk_ticks(const struct smack *s);       /* 24 ppqn ticks since Start */

#ifdef __cplusplus
}
#endif

#endif /* BREAK_ENGINE_H */
