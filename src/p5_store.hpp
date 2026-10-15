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
    int factory = 0;                    // 0..PW_FACTORY_N-1 when Factory
    std::string file;                   // patch name (no dir/extension) when File
};

std::string baseDir();                  // .../Documents/Muhammad Synth (created lazily)
bool ensureDir();

std::string sanitize(const std::string &name);
std::string patchPath(const std::string &name);         // baseDir/<NAME>.muhammad
std::vector<std::string> listPatches();                 // sorted, name without extension

bool savePatch(const std::string &name, const uint8_t raw[133]);
bool loadPatch(const std::string &name, uint8_t raw[133]);

// bank/program retrieval map (persisted in baseDir/bank-program.map)
void setSlot(int slot, const Source &src);             // slot = bank*8 + program
bool hasSlot(int slot);
Source slot(int slot);                                  // default: Factory{slot}
void clearSlot(int slot);
bool loadMap();
bool saveMap();

} // namespace p5store
