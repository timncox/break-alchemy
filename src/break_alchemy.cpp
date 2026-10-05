/*
 * Break -- an Ectocore-style break mangler for the Hermetic Modular Alchemy
 * Lab V2, on the Alchemy SDK.
 *
 * Breaks come off the SD card in the ectocore.rocks layout (16 banks x 16
 * samples, sliced, with kick/snare/transient marks and a grimoire) and play
 * through smack_core, Smack's seeded per-slice glitch engine. The ectocore
 * ideas are mapped onto it, not ported: no code from schollz/_core (GPL-3.0)
 * is in this repo -- see docs/ectocore-sd-format.md and DESIGN.md.
 *
 *   PLAY            [P1 BREAK]   B1   [P2 AMEN]
 *                   [P3 SAMPLE]  B2   [P4 AMEN RANDOM]
 *                   [P5 GRIMOIRE] B3  [P6 DJ FILTER]
 *   SHIFT (hold B3) VOLUME, SEED, BANK, PITCH RANGE, CLOCK RATIO, TEMPO
 *
 *   B1   tap: re-roll          hold: stop / play
 *   B2   tap: tap tempo        hold: punch (every slice retriggers)
 *   J1 clock in   J2 reset in   J3 AMEN cv   J4 BREAK cv   J5 SAMPLE cv
 *   J6 AMEN RANDOM cv   J7 clock out (eighths)   J8 trig out   J9/J10 out
 *
 * Built from smack-alchemy (same engine, same shim habits); what that file's
 * comments explain -- 128-frame blocks, the SDRAM allocator, pot catch and
 * the autosave rules -- is not repeated here.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daisy_seed.h"
#include "util/CpuLoadMeter.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/host_link/host.h"
#include "alchemy/storage/fs_extension.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/control_loop.h"
#include "alchemy/surface/cv_matrix.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/manual.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include "card_loader.h"
#include "clock_adapter.h"
#include "dj_filter.h"
#include "extras.h"
#include "picker.h"
#include "versio_alloc.h"

extern "C" {
#include "vendor/smack_core.h"
#include "break_engine.h"
#include "ecto_card.h"
}

#ifndef BREAK_VERSION
#define BREAK_VERSION "0.0.0"
#endif
#ifndef BREAK_GIT_HASH
#define BREAK_GIT_HASH "dev"
#endif

using namespace alchemy;
using daisy::System;

static constexpr uint32_t kBlockSize = 128u;
enum : uint8_t { kPagePlay = 0, kPageShift = 1, kNumAppPages = 2 };
enum : uint8_t { kSettingsMain = 0, kSettingsFirmware = 1 };

/* J7 / J8 are the STM DAC jacks (cv_jacks[4], [5]): written directly, no I2C. */
static constexpr uint8_t kJackClockOut = 4u;
static constexpr uint8_t kJackTrigOut  = 5u;
static constexpr uint32_t kPulseFrames = 480u;   /* 10 ms at 48 kHz */

/* ---- hardware + engine -------------------------------------------------- */

static AlchemyLab          hw;
static daisy::CpuLoadMeter cpu;
static uint8_t DSY_SDRAM_BSS g_pool[VERSIO_POOL_BYTES];

static smack_t*        S = nullptr;
static clock_adapter_t CLK;
static host_api_v1_t   HOST;

/* control -> audio */
struct Commit { int slot; uint32_t frames; int beats; int res; };
static volatile bool  G_COMMIT_PENDING = false;
static Commit         g_commit;
static volatile bool  G_RUN      = true;     /* play / stop */
static volatile float G_DJ_CTL   = 0.0f;
static volatile float G_VOL      = 1.0f;
static volatile int   G_TRIG_MODE = 0;       /* 0 kick 1 snare 2 transient 3 random */
static volatile bool  G_PUNCH    = false;

/* audio -> control */
static volatile float G_PHASE = 0.0f;
static volatile int   G_CELL  = -1;
static volatile uint32_t G_BEATS_SEEN = 0;

/* grid flags per ring slot, written before that slot is committed */
static ecto_grid_t g_grid[2];

/* ---- colours ------------------------------------------------------------ */

static constexpr LedPanel::Rgb kColBreak = {0xFF, 0x40, 0x20};
static constexpr LedPanel::Rgb kColAmen  = {0xFF, 0xA0, 0x20};
static constexpr LedPanel::Rgb kColSamp  = {0x40, 0xC0, 0xFF};
static constexpr LedPanel::Rgb kColJumpL = {0x80, 0x40, 0xFF};
static constexpr LedPanel::Rgb kColJumpR = {0xFF, 0x40, 0xC0};
static constexpr LedPanel::Rgb kColRune  = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColLp    = {0x20, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColHp    = {0xFF, 0xA0, 0x20};
static constexpr LedPanel::Rgb kColNotch = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kColVol   = {0x40, 0xFF, 0x80};
static constexpr LedPanel::Rgb kColSeed  = {0xC0, 0x60, 0xFF};
static constexpr LedPanel::Rgb kColBank  = {0x40, 0xFF, 0xC0};
static constexpr LedPanel::Rgb kColPitch = {0xFF, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColRatio = {0x80, 0x80, 0xFF};
static constexpr LedPanel::Rgb kColTempo = {0xFF, 0xC0, 0x40};
static constexpr LedPanel::Rgb kColDim   = {0x18, 0x18, 0x18};
static constexpr LedPanel::Rgb kColShift = {0x30, 0x10, 0x40};

static constexpr LedPanel::Rgb kRed    = {0xFF, 0x00, 0x00};
static constexpr LedPanel::Rgb kAmber  = {0xFF, 0x80, 0x00};
static constexpr LedPanel::Rgb kGreen  = {0x00, 0xFF, 0x00};
static constexpr LedPanel::Rgb kWhite  = {0xFF, 0xFF, 0xFF};
static constexpr LedPanel::Rgb kGrey   = {0x60, 0x60, 0x60};
static constexpr LedPanel::Rgb kBlue   = {0x00, 0x50, 0xFF};

/* ---- pages ---------------------------------------------------------------- */

static const char* const kRuneLabels[ECTO_RUNES] = {
    "Rune 1", "Rune 2", "Rune 3", "Rune 4", "Rune 5", "Rune 6", "Rune 7"};
static const char* const kRatioLabels[3] = {"/2", "=1", "x2"};
static const char* const kTrigLabels[4]  = {"Kick", "Snare", "Transient", "Random"};
static const char* const kEvolveLabels[2] = {"Off", "Each pass"};

static VirtualKnob kn_break = VirtualKnob(kPotTopLeft, "Break")
    .Linear(0.f, 100.f).Ident("break").Unit("%").Ring(Level(kColBreak));
static VirtualKnob kn_amen = VirtualKnob(kPotTopRight, "Amen")
    .Linear(0.f, 100.f).Ident("amen").Unit("%").Ring(Level(kColAmen));
static VirtualKnob kn_sample = VirtualKnob(kPotMiddleLeft, "Sample")
    .Linear(0.f, 1.f).Ident("sample").Ring(Level(kColSamp));
static VirtualKnob kn_jump = VirtualKnob(kPotMiddleRight, "Amen Random")
    .Linear(-1.f, 1.f).Ident("jump").Ring(Bipolar(kColJumpR, kColJumpL, kColNotch));
static VirtualKnob kn_rune = VirtualKnob(kPotBottomLeft, "Grimoire")
    .Selector(ECTO_RUNES).Labels(kRuneLabels).Ident("rune")
    .Ring(SelectorRing(kColRune, kColDim, ECTO_RUNES));
static VirtualKnob kn_djf = VirtualKnob(kPotBottomRight, "DJ Filter")
    .Linear(-1.f, 1.f).Ident("djf").Ring(Bipolar(kColHp, kColLp, kColNotch));

static VirtualKnob kn_vol = VirtualKnob(kPotTopLeft, "Volume")
    .Linear(0.f, 200.f).Ident("vol").Unit("%").Ring(Level(kColVol));
static VirtualKnob kn_seed = VirtualKnob(kPotTopRight, "Seed")
    .Linear(0.f, 127.f).Ident("seed").Ring(Level(kColSeed));
static VirtualKnob kn_bank = VirtualKnob(kPotMiddleLeft, "Bank")
    .Linear(0.f, 1.f).Ident("bank").Ring(Level(kColBank));
static VirtualKnob kn_pitch = VirtualKnob(kPotMiddleRight, "Pitch Range")
    .Linear(1.f, 24.f).Ident("pitch").Unit("st").Ring(Level(kColPitch));
static VirtualKnob kn_ratio = VirtualKnob(kPotBottomLeft, "Clock Ratio")
    .Selector(3).Labels(kRatioLabels).Ident("clk.ratio")
    .Ring(SelectorRing(kColRatio, kColDim, 3));
static VirtualKnob kn_tempo = VirtualKnob(kPotBottomRight, "Tempo")
    .Linear(50.f, 200.f).Ident("tempo").Unit("BPM").Ring(Level(kColTempo));

static Page play_page = Page(kPagePlay).Name("Play").Color("#ff6020")
    .Knobs(kn_break, kn_amen, kn_sample, kn_jump, kn_rune, kn_djf);
static Page shift_page = Page(kPageShift).Name("Shift").Color("#c060ff")
    .Knobs(kn_vol, kn_seed, kn_bank, kn_pitch, kn_ratio, kn_tempo);

/* ---- descriptor metadata -------------------------------------------------- */

static Jack jk_clk   ("J1",  "Clock",       JackSig::Trig);
static Jack jk_rst   ("J2",  "Reset",       JackSig::Trig);
static Jack jk_amen  ("J3",  "CV Amen",     JackSig::CvBi);
static Jack jk_brk   ("J4",  "CV Break",    JackSig::CvBi);
static Jack jk_samp  ("J5",  "CV Sample",   JackSig::CvBi);
static Jack jk_jump  ("J6",  "CV Random",   JackSig::CvBi);
static Jack jk_cko   ("J7",  "Clock Out",   JackSig::Trig);
static Jack jk_tro   ("J8",  "Trig Out",    JackSig::Trig);
static Jack jk_out_l ("J9",  "Out L",       JackSig::AudioOut);
static Jack jk_out_r ("J10", "Out R",       JackSig::AudioOut);

static VirtualButton bt_b1 = VirtualButton("b1", "Roll")
    .Action("Tap", "Re-roll the pattern")
    .Action("Hold", "Stop / play");
static VirtualButton bt_b2 = VirtualButton("b2", "Tap")
    .Action("Tap", "Tap tempo")
    .Action("Hold", "Punch: every slice retriggers");
static VirtualButton bt_b3 = VirtualButton("b3", "Shift")
    .Action("Hold", "Show the Shift page");

static Manual manual = Manual()
    .Tagline("Ectocore-style break mangling from the SD card")
    .Preamble(
        "**Break** plays sliced breaks from an ectocore.rocks card (16 banks "
        "of 16) and mangles them. BREAK is how much happens; GRIMOIRE picks "
        "which effects can; AMEN loops a short random sequence of slices "
        "(right turn = shorter... full right = random phrases); AMEN RANDOM "
        "jumps around (left: stays where it jumped; right: snaps back). "
        "Clock on J1 (two pulses per beat at x2), reset on J2, eighth-note "
        "clock out on J7, kick / snare / transient triggers on J8.");

/* ---- surfaces ------------------------------------------------------------ */

static ControlLoop loop(hw);
static Pager       pager = Pager(kNumAppPages, kNumPots)
                               .Shift(hw.buttons[kButtonB3], kPageShift);

/* Home preset slot: Smack 12, Mark 13, Belt 14, Relay 15, seq 5, Meld 6,
 * MI 7-11 -- 4 is the first free one that is not slot 0 (migration). */
static constexpr uint8_t kHomeSlot = 4u;
static Presets     presets(hw.seed.qspi);
static Settings    settings(hw, &pager);
static CvMatrix    cv_matrix(kNumCvInputs);
static SdCard      sd;
static hostlink::FsExtension fs_ext(sd);
static hostlink::Host host(presets, "break_alchemy", "Break", BREAK_VERSION, BREAK_GIT_HASH);
static SmackExtras   extras;
static SelectorHandle trig_mode;
static SelectorHandle evolve_mode;

static float host_get_bpm(void)
{
    const float b = clk_bpm(&CLK);
    return (b > 20.0f && b < 300.0f) ? b : 120.0f;
}

/* The free-running tempo. CLK.free_ignores_ratio is set at boot: on Smack the
 * clock ratio also scales free-run (a speed switch); here it only says what a
 * J1 pulse is worth. */
static void set_free_bpm(float bpm) { clk_set_free_bpm(&CLK, bpm); }

static void set_param_int(const char* key, int v)
{
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    smack_set_param(S, key, b);
}

/* A value that only counts when it moves past a step + hysteresis. */
struct Step
{
    int   last = -32768;
    float last_norm = 0.0f;
    bool  Changed(int v, float norm, float dead)
    {
        if (v == last) return false;
        if (last != -32768 && fabsf(norm - last_norm) < dead) return false;
        last = v;
        last_norm = norm;
        return true;
    }
};

/* ---- the card --------------------------------------------------------------- */

static uint16_t g_runes[ECTO_RUNES];
static int      g_want_bank = -1, g_want_sample = -1;   /* bank 0..15, slot 0..15 */
static int      g_have_bank = -1, g_have_sample = -1;   /* playing */
static int      g_load_bank = -1, g_load_sample = -1;   /* loading */
static int      g_load_dst  = 0;
static uint32_t g_next_scan = 0;
static bool     g_scanned   = false;
static bool     g_load_fail = false;

static int nth_of(float norm, int n)
{
    if (n <= 0) return -1;
    int i = (int)(norm * (float)n);
    return i >= n ? n - 1 : (i < 0 ? 0 : i);
}

static void mark_dirty(uint32_t now);
static float bpm_to_norm(float bpm) { return (bpm - 50.0f) / 150.0f; }

static void card_frame(uint32_t now)
{
    if (!g_scanned)
    {
        if (now < g_next_scan || picker::Busy()) return;
        g_next_scan = now + 2000u;
        memcpy(g_runes, ecto_default_runes, sizeof g_runes);
        if (card::Scan(sd, now, g_runes) == 0) return;
        g_scanned = true;
        brk_set_rune(ecto_rune_mask(g_runes[(int)kn_rune.Value()]));
    }

    /* Which sample the knobs ask for: BANK picks among banks present,
     * SAMPLE among that bank's samples. */
    const int bank = card::BankIndex(nth_of(kn_bank.Norm(), card::NumBanks()));
    const int smp  = card::SampleSlot(bank, nth_of(kn_sample.Norm(), card::NumSamples(bank)));
    static Step st_b, st_s;
    if (st_b.Changed(bank, kn_bank.Norm(), 0.01f) | st_s.Changed(smp, kn_sample.Norm(), 0.01f))
    {
        g_want_bank = bank;
        g_want_sample = smp;
        g_load_fail = false;
    }
    if (g_want_bank < 0 || g_want_sample < 0) return;
    if (G_COMMIT_PENDING) return;   /* the audio thread has not taken the last one */

    const card::State cs = card::Status();
    const bool want_new = (g_want_bank != g_have_bank || g_want_sample != g_have_sample);
    const bool loading_it = cs == card::State::Loading
                         && g_load_bank == g_want_bank && g_load_sample == g_want_sample;
    if (want_new && !loading_it && !g_load_fail)
    {
        g_load_dst = brk_playing_slot(S) == 0 ? 1 : 0;
        g_load_bank = g_want_bank;
        g_load_sample = g_want_sample;
        if (!card::Begin(g_load_bank, g_load_sample, brk_slot_ptr(S, g_load_dst), brk_slot_frames()))
            g_load_fail = true;
    }

    /* ~6 ms of card I/O per ~16 ms frame */
    if (card::Step(6u) == card::State::Done && card::Status() == card::State::Done
        && g_load_bank >= 0)
    {
        const card::Loaded& r = card::Result();
        g_grid[g_load_dst] = r.grid;
        g_commit = {g_load_dst, r.frames, r.beats, r.grid.slice_res_idx};
        G_COMMIT_PENDING = true;
        g_have_bank = g_load_bank;
        g_have_sample = g_load_sample;
        g_load_bank = g_load_sample = -1;
        /* free-running: adopt the break's own tempo */
        if (!clk_locked(&CLK) && r.bpm >= 50 && r.bpm <= 200)
        {
            extras.free_bpm = (float)r.bpm;
            set_free_bpm((float)r.bpm);
            pager.SetStored(kPageShift, kPotBottomRight, bpm_to_norm((float)r.bpm), loop.Phys());
            mark_dirty(now);
        }
    }
    else if (card::Status() == card::State::Failed)
    {
        g_load_fail = true;
        g_load_bank = g_load_sample = -1;
    }
}

/* ---- buttons (1 ms poll) ------------------------------------------------------ */

#define LONG_PRESS_MS  600u
#define PUNCH_HOLD_MS  250u

static uint8_t g_btn_swallow = 0;
static bool btn_live(uint8_t b)
{
    const bool    down = hw.buttons[b].Pressed();
    const uint8_t bit  = (uint8_t)(1u << b);
    if (g_btn_swallow & bit)
    {
        if (down) return false;
        g_btn_swallow = (uint8_t)(g_btn_swallow & ~bit);
    }
    return down;
}

static bool     b1_down = false, b1_fired = false;
static uint32_t b1_t0 = 0;
static bool     b2_down = false;
static uint32_t b2_t0 = 0;
static uint32_t g_taps[4];
static int      g_ntaps = 0;
static uint32_t g_tap_flash_until = 0;

static void tap_tempo(uint32_t now)
{
    if (g_ntaps && now - g_taps[(g_ntaps - 1) & 3] > 2000u) g_ntaps = 0;
    g_taps[g_ntaps & 3] = now;
    g_ntaps++;
    g_tap_flash_until = now + 60u;
    if (g_ntaps < 2 || clk_locked(&CLK)) return;
    const int n = g_ntaps < 4 ? g_ntaps : 4;
    const uint32_t span = g_taps[(g_ntaps - 1) & 3] - g_taps[(g_ntaps - n) & 3];
    const float bpm = 60000.0f * (float)(n - 1) / (float)span;
    if (bpm < 50.0f || bpm > 200.0f) return;
    extras.free_bpm = bpm;
    set_free_bpm(bpm);
    pager.SetStored(kPageShift, kPotBottomRight, bpm_to_norm(bpm), loop.Phys());
    mark_dirty(now);
}

static void OnPoll(uint32_t now)
{
    if (settings.IsActive())
    {
        G_PUNCH = false;
        b1_down = b2_down = false;
        g_btn_swallow = (uint8_t)((1u << kButtonB1) | (1u << kButtonB2) | (1u << kButtonB3));
        return;
    }

    const bool p1 = btn_live(kButtonB1);
    if (p1 && !b1_down) { b1_down = true; b1_fired = false; b1_t0 = now; }
    if (b1_down && p1 && !b1_fired && now - b1_t0 > LONG_PRESS_MS)
    {
        G_RUN = !G_RUN;          /* hold: stop / play, fires while held */
        b1_fired = true;
    }
    if (b1_down && !p1)
    {
        b1_down = false;
        if (!b1_fired) smack_set_param(S, "reroll", "1");
    }

    const bool p2 = btn_live(kButtonB2);
    if (p2 && !b2_down) { b2_down = true; b2_t0 = now; }
    if (b2_down && p2 && now - b2_t0 > PUNCH_HOLD_MS) G_PUNCH = true;
    if (b2_down && !p2)
    {
        b2_down = false;
        if (!G_PUNCH) tap_tempo(now);
        G_PUNCH = false;
    }
}

/* ---- control frame (~60 Hz) ------------------------------------------------------- */

static bool     g_dirty = false;
static uint32_t g_dirty_since = 0, g_dirty_first = 0;
static float    g_saved_peak = 0.0f;
static bool     prev_settings = false;
static bool     prev_locked = false;
static uint32_t last_lock_write = 0;

static void mark_dirty(uint32_t now)
{
    if (!g_dirty) g_dirty_first = now;
    g_dirty = true;
    g_dirty_since = now;
}

static void update_dj_ctl(float n)
{
    const float DEAD = 0.06f, SPAN = 0.5f - DEAD;
    float c = 0.0f;
    if (n < 0.5f - DEAD)      c = (n - (0.5f - DEAD)) / SPAN;
    else if (n > 0.5f + DEAD) c = (n - (0.5f + DEAD)) / SPAN;
    G_DJ_CTL = c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c);
}

static void OnFrame(void)
{
    const uint32_t now = System::GetNow();

    /* PLAY-page changes are worth saving (the pots catch them after a switch). */
    {
        static float seen[kNumPots];
        static bool  primed = false;
        for (uint8_t p = 0; p < kNumPots; p++)
        {
            const float s = pager.Stored(kPagePlay, p);
            if (primed && fabsf(s - seen[p]) > 0.01f) mark_dirty(now);
            seen[p] = s;
        }
        primed = true;
    }

    card_frame(now);

    /* BREAK: probability a slice carries an effect. Squared, so the first
     * half of the knob is the subtle half (the original cubes it). */
    static Step st_brk;
    const float bn = kn_break.Norm();
    const int   bp = (int)(bn * bn * 100.0f + 0.5f);
    if (st_brk.Changed(bp, bn, 0.0f)) set_param_int("fx_density", bp);

    /* AMEN + AMEN RANDOM: re-roll when the sequence length or the jump
     * amount moves a step, so the order follows the knobs. */
    static Step st_len, st_jump;
    const float an = kn_amen.Norm(), jv = kn_jump.Value();
    brk_set_amen(an, jv);
    const bool len_moved  = st_len.Changed(ecto_amen_len(an), an, 0.01f);
    const bool jump_moved = st_jump.Changed((int)lroundf(jv * 20.0f), kn_jump.Norm(), 0.01f);
    if (len_moved || jump_moved) smack_set_param(S, "reroll", "1");

    /* GRIMOIRE */
    static Step st_rune;
    const int rune = (int)kn_rune.Value();
    if (st_rune.Changed(rune, kn_rune.Norm(), 0.02f))
    {
        brk_set_rune(ecto_rune_mask(g_runes[rune < ECTO_RUNES ? rune : 0]));
        smack_set_param(S, "reroll", "1");
    }

    update_dj_ctl(kn_djf.Norm());
    G_VOL = kn_vol.Value() / 100.0f;

    /* SHIFT page */
    static Step st_seed, st_pitch, st_ratio;
    const int seed = (int)lroundf(kn_seed.Value());
    if (st_seed.Changed(seed, kn_seed.Norm(), 1.0f / 127.0f)) { set_param_int("seed", seed); mark_dirty(now); }
    const int pr = (int)lroundf(kn_pitch.Value());
    if (st_pitch.Changed(pr, kn_pitch.Norm(), 0.02f)) { set_param_int("pitch_range", pr); mark_dirty(now); }
    const int ratio = (int)kn_ratio.Value();
    if (st_ratio.Changed(ratio, kn_ratio.Norm(), 0.02f))
    {
        static const int ticks[3] = {CLK_TICKS_DIV2, CLK_TICKS_1X, CLK_TICKS_2X};
        clk_set_ratio(&CLK, ticks[ratio < 3 ? ratio : 2]);
        mark_dirty(now);
    }
    {
        const float bpm = kn_tempo.Value();
        if (!clk_locked(&CLK) && fabsf(bpm - extras.free_bpm) >= 0.5f)
        {
            extras.free_bpm = bpm;
            set_free_bpm(bpm);
            mark_dirty(now);
        }
    }

    G_TRIG_MODE = (int)trig_mode.Value();

    /* EVOLVE: a fresh roll each loop pass while anything random is up. */
    {
        static float last_phase = 0.0f;
        const float ph = G_PHASE;
        const bool  wrapped = ph + 0.5f < last_phase;
        last_phase = ph;
        const bool random_up = bn > 0.05f || fabsf(jv) > 0.05f || ecto_amen_len(an) < 0;
        if (wrapped && (int)evolve_mode.Value() == 1 && random_up)
            smack_set_param(S, "reroll", "1");
    }

    /* A locked clock's tempo becomes the free-run default (as Smack). */
    const bool locked = clk_locked(&CLK);
    if (locked)
    {
        const float b = clk_bpm(&CLK);
        if (b > 20.0f && b < 300.0f)
        {
            if (fabsf(b - extras.free_bpm) >= 0.5f)
            {
                extras.free_bpm = b;
                set_free_bpm(b);
                mark_dirty(now);
            }
            if (!prev_locked || now - last_lock_write > 1000u)
            {
                pager.SetStored(kPageShift, kPotBottomRight, bpm_to_norm(b), loop.Phys());
                last_lock_write = now;
            }
        }
    }
    prev_locked = locked;

    {
        float mx = cpu.GetMaxCpuLoad();
        if (mx > 1.0f) mx = 1.0f;
        if (mx > extras.cpu_peak) extras.cpu_peak = mx;
        if (extras.cpu_peak > g_saved_peak + 0.05f) mark_dirty(now);
    }

    const bool sact = settings.IsActive();
    if (prev_settings && !sact) mark_dirty(now);
    prev_settings = sact;

    if (g_dirty && (now - g_dirty_since >= 5000u || now - g_dirty_first >= 30000u)
        && !sact && !picker::Busy() && card::Status() != card::State::Loading
        && !hw.buttons[kButtonB1].Pressed() && !hw.buttons[kButtonB2].Pressed()
        && !hw.buttons[kButtonB3].Pressed())
    {
        presets.Save(kHomeSlot);
        g_dirty = false;
        g_saved_peak = extras.cpu_peak;
    }
}

/* ---- LEDs ------------------------------------------------------------------------ */

static void SampleOverdraw(LedPanel& panel, uint8_t pot, const ArcGeometry& geo,
                           float norm, uint32_t t_ms, void* ctx)
{
    (void)norm; (void)t_ms; (void)ctx;
    if (g_have_bank < 0) return;
    const float hour = geo.start_hour + geo.step_hours * G_PHASE * (float)(geo.arc_leds - 1);
    panel.SetRingByHour(pot, fmodf(hour, 12.0f), panel.ScaleGlobal(kWhite));
}

static void OnRender(uint32_t t_ms)
{
    LedPanel& L = hw.leds;
    if (settings.IsActive()) return;

    /* B1: the card. red = no card / load failed, amber = loading,
     * green = playing, grey = stopped. */
    LedPanel::Rgb st = kRed;
    if (card::Status() == card::State::Loading) st = kAmber;
    else if (g_have_bank >= 0) st = G_RUN ? kGreen : kGrey;
    if (g_load_fail) st = kRed;
    if (G_PUNCH) st = kWhite;
    L.SetButtonPair(kButtonB1, L.ScaleGlobal(st));

    /* B2: the clock. blue = J1 locked, grey = free; a white flash on each
     * beat and each tap; red when the audio callback runs hot. */
    const bool locked = clk_locked(&CLK);
    LedPanel::Rgb ck = locked ? kBlue : kGrey;
    static uint32_t last_beats = 0, flash_until = 0;
    if (G_BEATS_SEEN != last_beats) { last_beats = G_BEATS_SEEN; flash_until = t_ms + 40u; }
    /* free-running, the beat flashes white; locked, J1 is the beat you can
     * already see, so B2 stays a steady blue */
    if (t_ms < flash_until && !locked) ck = kWhite;
    if (t_ms < g_tap_flash_until) ck = kWhite;
    if (cpu.GetAvgCpuLoad() > 0.80f) ck = kRed;
    L.SetButtonPair(kButtonB2, L.ScaleGlobal(ck));

    if (pager.Page() != kPageShift) L.SetButtonPair(kButtonB3, L.ScaleGlobal(kColShift));
}

/* ---- audio ----------------------------------------------------------------------- */

static void emit_to_engine(void* ctx, uint8_t byte) { smack_on_midi((smack_t*)ctx, &byte, 1, 3); }

static int16_t     bufi[kBlockSize * 2];
static int16_t     zeros[kBlockSize * 2];
static float       buff[kBlockSize * 2];
static dj_filter_t djf_state;

static uint32_t g_ck_left = 0, g_tr_left = 0;   /* pulse frames left */
static uint32_t g_last_eighth = 0xFFFFFFFFu;
static int      g_last_cell = -1;
static uint32_t g_trig_rng = 0x1234567u;

static inline float soft(float x)
{
    /* unity below 0.5, then a smooth knee into saturation */
    const float a = fabsf(x);
    if (a < 0.5f) return x;
    const float y = 0.5f + (a - 0.5f) / (1.0f + 2.0f * (a - 0.5f));
    return x < 0 ? -y : y;
}

static void AudioCallback(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size)
{
    cpu.OnBlockStart();
    (void)in;

    if (hw.j1.RisingEdge()) clk_gate_edge(&CLK);
    if (hw.j2.RisingEdge()) { brk_reset(S); g_last_cell = -1; }

    if (G_COMMIT_PENDING)
    {
        brk_commit(S, g_commit.slot, g_commit.frames, g_commit.beats, g_commit.res);
        G_COMMIT_PENDING = false;
    }
    static bool ran = true;
    if (G_RUN != ran) { ran = G_RUN; brk_set_running(S, ran ? 1 : 0); }
    static bool punched = false;
    if (G_PUNCH != punched)
    {
        punched = G_PUNCH;
        smack_set_param(S, "punch_fx", punched ? "1" : "-1");   /* 1 = RETRIG */
    }

    clk_advance(&CLK, (int)size, emit_to_engine, S);

    if (size > kBlockSize)
    {
        for (size_t i = 0; i < size; i++) out[0][i] = out[1][i] = 0.0f;
        cpu.OnBlockEnd();
        return;
    }

    smack_process(S, zeros, bufi, (int)size);

    const float k = G_VOL * (1.0f / 32768.0f);
    const bool  playing = G_RUN && brk_playing_slot(S) >= 0;
    for (size_t i = 0; i < size; i++)
    {
        buff[2 * i]     = playing ? soft((float)bufi[2 * i] * k) : 0.0f;
        buff[2 * i + 1] = playing ? soft((float)bufi[2 * i + 1] * k) : 0.0f;
    }

    {
        static bool dj_on = false;
        const float ctl = G_DJ_CTL;
        if (ctl != 0.0f) { dj_on = true; dj_filter_block(&djf_state, buff, (int)(2 * size), ctl); }
        else if (dj_on)  { dj_on = false; dj_filter_reset(&djf_state); }
    }
    for (size_t i = 0; i < size; i++)
    {
        out[0][i] = buff[2 * i];
        out[1][i] = buff[2 * i + 1];
    }

    /* J7: eighth-note clock out. J8: trig out on the cells the mode picks. */
    if (playing)
    {
        const uint32_t e = brk_ticks(S) / 12u;
        if (e != g_last_eighth)
        {
            g_last_eighth = e;
            g_ck_left = kPulseFrames;
            if ((e & 1u) == 0u) G_BEATS_SEEN = G_BEATS_SEEN + 1;
        }
        const int cell = brk_cell(S);
        if (cell != g_last_cell && cell >= 0)
        {
            g_last_cell = cell;
            const int src = brk_source_cell(S);
            const int slot = brk_playing_slot(S);
            const uint8_t f = (src >= 0 && src < ECTO_GRID_MAX && slot >= 0) ? g_grid[slot].flags[src] : 0;
            bool fire = false;
            switch (G_TRIG_MODE)
            {
                case 0: fire = f & ECTO_G_KICK; break;
                case 1: fire = f & ECTO_G_SNARE; break;
                case 2: fire = f != 0; break;
                default:
                    g_trig_rng ^= g_trig_rng << 13; g_trig_rng ^= g_trig_rng >> 17; g_trig_rng ^= g_trig_rng << 5;
                    fire = (g_trig_rng & 3u) == 0u;
                    break;
            }
            if (fire) g_tr_left = kPulseFrames;
        }
        G_PHASE = brk_phase(S);
        G_CELL  = cell;
    }
    {
        static bool ck_hi = false, tr_hi = false;
        const bool ck = g_ck_left > 0, tr = g_tr_left > 0;
        if (ck != ck_hi) { ck_hi = ck; hw.cv_jacks[kJackClockOut].SetVolts(ck ? 5.0f : 0.0f); }
        if (tr != tr_hi) { tr_hi = tr; hw.cv_jacks[kJackTrigOut].SetVolts(tr ? 5.0f : 0.0f); }
        g_ck_left = g_ck_left > size ? g_ck_left - (uint32_t)size : 0u;
        g_tr_left = g_tr_left > size ? g_tr_left - (uint32_t)size : 0u;
    }

    cpu.OnBlockEnd();
}

/* ---- boot ------------------------------------------------------------------------- */

static void fault_forever(void)
{
    for (;;)
    {
        hw.leds.Clear();
        for (uint8_t b = 0; b < kNumButtons; b++) hw.leds.SetButtonPair(b, {0x80, 0x00, 0x00});
        hw.leds.Show();
        System::Delay(200);
    }
}

int main(void)
{
    hw.Init(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ, kBlockSize);
    cpu.Init(hw.SampleRate(), (int)hw.BlockSize());
    versio_alloc_init(g_pool, VERSIO_POOL_BYTES);
    dj_filter_reset(&djf_state);
    memcpy(g_runes, ecto_default_runes, sizeof g_runes);

    memset(&HOST, 0, sizeof HOST);
    HOST.api_version      = 1;
    HOST.sample_rate      = SMACK_SR;
    HOST.frames_per_block = kBlockSize;
    HOST.get_bpm          = host_get_bpm;
    clk_init(&CLK, SMACK_SR, 120.0f);
    clk_set_mode(&CLK, CLK_EXTERNAL);
    CLK.free_ignores_ratio = 1;

    S = smack_create(&HOST);
    if (!S || versio_alloc_failed()) fault_forever();
    brk_init(S);
    smack_set_param(S, "wet", "100");

    hw.cv_jacks[kJackClockOut].EnableCvOutput();
    hw.cv_jacks[kJackClockOut].SetVolts(0.0f);
    hw.cv_jacks[kJackTrigOut].EnableCvOutput();
    hw.cv_jacks[kJackTrigOut].SetVolts(0.0f);

    sd.Init();
    picker::Install(settings, kSettingsFirmware, sd, hw);
    settings.UseBrightness();
    settings.UsePresets(presets);
    /* Pots 2 and 3 are the SDK's preset pair; 0 is brightness. */
    trig_mode = settings.Page(kSettingsMain).Pot(1)
        .Selector(kTrigLabels).Default(0)
        .Ident("trig.mode").Name("Trig Out")
        .Help("What J8 fires on: the **kick** or **snare** slices the card "
              "marks, any **transient**, or at **random**. Fires at the start "
              "of the slice that holds it.");
    evolve_mode = settings.Page(kSettingsMain).Pot(4)
        .Selector(kEvolveLabels).Default(1)
        .Ident("evolve").Name("Evolve")
        .Help("**Each pass**: while BREAK, AMEN RANDOM or full AMEN is up, "
              "roll a fresh pattern every time the loop comes round, so the "
              "effects come and go. **Off**: one pattern until you re-roll.");

    /* CV: J3-J6 modulate, J7/J8 are outputs. */
    cv_matrix.Jack(0).To(kn_amen);
    cv_matrix.Jack(1).To(kn_break);
    cv_matrix.Jack(2).To(kn_sample);
    cv_matrix.Jack(3).To(kn_jump);
    cv_matrix.Jack(4).Off();
    cv_matrix.Jack(5).Off();

    kn_sample.Overdraw(SampleOverdraw);

    host.Product("Break")
        .BootSlot(kHomeSlot)
        .Pages(play_page, shift_page)
        .Jacks(jk_clk, jk_rst, jk_amen, jk_brk, jk_samp, jk_jump, jk_cko, jk_tro, jk_out_l, jk_out_r)
        .Buttons(bt_b1, bt_b2, bt_b3)
        .Attach(manual)
        .Extend(fs_ext);

    presets.Manage(pager);
    presets.Manage(settings);
    presets.Manage(extras);
    presets.Init();
    const bool had_home = presets.HasValid(kHomeSlot);
    presets.BootLoad();
    if (had_home) presets.Load(kHomeSlot);

    float phys[kNumPots];
    for (int i = 0; i < 8; i++) { hw.ProcessAllControls(); System::Delay(1); }
    for (uint8_t p = 0; p < kNumPots; p++) phys[p] = hw.pots[p].Value();

    if (!had_home)
    {
        pager.SetStored(kPageShift, kPotTopLeft,     0.5f,           phys); /* volume 100 % */
        pager.SetStored(kPageShift, kPotTopRight,    0.0f,           phys); /* seed 0       */
        pager.SetStored(kPageShift, kPotMiddleLeft,  0.0f,           phys); /* first bank   */
        pager.SetStored(kPageShift, kPotMiddleRight, 11.0f / 23.0f,  phys); /* 12 semis     */
        pager.SetStored(kPageShift, kPotBottomLeft,  2.5f / 3.0f,    phys); /* x2: 2 ppqn   */
        pager.SetStored(kPageShift, kPotBottomRight, bpm_to_norm(120.0f), phys);
        for (uint8_t p = 0; p < kNumPots; p++) pager.SetStored(kPagePlay, p, phys[p], phys);
    }
    set_free_bpm(extras.free_bpm);
    extras.cpu_peak = 0.0f;

    hw.StartAudio(AudioCallback);
    cpu.Reset();

    loop.Use(pager)
        .Use(settings)
        .Use(cv_matrix)
        .Use(play_page)
        .Use(shift_page)
        .Use(host)
        .OnFrame(OnFrame)
        .OnPoll(OnPoll)
        .OnRender(OnRender);

    for (;;) loop.Tick();
}
