/*
 * break-alchemy native tests: the card reader (ecto_card.c) and the engine
 * playing loaded samples (break_engine.c, which compiles the vendored engine
 * exactly as the firmware does, SDRAM allocator included).
 *
 *   make -C test
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ecto_card.h"
#include "../src/break_engine.h"
#include "../src/versio_alloc.h"
#include "../src/clock_adapter.h"
#include "../src/vendor/smack_core.h"

static int g_pass = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); exit(1); } g_pass++; } while (0)

static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

/* Build a .info as ectocore.rocks lays it out. */
static size_t make_info(uint8_t *b, uint32_t size, int bpm, int stereo, int version, int n,
                        const uint16_t *tk, int ntk)
{
    wr32(b, size);
    uint32_t f = (uint32_t)bpm | (0u << 9) | (1u << 13) | ((uint32_t)stereo << 15) | ((uint32_t)version << 16);
    wr32(b + 4, f);
    wr16(b + 8, 96);
    b[10] = (uint8_t)n;
    const uint32_t step = size / (uint32_t)n & ~3u;
    for (int i = 0; i < n; i++)
    {
        wr32(b + 11 + 4 * i, step * (uint32_t)i);
        wr32(b + 11 + 4 * n + 4 * i, step * (uint32_t)(i + 1));
        b[11 + 8 * n + i] = (uint8_t)(i % 4 == 0 ? ECTO_SLICE_KICK : (i % 4 == 2 ? ECTO_SLICE_SNARE : 0));
    }
    size_t len = 11 + 9 * (size_t)n;
    if (version == 1)
    {
        wr16(b + len, (uint16_t)ntk); wr16(b + len + 2, 0); wr16(b + len + 4, 0);
        len += 6;
        for (int i = 0; i < ntk; i++, len += 2) wr16(b + len, tk[i]);
    }
    return len;
}

static void test_info(void)
{
    static uint8_t b[4096];
    ecto_info_t in;
    /* 2 s of 120 BPM stereo at 44.1k = 4 beats */
    const uint32_t size = 44100u * 2u * 2u * 2u;
    size_t len = make_info(b, size, 120, 1, 0, 16, NULL, 0);
    CHECK(ecto_info_parse(b, len, &in) == 0);
    CHECK(in.bpm == 120 && in.stereo == 1 && in.n_slices == 16 && in.tempo_match == 1);
    CHECK(in.splice_trigger == 96 && in.oversampling == 1);
    CHECK(ecto_info_frames(&in) == 88200u && ecto_info_rate(&in) == 44100u);
    CHECK(ecto_info_parse(b, len - 1, &in) == -1);      /* exact length is the check */
    CHECK(ecto_info_parse(b, len + 1, &in) == -1);

    const uint16_t tk[3] = {0, 2756, 5512};
    len = make_info(b, size, 120, 1, 1, 16, tk, 3);
    CHECK(ecto_info_parse(b, len, &in) == 0);
    CHECK(in.version == 1 && in.n_trans[0] == 3 && in.trans[0][2] == 5512);
    CHECK(ecto_info_parse(b, len - 2, &in) == -1);

    /* version 2 is refused; a slice past size is refused */
    len = make_info(b, size, 120, 1, 0, 4, NULL, 0);
    b[6] = 2;
    CHECK(ecto_info_parse(b, len, &in) == -2);
    len = make_info(b, size, 120, 1, 0, 4, NULL, 0);
    wr32(b + 11 + 16 + 12, size + 4);
    CHECK(ecto_info_parse(b, len, &in) == -5);
    /* splice_trigger < 2 */
    len = make_info(b, size, 120, 1, 0, 4, NULL, 0);
    wr16(b + 8, 1);
    CHECK(ecto_info_parse(b, len, &in) == -3);
}

static void test_wav(void)
{
    uint8_t b[256];
    memset(b, 0, sizeof b);
    memcpy(b, "RIFF", 4); memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "fmt ", 4); wr32(b + 16, 16);
    wr16(b + 20, 1); wr16(b + 22, 2); wr32(b + 24, 44100); wr16(b + 34, 16);
    /* a LIST chunk (odd length, padded) before data: the 44-byte assumption breaks */
    memcpy(b + 36, "LIST", 4); wr32(b + 40, 5);
    memcpy(b + 50, "data", 4); wr32(b + 54, 1000);
    ecto_wav_t w;
    CHECK(ecto_wav_parse(b, sizeof b, &w) == 0);
    CHECK(w.channels == 2 && w.rate == 44100 && w.data_offset == 58 && w.data_bytes == 1000);
    ecto_info_t in; memset(&in, 0, sizeof in); in.oversampling = 1; in.stereo = 1;
    CHECK(ecto_music_offset(&w, &in) == 58u + 22050u * 4u);
    wr16(b + 34, 24);
    CHECK(ecto_wav_parse(b, sizeof b, &w) == -3);
    b[0] = 'X';
    CHECK(ecto_wav_parse(b, sizeof b, &w) == -1);
}

static void test_grid(void)
{
    static uint8_t b[4096];
    ecto_info_t in;
    const uint32_t size = 44100u * 2u * 2u * 2u;     /* 2 s stereo */
    const uint16_t tk[1] = {(uint16_t)(44100u / 16u)}; /* a kick at 1.0 s */
    make_info(b, size, 120, 1, 1, 16, tk, 1);
    CHECK(ecto_info_parse(b, 11 + 9 * 16 + 6 + 2, &in) == 0);
    const int beats = ecto_beats(ecto_info_frames(&in), ecto_info_rate(&in), in.bpm);
    CHECK(beats == 4);
    ecto_grid_t g;
    ecto_grid(&in, beats, &g);
    CHECK(g.loop_hs == 32 && g.n == 16 && g.slice_res_idx == 1);
    CHECK(g.flags[0] & ECTO_G_KICK);
    CHECK(g.flags[2] & ECTO_G_SNARE);
    CHECK(!(g.flags[1] & (ECTO_G_KICK | ECTO_G_SNARE)));
    CHECK(g.flags[8] & ECTO_G_TRANS);                 /* the transient at 1.0 s */
    /* 32 card slices over 4 beats -> 1 half-step cells */
    make_info(b, size, 120, 1, 0, 32, NULL, 0);
    CHECK(ecto_info_parse(b, 11 + 9 * 32, &in) == 0);
    ecto_grid(&in, beats, &g);
    CHECK(g.n == 32 && g.slice_res_idx == 0);
    CHECK(ecto_beats(44100 * 10, 44100, 170) == 28);
}

static void test_amen_and_runes(void)
{
    uint16_t o[64];
    uint32_t r = 1234;
    ecto_amen_order(o, 16, 0.0f, 0.0f, &r);
    for (int i = 0; i < 16; i++) CHECK(o[i] == i);
    CHECK(ecto_amen_len(0.0f) == 0 && ecto_amen_len(1.0f) == -1);
    /* a knob position with a 4-long sequence: positions repeat every 4 */
    float a = -1;
    for (float x = 0.06f; x < 0.94f; x += 0.005f) if (ecto_amen_len(x) == 4) { a = x; break; }
    CHECK(a > 0);
    ecto_amen_order(o, 16, a, 0.0f, &r);
    for (int i = 4; i < 16; i++) CHECK(o[i] == o[i - 4]);
    /* full left jump: every position jumps, order stays in range */
    ecto_amen_order(o, 16, 0.0f, -1.0f, &r);
    for (int i = 0; i < 16; i++) CHECK(o[i] < 16);
    /* inside the dead zone: straight */
    ecto_amen_order(o, 16, 0.0f, 0.03f, &r);
    for (int i = 0; i < 16; i++) CHECK(o[i] == i);

    /* the mirrored smack_fx_t codes match the engine */
    CHECK(ecto_rune_mask(1u << 13) == (1u << SMACK_FX_RETRIG));
    CHECK(ecto_rune_mask(1u << 0) == (1u << SMACK_FX_DIST));
    CHECK(ecto_rune_mask(1u << 15) == ((1u << SMACK_FX_TAPESTOP) | (1u << SMACK_FX_TAPESTART)));
    CHECK(ecto_rune_mask(1u << 6) == (1u << SMACK_FX_COMB));
    CHECK(ecto_rune_mask(1u << 1) == ((1u << SMACK_FX_CRUSH) | (1u << SMACK_FX_RINGMOD)));
    CHECK(!(ecto_rune_mask(0xFFFFu) & 1u));

    uint16_t runes[ECTO_RUNES] = {0};
    CHECK(ecto_grimoire_apply(runes, 3, "effect14-on") == 1 && runes[2] == (1u << 13));
    CHECK(ecto_grimoire_apply(runes, 3, "effect14-off") == 1 && runes[2] == 0);
    CHECK(ecto_grimoire_apply(runes, 3, "effect17-on") == 0);
    CHECK(ecto_grimoire_apply(runes, 8, "effect1-on") == 0);
    CHECK(ecto_grimoire_apply(runes, 1, "brightness-5") == 0);
}

/* ---- the engine with a loaded break ------------------------------------------ */

static float host_bpm(void) { return 120.0f; }

static void feed_ticks(smack_t *s, double *acc, int frames, double fpt)
{
    *acc += frames;
    while (*acc >= fpt) { const uint8_t t = 0xF8; smack_on_midi(s, &t, 1, 3); *acc -= fpt; }
}

static void test_engine(void)
{
    static uint8_t pool[VERSIO_POOL_BYTES];
    versio_alloc_init(pool, sizeof pool);
    host_api_v1_t host; memset(&host, 0, sizeof host);
    host.api_version = 1; host.sample_rate = SMACK_SR; host.frames_per_block = 128; host.get_bpm = host_bpm;
    smack_t *s = smack_create(&host);
    CHECK(s && !versio_alloc_failed());
    brk_init(s);
    CHECK(brk_playing_slot(s) == -1);

    /* slot 0: 4 beats at 120 BPM, 44.1k frames (the engine varispeeds),
     * a different DC level per beat so the order is visible */
    const uint32_t frames = 88200;
    int16_t *p = brk_slot_ptr(s, 0);
    for (uint32_t i = 0; i < frames; i++)
    {
        const int16_t v = (int16_t)(4000 * (1 + (int)(i / 22050)) * (i & 64 ? 1 : -1));
        p[2 * i] = v; p[2 * i + 1] = v;
    }
    /* slot 1 marked, to prove nothing writes it */
    int16_t *q = brk_slot_ptr(s, 1);
    for (int i = 0; i < 4096; i++) q[i] = 0x1234;

    smack_set_param(s, "fx_density", "0");
    const uint8_t start = 0xFA;
    smack_on_midi(s, &start, 1, 3);
    brk_commit(s, 0, frames, 4, 1);
    CHECK(brk_playing_slot(s) == 0 && brk_n_slices(s) == 16);

    int16_t in[256] = {0}, out[256];
    double acc = 0, fpt = SMACK_SR * 60.0 / (120.0 * 24.0);
    double energy = 0;
    for (int b = 0; b < 400; b++)            /* ~1 s, live input silent */
    {
        feed_ticks(s, &acc, 128, fpt);
        smack_process(s, in, out, 128);
        for (int i = 0; i < 256; i++) energy += fabs((double)out[i]);
    }
    CHECK(energy > 1e6);                     /* the loaded break plays */
    for (int i = 0; i < 4096; i++) CHECK(q[i] == 0x1234);   /* rec_hold: ring untouched */
    for (uint32_t i = 0; i < 64; i++) CHECK(p[2 * i] == (int16_t)(4000 * (i & 64 ? 1 : -1)));

    /* rune = retrigger only, every slice effected */
    brk_set_rune(1u << SMACK_FX_RETRIG);
    smack_set_param(s, "fx_density", "100");
    int seen = 0;
    for (int b = 0; b < 400; b++)
    {
        feed_ticks(s, &acc, 128, fpt);
        smack_process(s, in, out, 128);
        if (b % 8 == 0) { const int f = brk_cell_fx(s); CHECK(f == SMACK_FX_RETRIG); seen++; }
    }
    CHECK(seen > 10);
    /* empty rune: no effects at all */
    brk_set_rune(0);
    smack_set_param(s, "reroll", "1");
    for (int b = 0; b < 200; b++)
    {
        feed_ticks(s, &acc, 128, fpt);
        smack_process(s, in, out, 128);
        CHECK(brk_cell_fx(s) == 0);
    }
    /* Amen length 1: every position plays the same slice */
    float a1 = -1;
    for (float x = 0.06f; x < 0.94f; x += 0.005f) if (ecto_amen_len(x) == 1) { a1 = x; break; }
    brk_set_amen(a1, 0.0f);
    smack_set_param(s, "reroll", "1");
    int src0 = -2, same = 1;
    for (int b = 0; b < 300; b++)
    {
        feed_ticks(s, &acc, 128, fpt);
        smack_process(s, in, out, 128);
        const int sc = brk_source_cell(s);
        if (src0 == -2) src0 = sc; else if (sc != src0) same = 0;
    }
    CHECK(same && src0 >= 0 && src0 < 16);
    brk_set_amen(0.0f, 0.0f);
    smack_set_param(s, "reroll", "1");

    /* commit slot 1 (2 beats): the bar position carries over */
    for (uint32_t i = 0; i < 44100; i++) { q[2 * i] = 1000; q[2 * i + 1] = 1000; }
    brk_commit(s, 1, 44100, 2, 1);
    CHECK(brk_playing_slot(s) == 1 && brk_n_slices(s) == 8);
    for (int b = 0; b < 100; b++)
    {
        feed_ticks(s, &acc, 128, fpt);
        smack_process(s, in, out, 128);
    }
    CHECK(out[0] > 500 && out[0] < 1500);    /* slot 1's level, unaffected */

    /* stop: silence; play: back at the top */
    brk_set_running(s, 0);
    feed_ticks(s, &acc, 128, fpt); smack_process(s, in, out, 128);
    feed_ticks(s, &acc, 128, fpt); smack_process(s, in, out, 128);
    int quiet = 1;
    for (int i = 0; i < 256; i++) if (abs(out[i]) > 4) quiet = 0;
    CHECK(quiet);
    brk_set_running(s, 1);
    CHECK(brk_phase(s) < 0.01f);
    smack_destroy(s);
}

/* The emulator's card (tools/make_test_card.py) must parse here too, so a
 * generator / reader disagreement shows up natively, not as a silent emu. */
static void test_emu_card(void)
{
    static const char *names[] = {"bank1/0.0", "bank1/1.0", "bank1/2.0", "bank2/0.0"};
    static uint8_t b[1 << 16];
    for (int i = 0; i < 4; i++)
    {
        char p[128];
        snprintf(p, sizeof p, "../emu-card/%s.wav.info", names[i]);
        FILE *f = fopen(p, "rb");
        if (!f) { printf("  (no emu-card; run tools/make_test_card.py emu-card)\n"); return; }
        const size_t n = fread(b, 1, sizeof b, f);
        fclose(f);
        ecto_info_t in;
        CHECK(ecto_info_parse(b, n, &in) == 0);
        snprintf(p, sizeof p, "../emu-card/%s.wav", names[i]);
        f = fopen(p, "rb");
        CHECK(f != NULL);
        const size_t m = fread(b, 1, 512, f);
        fseek(f, 0, SEEK_END);
        const long fsz = ftell(f);
        fclose(f);
        ecto_wav_t w;
        CHECK(ecto_wav_parse(b, m, &w) == 0);
        /* pads + music fit the file exactly */
        CHECK((long)ecto_music_offset(&w, &in) + (long)in.size + (long)(ecto_music_offset(&w, &in) - w.data_offset) == fsz);
        ecto_grid_t g;
        ecto_grid(&in, ecto_beats(ecto_info_frames(&in), ecto_info_rate(&in), in.bpm), &g);
        CHECK(g.n >= in.n_slices / 2 && g.n <= in.n_slices * 2);
        CHECK(g.flags[0] & ECTO_G_KICK);   /* every generated break opens on a kick */
    }
}

/* Free-run ignores the clock ratio (it only interprets J1 pulses), and
 * tempos above 150 survive x2 -- the adapter clamps free_bpm at 300, which
 * a compensate-by-doubling approach ran into (170 played at 150). */
static void test_clock(void)
{
    clock_adapter_t c;
    clk_init(&c, 48000, 120.0f);
    c.free_ignores_ratio = 1;
    clk_set_ratio(&c, CLK_TICKS_2X);
    clk_set_free_bpm(&c, 170.0f);
    CHECK(fabsf(clk_bpm(&c) - 170.0f) < 0.5f);
    clk_set_ratio(&c, CLK_TICKS_DIV2);
    CHECK(fabsf(clk_bpm(&c) - 170.0f) < 0.5f);
    clock_adapter_t d;                       /* Smack's behaviour is unchanged */
    clk_init(&d, 48000, 120.0f);
    clk_set_ratio(&d, CLK_TICKS_2X);
    CHECK(fabsf(clk_bpm(&d) - 60.0f) < 0.5f);
}

int main(void)
{
    test_clock();
    test_emu_card();
    test_info();
    test_wav();
    test_grid();
    test_amen_and_runes();
    test_engine();
    printf("test_break: %d checks passed\n", g_pass);
    return 0;
}
