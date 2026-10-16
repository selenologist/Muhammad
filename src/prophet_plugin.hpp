// SPDX-License-Identifier: GPL-3.0-only
// CLAP plugin wrapper for the Prophet engine: params, events, state, preset load.
#pragma once
#include <clap/clap.h>
#include <clap/ext/latency.h>
#include <clap/ext/timer-support.h>
#include <cstring>
#include <string>
#include <atomic>

#include "p5_engine.hpp"
#include "p5_store.hpp"

namespace pw {

// param ids: 0..56 = native wire patch knob bytes (0..255, exact raw semantics)
//            64..71 = the engine editor overlays P_E0..P_E7 (signed)
//            80     = output trim
constexpr int NUM_OVERLAYS = 8;
constexpr clap_id PID_TRIM = 80;
constexpr clap_id PID_OS = 81;      // 0 = base rate (bit-exact), 1 = 2x oversample + Surge halfband
constexpr clap_id PID_TUNE = 82;    // master tune, -100..100 cents (control-path pitch offset)
constexpr clap_id PID_PROGRAM = 83; // factory program 0..N-1; writing loads it (panel buttons)
constexpr clap_id PID_BANK = 84;    // factory bank 0..N/8-1 (BANK SELECT cycles it)
constexpr clap_id PID_PAN_WIDTH = 85;  // port extension: Oberheim-style per-voice stereo spread    // factory bank 0..N/8-1 (BANK SELECT cycles it)

struct Plugin {
    explicit Plugin(const clap_host_t *host) : host(host) {}

    const clap_host_t *host = nullptr;
    clap_plugin_t *self = nullptr;
    p5::Engine engine{p5::KernelMode::Auto};
    double sr = 44100.0;
    bool active = false;
    float trim = 0.5f;   // headroom for stacked full-velocity voices (parameter 0..2)
    int32_t scratch[32] = {0};
    int bendRaw = 0;
    int program = -1;                   // factory program loaded (-1: edited/INIT)
    uint8_t slotRaw_[PW_FACTORY_N][133] = {};
    int16_t slotCode_[PW_FACTORY_N];                  // -1 init, -2 default, -3 file, else factory idx
    bool slotReady_ = false;
    bool hasSrc_ = false;
    uint8_t srcRaw_[133] = {};
    bool floatValid_ = false;                 // presetRecall() may toggle back once
    uint8_t floatSnap_[133] = {};
    std::string srcFile_;
    int lastProgClick_ = 0;
    const char *loadTag_ = "seed";           // which path last replaced the patch (diag)
    p5store::Source curSrc_;                   // identity of the loaded patch (picker highlight)
    int bank = 0;                       // panel bank cursor
    void *gui = nullptr;                // owned by gui_x11.cpp when the panel is open
    bool latencyDirty = false;          // OS changed latency while active: report on deactivate

    // ---- patch store / bank-program retrieval (see p5_store.hpp) ----
    // The loaded SOURCE (factory index or user file) is the "preset state" the
    // programmer PRESET button recalls; the LED floats lit while the panel
    // disagrees with it, exactly like the original's parameter floating.
    void programSlot(int slot);                       // resolve retrieval map -> load (MAIN thread)
    // Audio-thread MIDI PC may only memcpy precomputed bytes: the slot->raw
    // cache is built on the main thread (activate / map edits / saves), the
    // process() drain applies it, and the GUI tick adopts identity + syncs.
    void rebuildSlotCache();
    void adoptPendingIdentity();
    std::atomic<int> pcReq_{-1};                      // requested slot from audio
    std::atomic<bool> idSync_{false};                 // applied on audio: main must adopt
    int idSyncSlot_ = -1;
    void programSlotLoad(const p5store::Source &, int slot);
    void loadSource(const p5store::Source &s);                  // picker row click: LOAD ONLY
    void recallBasicInit();                           // hold-RECORD+PRESET: bare INIT
    void syncAllParams();
    bool loadUserPatch(const std::string &name);      // false if missing
    void savePatchNamed(const std::string &name);     // writes file + becomes source
    void assignSlotFromText(int slot, const std::string &text);
    bool panelDirty() const { return hasSrc_ && std::memcmp(engine.patch().raw, srcRaw_, 133) != 0; }
    void presetRecall();                              // PRESET: back to loaded source
    const std::string &currentFile() const { return srcFile_; }
    void markSource(int kind, int idx, const std::string &file);  // after any load
    int assignTarget() const { return bank * 8 + (program >= 0 ? program % 8 : lastProgClick_); }
    void noteProgramClick(int p8) { lastProgClick_ = p8 & 7; }
    // GUI-initiated param changes, latest-value-per-id (coalesced): a fast
    // drag must never queue a catch-up replay behind the host.
    static constexpr int PEND_IDS = 96;
    double pendVal[PEND_IDS] = {};
    bool pendSet[PEND_IDS] = {};
    void markPending(clap_id id, double v) { if (id < PEND_IDS) { pendVal[id] = v; pendSet[id] = true; } }

    void applyRaw(int idx, int v);
    int rawOf(int idx) const;
    double guiValue(clap_id id);                    // same units as par_get_value
    void guiSetLocal(clap_id id, double v);         // engine only (drag in progress)
    void guiSet(clap_id id, double v);              // apply + one host sync event
    void guiSync(clap_id id);                       // push current value once (gesture end)
    void loadProgram(int k);
    static const char *nameOf(clap_id id);
};

extern const clap_plugin_audio_ports_t kAudioPorts;
extern const clap_plugin_note_ports_t kNotePorts;
extern const clap_plugin_params_t kParams;
extern const clap_plugin_state_t kState;
extern const clap_plugin_preset_load_t kPresetLoad;
extern const clap_plugin_latency_t kLatency;
extern const clap_plugin_descriptor_t kDescriptor;

// GUI tables, defined in src/gui_x11.cpp (compiled when P5_GUI is set).
extern const clap_plugin_gui_t kGuiX11;
extern const clap_plugin_timer_support_t kTimerSupport;

const clap_plugin_t *prophet_create(const clap_host_t *host);

} // namespace pw
