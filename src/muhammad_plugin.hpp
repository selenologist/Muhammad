// SPDX-License-Identifier: GPL-3.0-only
// CLAP plugin wrapper for the Prophet engine: params, events, state, preset load.
#pragma once
#include <clap/clap.h>
#include <clap/ext/latency.h>
#include <clap/ext/timer-support.h>
#include <chrono>
#include <mutex>
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
constexpr clap_id PID_PAN_WIDTH = 85;  // port extension: Oberheim-style per-voice stereo spread
constexpr clap_id PID_DRIFT = 86;      // VINTAGE re-scoped: per-voice thermal walk depth (byte 37 retired)    // factory bank 0..N/8-1 (BANK SELECT cycles it)

struct Plugin {
    explicit Plugin(const clap_host_t *host) : host(host) {}

    const clap_host_t *host = nullptr;
    clap_plugin_t *self = nullptr;
    p5::Engine engine{p5::KernelMode::Auto};
    double sr = 44100.0;
    uint32_t minBlock = 0;                  // from activate (debug page)
    std::atomic<bool> active{false};
    std::atomic<float> trim{0.5f};   // headroom for stacked full-velocity voices (parameter 0..2)
    std::atomic<bool> latencyDirty{false};   // OS changed latency while active: report on deactivate
    int32_t scratch[32] = {0};
    int bendRaw = 0;
    int program = -1;                   // factory program loaded (-1: edited/INIT)   // MAIN-owned:
    std::atomic<int> bank{0};           // audio reads it for PC mapping -> atomic
    std::chrono::steady_clock::time_point stallSince_{};   // postCmd fallback timer (main-only)
    std::atomic<int> adoptMode_{0};     // 0: idSyncSlot_ is a slot; 1: audio loaded a program
    std::atomic<int> programA_{-1};     // audio-side program mirror (echo guard)
    // Double-buffered slot cache: rebuilds happen on MAIN while audio may be
    // mid-applyPcFromCache; writers fill the back buffer, then flip the index.
    uint8_t slotRaw_[PW_FACTORY_N][134] = {};
    uint8_t slotRawB_[PW_FACTORY_N][134] = {};
    int16_t slotCode_[PW_FACTORY_N];                    // -1 init, -2 default, -3 file, else factory idx
    int16_t slotCodeB_[PW_FACTORY_N];
    std::atomic<int> cacheSel_{0};
    std::atomic<bool> slotReady_{false};
    // Rev4 MIDI performance layer: CC7 volume * CC13 expression (linear,
    // default exactly 1.0f so untouched renders are bit-identical), CC64 damper.
    uint8_t rpnM_[16] = {127}, rpnL_[16] = {127};   // per-channel RPN select (deselected)
    int rpnV_[16] = {};                              // 14-bit RPN data entry value
    char tuneName_[40] = "12-TET";
    double tuneTones_[64] = {};                      // wheel-editable scale (cumulative cents)
    int tuneToneCount_ = 0;
    int tuneRoot_ = 0;                               // scale root key position (0..11)
    uint16_t tuneMask_ = 0;                          // chromatic degree positions, 0 = walk (ALL)
    int tuneWalk_ = 0;                               // degrees>12: every key advances one degree
    float ccVol = 1.0f, ccExp = 1.0f;
    bool sustain_ = false;
    uint8_t heldFlag_[128] = {}, heldNotes_[128] = {};
    int heldCnt_ = 0;
    bool hasSrc_ = false;
    uint8_t srcRaw_[134] = {};
    bool floatValid_ = false;                 // presetRecall() may toggle back once
    uint8_t floatSnap_[133] = {};
    std::string srcFile_;
    int lastProgClick_ = 0;
    const char *loadTag_ = "seed";           // which path last replaced the patch (diag)
    p5store::Source curSrc_;                   // identity of the loaded patch (picker highlight)
    void *gui = nullptr;                // owned by gui_x11.cpp when the panel is open

    // ---- patch store / bank-program retrieval (see p5_store.hpp) ----
    // The loaded SOURCE (factory index or user file) is the "preset state" the
    // programmer PRESET button recalls; the LED floats lit while the panel
    // disagrees with it, exactly like the original's parameter floating.
    void programSlot(int slot);                       // resolve retrieval map -> load (MAIN thread)
    // Audio-thread MIDI PC may only memcpy precomputed bytes: the slot->raw
    // cache is built on the main thread (activate / map edits / saves), the
    // process() drain applies it, and the GUI tick adopts identity + syncs.
    void rebuildSlotCache();
    // GUI-thread (pump) hook: announce deferred latency changes ONLY from a
    // main-thread context. Never from deactivate (host is tearing the graph
    // down around us) and never mid-process.
    void maybePingLatency();
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
    bool panelDirty()
    {   if (!hasSrc_) return false;
        refreshView();
        return std::memcmp(view_.raw, srcRaw_, 134) != 0; }
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

    // ---- threaded discipline (the engine is written ONLY by the audio
    // thread while active). Main-thread mutations ride a lock-free command
    // ring drained at the top of process(); main reads a seqlock view the
    // audio thread publishes at the end of every block. While inactive,
    // main owns the engine outright and every post applies directly.
    struct View
    {
        uint8_t raw[p5::RAW_MAX] = {};
        int16_t ov[8] = {};
        int16_t trimV[8][2][2] = {};                 // per-lane/osc scale+offset
        int16_t dev[128] = {};                       // tuning deviations
        int tune = 0, os = 1, pan = 0, drift = 0, wheel = 0, live = 0;
        float brUp = 0.f, brDn = 0.f;                // applied defaults (gui rails)
        int rbrUp = 2, rbrDn = 2;                    // RAW stored bend defaults (state blob)
    };
    struct Cmd
    {
        uint8_t op = 0; int32_t i0 = 0, i1 = 0, i2 = 0, i3 = 0;
        double d0 = 0.0, d1 = 0.0; uint8_t ev[64] = {};
        uint8_t pl[256] = {};                    // payload: LOAD 134 bytes, TUNING 256
    };
    enum : uint8_t { OP_RAW, OP_OVERLAY, OP_TUNE, OP_OS, OP_PAN, OP_TRIM,
                     OP_TUNING, OP_BRDEF, OP_LOAD, OP_RESET, OP_EVENT };
    static constexpr uint32_t RINGN = 256u;
    Cmd ring_[RINGN] = {};
    std::atomic<uint32_t> rw_{0}, rr_{0};            // rw: main writes; rr: audio
    View view_ = {}, pub_ = {};                      // main-private / audio-published
    std::mutex viewMx_;                              // pub_ handoff: audio try-locks
                                                     // (never blocks), main blocks

    void copyEngineInto(View &v);
    void refreshView();                              // main: current snapshot
    void publishView();                              // audio: end-of-block
    bool postCmd(const Cmd &c);                      // main: SPSC push (see impl re: stalls)
    void drainCmds();                                // consumer: apply everything queued
    void execCmd(const Cmd &c);
    // posting helpers: mutate view_ optimistically, then enqueue the FINAL value.
    void postRaw(int idx, int v);
    void postOverlay(int i, int v);
    void postTune(int v);
    void postOS(int v);
    void postPan(int v);
    void postTrimPair(int lane, int osc, int sc, int of);
    void postTuning(const int16_t *dev);             // nullptr = reset 12-TET
    void postBendDefault(float up, float dn);
    void postLoad(const p5::Patch &p);
    void postReset();

    // ---- gui_core facades (never let the panel touch the engine directly)
    int  rawOfView(int idx);
    void trimPair(int lane, int osc, int *sc, int *of)
    {   refreshView(); *sc = view_.trimV[lane][osc][0]; *of = view_.trimV[lane][osc][1]; }
    void setTrimPair(int lane, int osc, int sc, int of) { postTrimPair(lane, osc, sc, of); }
    void tuningCopy(int16_t out[128]) { refreshView(); std::memcpy(out, view_.dev, 256); }
    void setTuning(const int16_t *dev) { postTuning(dev); }
    float bendDefault(int which) { refreshView(); return which ? view_.brDn : view_.brUp; }
    void setBendDefault(float up, float dn) { postBendDefault(up, dn); }
    void nameBytes(char *dst20) { refreshView(); std::memcpy(dst20, view_.raw + p5::P5_NAME, 20); }

    void applyRaw(int idx, int v);
    int rawOf(int idx);
    double guiValue(clap_id id);                    // same units as par_get_value
    void guiSetLocal(clap_id id, double v);         // engine only (drag in progress)
    void togglePolarity(int rawIdx)   // right-click: bit 7 flips the sign (dial white)
    {   const int cur = rawOf(rawIdx);              // view (main) / engine (inactive)
        postRaw(rawIdx, cur ^ 128); }
    bool knobPolarity(clap_id id);    // dial-shape: bit 7 set on a polarity knob
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
extern const clap_plugin_gui_t kGui;      // defined by the platform shim (gui_x11/gui_win32)
extern const clap_plugin_timer_support_t kTimerSupport;

const clap_plugin_t *muhammad_create(const clap_host_t *host);

} // namespace pw
