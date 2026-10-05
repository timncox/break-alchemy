/*
 * card_loader -- see card_loader.h.
 *
 * SD rules (alchemy/storage/sd_card.h): every buffer FatFS DMAs into lives in
 * AXI SRAM (ALCHEMY_SDMMC_BSS), and each read lands at
 * staging + SdDmaStagingPad(file offset) so the sector-aligned part of the
 * transfer is word-aligned. The audio goes AXI -> SDRAM slot with a copy,
 * converting mono to stereo on the way.
 */
#include "card_loader.h"

#include <stdio.h>
#include <string.h>

#include "daisy_seed.h"
#include "alchemy/storage/sd_card.h"
#include "ff.h"

using daisy::System;

namespace card {
namespace {

constexpr uint32_t kChunk = 16384u;   /* bytes per f_read */

alignas(32) ALCHEMY_SDMMC_BSS FIL     s_fil;
alignas(32) ALCHEMY_SDMMC_BSS DIR     s_dir;
alignas(32) ALCHEMY_SDMMC_BSS FILINFO s_fno;
alignas(32) ALCHEMY_SDMMC_BSS uint8_t s_buf[kChunk + 32];
/* .info is at most 11 + 9*255 + 6 + 2*65535*3 in theory; real cards keep a
 * few hundred transients at most. 8 KB covers 255 slices + ~2700 transients. */
alignas(32) ALCHEMY_SDMMC_BSS uint8_t s_info[8192 + 32];

char     s_base[64]    = "";      /* "" or "/<folder>" */
uint16_t s_present[ECTO_BANKS];   /* bit s = slot s has a .info */
State    s_state       = State::NoCard;
Loaded   s_res;
ecto_info_t s_ii;
ecto_wav_t  s_wav;
const char* s_err      = "";

int16_t* s_dst         = nullptr;
uint32_t s_cap         = 0;       /* frames */
uint32_t s_want        = 0;       /* frames to read */
uint32_t s_done        = 0;       /* frames written */
uint32_t s_offset      = 0;       /* file offset of the next read */
uint8_t  s_carry[4];              /* a split frame across chunk reads */
uint32_t s_carry_n     = 0;

void path(char* out, size_t n, int bank, int slot, const char* tail)
{
    snprintf(out, n, "%s/bank%d/%d.0.wav%s", s_base, bank + 1, slot, tail);
}

bool read_at(uint32_t off, uint32_t len, uint8_t** data, UINT* got)
{
    if (f_lseek(&s_fil, off) != FR_OK) return false;
    uint8_t* p = s_buf + alchemy::SdDmaStagingPad(off);
    if (f_read(&s_fil, p, len, got) != FR_OK) return false;
    *data = p;
    return true;
}

bool bank_dir_exists(const char* base)
{
    char p[80];
    for (int b = 0; b < ECTO_BANKS; b++)
    {
        snprintf(p, sizeof p, "%s/bank%d", base, b + 1);
        if (f_stat(p, &s_fno) == FR_OK && (s_fno.fattrib & AM_DIR)) return true;
    }
    return false;
}

void read_grimoire(uint16_t runes[ECTO_RUNES])
{
    char p[96];
    for (int r = 1; r <= ECTO_RUNES; r++)
    {
        snprintf(p, sizeof p, "%s/settings/grimoire/rune%d", s_base, r);
        if (f_opendir(&s_dir, p) != FR_OK) continue;
        uint16_t set = 0;        /* a rune folder replaces the default */
        bool     any = false;
        uint16_t tmp[ECTO_RUNES] = {0};
        while (f_readdir(&s_dir, &s_fno) == FR_OK && s_fno.fname[0])
            if (ecto_grimoire_apply(tmp, r, s_fno.fname)) any = true;
        f_closedir(&s_dir);
        set = tmp[r - 1];
        if (any) runes[r - 1] = set;
    }
}

} // namespace

int Scan(alchemy::SdCard& sd, uint32_t now_ms, uint16_t runes[ECTO_RUNES])
{
    memset(s_present, 0, sizeof s_present);
    s_base[0] = 0;
    if (!sd.EnsureMounted(now_ms)) { s_state = State::NoCard; return 0; }

    if (!bank_dir_exists(""))
    {
        /* the ZIP's top folder, copied whole */
        if (f_opendir(&s_dir, "/") == FR_OK)
        {
            char cand[64];
            while (f_readdir(&s_dir, &s_fno) == FR_OK && s_fno.fname[0])
            {
                if (!(s_fno.fattrib & AM_DIR) || s_fno.fname[0] == '.') continue;
                const size_t n = strlen(s_fno.fname);
                if (n + 2 > sizeof cand) continue;     /* paths here stay short */
                cand[0] = '/';
                memcpy(cand + 1, s_fno.fname, n + 1);
                if (bank_dir_exists(cand)) { memcpy(s_base, cand, n + 2); break; }
            }
            f_closedir(&s_dir);
        }
    }

    char p[96];
    for (int b = 0; b < ECTO_BANKS; b++)
        for (int s = 0; s < ECTO_SLOTS; s++)
        {
            path(p, sizeof p, b, s, ".info");
            if (f_stat(p, &s_fno) == FR_OK && s_fno.fsize >= 11 && s_fno.fsize <= 8192)
                s_present[b] = (uint16_t)(s_present[b] | (1u << s));
        }
    read_grimoire(runes);
    s_state = NumBanks() ? State::Idle : State::NoCard;
    return NumBanks();
}

int NumBanks()
{
    int n = 0;
    for (int b = 0; b < ECTO_BANKS; b++) n += s_present[b] ? 1 : 0;
    return n;
}

int BankIndex(int nth)
{
    for (int b = 0; b < ECTO_BANKS; b++)
        if (s_present[b] && nth-- == 0) return b;
    return -1;
}

int NumSamples(int bank)
{
    if (bank < 0 || bank >= ECTO_BANKS) return 0;
    int n = 0;
    for (int s = 0; s < ECTO_SLOTS; s++) n += (s_present[bank] >> s) & 1;
    return n;
}

int SampleSlot(int bank, int nth)
{
    if (bank < 0 || bank >= ECTO_BANKS) return -1;
    for (int s = 0; s < ECTO_SLOTS; s++)
        if ((s_present[bank] >> s & 1) && nth-- == 0) return s;
    return -1;
}

static bool fail(const char* why)
{
    f_close(&s_fil);
    s_err   = why;
    s_state = State::Failed;
    return false;
}

bool Begin(int bank, int slot, int16_t* dst, uint32_t cap_frames)
{
    if (s_state == State::Loading) f_close(&s_fil);
    char p[96];
    UINT got = 0;
    uint8_t* d = nullptr;

    path(p, sizeof p, bank, slot, ".info");
    if (f_open(&s_fil, p, FA_READ) != FR_OK) return fail("no .info");
    const uint32_t isz = (uint32_t)f_size(&s_fil);
    if (isz > 8192u) return fail(".info too big");
    uint8_t* ip = s_info + alchemy::SdDmaStagingPad(0);
    if (f_read(&s_fil, ip, isz, &got) != FR_OK || got != isz) return fail(".info read");
    f_close(&s_fil);
    if (ecto_info_parse(ip, isz, &s_ii) != 0) return fail(".info invalid");

    path(p, sizeof p, bank, slot, "");
    if (f_open(&s_fil, p, FA_READ) != FR_OK) return fail("no wav");
    if (!read_at(0, 512, &d, &got)) return fail("wav read");
    if (ecto_wav_parse(d, got, &s_wav) != 0) return fail("wav header");

    const uint32_t frames = ecto_info_frames(&s_ii);
    const uint32_t rate   = ecto_info_rate(&s_ii);
    int beats = ecto_beats(frames, rate, s_ii.bpm);
    if (beats > 64) beats = 64;
    /* whole beats only, so the loop stays on the grid */
    uint32_t want = (uint32_t)((double)beats * 60.0 / s_ii.bpm * rate + 0.5);
    if (want > frames) want = frames;
    while (want > cap_frames && beats > 1)
    {
        beats /= 2;   /* halve to a power-of-two bar fraction that fits */
        want = (uint32_t)((double)beats * 60.0 / s_ii.bpm * rate + 0.5);
    }
    if (want > cap_frames) want = cap_frames;

    s_res.bank   = bank;
    s_res.slot   = slot;
    s_res.beats  = beats;
    s_res.bpm    = s_ii.bpm;
    s_res.frames = 0;
    ecto_grid(&s_ii, beats, &s_res.grid);

    s_dst     = dst;
    s_cap     = cap_frames;
    s_want    = want;
    s_done    = 0;
    s_carry_n = 0;
    s_offset  = ecto_music_offset(&s_wav, &s_ii);
    s_state   = State::Loading;
    s_err     = "";
    return true;
}

State Step(uint32_t budget_ms)
{
    if (s_state != State::Loading) return s_state;
    const uint32_t t0 = System::GetNow();
    const uint32_t ch = s_wav.channels;
    const uint32_t fb = ch * 2u;            /* bytes per source frame */
    do
    {
        const uint32_t left = (s_want - s_done) * fb - s_carry_n;
        const uint32_t len  = left < kChunk ? left : kChunk;
        UINT     got = 0;
        uint8_t* d   = nullptr;
        if (!read_at(s_offset, len, &d, &got)) { fail("wav read"); return s_state; }
        if (got == 0) { s_want = s_done; break; }   /* short file: keep what we have */
        s_offset += got;

        uint32_t i = 0;
        /* finish a frame split across the previous read */
        while (s_carry_n && i < got)
        {
            s_carry[s_carry_n++] = d[i++];
            if (s_carry_n == fb)
            {
                const int16_t l = (int16_t)(s_carry[0] | s_carry[1] << 8);
                const int16_t r = ch == 2 ? (int16_t)(s_carry[2] | s_carry[3] << 8) : l;
                s_dst[2 * s_done] = l; s_dst[2 * s_done + 1] = r;
                s_done++;
                s_carry_n = 0;
            }
        }
        for (; i + fb <= got && s_done < s_want; i += fb)
        {
            const int16_t l = (int16_t)(d[i] | d[i + 1] << 8);
            const int16_t r = ch == 2 ? (int16_t)(d[i + 2] | d[i + 3] << 8) : l;
            s_dst[2 * s_done] = l; s_dst[2 * s_done + 1] = r;
            s_done++;
        }
        for (; i < got && s_carry_n < fb; i++) s_carry[s_carry_n++] = d[i];
        if (got < len) { s_want = s_done; break; }
    } while (s_done < s_want && System::GetNow() - t0 < budget_ms);

    if (s_done >= s_want)
    {
        f_close(&s_fil);
        if (s_done < 256u) { s_err = "too short"; s_state = State::Failed; return s_state; }
        s_res.frames = s_done;
        s_state      = State::Done;
    }
    return s_state;
}

State Status() { return s_state; }
const Loaded& Result() { return s_res; }
const char* LastError() { return s_err; }

} // namespace card
