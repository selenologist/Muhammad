// SPDX-License-Identifier: GPL-3.0-only
// Runtime factory-bank loader. Sequential's 200 v1.03 wire programs are NOT
// compiled into the binary (GPL-purity ruling): they live in one strippable
// sidecar, muhammad-factory.p5fac. A distributor deletes that single file and
// every GPL claim covers the artifact; the plugin then boots INIT, lists an
// empty factory bank, and every user patch/tuning keeps working.
//
// Threading: load() runs on the MAIN thread (plugin init, before activate()).
// The audio thread only reads raw() behind an acquire load of the ready count
// that the filler publishes with a release store - once published the bytes
// never change, so a voice event mid-run can safely copy from the blob.
#pragma once
#include <cstdint>

namespace p5fac {

// Fill from the first candidate (in order): $P5_FACTORY (if set it is
// AUTHORITATIVE: a bad path is a hard miss, no fallbacks - lets tests and
// operators force both sides), the sidecar next to the plugin module,
// module/../Resources/ (macOS bundle), and the user's patch dir (survives
// AU/VST3 wrapper embedding, where the module path is inside the wrapper).
// Idempotent; returns loaded(). Main thread only.
bool load();

// audio-safe readers
bool loaded();
int  count();                    // 0..200 (200 only when loaded)
const uint8_t *raw(int i);       // 133 bytes, valid iff 0 <= i < count()
const char *pathUsed();          // diagnostics; "" when unloaded

} // namespace p5fac
