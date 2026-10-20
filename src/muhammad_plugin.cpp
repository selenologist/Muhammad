// SPDX-License-Identifier: GPL-3.0-only
// CLAP plugin wrapper for the Muhammad engine (see muhammad_plugin.hpp).
#include "muhammad_plugin.hpp"
#include "p5_store.hpp"
#include "p5_default.hpp"   // embedded boot patch (tools/gen_default_hdr.py)

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <thread>
#include <cctype>
#if defined(__SSE__) || defined(__SSE2__)
#include <xmmintrin.h>          // MXCSR FTZ/DAZ: denormals must never tax the audio thread
#endif

namespace pw {

// ---- parameter model ---------------------------------------------------------
static const char *kRawNames[58] = {
    "OSC A COARSE", "OSC B COARSE", "OSC B FINE", "OSC A SAW", "OSC A PULSE",
    "OSC B SAW", "OSC B TRI", "OSC B PULSE", "OSC A PULSE W", "OSC B PULSE W",
    "SYNC", "OSC B LOW", "OSC B KEYBD", "GLIDE", "OSC A LEVEL", "OSC B LEVEL",
    "NOISE", "CUTOFF", "RESONANCE", "KEYBD>FLT", "FILTER MODEL",
    "LFO RATE", "LFO INIT", "LFO SAW", "LFO TRI", "LFO PULSE",
    "WHEEL MIX", "WHEEL>OSCA", "WHEEL>OSCB", "PW WHEEL A", "PW WHEEL B",
    "WHEEL>FLT", "POLYMOD AMT", "POLYMOD B", "POLYMOD>OSCA", "POLYMOD>PW",
    "POLYMOD>FLT", "VINTAGE", "PRESS>FLT", "PRESS>LFO", "ENV>FLT",
    "VEL>FLT", "VEL>AMP", "FLT ATTACK", "AMP ATTACK", "FLT DECAY", "AMP DECAY",
    "FLT SUSTAIN", "AMP SUSTAIN", "FLT RELEASE", "AMP RELEASE", "RELEASE ON",
    "UNISON", "UNISON CNT", "UNISON DET",
    "BEND RANGE", "RETRIGGER", "FEEDBACK"
};
static const int kRawIndex[58] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 86, 87, 133
};
static const char *kOverlayNames[8] = {
    "EDIT CUTOFF", "EDIT RESO", "EDIT TUNE A", "EDIT TUNE B",
    "EDIT PW A", "EDIT PW B", "EDIT MIX A", "EDIT MIX B"
};
static const double kOverlayMin[8] = { -63, -63, -24, -24, -63, -63, -63, -63 };
static const double kOverlayMax[8] = {  63,  63,  24,  24,  63,  63,  63,  63 };


static bool isRawParam(clap_id id) { return id < 58; }

// Effective ranges (shared with the GUI/golden normalizer in p5_common.hpp),
// so host display, engine behaviour and the panel render cannot drift.
using KnobClamp = p5::KnobRange;
static const KnobClamp *knobClamp(clap_id id) { return p5::knobRange((int)id); }

static bool isOverlayParam(clap_id id) { return id >= 64 && id < 64 + NUM_OVERLAYS; }

// clap_param_info_t.default_value: the INIT patch byte is the ground-truth
// default for every wire knob (review 5.7: without defaults, host "reset to
// default" sent TRIM to 0 = silence and coarse tuning to -12 semitones).
double Plugin::defaultOf(clap_id id)
{
    static uint8_t initRaw[p5::RAW_MAX];
    static bool built = false;
    if (!built)
    {   p5::Patch p; p5::patchInit(p);
        std::memcpy(initRaw, p.raw, sizeof initRaw);
        built = true; }
    if (isRawParam(id))
    {   int v = initRaw[kRawIndex[id]];
        if (p5::polarityByte(kRawIndex[id])) v &= 127;
        if (const KnobClamp *k = knobClamp(id)) { if (v > (int)k->hi) v = (int)k->hi; if (v < (int)k->lo) v = (int)k->lo; }
        else if (v > 127) v = 127;
        return (double)v;
    }
    if (isOverlayParam(id)) return 0.0;
    if (id == PID_TRIM) return 0.5;
    if (id == PID_DRIFT) { int v = initRaw[p5::P5_VINTAGE]; return v > 127 ? 127 : v; }
    return 0.0;
}

const char *Plugin::nameOf(clap_id id)
{
    if (isRawParam(id)) return kRawNames[id];
    if (isOverlayParam(id)) return kOverlayNames[id - 64];
    if (id == PID_TRIM) return "OUTPUT";
    if (id == PID_OS) return "OVERSAMPLE 2X";
    if (id == PID_TUNE) return "MASTER TUNE";
    if (id == PID_PROGRAM) return "PROGRAM";
    if (id == PID_BANK) return "BANK";
    if (id == PID_PAN_WIDTH) return "PAN WIDTH";
    if (id == PID_DRIFT) return "DRIFT";
    return "?";
}

static void dispatchEvent(Plugin *s, const clap_event_header_t *ev);   // fwd
static void applyPcFromCache(Plugin *s, int slot);   // fwd: process() pending-PC drain

void Plugin::applyRaw(int idx, int v)
{
    if (idx < 0 || idx >= (int)p5::RAW_MAX) return;
    postRaw(idx, v < 0 ? 0 : v > 255 ? 255 : v);
}
int Plugin::rawOf(int idx)
{   return (idx >= 0 && idx < (int)p5::RAW_MAX) ? model_.raw[idx] : 0; }
int Plugin::rawOfView(int idx) { return rawOf(idx); }

// ---- threaded discipline (patch/patchMain) -----------------------------------
// Main owns model_ (the authoritative persistent state) and never writes the
// engine while active; it stages snapshots the audio thread pulls at block
// top. The audio thread owns the engine and the host-facing base_/mod_
// bookkeeping; what hosts change flows back through coalesced per-id reports
// merged into model_ under a generation rule (a byte main wrote after the
// audio's last apply stays main's - the newest intent wins, both directions).
// Host flush/reset run on the AUDIO thread while active and apply directly
// there - never through main's machinery (review C1). While inactive, main is
// both threads: model_, engine and the atomics move together.

void Plugin::copyEngineInto(Pub &v)
{
    std::memcpy(v.raw, engine.patch().raw, sizeof v.raw);
    for (int i = 0; i < 8; i++) v.ov[i] = (int16_t)engine.overlay(i);
    for (int l = 0; l < 8; l++)
        for (int o = 0; o < 2; o++)
        {   int sc, of; engine.voiceTrim(l, o, &sc, &of);
            v.trimV[l][o][0] = (int16_t)sc; v.trimV[l][o][1] = (int16_t)of; }
    std::memcpy(v.dev, engine.tuningDeviation(), sizeof v.dev);
    v.tune = engine.tuneCents(); v.os = engine.oversample();
    v.pan = engine.panWidth();   v.live = engine.liveVoices();
    v.wheel = engine.wheelPos();
    v.brUp = (float)engine.bendRangeUpDefault(); v.brDn = (float)engine.bendRangeDownDefault();
    v.rbrUp = engine.rawBendRangeUpDefault(); v.rbrDn = engine.rawBendRangeDownDefault();
    v.brSet = engine.rawBendRangeUpDefault() >= 0 || model_.brSet;   // resolved-def is authoritative once set
    std::memcpy(v.base, base_, sizeof v.base);
    std::memcpy(v.mod, mod_, sizeof v.mod);
    v.appliedGen = appliedGenCursor_;
}

void Plugin::publishView()
{   std::unique_lock<std::mutex> lk(viewMx_, std::try_to_lock);
    if (!lk.owns_lock()) return;      // main is mid-copy: skip this block
    copyEngineInto(pub_);             // (never blocks the audio thread)
}

// Re-merge pub_/rep reports into model_. Called at main-thread entries
// (get_value, frame start, save/load, activate edges). The lock is held by
// audio only for a microsecond copy, so main blocking here is bounded.
void Plugin::refreshView()
{
    if (!active.load(std::memory_order_acquire)) return;   // inactive: model IS truth
    const Pub p = [this] { std::lock_guard<std::mutex> lk(viewMx_); Pub t = pub_; return t; }();
    std::memcpy(hostBase_, p.base, sizeof p.base);
    if (p.appliedGen == lastMergeGen_) { drainReports(p.appliedGen); return; }
    lastMergeGen_ = p.appliedGen;
    // Engine-side persistent fields merged ONLY where main has not written
    // since the audio's last applied generation: main's edit is the newer
    // truth (it is also already staged for the engine).
    const uint32_t g = p.appliedGen;
    bool touched = false;
    for (int i = 0; i < (int)p5::RAW_MAX; i++)
        if (rawGen_[i] <= g && model_.raw[i] != p.raw[i]) { model_.raw[i] = p.raw[i]; touched = true; }
    for (int o = 0; o < 8; o++)
        if (ovGen_[o] <= g && model_.ov[o] != p.ov[o]) { model_.ov[o] = p.ov[o]; touched = true; }
    for (int l = 0; l < 8; l++) for (int o = 0; o < 2; o++) for (int k = 0; k < 2; k++)
        if (trimGen_[l][o][k] <= g && model_.trimV[l][o][k] != p.trimV[l][o][k])
        { model_.trimV[l][o][k] = p.trimV[l][o][k]; touched = true; }
    if (devGen_ <= g && std::memcmp(model_.dev, p.dev, 256) != 0)
    {   std::memcpy(model_.dev, p.dev, 256); touched = true; }
    if (tuneGen_ <= g) model_.tune = p.tune;
    if (osGen_ <= g)   model_.os = p.os;
    if (panGen_ <= g)  model_.pan = p.pan;
    if (brGen_ <= g)
    {   model_.brUp = p.brUp; model_.brDn = p.brDn;
        model_.rbrUp = p.rbrUp; model_.rbrDn = p.rbrDn; }
    model_.live = p.live; model_.wheel = p.wheel;    // display-only
    drainReports(g);
    if (touched) changeTick_.fetch_add(1, std::memory_order_relaxed);
}

// audio->main coalesced reports: the LATEST value per id, no ordering needed
// (each is the full base the engine now holds; generation rule as in pub).
void Plugin::reportParam(clap_id id, double baseVal)
{   // audio thread: SPSC producer. Write the slot, then publish the index.
    if (id >= (clap_id)NIDS) return;
    const uint32_t w = repW_.load(std::memory_order_relaxed);
    if (w - repRpub_.load(std::memory_order_acquire) >= 256u)
    {   repLost_.store(true, std::memory_order_release); return; }
    repRing_[w & 255u] = Rep{ baseVal, appliedGenCursor_, (uint16_t)id };
    repW_.store(w + 1u, std::memory_order_release);
}

void Plugin::drainReports(uint32_t curGen)
{
    (void)curGen;
    if (repLost_.exchange(false, std::memory_order_acq_rel))
        lastMergeGen_ = 0;                       // reports were dropped: full re-merge
    const uint32_t w = repW_.load(std::memory_order_acquire);
    for (; repR_ != w; repR_++)
    {   const Rep r = repRing_[repR_ & 255u];
        const clap_id id = (clap_id)r.id;
        if (id < (clap_id)NIDS) hostBase_[id] = r.v;
        if (isRawParam(id))
        {   const int idx = kRawIndex[id];
            if (rawGen_[idx] <= r.gen)
            {   int v = (int)(r.v < 0 ? 0 : r.v > 255 ? 255 : r.v + 0.5);
                if (p5::polarityByte(idx)) v = (model_.raw[idx] & 128) | (v & 127);
                if (model_.raw[idx] != (uint8_t)v)
                {   model_.raw[idx] = (uint8_t)v; changeTick_.fetch_add(1, std::memory_order_relaxed); } } }
        else if (id == PID_TUNE && tuneGen_ <= r.gen) model_.tune = (int)r.v;
        else if (id == PID_OS && osGen_ <= r.gen)    model_.os = ((int)r.v > 0 ? 2 : 1);
        else if (id == PID_PAN_WIDTH && panGen_ <= r.gen) model_.pan = (int)r.v;
        else if (id == PID_DRIFT)
        {   const int idx = p5::P5_VINTAGE;
            if (rawGen_[idx] <= r.gen) model_.raw[idx] = (uint8_t)(r.v < 0 ? 0 : r.v > 127 ? 127 : r.v + 0.5); }
        repRpub_.store(repR_ + 1u, std::memory_order_release);
    }
}

static void requestHostParamFlush(Plugin *s);   // fwd (defined with the param paths)

// ---- state mailbox (main -> audio) --------------------------------------------
// Latest-WHOLE-STATE semantics: every main mutation stages the full model and
// the audio applies the newest snapshot when it can. Coalescing is structural
// (no per-motion queue, review H2): a stalled host cannot overflow anything,
// and main NEVER executes engine code while active.
void Plugin::stagePush()
{   std::lock_guard<std::mutex> lk(viewMx_);
    pushed_ = model_;
    pushedGen_.store(gen_, std::memory_order_relaxed);
    pushedValid_ = true;
    pushedDirty_.store(true, std::memory_order_release);
}

// Which param id (if any) addresses a patch byte as its BASE value.
static int paramIdForByte(int idx)
{   for (int id = 0; id < 58; id++) if (kRawIndex[id] == idx) return id;
    return -1; }

void Plugin::applyPushed()
{   // audio thread, top of every block. The lock is held by main only for a
    // microsecond snapshot copy, so try_lock failing is a one-block delay,
    // never a stall.
    if (!pushedDirty_.load(std::memory_order_acquire)) return;
    Model m; uint32_t pg;
    {   std::unique_lock<std::mutex> lk(viewMx_, std::try_to_lock);
        if (!lk.owns_lock()) return;
        if (!pushedValid_) { pushedDirty_.store(false, std::memory_order_relaxed); return; }
        m = pushed_; pg = pushedGen_.load(std::memory_order_relaxed);
        pushedValid_ = false;
        pushedDirty_.store(false, std::memory_order_relaxed);
    }
    appliedGenCursor_ = pg;
    const uint32_t lg = loadGen_.load(std::memory_order_relaxed);
    if (lg != appliedLoadGen_)
    {   // a full LOAD happened on main: whole-patch semantics (the voices
        // fade under E6, knob state snaps under E2, the wheel re-seeds).
        appliedLoadGen_ = lg;
        p5::Patch p; std::memset(&p, 0, sizeof p);
        std::memcpy(p.raw, m.raw, 134);
        p.size = 133; p.model = 0x32; p.command = 3;
        engine.setPatch(p);
    }
    else
    {   const uint8_t *cur = engine.patch().raw;
        for (int i = 0; i < (int)p5::RAW_MAX; i++)
        {   if (cur[i] == m.raw[i]) continue;
            int v = m.raw[i];
            // A host-modulated knob's ENGINE byte is base+mod, but the model
            // carries the BASE (what the patch is, what saves must write).
            const int id = paramIdForByte(i);
            if (modActive_ && id >= 0 && mod_[id] != 0.0)
            {   double e = (double)v + mod_[id];
                if (e < 0.0) e = 0.0; if (e > 127.0) e = 127.0;
                const int ev = (int)(e + 0.5);
                if (p5::polarityByte(i)) v = (cur[i] & 128) | (ev & 127);
                else v = ev;
            }
            engine.editRaw(i, v);
        }
        if (modActive_)
            for (int id = 0; id < 58; id++)
            {   if (mod_[id] == 0.0) continue;
                double e = base_[id] + mod_[id];
                if (e < 0.0) e = 0.0; if (e > 127.0) e = 127.0;
                const int idx = kRawIndex[id];
                int v = (int)(e + 0.5);
                if (p5::polarityByte(idx)) v = (engine.rawValue(idx) & 128) | (v & 127);
                engine.editRaw(idx, v);
            }
    }
    for (int o = 0; o < 8; o++)
        if (engine.overlay(o) != m.ov[o]) engine.setOverlay(o, m.ov[o]);
    for (int l = 0; l < 8; l++)
        for (int oc = 0; oc < 2; oc++)
        {   int sc, of; engine.voiceTrim(l, oc, &sc, &of);
            if (sc != m.trimV[l][oc][0] || of != m.trimV[l][oc][1])
                engine.setVoiceTrim(l, oc, m.trimV[l][oc][0], m.trimV[l][oc][1]); }
    if (std::memcmp(engine.tuningDeviation(), m.dev, 256) != 0)
        engine.setTuningDeviation(m.dev);
    if (engine.tuneCents() != m.tune) engine.setTuneCents(m.tune);
    if (engine.oversample() != m.os)
    {   engine.setOverSample(m.os);                 // 3-sample halfband latency
        latencyDirty.store(true, std::memory_order_relaxed); }
    if (engine.panWidth() != m.pan) engine.setPanWidth(m.pan);
    engine.setBendRangeDefault(m.brUp, m.brDn);     // idempotent, keeps views exact
}

// main-thread, engine owned: apply the authoritative model to the engine.
// Used at the activate edges and while inactive (there is no audio thread).
void Plugin::applyModelToEngineDirect()
{
    p5::Patch p; std::memset(&p, 0, sizeof p);
    std::memcpy(p.raw, model_.raw, 134);
    p.size = 133; p.model = 0x32; p.command = 3;
    engine.setPatch(p);
    for (int o = 0; o < 8; o++) engine.setOverlay(o, model_.ov[o]);
    for (int l = 0; l < 8; l++)
        for (int oc = 0; oc < 2; oc++)
            engine.setVoiceTrim(l, oc, model_.trimV[l][oc][0], model_.trimV[l][oc][1]);
    engine.setTuningDeviation(model_.dev);
    engine.setTuneCents(model_.tune);
    engine.setOverSample(model_.os);
    engine.setPanWidth(model_.pan);
    engine.setBendRangeDefault(model_.brUp, model_.brDn);
    appliedLoadGen_ = loadGen_.load(std::memory_order_relaxed);
}

// ---- output-event ring (main -> audio -> host out_events) ---------------------
void Plugin::postOut(OutKind k, clap_id id, double v)
{   // main produces, audio consumes. Full queue: the LATEST-value nature of
    // OK_VALUE means dropping the OLDEST queued same-id entry is safe, but
    // SPSC forbids touching past the head - so drop NEWEST on overflow and
    // let the next change (or the next par_flush/get_value echo) resync.
    const uint32_t w = ow_.load(std::memory_order_relaxed);
    const uint32_t n = (w + 1u) & (OQ_RING - 1u);
    if (n == or_.load(std::memory_order_acquire)) { outDropped_++; return; }
    outRing_[w] = OutEv{ (uint8_t)k, (uint16_t)id, v };
    ow_.store(n, std::memory_order_release);
}

void Plugin::drainOut(const clap_output_events_t *out)
{   // audio thread. Each queued OK_VALUE is the GUI's/harness's NEW BASE for
    // the param: record it, re-apply the engine byte with the host modulation
    // on top, and echo the value to the host (review H1: this happens from
    // BOTH process() and params.flush(), so playback edits reach the host).
    const uint32_t w = ow_.load(std::memory_order_acquire);
    for (uint32_t r = or_.load(std::memory_order_relaxed); r != w; r = (r + 1u) & (OQ_RING - 1u))
    {   const OutEv e = outRing_[r];
        const clap_id id = (clap_id)e.id;
        if (e.kind == OK_VALUE)
        {   if (id < (clap_id)NIDS) base_[id] = e.v;
            if (isRawParam(id))
            {   const int idx = kRawIndex[id];
                double eff = e.v + (modActive_ ? mod_[id] : 0.0);
                if (eff < 0.0) eff = 0.0; if (eff > 127.0) eff = 127.0;
                int v = (int)(eff + 0.5);
                if (p5::polarityByte(idx)) v = (engine.rawValue(idx) & 128) | (v & 127);
                engine.editRaw(idx, v);
            }
            else if (id >= 64 && id < 64 + NUM_OVERLAYS)
                engine.setOverlay((int)(id - 64), (int)(e.v + (e.v < 0 ? -0.5 : 0.5)));
            else if (id == PID_TUNE) engine.setTuneCents((int)(e.v + (e.v < 0 ? -0.5 : 0.5)));
            else if (id == PID_PAN_WIDTH) engine.setPanWidth((int)(e.v + 0.5));
            else if (id == PID_DRIFT) engine.setDriftDepth((int)(e.v + 0.5));
        }
        if (!out) continue;
        clap_event_param_value pe{};
        clap_event_param_gesture ge{};
        const clap_event_header_t *hp = nullptr;
        if (e.kind == OK_VALUE)
        {   pe.header.type = CLAP_EVENT_PARAM_VALUE;
            pe.header.size = sizeof pe;
            pe.header.time = 0;
            pe.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            pe.header.flags = CLAP_EVENT_IS_LIVE;
            pe.note_id = -1; pe.port_index = -1; pe.channel = -1; pe.key = -1;
            pe.param_id = id; pe.cookie = nullptr; pe.value = e.v;
            hp = &pe.header;
        }
        else
        {   ge.header.type = e.kind == OK_BEGIN ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                : CLAP_EVENT_PARAM_GESTURE_END;
            ge.header.size = sizeof ge;
            ge.header.time = 0;
            ge.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ge.header.flags = CLAP_EVENT_IS_LIVE;
            ge.param_id = id;
            hp = &ge.header;
        }
        out->try_push(out, hp);
    }
    or_.store(w, std::memory_order_release);
}

void Plugin::beginGesture(clap_id id)
{   if (id >= PEND_IDS || dragOpen_[id]) return;
    dragOpen_[id] = true;
    postOut(OK_BEGIN, id, 0.0);
    if (!active.load(std::memory_order_relaxed))
        requestHostParamFlush(this);        // inactive: main-thread flush will emit it
}
void Plugin::endGesture(clap_id id)
{   if (id >= PEND_IDS || !dragOpen_[id]) return;
    dragOpen_[id] = false;
    postOut(OK_END, id, 0.0);
    if (!active.load(std::memory_order_relaxed))
        requestHostParamFlush(this);
}

// ---- posting helpers: mutate model_ (generation-stamped), then stage ---------
void Plugin::postRaw(int idx, int v)
{
    if (idx < 0 || idx >= (int)p5::RAW_MAX) return;
    if (v < 0) v = 0; if (v > 255) v = 255;
    model_.raw[idx] = (uint8_t)v;
    rawGen_[idx] = ++gen_;            // stamped even when unchanged: MAIN wrote last
    changeTick_.fetch_add(1, std::memory_order_relaxed);
    markHostDirty();
    if (!active.load(std::memory_order_relaxed)) { engine.editRaw(idx, v); return; }
    stagePush();
}
void Plugin::postOverlay(int i, int v)
{
    if (i < 0 || i >= 8) return;
    model_.ov[i] = (int16_t)v;
    ovGen_[i] = ++gen_;
    changeTick_.fetch_add(1, std::memory_order_relaxed);
    markHostDirty();
    if (!active.load(std::memory_order_relaxed)) { engine.setOverlay(i, v); return; }
    stagePush();
}
void Plugin::postTune(int v)
{
    model_.tune = v;
    tuneGen_ = ++gen_;
    changeTick_.fetch_add(1, std::memory_order_relaxed);
    markHostDirty();
    if (!active.load(std::memory_order_relaxed)) { engine.setTuneCents(v); return; }
    stagePush();
}
void Plugin::postOS(int v)
{
    model_.os = v;
    osGen_ = ++gen_;                 // always stamped (report-vs-load ordering)
    changeTick_.fetch_add(1, std::memory_order_relaxed);
    markHostDirty();
    if (!active.load(std::memory_order_relaxed))
    {   engine.setOverSample(v);
        if (host && host->get_extension)
        {   auto *l = (const clap_host_latency_t *)host->get_extension(host, CLAP_EXT_LATENCY);
            if (l && l->changed) l->changed(host); }
        return; }
    // Latency moves with the decimator. A changed() ping while active is the
    // old Bitwig "plugin misbehaving" wall; request_restart() is the spec's
    // answer (latency.h) and is legal from MAIN only - deferred to the next
    // main-thread entry (maybePingLatency), applied by the host's re-activate.
    latencyDirty.store(true, std::memory_order_relaxed);
    restartPending_ = true;
    stagePush();
}
void Plugin::postPan(int v)
{
    model_.pan = v;
    panGen_ = ++gen_;
    changeTick_.fetch_add(1, std::memory_order_relaxed);
    markHostDirty();
    if (!active.load(std::memory_order_relaxed)) { engine.setPanWidth(v); return; }
    stagePush();
}
void Plugin::postTrimPair(int lane, int osc, int sc, int of)
{
    if (lane < 0 || lane >= 8 || osc < 0 || osc > 1) return;
    model_.trimV[lane][osc][0] = (int16_t)sc; model_.trimV[lane][osc][1] = (int16_t)of;
    trimGen_[lane][osc][0] = trimGen_[lane][osc][1] = ++gen_;
    changeTick_.fetch_add(1, std::memory_order_relaxed);
    markHostDirty();
    if (!active.load(std::memory_order_relaxed)) { engine.setVoiceTrim(lane, osc, sc, of); return; }
    stagePush();
}
void Plugin::postTuning(const int16_t *dev)
{
    if (dev) std::memcpy(model_.dev, dev, 256);
    else std::memset(model_.dev, 0, 256);
    devGen_ = ++gen_;
    changeTick_.fetch_add(1, std::memory_order_relaxed);
    markHostDirty();
    if (!active.load(std::memory_order_relaxed)) { engine.setTuningDeviation(dev); return; }
    stagePush();
}
void Plugin::postBendDefault(float up, float dn)
{
    model_.brUp = up; model_.brDn = dn;
    brGen_ = ++gen_;
    changeTick_.fetch_add(1, std::memory_order_relaxed);
    markHostDirty();
    if (!active.load(std::memory_order_relaxed)) { engine.setBendRangeDefault(up, dn); return; }
    stagePush();
}
void Plugin::postLoad(const p5::Patch &p)
{
    std::memcpy(model_.raw, p.raw, sizeof model_.raw);      // sync sees the new patch
    for (int i = 0; i < (int)p5::RAW_MAX; i++) rawGen_[i] = ++gen_;
    loadGen_.fetch_add(1, std::memory_order_relaxed);       // whole-patch semantics
    changeTick_.fetch_add(1, std::memory_order_relaxed);
    markHostDirty();
    if (!active.load(std::memory_order_relaxed)) { engine.setPatch(p); return; }
    stagePush();
}
void Plugin::postReset()
{
    if (!active.load(std::memory_order_relaxed))
    {   engine.silenceReset(); engine.setBend(0); engine.wheelReset(); engine.setAftertouch(0);
        return; }
    // plugin_reset while active runs ON the audio thread and applies there
    // directly; this helper is only reached from main-thread paths while the
    // transport is stopped. (If a host ever calls it from a wrong thread, the
    // engine is the audio's: nothing to do - the reset was applied already.)
}

// ---- descriptor --------------------------------------------------------------
static const char *const kFeatures[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                         CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr };

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "muhammad.unresolvable",
    "Muhammad",
    "selenologist",
    "https://github.com/keremimo/melodee",
    nullptr, nullptr,
    P5_VERSION,        // from CMake (single source); the version Bitwig shows proves which binary is loaded
    "8-voice subtractive synth - engine ported from the Melodee firmware",
    kFeatures
};

// ---- params <-> engine -------------------------------------------------------
// Raw bytes past 127 only matter where the engine reads the BYTE, not valueOf():
// the continuous OSC-B freq (KEYBOARD off), the glide rate, and the >1 switch
// semantics (LOW_B octave shift, B-key off, SSI filter). Everything else
// sound-wise clamps at 127 in valueOf, so the surface may clamp honestly.
static bool rawByteCarriesMeaning(int idx)
{
    switch (idx) {
        case p5::P5_FREQ_A: case p5::P5_FREQ_B: case p5::P5_GLIDE:
        case p5::P5_LOW_B: case p5::P5_KEY_B: case p5::P5_SYNC:
        case p5::P5_FILTER_REV:
            return true;
        default: return false;
    }
}

// Bit 7 = knob POLARITY (p5::polarityByte): host/CC writes address the 7-bit
// LEVEL only — the flag holds — and the host surface never sees it.

// One writer PER VALUE, decided by the CLAP thread ANNOTATION (review C1):
// Plugin::applyValue/applyMod run on the AUDIO thread while active (host
// events via process()/flush, the out-ring drain) and on MAIN while inactive
// (single-threaded paths, tests, transport stopped). They set the host-facing
// BASE value and write BASE+MOD to the engine - the spec's law, the value
// heard is param_value + param_mod (review C2: PARAM_MOD is an OFFSET, never
// an absolute value). Main-thread callers while ACTIVE never reach them:
// applyParam mutates model_ and queues the base for the audio thread.
static void requestHostParamFlush(Plugin *s);   // fwd (out paths)
static double paramValue(Plugin *s, clap_id id); // fwd (readback of model_)

static p5::Patch factoryPatch(int k)          // mirror of Engine::loadFactory
{
    p5::Patch p;
    if (k < 0 || k >= (int)PW_FACTORY_N) { p5::patchInit(p); return p; }
    std::memset(&p, 0, sizeof p);
    std::memcpy(p.raw, PW_FACTORY_RAW[k], p5::WIRE_SIZE);
    p.raw[p5::P5_FEEDBACK] = 0;
    p.size = p5::RAW_MAX; p.model = 0x32; p.command = 2;
    p.group = (uint8_t)(k / 40u); p.program = (uint8_t)(k % 40u);
    return p;
}

// The engine byte a host/panel value addresses, after this parameter's
// display clamp (shared law so engine, host surface and save agree).
static double clampHostValue(clap_id id, double value)
{
    if (isRawParam(id)) {
        int v = (int)(value < 0 ? 0 : value + 0.5);
        if (const KnobClamp *k = knobClamp(id)) {
            if (v < (int)k->lo) v = (int)k->lo;
            if (v > (int)k->hi) v = (int)k->hi;
        } else if (v > 127 && !rawByteCarriesMeaning(kRawIndex[id])) v = 127;
        return (double)v;
    }
    if (isOverlayParam(id)) return value;
    if (id == PID_TRIM) return value < 0 ? 0 : value > 2 ? 2 : value;
    if (id == PID_OS) return value > 0.5 ? 1 : 0;
    if (id == PID_TUNE) { int v = (int)(value + (value < 0 ? -0.5 : 0.5)); return v < -100 ? -100 : v > 100 ? 100 : v; }
    if (id == PID_PROGRAM) return value < 0 ? 0 : value > (double)(PW_FACTORY_N - 1) ? (double)(PW_FACTORY_N - 1) : value;
    if (id == PID_BANK) { double mx = PW_FACTORY_N / 8 - 1; return value < 0 ? 0 : value > mx ? mx : value; }
    if (id == PID_PAN_WIDTH) { int v = (int)(value + 0.5); return v < 0 ? 0 : v > 127 ? 127 : v; }
    if (id == PID_DRIFT) { int v = (int)(value + 0.5); return v < 0 ? 0 : v > 127 ? 127 : v; }
    return value;
}

// engine-side write: base + (active ? report-to-main : model-echo)
void Plugin::applyValue(clap_id id, double value)
{
    const double base = clampHostValue(id, value);
    if (id < NIDS) base_[id] = base;
    const bool live = active.load(std::memory_order_relaxed);   // who I am: audio iff active

    if (isRawParam(id))
    {
        const int idx = kRawIndex[id];
        double e = base + mod_[id];
        if (e < 0.0) e = 0.0; if (e > 127.0) e = 127.0;
        int v = (int)(e + 0.5);
        if (p5::polarityByte(idx))                       // bit 7 = polarity: LEVEL write, flag holds
            v = (engine.rawValue(idx) & 128) | (v & 127);
        engine.editRaw(idx, v);
        if (idx == p5::P5_VINTAGE) base_[PID_DRIFT] = base;   // the byte-37 alias pair
    }
    else if (isOverlayParam(id))
    {   double e = base + mod_[id];
        engine.setOverlay((int)(id - 64), (int)(e + (e < 0 ? -0.5 : 0.5))); }
    else if (id == PID_TRIM)
        trim.store((float)(base + mod_[id]), std::memory_order_relaxed);
    else if (id == PID_OS)
    {   engine.setOverSample(base > 0.5 ? 2 : 1);
        if (!live)
        {   if (host && host->get_extension)
            {   auto *l = (const clap_host_latency_t *)host->get_extension(host, CLAP_EXT_LATENCY);
                if (l && l->changed) l->changed(host); }
        }
        else { latencyDirty.store(true, std::memory_order_relaxed); restartPending_ = true; }
    }
    else if (id == PID_TUNE)
    {   double e = base + mod_[id]; engine.setTuneCents((int)(e + (e < 0 ? -0.5 : 0.5))); }
    else if (id == PID_PROGRAM)
    {
        const int k = (int)(base + 0.5);
        if (live)
        {   // in-band program event mid-render: the audio thread owns the
            // engine - load HERE. Identity + host echo: adopt at main.
            const int cur = programA_.load(std::memory_order_relaxed);
            if (cur < 0 && k == 0) return;                   // placeholder echo
            if (cur >= 0 && k == cur) return;
            if (k >= 0 && k < (int)PW_FACTORY_N)
            {   engine.loadFactory((uint32_t)k);
                // mirror the new bytes into the base bookkeeping (C2): a
                // loaded patch REPLACES the base values of the wire knobs.
                for (int pid = 0; pid < 58; pid++)
                {   int v = engine.rawValue(kRawIndex[pid]);
                    if (p5::polarityByte(kRawIndex[pid])) v &= 127;
                    else if (v > 127 && !rawByteCarriesMeaning(kRawIndex[pid])) v = 127;
                    base_[pid] = v; }
                base_[PID_DRIFT] = base_[37];
                for (clap_id pid = 0; pid < 58; pid++) reportParam(pid, base_[pid]);
                reportParam(PID_DRIFT, base_[PID_DRIFT]);   // main merges the new patch NOW
                programA_.store(k, std::memory_order_relaxed);
                idSyncSlot_.store(k, std::memory_order_relaxed);
                adoptMode_.store(1, std::memory_order_relaxed);
                idSync_.store(true, std::memory_order_release);
                // echo of the rescan: only legal from the SAME entry point's
                // out list (request_flush is [!audio-thread], params.h:380) -
                // if we are mid-process/flush, curOut_ is set and emit lands;
                // otherwise main adopts and echoes at the next entry.
                emitResync();
            }
            return;
        }
        // Host echo guard (inactive path): while the patch is not a factory
        // pick we report PROGRAM=0 as a placeholder; a pushed-back 0 must not
        // silently replace what the user just picked. Only a real change
        // drives a load.
        if (program < 0 && k == 0) return;
        if (program >= 0 && k == program) return;
        loadProgram(k);
        return;
    }
    else if (id == PID_BANK) bank.store((int)base);
    else if (id == PID_PAN_WIDTH)
    {   double e = base + mod_[id]; engine.setPanWidth(e < 0 ? 0 : e > 127 ? 127 : (int)(e + 0.5)); }
    else if (id == PID_DRIFT)
    {   double e = base + mod_[id]; engine.setDriftDepth((int)(e + 0.5));
        base_[37] = base; }
    // base_/trim are audio-owned while live: the ONLY channel back to main is
    // the coalesced report (program loads cover themselves via loadFactory's
    // base mirror + the pub raw merge).
    if (live && id < NIDS && id != PID_PROGRAM && id != PID_BANK)
    {   reportParam(id, base);
        if (isRawParam(id) && kRawIndex[id] == p5::P5_VINTAGE) reportParam(PID_DRIFT, base); }
}

// PARAM_MOD: a modulation OFFSET on top of the base (events with a per-
// note/polyphonic address are not modelled by this engine - ignore them and
// keep the monophonic offset at the -1 wildcard address).
void Plugin::applyMod(clap_id id, double amount)
{
    if (id >= NIDS) return;
    if (id == PID_PROGRAM || id == PID_BANK || id == PID_OS) return;   // not modulatable (5.1/5.9)
    const bool was = (mod_[id] != 0.0);
    mod_[id] = amount;
    modActive_ += (amount != 0.0) ? (was ? 0 : 1) : (was ? -1 : 0);
    if (modActive_ < 0) modActive_ = 0;
    applyValue(id, base_[id]);
}

// Emit current values for every wire param + the sidecar knobs into the
// host's output list (in-band program load: the whole patch moved).
void Plugin::emitResync()
{
    const clap_output_events_t *out = curOut_;
    if (!out) return;
    for (clap_id id = 0; id < 58; id++)
    {   clap_event_param_value pe{};
        pe.header.type = CLAP_EVENT_PARAM_VALUE; pe.header.size = sizeof pe;
        pe.header.time = 0; pe.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        pe.header.flags = CLAP_EVENT_IS_LIVE;
        pe.note_id = -1; pe.port_index = -1; pe.channel = -1; pe.key = -1;
        pe.param_id = id; pe.cookie = nullptr;
        int v = engine.rawValue(kRawIndex[id]);
        if (p5::polarityByte(kRawIndex[id])) v &= 127;
        else if (v > 127 && !rawByteCarriesMeaning(kRawIndex[id])) v = 127;
        pe.value = (double)v;
        out->try_push(out, &pe.header); }
}

// MAIN-thread entry while active (guiSet, resync echoes): mutate model_
// (generation-stamped), queue the base for the audio out-drain, never touch
// the engine (review C1: the single writer stays the audio thread).
static void applyParam(Plugin *s, clap_id id, double value)
{
    if (!s->active.load(std::memory_order_relaxed)) { s->applyValue(id, value); return; }
    const double base = clampHostValue(id, value);
    if (isRawParam(id))
    {   const int idx = kRawIndex[id];
        int v = (int)(base + 0.5);
        if (p5::polarityByte(idx)) v = (s->model_.raw[idx] & 128) | (v & 127);
        s->postRaw(idx, v);
        if (idx == p5::P5_VINTAGE) s->markPending(PID_DRIFT, base);
    }
    else if (isOverlayParam(id)) s->postOverlay((int)(id - 64), (int)(base + (base < 0 ? -0.5 : 0.5)));
    else if (id == PID_TRIM) s->trim.store((float)base, std::memory_order_relaxed);
    else if (id == PID_OS) s->postOS((int)base > 0 ? 2 : 1);
    else if (id == PID_TUNE) s->postTune((int)base);
    else if (id == PID_PROGRAM)
    {   const int k = (int)(base + 0.5);
        if (s->program < 0 && k == 0) return;                // placeholder echo
        if (s->program >= 0 && k == s->program) return;
        s->loadProgram(k);                                   // main-side: posts the patch
        return;                                              // (loadProgram echoes params)
    }
    else if (id == PID_BANK) s->bank.store((int)base);
    else if (id == PID_PAN_WIDTH) s->postPan((int)base);
    else if (id == PID_DRIFT) s->postRaw(p5::P5_VINTAGE, (int)base);
    s->markPending(id, paramValue(s, id));
}


static void requestHostParamFlush(Plugin *s)
{
    if (!s->host || !s->host->get_extension) return;
    auto *hp = (const clap_host_params_t *)s->host->get_extension(s->host, CLAP_EXT_PARAMS);
    if (hp && hp->request_flush) hp->request_flush(s->host);
}

// The panel changing anything project-worthy marks the host's session dirty
// (review 5.12: hosts never prompted to save after polarity/trim/tuning or
// GUI preset changes).
void Plugin::markHostDirty()
{
    if (!host || !host->get_extension) return;
    auto *hs = (const clap_host_state_t *)host->get_extension(host, CLAP_EXT_STATE);
    if (hs && hs->mark_dirty) hs->mark_dirty(host);
}

static Plugin *fromPlugin(const clap_plugin_t *p);   // fwd (vtable section)
static void plugin_on_timer(const clap_plugin_t *p, clap_id id)
{   auto *s = fromPlugin(p);
    if (s && s->bgTimerOn_ && id == s->bgTimerId_)
    {   s->adoptPendingIdentity();
        s->maybePingLatency();
        s->refreshView();
        return; }
#ifdef P5_GUI
    if (kTimerSupport.on_timer) kTimerSupport.on_timer(p, id);
#else
    (void)p; (void)id;
#endif
}
static const clap_plugin_timer_support_t kTimerDispatch = { plugin_on_timer };

static double paramValue(Plugin *s, clap_id id)
{
    if (isRawParam(id)) {
        int v = s->model_.raw[kRawIndex[id]];
        if (p5::polarityByte(kRawIndex[id])) v &= 127;   // bit 7 = polarity, not level
        if (const KnobClamp *k = knobClamp(id)) {
            if (k->defWhenZero && v == 0) v = (int)k->def;   // raw 0 COUNT = "default 5"
            if (v > (int)k->hi) v = (int)k->hi;         // saturating knobs
        } else if (v > 127) v = 127;                    // engine reads clamped too
        return (double)v;
    }
    if (isOverlayParam(id)) return (double)s->model_.ov[(int)(id - 64)];
    if (id == PID_TRIM) return (double)s->trim.load(std::memory_order_relaxed);
    if (id == PID_OS) return (double)(s->model_.os - 1);
    if (id == PID_TUNE) return s->model_.tune;
    if (id == PID_PROGRAM)
    {   const int pa = s->programA_.load(std::memory_order_relaxed);
        if (pa >= 0) return pa;                 // an audio-side load is the truth
        return s->program < 0 ? 0 : s->program; }
    if (id == PID_BANK) return (double)s->bank.load();
    if (id == PID_PAN_WIDTH) return s->model_.pan;
    if (id == PID_DRIFT) { int v = s->model_.raw[p5::P5_VINTAGE]; return v > 127 ? 127 : v; }
    return 0;
}

double Plugin::guiValue(clap_id id) { return paramValue(this, id); }

bool Plugin::knobPolarity(clap_id id)
{   if (!isRawParam(id)) return false;
    const int ri = kRawIndex[id];
    return p5::polarityByte(ri) && (rawOf(ri) & 128) != 0; }

void Plugin::guiSetLocal(clap_id id, double v)
{
    // Gesture in progress: the engine follows instantly (via model_ push),
    // the host learns values through the queued out_events stream, wrapped
    // in BEGIN/END so it can record automation and group undo (review H1).
    beginGesture(id);
    applyParam(this, id, v);
}

void Plugin::guiSet(clap_id id, double v)
{
    applyParam(this, id, v);
    if (!active.load(std::memory_order_relaxed)) requestHostParamFlush(this);
}

void Plugin::guiSync(clap_id id)
{
    markPending(id, paramValue(this, id));
    endGesture(id);
    if (!active.load(std::memory_order_relaxed)) requestHostParamFlush(this);
}

void Plugin::loadProgram(int k)
{
    loadTag_ = "fac";
    if (k < 0 || k >= (int)PW_FACTORY_N) return;
    postLoad(factoryPatch(k));              // audio owner applies at next block
    markSource(0, k, std::string());
    curSrc_.kind = p5store::SourceKind::Factory; curSrc_.factory = k; curSrc_.file.clear();
    program = k;
    programA_.store(k, std::memory_order_relaxed);
    bank.store(k / 8);
    // the whole patch changed under the host's feet: sync every knob
    for (clap_id id = 0; id < 58; id++)
        markPending(id, paramValue(this, id));
    requestHostParamFlush(this);
}

void Plugin::markSource(int kind, int idx, const std::string &file)
{
    std::memcpy(srcRaw_, model_.raw, 133);    // callers post/own the model first
    hasSrc_ = true;
    floatValid_ = false;
    // one identity truth: every source-bearing load re-stamps curSrc_ here
    curSrc_ = p5store::Source{};                       // default: unbound sentinel
    if (kind == 1 && !file.empty())
    { curSrc_.kind = p5store::SourceKind::File; curSrc_.file = file; }
    else if (idx >= 0 || idx == -1 || idx == -2)
    { curSrc_.kind = p5store::SourceKind::Factory; curSrc_.factory = idx; }
    srcFile_.clear();
    if (kind == 1) srcFile_ = file;
    (void)idx;
}

bool Plugin::loadUserPatchExact(const std::string &file)
{   // The picker and the host browser list RAW filenames; loading must use
    // the same token, never a re-sanitized one (review 5.8: files with
    // spaces/parens/non-ASCII used to silently load INIT).
    if (file.empty() || file.size() > 128) return false;
    if (file.find('/') != std::string::npos || file.find('\\') != std::string::npos ||
        file.find("..") != std::string::npos) return false;              // S13: no traversal
    loadTag_ = "user";
    uint8_t raw[134];
    if (!p5store::loadPatch(file, raw)) return false;
    p5::Patch p;
    std::memset(&p, 0, sizeof p);
    std::memcpy(p.raw, raw, 134);      // full store, feedback/polarity included
    p.size = 133; p.model = 0x32; p.command = 3;
    postLoad(p);
    program = -1;
    programA_.store(-1, std::memory_order_relaxed);
    markSource(1, -1, file);
    curSrc_.kind = p5store::SourceKind::File; curSrc_.factory = 0; curSrc_.file = file;
    syncAllParams();
    return true;
}

bool Plugin::loadUserPatch(const std::string &name)
{   return loadUserPatchExact(p5store::sanitize(name));
}


void Plugin::syncAllParams()
{   for (clap_id id = 0; id < 58; id++) markPending(id, paramValue(this, id));
    requestHostParamFlush(this);
}

// load WITHOUT touching the retrieval map (picker row click)
void Plugin::loadSource(const p5store::Source &s)
{
    curSrc_ = s;
    if (s.kind == p5store::SourceKind::File)
    {
        if (loadUserPatch(s.file)) return;
        loadTag_ = "def";
        p5::Patch i; p5::patchInit(i); p5::patchDefaultName(i);
        postLoad(i); program = -1; programA_.store(-1); markSource(0, -2, {}); syncAllParams();
        return;
    }
    const int idx = s.factory;
    if (idx >= 0 && idx < (int)PW_FACTORY_N) { loadProgram(idx); return; }
    if (idx == -2 && loadUserPatch("Peace Be Upon Him"))
    { curSrc_ = s; return; }   // recorded default wins, but the requested row identity stays
    p5::Patch i; p5::patchInit(i);
    if (idx != -1) p5::patchDefaultName(i);
    postLoad(i); program = -1; programA_.store(-1); markSource(0, idx, {}); syncAllParams();
}

void Plugin::rebuildSlotCache()
{   // MAIN thread: every retrieval slot gets its raw bytes resolved NOW, so a
    // MIDI program change on the audio thread is only a memcpy. The publish
    // is a generation handshake (odd = in flight, S4.4): a reader that races
    // a double rebuild DEFERS the PC one block instead of memcpy-ing torn
    // bytes into the engine.
    const uint32_t g0 = cacheGen_.load(std::memory_order_relaxed);
    cacheGen_.store(g0 + 1, std::memory_order_release);         // odd: filling
    const int back = 1 - cacheSel_.load(std::memory_order_relaxed);
    uint8_t (*raw)[134] = back ? slotRawB_ : slotRaw_;
    int16_t *codeB = back ? slotCodeB_ : slotCode_;
    for (int i = 0; i < (int)PW_FACTORY_N; i++)
    {
        const p5store::Source s = p5store::slot(i);
        int code = i;
        if (s.kind == p5store::SourceKind::File)
        {
            if (p5store::loadPatch(s.file, raw[i])) { codeB[i] = -3; }
            else { std::memcpy(raw[i], PW_FACTORY_RAW[i], 133); raw[i][133] = 0;
                   codeB[i] = (int16_t)i; }
            continue;
        }
        code = s.factory;
        if (code >= 0 && code < (int)PW_FACTORY_N)
        {   std::memcpy(raw[i], PW_FACTORY_RAW[code], 133); raw[i][133] = 0; }
        else { p5::Patch p; p5::patchInit(p); if (code != -1) p5::patchDefaultName(p);
               std::memcpy(raw[i], p.raw, 133); }
        codeB[i] = (int16_t)code;
    }
    cacheSel_.store(back, std::memory_order_release);
    slotReady_.store(true, std::memory_order_release);
    cacheGen_.store(g0 + 2, std::memory_order_release);         // even: complete
}

void Plugin::adoptPendingIdentity()
{   // main thread: an audio-thread PC changed the patch from the cache; pull
    // the string-bearing identity and GUI sync here, never in process().
    if (!idSync_.exchange(false)) return;
    refreshView();
    std::memcpy(srcRaw_, model_.raw, 134); hasSrc_ = true; floatValid_ = false;
    if (adoptMode_.load(std::memory_order_relaxed) == 1)
    {   const int k = programA_.load(std::memory_order_relaxed);
        program = k; bank.store(k / 8); lastProgClick_ = k % 8;
        curSrc_.kind = p5store::SourceKind::Factory; curSrc_.factory = k; curSrc_.file.clear();
        srcFile_.clear();
    }
    else
    {   const int slot = idSyncSlot_.load(std::memory_order_relaxed);
        const int sel = cacheSel_.load(std::memory_order_relaxed);
        const int code = sel ? slotCodeB_[slot] : slotCode_[slot];
        program = (code >= 0 && code < (int)PW_FACTORY_N) ? code : -1;
        lastProgClick_ = slot % 8;
        const p5store::Source src = p5store::slot(slot);
        curSrc_ = src;
        if (src.kind == p5store::SourceKind::File) srcFile_ = src.file; else srcFile_.clear();
    }
    for (clap_id id = 0; id < 58; id++) markPending(id, paramValue(this, id));
    markPending(PID_PROGRAM, program < 0 ? 0 : program);
    markPending(PID_BANK, (double)bank.load());
    requestHostParamFlush(this);
    changeTick_.fetch_add(1, std::memory_order_relaxed);
}

static void applyPcFromCache(Plugin *s, int slot)   // AUDIO thread only
{
    if (!s->slotReady_.load(std::memory_order_acquire)) return;
    if (slot < 0 || slot >= (int)PW_FACTORY_N) return;
    uint8_t raw[134];
    for (int tries = 0; tries < 3; tries++)
    {   const uint32_t g = s->cacheGen_.load(std::memory_order_acquire);
        if (g & 1u) { s->pendingPc_.store(slot, std::memory_order_release); return; }  // filling
        const int sel = s->cacheSel_.load(std::memory_order_acquire);
        std::memcpy(raw, sel ? s->slotRawB_[slot] : s->slotRaw_[slot], 134);
        if (s->cacheGen_.load(std::memory_order_acquire) == g) goto applied;           // stable
    }
    s->pendingPc_.store(slot, std::memory_order_release);      // rebuild racing: next block
    return;
applied:
    {
        p5::Patch p;
        std::memset(&p, 0, sizeof p);
        std::memcpy(p.raw, raw, 134);
        p.size = 133; p.model = 0x32; p.command = 3;
        s->engine.setPatch(p);                        // pure POD state, no I/O
        for (clap_id id = 0; id < 58; id++)
        {   int v = s->engine.rawValue(kRawIndex[id]);
            if (p5::polarityByte(kRawIndex[id])) v &= 127;
            else if (v > 127 && !rawByteCarriesMeaning(kRawIndex[id])) v = 127;
            s->base_[id] = v; }
        s->base_[PID_DRIFT] = s->base_[37];
        for (clap_id pid = 0; pid < 58; pid++) s->reportParam(pid, s->base_[pid]);
        s->reportParam(PID_DRIFT, s->base_[PID_DRIFT]);
        s->idSyncSlot_.store(slot, std::memory_order_relaxed);    // identity: main adopts
        s->adoptMode_.store(0, std::memory_order_relaxed);
        s->idSync_.store(true, std::memory_order_release);
        s->emitResync();        // values to pr->out_events (process context);
                                // request_flush is [!audio-thread] - never here
    }
}


void Plugin::programSlot(int slot)
{
    loadTag_ = "slot";
    programSlotLoad(p5store::slot(slot), slot);
}

void Plugin::programSlotLoad(const p5store::Source &s, int slot)
{
    if (s.kind == p5store::SourceKind::File && loadUserPatch(s.file))
    { bank = slot / 8; lastProgClick_ = slot % 8; return; }
    // missing file: fall through to the slot's factory default
    p5store::Source f; f.kind = p5store::SourceKind::Factory; f.factory = slot;
    loadSource(s.kind == p5store::SourceKind::Factory ? s : f);
    lastProgClick_ = slot % 8;
}

void Plugin::recallBasicInit()
{   // SHIFT+PRESET: strip back to the bare INIT patch
    loadTag_ = "init";
    p5::Patch i; p5::patchInit(i);
    postLoad(i); program = -1; programA_.store(-1); markSource(0, -1, {});
    floatValid_ = false;
    syncAllParams();
}

void Plugin::maybePingLatency()
{
    // Main-thread entry (GUI tick / refresh): announce a latency move that
    // happened WHILE ACTIVE with request_restart() - the spec's latency
    // contract (latency.h allows changed() only in activate; a ping from the
    // pump was the old Bitwig "plugin misbehaving" wall, and from deactivate
    // it re-enters a dying instance). The host re-activates, re-reads
    // get_latency() and delay-compensation lands within a frame.
    if (!restartPending_) return;
    if (!active.load(std::memory_order_relaxed)) { restartPending_ = false; return; }
    if (host && host->request_restart)
    {   restartPending_ = false;
        host->request_restart(host); }
}

void Plugin::savePatchNamed(const std::string &name)
{
    loadTag_ = "saved";
    const std::string n = p5store::sanitize(name);
    if (n.empty()) return;
    refreshView();                        // commitWheelToPatch mirrored on main:
    p5::Patch p = {};                     // wheel position IS the saved depth
    std::memcpy(p.raw, model_.raw, 134);
    p.raw[p5::P5_LFO_INITIAL] = (uint8_t)(model_.wheel & 127);
    p.size = 133; p.model = 0x32; p.command = 3;
    std::memset(p.raw + p5::P5_NAME, ' ', p5::P5_NAME_LEN);
    const size_t len = n.size() < p5::P5_NAME_LEN ? n.size() : p5::P5_NAME_LEN;
    std::memcpy(p.raw + p5::P5_NAME, n.data(), len);
    // Name-only edit: patch the 20 name BYTES, never a whole-patch LOAD -
    // re-postLoad()ing killed every sounding voice just to set a name (5.8).
    for (int i = 0; i < p5::P5_NAME_LEN; i++) postRaw(p5::P5_NAME + i, p.raw[p5::P5_NAME + i]);
    postRaw(p5::P5_LFO_INITIAL, p.raw[p5::P5_LFO_INITIAL]);
    program = -1;
    programA_.store(-1, std::memory_order_relaxed);
    p5store::savePatch(n, p.raw);
    markSource(1, -1, n);
    rebuildSlotCache();
    curSrc_.kind = p5store::SourceKind::File; curSrc_.factory = 0; curSrc_.file = n;
    for (clap_id id = 0; id < 58; id++)
        markPending(id, paramValue(this, id));
    requestHostParamFlush(this);
}

void Plugin::assignSlotFromText(int slot, const std::string &textRaw)
{
    std::string t = textRaw;
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.empty()) { p5store::clearSlot(slot); p5store::saveMap(); return; }
    p5store::Source s;
    if (t == "INIT") { s.kind = p5store::SourceKind::Factory; s.factory = -1;
                       p5store::setSlot(slot, s); p5store::saveMap(); return; }
    size_t i = 0;
    bool numeric = true;
    for (; i < t.size(); i++) if (!std::isdigit((unsigned char)t[i])) { numeric = false; break; }
    if (numeric || (t.size() >= 3 && t.compare(0, 3, "FAC") == 0))
    {
        s.kind = p5store::SourceKind::Factory;
        s.factory = std::atoi(numeric ? t.c_str() : t.c_str() + 3);
        if (s.factory < 0 || s.factory >= (int)PW_FACTORY_N) return;
    }
    else
    {
        s.kind = p5store::SourceKind::File;
        s.file = p5store::sanitize(t);
        if (s.file.empty()) return;
    }
    p5store::setSlot(slot, s);
    p5store::saveMap();
    rebuildSlotCache();
}

void Plugin::presetRecall()
{   loadTag_ = "recall";
    // PRESET: un-float; press again (nothing touched since) and the floating edits come BACK
    if (!hasSrc_) return;
    refreshView();
    const uint8_t *cur = model_.raw;
    if (!panelDirty() && floatValid_ && std::memcmp(cur, floatSnap_, 133) != 0)
    {   // second press right after a recall: restore the pre-recall panel state
        p5::Patch p = {};
        std::memcpy(p.raw, model_.raw, 134);
        std::memcpy(p.raw, floatSnap_, 133);
        p.size = 133; p.model = 0x32; p.command = 3;
        postLoad(p);
        syncAllParams();
        return;
    }
    if (panelDirty())
    {
        std::memcpy(floatSnap_, cur, 133);
        floatValid_ = true;
    }
    p5::Patch p = {};
    std::memcpy(p.raw, model_.raw, 134);
    std::memcpy(p.raw, srcRaw_, 133);
    p.size = 133; p.model = 0x32; p.command = 3;
    postLoad(p);
    syncAllParams();
}


// Sequential "Prophet-5 (Rev4 family) MIDI Implementation 1.4" received-CC
// table -> raw wire PARAM ids (see CC_MAP.md; verified against the impl PDF).
static int ccRawParam(int cc)
{   switch (cc)
    {
    case   3: return 0;   case   9: return 1;   case  14: return 2;   case  15: return 3;
    case  20: return 4;   case  21: return 8;   case  22: return 9;   case  23: return 10;
    case  24: return 11;  case  25: return 12;  case  26: return 13;  case  27: return 14;
    case  28: return 15;  case  29: return 16;  case  30: return 5;   case  31: return 18;
    case  35: return 19;  case  41: return 20;  case  46: return 21;  case  47: return 22;
    case  52: return 6;   case  53: return 26;  case  54: return 27;  case  55: return 28;
    case  56: return 29;  case  57: return 30;  case  58: return 31;  case  59: return 32;
    case  60: return 33;  case  61: return 34;  case  62: return 35;  case  63: return 36;
    case  70: return 55;  case  71: return 56;  case  73: return 17;  case  86: return 38;
    case  87: return 39;  case  89: return 40;  case  90: return 41;  case 102: return 42;
    case 103: return 43;  case 104: return 44;  case 105: return 45;  case 106: return 46;
    case 107: return 47;  case 108: return 48;  case 109: return 49;  case 110: return 50;
    case 111: return 51;  case 112: return 52;  case 113: return 53;  case 114: return 54;
    case 116: return 7;   case 117: return 23;  case 118: return 24;  case 119: return 25;
    default: return -1;
    }
}

// ---- events ------------------------------------------------------------------
static void dispatchNoteOn(Plugin *s, int key, int vel, int ch = 0)
{
    if (key < 0 || key > 127) return;
    if (vel <= 0) vel = 1;
    if (vel > 127) vel = 127;
    const int c = (ch >= 0 && ch < 16) ? ch : 0;        // CLAP -1 wildcard -> part channel
    if (s->heldFlag_[c][key])
    {   // re-struck under the pedal: it must NOT die at pedal-up (review 5.10)
        s->heldFlag_[c][key] = 0;
        const int n = s->heldCnt2_[c];
        for (int i = 0; i < n; i++)
            if (s->heldNotes_[c][i] == key)
            {   for (int j = i; j + 1 < n; j++) s->heldNotes_[c][j] = s->heldNotes_[c][j + 1];
                s->heldCnt2_[c] = n - 1; break; }
    }
    s->engine.noteOn(key, vel, c);
}

// CC64 damper PER MIDI CHANNEL (MPE-correct): a note released on a pedalled
// channel keeps holding until THAT pedal lifts.
static void noteOffGated(Plugin *s, int key, int ch = -1)
{
    if (key < 0)
    {   for (int k = 0; k < 128; k++) noteOffGated(s, k, ch);   // wildcard all-off
        return; }
    if (key >= 128) return;
    if (ch >= 0 && ch < 16 && (s->sustainCh_ & (1u << ch)))
    {   if (!s->heldFlag_[ch][key])
        {   s->heldFlag_[ch][key] = 1; s->heldNotes_[ch][s->heldCnt2_[ch]++] = (uint8_t)key; }
        return; }
    if (ch >= 0 && ch < 16) s->heldFlag_[ch][key] = 0;
    s->engine.noteOff(key, ch);
}

static void panicVoices(Plugin *s, bool kill)   // CC120 kills, CC123 releases
{
    for (int k = 0; k < 128; k++)
        if (kill) s->engine.noteKill(k); else s->engine.noteOff(k);
    for (int c = 0; c < 16; c++)
    {   std::memset(s->heldFlag_[c], 0, sizeof s->heldFlag_[c]);
        std::memset(s->heldNotes_[c], 0, sizeof s->heldNotes_[c]);
        s->heldCnt2_[c] = 0; }
}


// ---- events ------------------------------------------------------------------
// dispatchEvent runs on the ENGINE-OWNING thread: audio while active (in-band
// process() events and params.flush - review C1), main while inactive.
static void dispatchEvent(Plugin *s, const clap_event_header_t *ev)
{
    switch (ev->type)
    {   case CLAP_EVENT_NOTE_ON: case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: case CLAP_EVENT_NOTE_END:
        case CLAP_EVENT_PARAM_VALUE: case CLAP_EVENT_PARAM_MOD:
            // S13: a foreign space whose type number collides must not be
            // read through the core struct layout.
            if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        default: break;
    }
    switch (ev->type) {
    case CLAP_EVENT_NOTE_ON: {
        auto *e = reinterpret_cast<const clap_event_note *>(ev);
        dispatchNoteOn(s, e->key, (int)(e->velocity * 127.0 + 0.5), e->channel);
        break;
    }
    case CLAP_EVENT_NOTE_OFF: {
        auto *e = reinterpret_cast<const clap_event_note *>(ev);
        noteOffGated(s, e->key, e->channel);
        break;
    }
    case CLAP_EVENT_NOTE_CHOKE: {
        auto *e = reinterpret_cast<const clap_event_note *>(ev);
        if (e->key < 0) for (int k = 0; k < 128; k++) s->engine.noteKill(k, e->channel);
        else            s->engine.noteKill(e->key, e->channel);   // choke KILLS (5.10)
        break;
    }
    case CLAP_EVENT_NOTE_END: {
        auto *e = reinterpret_cast<const clap_event_note *>(ev);
        noteOffGated(s, e->key, e->channel);
        break;
    }
    case CLAP_EVENT_PARAM_VALUE: {
        auto *e = reinterpret_cast<const clap_event_param_value *>(ev);
        s->applyValue(e->param_id, e->value);
        break;
    }
    case CLAP_EVENT_PARAM_MOD: {
        auto *e = reinterpret_cast<const clap_event_param_mod *>(ev);
        if (e->note_id < 0 && e->port_index < 0 && e->channel < 0 && e->key < 0)
            s->applyMod(e->param_id, e->amount);      // per-voice mods: not modelled
        break;
    }
    case CLAP_EVENT_MIDI: {
        auto *e = reinterpret_cast<const clap_event_midi *>(ev);
        const uint8_t st = e->data[0] & 0xF0u, ch = e->data[0] & 15u, d1 = e->data[1], d2 = e->data[2];
        switch (st) {
        case 0x90: if (d2) dispatchNoteOn(s, d1, d2, ch); else noteOffGated(s, d1, ch); break;
        case 0x80: noteOffGated(s, d1, ch); break;
        case 0xE0: s->engine.setBendChannel(ch, (int)(((d2 << 7) | d1) - 8192)); break;
        case 0xB0:
            if (d1 == 1) s->engine.setModWheel(d2);
            else if (d1 == 101) s->rpnM_[ch] = d2;          // RPN select: MSB 101, LSB 100
            else if (d1 == 100) s->rpnL_[ch] = d2;
            else if (d1 == 6 || d1 == 38)
            {   // Data Entry after RPN 0,0 = pitch bend sensitivity: MSB semitones, LSB cents
                if (s->rpnM_[ch] == 0 && s->rpnL_[ch] == 0)
                {
                    if (d1 == 6) s->rpnV_[ch] = (s->rpnV_[ch] & 127) | (d2 << 7);
                    else         s->rpnV_[ch] = (s->rpnV_[ch] & ~127) | d2;
                    const double semis = (double)(s->rpnV_[ch] >> 7) + (double)(s->rpnV_[ch] & 127) / 128.0;
                    s->engine.setBendRangeSemitones(ch, semis, semis);   // RPN is symmetric
                }
                else { const int r = ccRawParam(d1); if (r >= 0) s->applyValue((clap_id)r, d2); }
            }
            else if (d1 == 2) s->engine.setBendRangeSemitones(ch, d2, d2);   // MPE absolute RCS
            else if (d1 == 0) s->applyValue(PID_BANK, d2 / 8);      // CC0: bank cursor
            else if (d1 == 7) s->ccVol = d2 * (1.0f / 127.0f);
            else if (d1 == 13) s->ccExp = d2 * (1.0f / 127.0f);
            else if (d1 == 32) s->applyValue(PID_BANK, d2 % (int)(PW_FACTORY_N / 8));
            else if (d1 == 64)
            {   // CC64 damper, per channel
                const bool on = d2 >= 64;
                const uint16_t bit = (uint16_t)(1u << ch);
                if ((s->sustainCh_ & bit) && !on)
                {   for (int i = 0; i < s->heldCnt2_[ch]; i++)
                    {   const uint8_t hk = s->heldNotes_[ch][i];
                        s->engine.noteOff(hk, ch); s->heldFlag_[ch][hk] = 0; }
                    s->heldCnt2_[ch] = 0;
                }
                if (on) s->sustainCh_ |= bit; else s->sustainCh_ = (uint16_t)(s->sustainCh_ & ~bit);
            }
            else if (d1 == 74) s->engine.setBrightness(d2);
            else if (d1 == 85) s->applyValue(PID_DRIFT, d2);         // Rev4 CC85 "vintage" == DRIFT
            // Muhammad extensions (MMA-unassigned in both Rev4 impl and GM;
            // see CC_MAP.md): live control of FEEDBACK + the sidecar knobs.
            else if (d1 == 42) s->applyValue((clap_id)57, d2);                    // FEEDBACK 0..127
            else if (d1 == 43) s->applyValue(PID_PAN_WIDTH, d2);                  // PAN WIDTH 0..127
            else if (d1 == 44) s->applyValue(PID_OS, d2 >= 64 ? 1 : 0);           // OVERSAMPLE toggle
            else if (d1 == 45) s->applyValue(PID_TUNE, (d2 - 64) * 100 / 63);     // MASTER TUNE +-100c
            else if (d1 == 48) s->applyValue(PID_TRIM, (d2 * 3) / 128);           // OUTPUT trim 0..2
            else if (d1 == 120) panicVoices(s, true);    // All Sound Off: KILL
            else if (d1 == 123) panicVoices(s, false);   // All Notes Off: release
            else if (d1 == 121)
            {   s->ccVol = s->ccExp = 1.0f; s->engine.setBrightness(0);
                s->engine.setAftertouch(0); s->engine.wheelReset();
                for (int c = 0; c < 16; c++) { s->rpnM_[c] = s->rpnL_[c] = 127; s->rpnV_[c] = 0; } }
            else { const int r = ccRawParam(d1); if (r >= 0) s->applyValue((clap_id)r, d2); }
            break;
        case 0xC0: applyPcFromCache(s, d1 < 8 ? s->bank * 8 + d1 : d1); break;  // audio-safe cache memcpy
        case 0xD0: s->engine.setAftertouch(d2); break;   // channel pressure: part-level (AT->flt)
        default: break;
        }
        break;
    }
    default: break;
    }
}

// ---- plugin vtable -----------------------------------------------------------
static Plugin *fromPlugin(const clap_plugin_t *p)
{ return p ? static_cast<Plugin *>(p->plugin_data) : nullptr; }

static bool plugin_init(const clap_plugin_t *p)
{
    std::fprintf(stderr, "[p5] plugin init: Muhammad build %s ("
#if defined(_WIN32)
                 "gui-win32"
#else
                 "gui-x11"
#endif
                 ", base+mod, audio-side flush)\n",
                 kDescriptor.version);
    auto *s = fromPlugin(p);
    if (!s) return false;
    for (int i = 0; i < 16; i++) { s->rpnM_[i] = s->rpnL_[i] = 127; s->rpnV_[i] = 0; }
    return true;
}

static void plugin_destroy(const clap_plugin_t *p)
{
    auto *s = fromPlugin(p);
    if (!s) return;
    // Self-defense: if the host deletes the device WITHOUT destroying the
    // GUI extension first (Bitwig: engine crash on instance delete), the
    // Ctx and its REGISTERED HOST TIMER would outlive the plugin - the
    // next timer dispatch would hit freed memory. kGui.destroy is safe to
    // call here: it early-outs once (and only once) a live Ctx exists.
#ifdef P5_GUI
    if (s->gui && kGui.destroy) kGui.destroy(p);
#endif
    delete s->self;
    delete s;
}

// Snapshot the wire params into the audio-side base bookkeeping (fresh
// activate: the engine's patch is the model, base_ mirrors its display law).
void Plugin::mirrorBaseFromEngine()
{
    for (clap_id id = 0; id < 58; id++)
    {   int v = engine.rawValue(kRawIndex[id]);
        if (p5::polarityByte(kRawIndex[id])) v &= 127;
        else if (v > 127 && !rawByteCarriesMeaning(kRawIndex[id])) v = 127;
        base_[id] = v; }
    for (clap_id id = 64; id < 64 + NUM_OVERLAYS; id++) base_[id] = engine.overlay(id - 64);
    base_[PID_TRIM] = trim.load(std::memory_order_relaxed);
    base_[PID_OS] = model_.os - 1;
    base_[PID_TUNE] = model_.tune;
    base_[PID_PROGRAM] = program < 0 ? 0 : program;
    base_[PID_BANK] = bank.load();
    base_[PID_PAN_WIDTH] = model_.pan;
    base_[PID_DRIFT] = base_[37];
}

static bool plugin_activate(const clap_plugin_t *p, double sample_rate, uint32_t min_block, uint32_t)
{
    auto *s = fromPlugin(p);
    s->sr = sample_rate;
    s->minBlock = min_block;
    s->engine.activate(sample_rate);      // rate-dependent state ONLY (review C3)
    s->engine.setSmoothing(true);         // PC playability; the A/B harness keeps it off
    s->engine.setPcMode(true);            // LFO INITIAL = wheel state at patch load
    s->engine.setReleaseCut(1.0f / 16384.0f);   // cut tails -24 dB deeper than firmware (-84 dBFS)
    p5store::loadMap();                   // bank/program retrieval map
    s->rebuildSlotCache();
    // Re-push the authoritative model (patch/patchMain contract): whatever
    // the engine forgot at its rate reset, the model restamps.
    s->applyModelToEngineDirect();
    s->mirrorBaseFromEngine();
    s->appliedGenCursor_ = s->gen_;
    s->lastMergeGen_ = 0;
    s->ow_.store(0); s->or_.store(0);
    s->repW_.store(0); s->repR_ = 0; s->repLost_.store(false);   // report ring: clean slate
    s->pendingPc_.store(-1); s->pcReq_.store(-1);
    s->idSync_.store(false); s->restartPending_ = false;
    s->latencyDirty.store(false);
    s->programA_.store(s->program, std::memory_order_relaxed);   // echo guard mirrors truth (5.8)
    for (int i = 0; i < Plugin::PEND_IDS; i++) s->dragOpen_[i] = false;
    s->publishView();                     // seed before going live
    s->active.store(true, std::memory_order_release);
    // Main-thread background timer: identity adoption + latency announce run
    // even with the panel closed. On hosts that never fire timers (older
    // Bitwig) everything still works opportunistically at GUI/save entries.
    if (s->host && s->host->get_extension)
    {   auto *t = (const clap_host_timer_support_t *)s->host->get_extension(s->host, CLAP_EXT_TIMER_SUPPORT);
        if (t && t->register_timer && !s->bgTimerOn_)
        {   if (t->register_timer(s->host, 100000, &s->bgTimerId_)) s->bgTimerOn_ = true; }
    }
    return true;
}

static void plugin_deactivate(const clap_plugin_t *p)
{
    auto *s = fromPlugin(p);
    s->active.store(false, std::memory_order_release);
    // Audio is gone; main reclaims the engine. Apply the model (covers any
    // push staged while the host stopped calling process()), then silence
    // performance state: voices that survived a host-side deactivate would
    // drone on the next activation without a note-off (review 5.10).
    s->applyModelToEngineDirect();
    s->engine.silenceReset();
    s->engine.setBend(0); s->engine.wheelReset(); s->engine.setAftertouch(0);
    for (int i = 0; i < 16; i++) s->engine.setBendChannel(i, 0);
    s->sustainCh_ = 0;
    for (int c = 0; c < 16; c++)
    {   std::memset(s->heldFlag_[c], 0, sizeof s->heldFlag_[c]); s->heldCnt2_[c] = 0; }
    s->publishView();
    // Latency is NEVER announced here: the ping makes the host re-query and
    // reconfigure the graph from inside a teardown (the Bitwig
    // device-delete crash). Hosts re-read get_latency() at every activate.
    s->restartPending_ = false;
    s->latencyDirty.store(false, std::memory_order_relaxed);
    if (s->bgTimerOn_ && s->host && s->host->get_extension)
    {   auto *t = (const clap_host_timer_support_t *)s->host->get_extension(s->host, CLAP_EXT_TIMER_SUPPORT);
        if (t && t->unregister_timer) t->unregister_timer(s->host, s->bgTimerId_);
        s->bgTimerOn_ = false; }
}

static bool plugin_start(const clap_plugin_t *p)
{   // start_processing runs on the AUDIO thread; process() drains everything
    // at block top, so there is deliberately nothing to do here.
    (void)p; return true; }
static void plugin_stop(const clap_plugin_t *) {}

static void plugin_reset(const clap_plugin_t *p)
{
    auto *s = fromPlugin(p);
    if (!s) return;
    // [audio-thread & active] or [main & inactive]: the engine OWNER calls
    // this, so it applies DIRECTLY - never through the posting machinery
    // (review C1). silenceReset keeps trims/pan/patch and re-dirties panG.
    s->engine.silenceReset();
    s->engine.setBend(0); s->engine.wheelReset(); s->engine.setAftertouch(0);
    s->engine.setBrightness(0);
    for (int i = 0; i < 16; i++) { s->engine.setBendChannel(i, 0);
        s->rpnM_[i] = s->rpnL_[i] = 127; s->rpnV_[i] = 0; }
    s->ccVol = s->ccExp = 1.0f;
    s->sustainCh_ = 0;
    for (int c = 0; c < 16; c++)
    {   std::memset(s->heldFlag_[c], 0, sizeof s->heldFlag_[c]); s->heldCnt2_[c] = 0; }
    s->pendingPc_.store(-1);
}

// render `frames` samples into the host buffers (mono engine duplicated to L/R)
static void renderChunk(Plugin *s, clap_audio_buffer_t *ab, uint32_t from, uint32_t frames)
{
    const bool stereo = ab->channel_count >= 2 && ab->data32[1] != nullptr;   // S13
    while (frames) {
        uint32_t k = frames > 32 ? 32 : frames;
        s->engine.process(s->scratch, k);
        float *L = ab->data32[0] + from, *R = stereo ? ab->data32[1] + from : nullptr;
        const float trim = (float)s->trim.load(std::memory_order_relaxed) * (1.0f / 32768.0f) * s->ccVol * s->ccExp;
        const bool pan = s->engine.panActive();
        const int32_t *rbuf = s->engine.mixR();
        for (uint32_t i = 0; i < k; i++) {
            float v = (float)s->scratch[i] * trim;
            L[i] = v;
            if (R) R[i] = pan ? (float)rbuf[i] * trim : v;
        }
        from += k;
        frames -= k;
    }
}

static clap_process_status plugin_process(const clap_plugin_t *p, const clap_process_t *pr)
{
#if defined(__SSE__) || defined(__SSE2__)
    {   const unsigned csr = _mm_getcsr();
        if ((csr & (0x8000u | 0x40u)) != (0x8000u | 0x40u)) _mm_setcsr(csr | 0x8000u | 0x40u);
    }
#endif
    auto *s = fromPlugin(p);
    if (!s->active) return CLAP_PROCESS_ERROR;
    const uint32_t count = pr->frames_count;
    clap_audio_buffer_t *ab = &pr->audio_outputs[0];
    if (!ab || !ab->data32 || !ab->data32[0]) return CLAP_PROCESS_ERROR;

    s->curOut_ = pr->out_events;                   // in-band echoes land here
    s->applyPushed();                              // main's authoritative latest state
    s->drainOut(pr->out_events);                   // GUI values + gestures reach the host
                                                   // from process() TOO (review H1): a
                                                   // spec-following host answers
                                                   // request_flush with process(), not flush
    const int pp = s->pendingPc_.exchange(-1, std::memory_order_acq_rel);
    if (pp >= 0) applyPcFromCache(s, pp);          // PC deferred by cache contention
    const clap_input_events_t *in = pr->in_events;
    uint32_t pos = 0;
    if (in) {
        uint32_t n = in->size(in);
        for (uint32_t i = 0; i < n; i++) {
            const clap_event_header_t *ev = in->get(in, i);
            if (!ev) continue;
            uint32_t t = ev->time > count ? count : ev->time;
            if (t > pos) { renderChunk(s, ab, pos, t - pos); pos = t; }
            dispatchEvent(s, ev);
        }
    }
    if (pos < count) renderChunk(s, ab, pos, count - pos);
    s->curOut_ = nullptr;
    s->publishView();                              // main's next read sees this block's truth
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t lat_get(const clap_plugin_t *p)
{
    auto *s = fromPlugin(p);
    if (!s) return 0u;
    s->refreshView();
    return s->model_.os == 2 ? 3u : 0u;
}
const clap_plugin_latency_t kLatency = { lat_get };


static const void *plugin_get_extension(const clap_plugin_t *, const char *id)
{
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) ||
        !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)) return &kPresetLoad;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimerDispatch;
#ifdef P5_GUI
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
#if !defined(_WIN32) && !defined(__APPLE__)
    if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &kPosixFd;   // 4.7
#endif
#endif
    return nullptr;
}

static const clap_plugin_t kPluginTemplate = {
    &kDescriptor, nullptr, plugin_init, plugin_destroy, plugin_activate, plugin_deactivate,
    plugin_start, plugin_stop, plugin_reset, plugin_process, plugin_get_extension, nullptr
};

// ---- audio / note ports ------------------------------------------------------
static uint32_t ap_count(const clap_plugin_t *, bool is_input) { return is_input ? 0 : 1; }
static bool ap_get(const clap_plugin_t *, uint32_t idx, bool is_input, clap_audio_port_info_t *info)
{
    if (is_input || idx != 0) return false;
    std::memset(info, 0, sizeof *info);
    info->id = 0;
    std::snprintf(info->name, sizeof info->name, "Stereo Out");
    info->channel_count = 2;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->port_type = nullptr;               // plain stereo pair
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t kAudioPorts = { ap_count, ap_get };

static uint32_t np_count(const clap_plugin_t *, bool is_input) { return is_input ? 1 : 0; }
static bool np_get(const clap_plugin_t *, uint32_t idx, bool is_input, clap_note_port_info_t *info)
{
    if (!is_input || idx != 0) return false;
    std::memset(info, 0, sizeof *info);
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof info->name, "Muhammad Keys");
    return true;
}
const clap_plugin_note_ports_t kNotePorts = { np_count, np_get };

// ---- params ------------------------------------------------------------------
static uint32_t par_count(const clap_plugin_t *) { return PARAM_COUNT; }
static bool par_get_info(const clap_plugin_t *, uint32_t index, clap_param_info_t *info)
{
    if (index >= PARAM_COUNT) return false;
    std::memset(info, 0, sizeof *info);
    clap_id id;
    if (index < 58) {
        id = index; info->min_value = 0; info->max_value = 127;
        if (const KnobClamp *k = knobClamp(id)) { info->min_value = k->lo; info->max_value = k->hi; }
    }
    else if (index < 58 + NUM_OVERLAYS) {
        int o = (int)(index - 58); id = (clap_id)(64 + o);
        info->min_value = kOverlayMin[o]; info->max_value = kOverlayMax[o];
    } else if (index < 58 + NUM_OVERLAYS + 1) { id = PID_TRIM; info->min_value = 0; info->max_value = 2; }
    else if (index < 58 + NUM_OVERLAYS + 2) { id = PID_OS; info->min_value = 0; info->max_value = 1; }
    else if (index < 58 + NUM_OVERLAYS + 3) { id = PID_TUNE; info->min_value = -100; info->max_value = 100; }
    else if (index < 58 + NUM_OVERLAYS + 4) { id = PID_PROGRAM; info->min_value = 0; info->max_value = PW_FACTORY_N - 1; }
    else if (index < 58 + NUM_OVERLAYS + 5) { id = PID_BANK; info->min_value = 0; info->max_value = PW_FACTORY_N / 8 - 1; }
    else if (index < 58 + NUM_OVERLAYS + 6) { id = PID_PAN_WIDTH; info->min_value = 0; info->max_value = 127; }
    else { id = PID_DRIFT; info->min_value = 0; info->max_value = 127; }
    info->id = id;
    info->default_value = Plugin::defaultOf(id);           // 5.7: hosts must see a default
    const bool stepped = isRawParam(id) || id == PID_TUNE || id == PID_PROGRAM || id == PID_BANK ||
                         id == PID_PAN_WIDTH || id == PID_DRIFT;
    // C2/5.9: PROGRAM, BANK and OVERSAMPLE are discrete patch/perf switches -
    // automating or modulating them re-loads factories at audio rate and
    // moves latency without notice. They stay panel/CC-only.
    const bool switchish = id == PID_PROGRAM || id == PID_BANK || id == PID_OS;
    info->flags = (switchish ? 0u : (CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE)) |
                  (stepped ? CLAP_PARAM_IS_STEPPED : 0u);
    std::snprintf(info->name, sizeof info->name, "%s", Plugin::nameOf(id));
    std::snprintf(info->module, sizeof info->module,
                  isRawParam(id) ? "P5" : isOverlayParam(id) ? "EDIT" : "PERFORM");
    return true;
}
static bool knownParamId(clap_id id)
{   return isRawParam(id) || isOverlayParam(id) || (id >= PID_TRIM && id <= PID_DRIFT); }
static bool par_get_value(const clap_plugin_t *p, clap_id id, double *out)
{
    auto *s = fromPlugin(p);
    if (!s || !knownParamId(id)) return false;          // 5.7: unknown ids answer FALSE
    s->refreshView();                                   // merge audio truth first
    *out = paramValue(s, id);
    return true;
}
static bool par_value_to_text(const clap_plugin_t *, clap_id id, double v, char *buf, uint32_t cap)
{
    if (id == 20) { std::snprintf(buf, cap, "%s", v < 1.0 ? "SSI" : "CURTIS"); return true; }
    if (id == 52 || id == 51) { std::snprintf(buf, cap, "%s", v < 1.0 ? "OFF" : "ON"); return true; }
    if (id == 19) { static const char *const kKt[3] = {"OFF","HALF","FULL"};
                    std::snprintf(buf, cap, "%s", kKt[(int)v < 0 ? 0 : (int)v > 2 ? 2 : (int)v]); return true; }
    if (id == 56) { static const char *const kPr[4] = {"LOW","LOW RET","LAST","LAST RET"};
                    std::snprintf(buf, cap, "%s", kPr[(int)v & 3]); return true; }
    if (id == PID_TUNE) { std::snprintf(buf, cap, "%+d cents", (int)(v < 0 ? v - 0.5 : v + 0.5)); return true; }
    if (id == PID_PROGRAM || id == PID_BANK || id == PID_PAN_WIDTH || id == PID_DRIFT) { std::snprintf(buf, cap, "%d", (int)(v + 0.5)); return true; }
    if (isOverlayParam(id)) std::snprintf(buf, cap, "%+g", v);
    else std::snprintf(buf, cap, "%g", v);
    return true;
}
static bool par_text_to_value(const clap_plugin_t *, clap_id id, const char *text, double *out)
{
    double v = 0;
    if (std::sscanf(text, "%lf", &v) != 1) return false;
    if (isRawParam(id)) {
        if (const KnobClamp *k = knobClamp(id)) v = v < k->lo ? k->lo : v > k->hi ? k->hi : v;
        else v = v < 0 ? 0 : v > 127 ? 127 : v;
    } else if (id == PID_TRIM) v = v < 0 ? 0 : v > 2 ? 2 : v;
    else if (id == PID_OS) v = v < 0 ? 0 : v > 1 ? 1 : v;
    else if (id == PID_TUNE) v = v < -100 ? -100 : v > 100 ? 100 : v;
    else if (id == PID_PROGRAM) v = v < 0 ? 0 : v > PW_FACTORY_N - 1 ? PW_FACTORY_N - 1 : v;
    else if (id == PID_BANK) v = v < 0 ? 0 : v > PW_FACTORY_N / 8 - 1 ? PW_FACTORY_N / 8 - 1 : v;
    else if (id == PID_PAN_WIDTH) v = v < 0 ? 0 : v > 127 ? 127 : v;
    *out = v;
    return true;
}
static void par_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out)
{
    auto *s = fromPlugin(p);
    if (!s || !in) return;
    // params.flush is [active ? audio-thread : main-thread] (params.h:303).
    // In BOTH cases this thread is the engine's owner while the call runs,
    // so the input is dispatched DIRECTLY (review C1: no postCmd, no
    // viewMx_ from audio, no second producer on any queue). GUI values and
    // gestures reach the host from here too, and a publish makes the flush
    // readback (get_value right after) immediate.
    const bool live = s->active.load(std::memory_order_relaxed);
    if (live) s->curOut_ = out;
    s->applyPushed();
    s->drainOut(out);
    const uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; i++)
    {   const clap_event_header_t *ev = in->get(in, i);
        if (!ev) continue;
        dispatchEvent(s, ev); }
    const int pp = s->pendingPc_.exchange(-1, std::memory_order_acq_rel);
    if (pp >= 0) applyPcFromCache(s, pp);
    if (live) { s->curOut_ = nullptr; s->publishView(); }
}
const clap_plugin_params_t kParams = { par_count, par_get_info, par_get_value,
                                       par_value_to_text, par_text_to_value, par_flush };

// ---- state (versioned binary blob) --------------------------------------------
#define PW_STATE_MAGIC 0x5035434Cu            // "P5CL"
#define PW_STATE_VERSION 11u   // v11: patch grew to 134 bytes (byte 133 = FEEDBACK)

// Host streams may deliver SHORT reads and writes (stream.h) - the old
// single-shot compare against n failed chunking hosts outright (review H5).
static bool wrAll(const clap_ostream_t *o, const void *b, size_t n)
{   size_t off = 0;
    while (off < n)
    {   const int64_t w = o->write(o, (const char *)b + off, (uint64_t)(n - off));
        if (w <= 0) return false;
        off += (size_t)w; }
    return true; }

static bool state_save(const clap_plugin_t *p, const clap_ostream_t *out)
{
    auto *s = fromPlugin(p);
    s->adoptPendingIdentity();
    s->refreshView();                       // model_ = the whole truth now
    char buf[2048]; size_t off = 0;   // whole v11 blob packed here (see W guard)
    auto W = [&](const void *b, size_t n) { if (off + n > sizeof buf) return false;
                                            std::memcpy(buf + off, b, n); off += n; return true; };
    uint32_t head[2] = { PW_STATE_MAGIC, PW_STATE_VERSION };
    int16_t gui[3] = { (int16_t)s->model_.tune, (int16_t)s->program, (int16_t)s->bank.load() };
    int16_t tr[32];
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 2; k++)
        { tr[4*i+2*k] = s->model_.trimV[i][k][0]; tr[4*i+2*k+1] = s->model_.trimV[i][k][1]; }
    float trim = s->trim.load(std::memory_order_relaxed);
    int32_t os = s->model_.os;
    int16_t pan = (int16_t)s->model_.pan;
    int16_t br[2] = { (int16_t)s->model_.rbrUp, (int16_t)s->model_.rbrDn };
    int32_t tn = s->tuneToneCount_;
    double tt[64] = {};
    for (int i = 0; i < s->tuneToneCount_ && i < 64; i++) tt[i] = s->tuneTones_[i];
    int32_t tmeta[3] = { s->tuneRoot_, (int32_t)s->tuneMask_, s->tuneWalk_ };
    if (!(W(head, sizeof head) && W(s->model_.raw, 134) && W(s->model_.ov, sizeof s->model_.ov)
          && W(&trim, sizeof trim) && W(&os, sizeof os) && W(gui, sizeof gui) && W(&pan, sizeof pan)
          && W(tr, sizeof tr) && W(s->model_.dev, sizeof s->model_.dev) && W(br, sizeof br)
          && W(s->tuneName_, sizeof s->tuneName_) && W(&tn, sizeof tn) && W(tt, sizeof tt)
          && W(tmeta, sizeof tmeta)))
        return false;
    return wrAll(out, buf, off);
}

namespace {
struct RCursor { const char *p; size_t n = 0, off = 0; bool ok = true;
    bool rd(void *b, size_t k)
    {   if (!ok || off + k > n) { ok = false; return false; }
        std::memcpy(b, p + off, k); off += k; return true; } };
}

static bool state_load_impl(const clap_plugin_t *p, const clap_istream_t *in);
static bool state_load(const clap_plugin_t *p, const clap_istream_t *in)
{   try { return state_load_impl(p, in); } catch (...) { return false; } }   // S3
static bool state_load_impl(const clap_plugin_t *p, const clap_istream_t *in)
{
    auto *s = fromPlugin(p);
    // Read the whole stream first (size-capped), parse into locals, and only
    // POST on a complete parse: a failure mid-blob used to leave tune/pan/
    // trims half-applied (review H5: non-transactional load).
    // Host streams legally answer SHORTER than asked - or 0/negative when the
    // full request cannot be satisfied. Halve the request on a miss so the
    // tail of the blob is always reachable (a host that answers 0 for any
    // request past EOF still terminates cleanly).
    std::vector<char> blob;
    {   char tmp[512];
        for (int guard = 0; guard < (1 << 22);)
        {   size_t want = 512;
            bool advanced = false;
            while (want >= 1)
            {   const int64_t r = in->read(in, tmp, want);
                if (r > 0)
                {   blob.insert(blob.end(), tmp, tmp + r);
                    if (blob.size() > (4096u * 1024u)) return false;
                    advanced = true; guard++; break; }
                want >>= 1; }
            if (!advanced) break; } }
    RCursor c{ blob.data(), blob.size(), 0, true };
    uint32_t head[2];
    if (!c.rd(head, sizeof head)) return false;
    if (head[0] != PW_STATE_MAGIC || head[1] == 0u || head[1] > PW_STATE_VERSION) return false;

    // parsed-into-locals targets
    uint8_t raw[134] = {}; size_t rsz = 133;
    int16_t ov[8] = {}; float trim = 0.5f; int32_t os = 1;
    int tune = 0, program = -1, bankv = 0, pan = 0;
    // absent/short trim side-channel = neutral 64 (not 0 - see ctor comment)
    int16_t tr[32]; for (int i = 0; i < 32; i++) tr[i] = 64; int trn = 0;
    int16_t dev[128] = {}; bool hasDev = false; int16_t br[2] = { 256, 256 }; bool hasBr = false;
    char name[40] = "12-TET"; int32_t migDr = -1;
    int32_t tn = 0; double tt[64] = {}; bool hasTones = false;
    int16_t migDr16 = -1;                    // v5..v9 side-channel drift
    int32_t tmeta[3] = { 0, 0, 0 };

    if (head[1] == 1u)
    {   if (!c.rd(raw, 133)) return false;
        uint8_t pad; if (!c.rd(&pad, 1)) return false;          // v1 struct padding
        if (!c.rd(ov, sizeof ov) || !c.rd(&trim, sizeof trim)) return false;
        rsz = 133;
    }
    else
    {   rsz = head[1] >= 11u ? 134u : 133u;
        if (!c.rd(raw, rsz) || !c.rd(ov, sizeof ov) ||
            !c.rd(&trim, sizeof trim) || !c.rd(&os, sizeof os)) return false;
        if (head[1] >= 3u)
        {   int16_t gui[3] = { 0, -1, 0 };
            if (!c.rd(gui, sizeof gui)) return false;
            tune = gui[0]; program = gui[1]; bankv = gui[2];
            if (head[1] >= 4u) { int16_t pv = 0; if (!c.rd(&pv, sizeof pv)) return false; pan = pv; }
            if (head[1] >= 5u)
            {   // the historical side-channel drift field is int16 (v5..v9)
                if (head[1] <= 9u && !c.rd(&migDr16, sizeof migDr16)) return false;
                migDr = migDr16;
                trn = head[1] >= 6u ? 32 : 16;
                if (!c.rd(tr, (size_t)trn * sizeof tr[0])) return false;
                if (head[1] >= 7u)
                {   if (!c.rd(dev, sizeof dev)) return false;
                    for (int k = 0; k < 128; k++)
                    { if (dev[k] < -512) dev[k] = -512; if (dev[k] > 512) dev[k] = 512; }
                    hasDev = true;
                    if (!c.rd(br, sizeof br)) return false;
                    hasBr = true;
                    if (!c.rd(name, sizeof name)) return false;
                    name[sizeof name - 1] = 0;
                    if (head[1] >= 8u)
                    {   if (!c.rd(&tn, sizeof tn)) return false;
                        if (!c.rd(tt, sizeof tt)) return false;
                        hasTones = true;
                        if (head[1] >= 9u && !c.rd(tmeta, sizeof tmeta)) return false; }
                }
            }
        }
    }
    if (!c.ok) return false;                       // nothing posted yet (H5)

    // bounds: host numbers are guessed-at data
    { if (program < -1 || program >= (int)PW_FACTORY_N) program = -1;
      if (bankv < 0 || bankv > 127) bankv = 0;
      if (!(trim >= 0.0f && trim <= 2.0f)) trim = 0.5f;   // H4: the PARAM range is 0..2
                                                           // (CC48 reaches 2); NaN -> default
      if (os != 2) os = 1; }

    p5::Patch patch;
    std::memset(&patch, 0, sizeof patch);
    std::memcpy(patch.raw, raw, rsz);
    patch.size = 133; patch.model = 0x32; patch.command = 3;
    s->postTune(tune);
    s->postLoad(patch);
    s->program = program;
    s->programA_.store(program, std::memory_order_relaxed);   // 5.8: echo guard starts synced
    s->bank.store(bankv);
    s->postPan(pan);
    for (int i = 0; i < 8; i++)
        if (trn >= 32) s->postTrimPair(i, 0, tr[4*i], tr[4*i+1]),
                       s->postTrimPair(i, 1, tr[4*i+2], tr[4*i+3]);
        else if (trn >= 16) s->postTrimPair(i, 0, tr[2*i], tr[2*i+1]),
                            s->postTrimPair(i, 1, tr[2*i], tr[2*i+1]);
    if (head[1] <= 9u && migDr >= 0)
        s->postRaw(p5::P5_VINTAGE, migDr > 127 ? 127 : migDr);
    if (hasDev) s->postTuning(dev);
    std::memcpy(s->tuneName_, name, sizeof s->tuneName_);
    if (hasBr && br[0] >= 0 && br[1] >= 0)
        s->postBendDefault(br[0] / 256.0f, br[1] / 256.0f);
    if (hasTones && tn >= 2 && tn <= 64)
    {   s->tuneToneCount_ = tn;
        for (int i = 0; i < tn; i++) s->tuneTones_[i] = tt[i]; }
    else s->tuneToneCount_ = 0;
    if (head[1] >= 9u) { s->tuneRoot_ = tmeta[0]; s->tuneMask_ = (uint16_t)tmeta[1]; s->tuneWalk_ = tmeta[2]; }
    s->markSource(0, s->program, s->srcFile_);   // restored session floats as saved
    if (s->program >= 0 && !std::memcmp(patch.raw, PW_FACTORY_RAW[s->program], 133)
        && patch.raw[133] == 0)   // factory identity only while the extension byte is 0
    { s->curSrc_.kind = p5store::SourceKind::Factory; s->curSrc_.factory = s->program; }
    else if (!s->srcFile_.empty())
    { s->curSrc_.kind = p5store::SourceKind::File; s->curSrc_.file = s->srcFile_; }
    for (int i = 0; i < 8; i++) s->postOverlay(i, ov[i]);
    s->trim.store(trim, std::memory_order_relaxed);
    s->postOS(os);
    return true;
}

const clap_plugin_state_t kState = { state_save, state_load };

// ---- preset load (built-in factory programs) -----------------------------------
// load_key: "0" = INIT, "1".."200" = Sequential's v1.03 programs.
static bool preset_from_location(const clap_plugin_t *p, uint32_t location_kind,
                                 const char *location, const char *load_key)
{
    auto *s = fromPlugin(p);
    if (!load_key) return false;
    const bool pluginLoc = location_kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
    bool ok = false;
    if (!std::strncmp(load_key, "user:", 5))
    {   if (!pluginLoc && location_kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE) return false;
        ok = s->loadUserPatchExact(load_key + 5); }
    else if (!pluginLoc) return false;
    else if (!std::strcmp(load_key, "init"))
    {   s->recallBasicInit(); ok = true; }
    else
    {   char *end = nullptr;
        const long k = std::strtol(load_key, &end, 10);
        if (!end || *end != '\0' || k < 0 || k > (long)PW_FACTORY_N) return false;   // 5.8: garbage must FAIL, not load 0
        if (k == 0)
        {   if (s->loadUserPatch("Peace Be Upon Him")) ok = true;
            else { p5::Patch i; p5::patchInit(i); p5::patchDefaultName(i);
                   s->postLoad(i); s->program = -1; s->programA_.store(-1);
                   s->markSource(0, 0, std::string()); s->syncAllParams(); ok = true; } }
        else
        {   s->loadProgram((int)(k - 1));   // 5.8: through the SAME path as panel clicks
            ok = true; }
    }
    if (ok && s->host && s->host->get_extension)
    {   s->refreshView();
        // tell the host it landed: rescan values + loaded() (5.8)
        auto *hp = (const clap_host_params_t *)s->host->get_extension(s->host, CLAP_EXT_PARAMS);
        if (hp && hp->rescan) hp->rescan(s->host, CLAP_PARAM_RESCAN_VALUES);
        auto *hl = (const clap_host_preset_load_t *)s->host->get_extension(s->host, CLAP_EXT_PRESET_LOAD);
        if (hl && hl->loaded) hl->loaded(s->host, location_kind, location, load_key);
    }
    return ok;
}

const clap_plugin_preset_load_t kPresetLoad = { preset_from_location };

// A brand-new instance shows the default patch (bare INIT content, curated
// name) so the first thing you see when loading is "Peace Be Upon Him".
static void seedDefaultPatch(Plugin *s)
{
    uint8_t raw[134];
    if (p5store::loadPatch("Peace Be Upon Him", raw))
    {   // the user recorded a real default: that file IS the default patch now
        p5::Patch p; std::memset(&p, 0, sizeof p); std::memcpy(p.raw, raw, 134);
        p.size = 133; p.model = 0x32; p.command = 3;
        s->postLoad(p);
        s->markSource(1, -1, "Peace Be Upon Him");
        s->curSrc_.kind = p5store::SourceKind::File; s->curSrc_.file = "Peace Be Upon Him";
        return;
    }
    // No recorded file here (fresh machine, CI artifact): boot on the
    // EMBEDDED snapshot of the user's "Peace Be Upon Him" - every platform's
    // first load is the same sound. tools/gen_default_hdr.py refreshes it.
    p5::Patch e; std::memset(&e, 0, sizeof e);
    std::memcpy(e.raw, pw::PW_DEFAULT_RAW, 134);
    e.size = 133; e.model = 0x32; e.command = 3;
    s->postLoad(e);
    s->markSource(1, -1, std::string("Peace Be Upon Him"));
    s->curSrc_.kind = p5store::SourceKind::Factory; s->curSrc_.factory = -2;
}

// ---- instance creation ----------------------------------------------------------
const clap_plugin_t *muhammad_create(const clap_host_t *host)
{
    auto *data = new Plugin(host);
    seedDefaultPatch(data);
    data->self = new clap_plugin_t(kPluginTemplate);
    data->self->plugin_data = data;
    return data->self;
}


} // namespace pw
