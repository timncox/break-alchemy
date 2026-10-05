/*
 * card_loader -- finds ectocore.rocks banks on the SD card and streams one
 * sample at a time into a ring slot, a few milliseconds per control frame so
 * the panel never freezes while a break loads.
 *
 * Control thread only (FatFS is not reentrant, and the audio interrupt never
 * touches the card). The caller hands over the destination: the ring slot
 * that is NOT playing, so writing it races nothing.
 */
#pragma once

#include <stdint.h>

#include "ecto_card.h"

namespace alchemy { class SdCard; }

namespace card {

/* What a finished load produced, for brk_commit + trig out. */
struct Loaded
{
    int         bank, slot;   /* 0-based */
    uint32_t    frames;       /* stereo frames written */
    int         beats;
    int         bpm;
    ecto_grid_t grid;
};

enum class State : uint8_t { NoCard, Idle, Loading, Done, Failed };

/* Look for the banks: /bank1../bank16, or inside the first top-level folder
 * that has them (the ZIP's folder copied whole). Reads the grimoire into
 * runes (left as given when the card has none). Returns the bank count. */
int Scan(alchemy::SdCard& sd, uint32_t now_ms, uint16_t runes[ECTO_RUNES]);

int  NumBanks();                 /* banks with at least one sample */
int  BankIndex(int nth);         /* nth existing bank -> bank number 0..15 */
int  NumSamples(int bank);       /* samples present in a bank (0..16) */
int  SampleSlot(int bank, int nth); /* nth present sample -> slot 0..15 */

/* Start loading bank/slot into dst (stereo int16 pairs, cap frames). */
bool Begin(int bank, int slot, int16_t* dst, uint32_t cap_frames);

/* Work for up to budget_ms. Returns the state after. */
State Step(uint32_t budget_ms);

State Status();
const Loaded& Result();          /* valid once Status() == Done */
const char* LastError();

} // namespace card
