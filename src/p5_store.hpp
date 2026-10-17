// SPDX-License-Identifier: GPL-3.0-only
// Patch files are where patches really live: Documents/Muhammad Synth/*.muhammad
// (133 raw wire bytes + name inside). The bank/program grid is only a retrieval
// map — assignable to a factory patch or a user file, and it is what MIDI
// program change (and CC0 bank select) resolves through.
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace p5store {

constexpr int kSlotBanks = 25;          // 200 factory programs / 8 per bank
constexpr int kSlotsPerBank = 8;

enum class SourceKind { Factory, File };
struct Source {
    SourceKind kind = SourceKind::Factory;
    int factory = -9999;                // 0..PW_FACTORY_N-1 when bound; sentinel default never matches a slot
    std::string file;                   // patch name (no dir/extension) when File
};

std::string baseDir();                  // .../Documents/Muhammad Synth (created lazily)
bool ensureDir();

std::string sanitize(const std::string &name);
std::string patchPath(const std::string &name);         // baseDir/<NAME>.muhammad
std::vector<std::string> listPatches();

// Tunings (microtonal scale/keyboard files, Surge tuning-library formats).
// Files live in <base>/Tunings; a .scl paired with a same-stem .kbm is loaded
// together. dev16 is per-MIDI-key deviation from 12-TET in 1/16-semitone.
std::string tuningsDir();
std::vector<std::string> listTunings();
bool loadTuningDev(const std::string &fileName, int16_t *dev16, char *nameOut, int nameCap,
                   double *tonesOut = nullptr, int *tonesN = nullptr);

// Surge-style polar editor round trip: tones[] are cumulative cents of degrees
// 1..n (the last entry is the octave). Rebuild the per-key deviation table from
// an edited scale. kMaxTones bounds editable scale complexity; bigger files load
// for playback but the wheel starts from the 12-TET grid.
constexpr int kMaxTones = 64;
bool buildScaleTones(const double *tones, int n, int16_t *dev16);                 // sorted, name without extension

bool savePatch(const std::string &name, const uint8_t raw[133]);
// Case-insensitive collision on purpose: filenames stay faithful to the typed
// case, but "peace" and "PEACE" must confirm as an overwrite even on a
// case-sensitive fs, because they collide the moment the folder is copied.
std::string foldCollision(const std::string &name);   // existing name or ""
bool loadPatch(const std::string &name, uint8_t raw[133]);

// bank/program retrieval map (persisted in baseDir/bank-program.map)
void setSlot(int slot, const Source &src);             // slot = bank*8 + program
bool hasSlot(int slot);
Source slot(int slot);                                  // default: Factory{slot}
void clearSlot(int slot);
bool findSlotFor(const Source &s, int &slot);        // first bound slot, or false
bool loadMap();
bool saveMap();

} // namespace p5store
