// SPDX-License-Identifier: GPL-3.0-only
// CLAP plugin wrapper for the Prophet engine: params, events, state, preset load.
#pragma once
#include <clap/clap.h>
#include <clap/ext/latency.h>
#include <clap/ext/timer-support.h>
#include <cstdint>
#include <cstring>
#include <mutex>
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
constexpr clap_id PID_DRIFT = 86;      // VINTAGE re-scoped: per-voice thermal walk depth (byte 37)
constexpr uint32_t PARAM_COUNT = 58 + NUM_OVERLAYS + 7;   // FEEDBACK rides index 57

// id-space size: param ids are SPARSE (0..57, 64..71, 80..86), so
// every id-indexed bookkeeping array is sized to 96, NOT to PARAM_COUNT.
static constexpr uint32_t NIDS = 96;

struct Plugin {
    explicit Plugin(const clap_host_t *host) : host(host) {}

    const clap_host_t *host = nullptr;
    clap_plugin_t *self = nullptr;
    p5::Engine engine{p5::KernelMode::Auto};
    double sr = 44100.0;
    uint32_t minBlock = 0;                  // from activate (debug page)
    std::atomic<bool> active{false};
    std::atomic<float> trim{0.5f};   // headroom for stacked full-velocity voices (parameter 0..2)
    std::atomic<bool> latencyDirty{false};  // OS changed latency: request_restart at next main entry
    bool restartPending_ = false;
    int32_t scratch[32] = {0};
    // Bend / MIDI-performance state: touched ONLY from the audio thread (host
    // events + reset arrive there while active, on main while inactive - never
    // both). Plain fields are correct under that ownership.
    int bendRaw = 0;
    int program = -1;                   // factory program loaded (-1: edited/INIT)   // MAIN-owned:
    std::atomic<int> bank{0};           // audio reads it for PC mapping -> atomic
    std::atomic<int> programA_{-1};     // audio-side program mirror (echo guard)

    // ---- threaded discipline (patch/patchMain model) ------------------------
    // Main owns an AUTHORITATIVE copy of all persistent state (model_). While
    // active it is the ONLY writer of model_, and NEVER writes the engine:
    // every mutation bumps the generation, refreshes pushed_, and the audio
    // thread applies the LATEST whole-state snapshot at the top of process()
    // (mutex try-locked by audio; main blocks only on a microsecond copy).
    // The audio thread is the engine's sole writer while active; everything
    // IT changes (host params, CC, PC) flows back to main through the
    // end-of-block publish, merged into model_ per-field with generation
    // rules: a field main touched after the audio's last sync is main's.
    // Host flush/reset run ON THE AUDIO THREAD while active and apply there
    // directly - never through this machinery (review C1).
    struct Model
    {
        uint8_t raw[p5::RAW_MAX] = {};
        int16_t ov[8] = {};
        int16_t trimV[8][2][2] = {};                 // per-lane/osc scale+offset
        int16_t dev[128] = {};                       // tuning deviations
        int tune = 0, os = 1, pan = 0, wheel = 0, live = 0;
        float brUp = -1.f, brDn = -1.f;               // applied defaults (gui rails); -1 = unset -> wire
        bool brSet = false;                           // main explicitly set the bend-range default
        int rbrUp = -1, rbrDn = -1;                   // RAW stored bend defaults (state blob)
    };
    // Published snapshot: engine truth + the audio-side base/mod bookkeeping.
    struct Pub
    {
        uint8_t raw[p5::RAW_MAX] = {};
        int16_t ov[8] = {};
        int16_t trimV[8][2][2] = {};
        int16_t dev[128] = {};
        int tune = 0, os = 1, pan = 0, wheel = 0, live = 0;
        float brUp = -1.f, brDn = -1.f;
        bool brSet = false;
        int rbrUp = -1, rbrDn = -1;
        double base[NIDS] = {};                      // host-facing value (pre-modulation)
        double mod[NIDS] = {};                       // host modulation offsets
        uint32_t appliedGen = 0;                     // model generation the audio last applied
    };
    static constexpr uint32_t OQ_RING = 1024u;       // main->audio events for clap out_events
    enum OutKind : uint8_t { OK_BEGIN, OK_END, OK_VALUE };
    struct OutEv { uint8_t kind = 0; uint16_t id = 0; double v = 0.0; };
    struct Rep { double v = 0.0; uint32_t gen = 0; uint16_t id = 0; };  // reported base + generation

    Model model_ = {};                       // MAIN-ONLY authoritative state
    Pub pub_ = {};                           // AUDIO-published snapshot (mutex-guarded)
    Model pushed_ = {};                      // what main last staged for the audio thread
    uint32_t gen_ = 0;                       // main-side mutation counter
    std::atomic<uint32_t> loadGen_{0};       // bumped by full-patch LOAD semantics
    uint32_t rawGen_[p5::RAW_MAX] = {};      // last main mutation gen per patch byte
    uint32_t ovGen_[8] = {};
    uint32_t trimGen_[8][2][2] = {};
    uint32_t devGen_ = 0;
    uint32_t tuneGen_ = 0, osGen_ = 0, panGen_ = 0, brGen_ = 0;
    uint32_t lastMergeGen_ = 0;              // dedupe: merged this publish already
    bool pushedValid_ = false;               // under viewMx_ until the audio takes it
    std::atomic<bool> pushedDirty_{false};   // audio fast-path gate for stagePush
    std::atomic<uint32_t> pushedGen_{0};     // gen_ at stage time (audio tags its applies)
    uint32_t appliedGenCursor_ = 0;          // audio-private: gen the last apply consumed
    uint32_t appliedLoadGen_ = 0;            // audio-private: loadGen the last setPatch consumed

    // audio-thread base/mod bookkeeping while active (main reads the pub_ copies)
    double base_[NIDS] = {};
    double mod_[NIDS] = {};
    uint32_t modActive_ = 0;                 // count of nonzero offsets (hot-path gate)

    // main->audio output-event ring (drained into out_events by process/flush)
    OutEv outRing_[OQ_RING] = {};
    std::atomic<uint32_t> ow_{0}, or_{0};    // SPSC: main produces, audio consumes
    uint32_t outDropped_ = 0;                // (audio-private; surfaced via pub on demand)

    // audio->main report ring (host writes via process/flush): strict SPSC -
    // audio writes entry[rw] then publishes rw; main reads to rw and advances
    // rr. On overflow (main not draining), audio DROPS the report and raises
    // repLost_: main then force-re-merges the whole publish instead.
    Rep repRing_[256] = {};
    std::atomic<uint32_t> repW_{0};               // audio writes
    uint32_t repR_ = 0;                           // main consumes
    std::atomic<uint32_t> repRpub_{0};            // ...published for the audio overflow check
    std::atomic<bool> repLost_{false};

    double hostBase_[NIDS] = {};      // main mirror of pub_.base (get_value)

    std::mutex viewMx_;                              // pub_/pushed_ handoff: audio try-locks
                                                     // (never blocks), main blocks (µs copies)
    void copyEngineInto(Pub &v);                     // audio (under lock): engine -> snapshot
    void refreshView();                              // main: merge publish into model_, cheap
    void publishView();                              // audio: end-of-block
    void stagePush();                                // main: model_ -> pushed_ (under lock)
    void applyPushed();                              // audio: pushed_ -> engine (under lock)
    void applyModelToEngineDirect();                 // main, inactive/activate edges only
    void drainReports(uint32_t curGen);              // main: audio->main latest-value merge

    void postOut(OutKind k, clap_id id, double v);   // main: queue a host output event
    void drainOut(const clap_output_events_t *out);  // audio: queue -> out_events (+base sync)
    void reportParam(clap_id id, double baseVal);    // audio: coalesced main merge
    void emitResync();                               // audio: echo wire params to curOut_

    // process()/flush() input-list echo target (audio-only, null elsewhere)
    const clap_output_events_t *curOut_ = nullptr;

    // engine-owning-thread value application (audio while active, main while
    // inactive) - base+mod law, see muhammad_plugin.cpp
    void applyValue(clap_id id, double baseValue);
    void mirrorBaseFromEngine();              // activate edge: base_ <- engine truth
    void applyMod(clap_id id, double amount);

    // posting helpers: mutate model_ (generation-stamped), then stage.
    void postRaw(int idx, int v);
    void postOverlay(int i, int v);
    void postTune(int v);
    void postOS(int v);
    void postPan(int v);
    void postTrimPair(int lane, int osc, int sc, int of);
    void postTuning(const int16_t *dev);             // nullptr = reset 12-TET
    void postBendDefault(float up, float dn);
    void postLoad(const p5::Patch &p);               // LOAD semantics (voice fade, snap)
    void postReset();                                // main-thread reset (inactive only)

    // ---- patch store / bank-program retrieval (see p5_store.hpp) ----
    void programSlot(int slot);                       // resolve retrieval map -> load (MAIN thread)
    // Audio-thread MIDI PC may only memcpy precomputed bytes: the slot->raw
    // cache is built on the main thread. Double-buffered behind a seqlock-ish
    // odd/even generation: readers verify, and a torn/contended read defers
    // the program change ONE block (pendingPc_) instead of applying garbage.
    void rebuildSlotCache();
    uint8_t slotRaw_[PW_FACTORY_N][134] = {};
    uint8_t slotRawB_[PW_FACTORY_N][134] = {};
    int16_t slotCode_[PW_FACTORY_N] = {};             // -1 init, -2 default, -3 file, else factory idx
    int16_t slotCodeB_[PW_FACTORY_N] = {};
    std::atomic<int> cacheSel_{0};
    std::atomic<uint32_t> cacheGen_{0};               // odd = rebuild in flight
    std::atomic<bool> slotReady_{false};
    std::atomic<int> pendingPc_{-1};                  // audio: PC deferred by cache contention

    // Rev4 MIDI performance layer: CC7 volume * CC13 expression (linear,
    // default exactly 1.0f so untouched renders are bit-identical), CC64 damper.
    // AUDIO-thread-only fields (see bendRaw above).
    uint8_t rpnM_[16] = {127}, rpnL_[16] = {127};   // per-channel RPN select (deselected)
    int rpnV_[16] = {};                              // 14-bit RPN data entry value
    float ccVol = 1.0f, ccExp = 1.0f;
    uint16_t sustainCh_ = 0;                    // CC64 damper, PER MIDI CHANNEL (MPE-correct)
    uint8_t heldFlag_[16][128] = {}, heldNotes_[16][128] = {};
    int heldCnt2_[16] = {};

    // ---- MAIN-owned persistent identity / tuning (never engine state) ----
    char tuneName_[40] = "12-TET";
    double tuneTones_[64] = {};                      // wheel-editable scale (cumulative cents)
    int tuneToneCount_ = 0;
    int tuneRoot_ = 0;                               // scale root key position (0..11)
    uint16_t tuneMask_ = 0;                          // chromatic degree positions, 0 = walk (ALL)
    int tuneWalk_ = 0;                               // degrees>12: every key advances one degree
    bool hasSrc_ = false;
    uint8_t srcRaw_[134] = {};
    bool floatValid_ = false;                 // presetRecall() may toggle back once
    uint8_t floatSnap_[133] = {};
    std::string srcFile_;
    int lastProgClick_ = 0;
    const char *loadTag_ = "seed";            // which path last replaced the patch (diag)
    p5store::Source curSrc_;                   // identity of the loaded patch (picker highlight)
    void *gui = nullptr;                // owned by gui_x11.cpp when the panel is open

    // The loaded SOURCE (factory index or user file) is the "preset state" the
    // programmer PRESET button recalls; the LED floats lit while the panel
    // disagrees with it, exactly like the original's parameter floating.
    void maybePingLatency();                        // main entries: restart/changed at safe times
    void adoptPendingIdentity();
    std::atomic<int> pcReq_{-1};                      // requested slot from audio
    std::atomic<bool> idSync_{false};                 // applied on audio: main must adopt
    std::atomic<int> idSyncSlot_{-1};
    std::atomic<int> adoptMode_{0};               // 0: slot cache load; 1: audio loaded a program
    void programSlotLoad(const p5store::Source &, int slot);
    void loadSource(const p5store::Source &s);                  // picker row click: LOAD ONLY
    void recallBasicInit();                           // hold-RECORD+PRESET: bare INIT
    void syncAllParams();
    bool loadUserPatch(const std::string &name);      // false if missing
    bool loadUserPatchExact(const std::string &file); // exact filename, no sanitize
    void savePatchNamed(const std::string &name);     // writes file + becomes source
    void assignSlotFromText(int slot, const std::string &textRaw);
    bool panelDirty()
    {   if (!hasSrc_) return false;
        return std::memcmp(model_.raw, srcRaw_, 134) != 0; }
    void presetRecall();                              // PRESET: back to loaded source
    const std::string &currentFile() const { return srcFile_; }
    void markSource(int kind, int idx, const std::string &file);  // after any load
    void markHostDirty();                     // host_state->mark_dirty on panel edits
    int assignTarget() const { return bank * 8 + (program >= 0 ? program % 8 : lastProgClick_); }
    void noteProgramClick(int p8) { lastProgClick_ = p8 & 7; }

    // GUI-initiated param changes queued for the host as out_events, latest-
    // value-per-id (the audio drain coalesces per block). Gesture begin/end
    // wraps every drag so hosts can record automation and group undo.
    static constexpr int PEND_IDS = (int)NIDS;
    bool dragOpen_[PEND_IDS] = {};                // gesture BEGIN sent, END owed
    void markPending(clap_id id, double v) { postOut(OK_VALUE, id, v); }
    void beginGesture(clap_id id);
    void endGesture(clap_id id);
    // dirty-state / repaint propagation: bumped by every main-side mutation
    // and merged publish (gui shims repaint on movement only).
    std::atomic<uint32_t> changeTick_{1};

    // background main-thread timer (adopt identity + latency announce even
    // with the panel closed; opportunistic on hosts that never fire timers)
    clap_id bgTimerId_ = 0;
    bool bgTimerOn_ = false;

    // ---- gui_core facades (never let the panel touch the engine directly)
    int  rawOfView(int idx);
    void trimPair(int lane, int osc, int *sc, int *of)
    {   *sc = model_.trimV[lane][osc][0]; *of = model_.trimV[lane][osc][1]; }
    void setTrimPair(int lane, int osc, int sc, int of) { postTrimPair(lane, osc, sc, of); }
    void tuningCopy(int16_t out[128]) { std::memcpy(out, model_.dev, 256); }
    void setTuning(const int16_t *dev) { postTuning(dev); }
    float bendDefault(int which) { return which ? model_.brDn : model_.brUp; }
    void setBendDefault(float up, float dn) { postBendDefault(up, dn); }
    void nameBytes(char *dst20) { std::memcpy(dst20, model_.raw + p5::P5_NAME, 20); }

    void applyRaw(int idx, int v);
    int rawOf(int idx);
    bool knobPolarity(clap_id id);
    void togglePolarity(int rawIdx)   // right-click: bit 7 flips the sign (dial white)
    {   postRaw(rawIdx, rawOf(rawIdx) ^ 128); }
    double guiValue(clap_id id);                    // same units as par_get_value
    void guiSetLocal(clap_id id, double v);         // apply + queue output value (+BEGIN)
    void guiSet(clap_id id, double v);              // apply + queue output value + host sync
    void guiSync(clap_id id);                       // push current value once (gesture end)
    void loadProgram(int k);
    static const char *nameOf(clap_id id);
    static double defaultOf(clap_id id);            // clap_param_info_t.default_value
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
