// SPDX-License-Identifier: GPL-3.0-only
// CLAP plugin wrapper for the Prophet engine (see prophet_plugin.hpp).
#include "prophet_plugin.hpp"
#include "p5_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pw {

// ---- parameter model ---------------------------------------------------------
static const char *kRawNames[57] = {
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
    "BEND RANGE", "RETRIGGER"
};
static const int kRawIndex[57] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 86, 87
};
static const char *kOverlayNames[8] = {
    "EDIT CUTOFF", "EDIT RESO", "EDIT TUNE A", "EDIT TUNE B",
    "EDIT PW A", "EDIT PW B", "EDIT MIX A", "EDIT MIX B"
};
static const double kOverlayMin[8] = { -63, -63, -24, -24, -63, -63, -63, -63 };
static const double kOverlayMax[8] = {  63,  63,  24,  24,  63,  63,  63,  63 };

constexpr uint32_t PARAM_COUNT = 57 + NUM_OVERLAYS + 5;   // + trim, oversample, tune, program, bank

static bool isRawParam(clap_id id) { return id < 57; }

// Effective ranges (shared with the GUI/golden normalizer in p5_common.hpp),
// so host display, engine behaviour and the panel render cannot drift.
using KnobClamp = p5::KnobRange;
static const KnobClamp *knobClamp(clap_id id) { return p5::knobRange((int)id); }

static bool isOverlayParam(clap_id id) { return id >= 64 && id < 64 + NUM_OVERLAYS; }

const char *Plugin::nameOf(clap_id id)
{
    if (isRawParam(id)) return kRawNames[id];
    if (isOverlayParam(id)) return kOverlayNames[id - 64];
    if (id == PID_TRIM) return "OUTPUT";
    if (id == PID_OS) return "OVERSAMPLE 2X";
    if (id == PID_TUNE) return "MASTER TUNE";
    if (id == PID_PROGRAM) return "PROGRAM";
    if (id == PID_BANK) return "BANK";
    return "?";
}

void Plugin::applyRaw(int idx, int v)
{
    if (idx < 0 || idx >= (int)p5::RAW_MAX) return;
    engine.editRaw(idx, v < 0 ? 0 : v > 255 ? 255 : v);
}
int Plugin::rawOf(int idx) const { return engine.rawValue(idx); }

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
    "1.9.2-polymod",   // bump per shipped build: the version Bitwig shows proves which binary is loaded
    "P5 clone (PROPHET engine ported from the Melodee firmware)",
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

static void applyParam(Plugin *s, clap_id id, double value)
{
    if (isRawParam(id)) {
        int v = (int)(value < 0 ? 0 : value + 0.5);
        if (const KnobClamp *k = knobClamp(id)) {
            if (v < (int)k->lo) v = (int)k->lo;
            if (v > (int)k->hi) v = (int)k->hi;
        } else if (v > 127 && !rawByteCarriesMeaning(kRawIndex[id])) v = 127;
        s->applyRaw(kRawIndex[id], v);
    }
    else if (isOverlayParam(id))
        s->engine.setOverlay((int)(id - 64), (int)(value < 0 ? value - 0.5 : value + 0.5));
    else if (id == PID_TRIM) s->trim = (float)value;
    else if (id == PID_OS) {
        s->engine.setOverSample(value > 0.5 ? 2 : 1);
        // Latency changed with the decimator — but hosts dislike a ping while
        // activated (old Bitwig logs "plugin misbehaving"). Defer to deactivate;
        // 3 samples of stale latency mid-session is inaudible, the warning is not.
        if (!s->active) {
            if (s->host && s->host->get_extension) {
                auto *l = (const clap_host_latency_t *)s->host->get_extension(s->host, CLAP_EXT_LATENCY);
                if (l && l->changed) l->changed(s->host);
            }
        } else {
            s->latencyDirty = true;
        }
    }
    else if (id == PID_TUNE)
        s->engine.setTuneCents((int)(value < 0 ? value - 0.5 : value + 0.5));
    else if (id == PID_PROGRAM)
    {
        // Host echo guard: while the patch is not a factory pick (INIT, PBEUH,
        // user file, assigned slot) we report PROGRAM=0 as a placeholder; if the
        // host pushes that same 0 back (flush / restore / automation resend),
        // loading factory 0 here would silently replace the patch the user just
        // picked. Only a real change drives loadProgram.
        if (s->program < 0 && (int)(value + 0.5) == 0) return;   // placeholder echo, always
        if (s->program >= 0 && (int)(value + 0.5) == s->program) return;
        s->loadProgram((int)(value + 0.5));
    }
    else if (id == PID_BANK) {
        int v = (int)(value + 0.5);
        if (v < 0) v = 0;
        if (v > (int)PW_FACTORY_N / 8 - 1) v = (int)PW_FACTORY_N / 8 - 1;
        s->bank = v;
    }
}

static void requestHostParamFlush(Plugin *s)
{
    if (!s->host || !s->host->get_extension) return;
    auto *hp = (const clap_host_params_t *)s->host->get_extension(s->host, CLAP_EXT_PARAMS);
    if (hp && hp->request_flush) hp->request_flush(s->host);
}

static double paramValue(Plugin *s, clap_id id)
{
    if (isRawParam(id)) {
        int v = s->rawOf(kRawIndex[id]);
        if (const KnobClamp *k = knobClamp(id)) {
            if (k->defWhenZero && v == 0) v = k->def;   // raw 0 COUNT = "default 5"
            if (v > (int)k->hi) v = (int)k->hi;         // saturating knobs
        } else if (v > 127) v = 127;                    // engine reads clamped too
        return (double)v;
    }
    if (isOverlayParam(id)) return (double)s->engine.overlay((int)(id - 64));
    if (id == PID_TRIM) return s->trim;
    if (id == PID_OS) return (double)(s->engine.oversample() - 1);
    if (id == PID_TUNE) return s->engine.tuneCents();
    if (id == PID_PROGRAM) return s->program < 0 ? 0 : s->program;
    if (id == PID_BANK) return s->bank;
    return 0;
}

double Plugin::guiValue(clap_id id) { return paramValue(this, id); }

void Plugin::guiSetLocal(clap_id id, double v)
{
    // Gesture in progress: the engine follows instantly; the host learns the
    // FINAL value at gesture end. Pushing per-motion events would make the
    // host echo the whole drag history back at us as input events, and the
    // stale echoes re-apply old positions — the audible "catch-up trail".
    applyParam(this, id, v);
}

void Plugin::guiSet(clap_id id, double v)
{
    applyParam(this, id, v);
    markPending(id, paramValue(this, id));
    requestHostParamFlush(this);
}

void Plugin::guiSync(clap_id id)
{
    markPending(id, paramValue(this, id));
    requestHostParamFlush(this);
}

void Plugin::loadProgram(int k)
{
    loadTag_ = "fac";
    if (k < 0 || k >= (int)PW_FACTORY_N) return;
    engine.loadFactory((uint32_t)k);
    markSource(0, k, std::string());
    curSrc_.kind = p5store::SourceKind::Factory; curSrc_.factory = k; curSrc_.file.clear();
    program = k;
    bank = k / 8;
    // the whole patch changed under the host's feet: sync every knob
    for (clap_id id = 0; id < 57; id++)
        markPending(id, paramValue(this, id));
    requestHostParamFlush(this);
}

void Plugin::markSource(int kind, int idx, const std::string &file)
{
    std::memcpy(srcRaw_, engine.patch().raw, 133);
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

bool Plugin::loadUserPatch(const std::string &name)
{
    loadTag_ = "user";
    uint8_t raw[133];
    if (!p5store::loadPatch(name, raw)) return false;
    p5::Patch p;
    std::memset(&p, 0, sizeof p);
    std::memcpy(p.raw, raw, 133);
    p.size = 133; p.model = 0x32; p.command = 3;
    engine.setPatch(p);
    program = -1;
    markSource(1, -1, p5store::sanitize(name));
    curSrc_.kind = p5store::SourceKind::File; curSrc_.factory = 0; curSrc_.file = p5store::sanitize(name);
    for (clap_id id = 0; id < 57; id++)
        markPending(id, paramValue(this, id));
    requestHostParamFlush(this);
    return true;
}

void Plugin::syncAllParams()
{
    for (clap_id id = 0; id < 57; id++) markPending(id, paramValue(this, id));
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
        engine.setPatch(i); program = -1; markSource(0, -2, {}); syncAllParams();
        return;
    }
    const int idx = s.factory;
    if (idx >= 0 && idx < (int)PW_FACTORY_N) { loadProgram(idx); return; }
    if (idx == -2 && loadUserPatch("Peace Be Upon Him"))
    { curSrc_ = s; return; }   // recorded default wins, but the requested row identity stays
    p5::Patch i; p5::patchInit(i);
    if (idx != -1) p5::patchDefaultName(i);
    engine.setPatch(i); program = -1; markSource(0, idx, {}); syncAllParams();
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
    engine.setPatch(i); program = -1; markSource(0, -1, {});
    floatValid_ = false;
    syncAllParams();
}

void Plugin::savePatchNamed(const std::string &name)
{
    loadTag_ = "saved";
    const std::string n = p5store::sanitize(name);
    if (n.empty()) return;
    p5::Patch p = engine.patch();
    std::memset(p.raw + p5::P5_NAME, ' ', p5::P5_NAME_LEN);
    const size_t len = n.size() < p5::P5_NAME_LEN ? n.size() : p5::P5_NAME_LEN;
    std::memcpy(p.raw + p5::P5_NAME, n.data(), len);
    engine.setPatch(p);
    program = -1;
    p5store::savePatch(n, engine.patch().raw);
    markSource(1, -1, n);
    curSrc_.kind = p5store::SourceKind::File; curSrc_.factory = 0; curSrc_.file = n;
    for (clap_id id = 0; id < 57; id++)
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
}

void Plugin::presetRecall()
{   loadTag_ = "recall";
    // PRESET: un-float; press again (nothing touched since) and the floating edits come BACK
    if (!hasSrc_) return;
    const uint8_t *cur = engine.patch().raw;
    if (!panelDirty() && floatValid_ && std::memcmp(cur, floatSnap_, 133) != 0)
    {   // second press right after a recall: restore the pre-recall panel state
        p5::Patch p = engine.patch();
        std::memcpy(p.raw, floatSnap_, 133);
        engine.setPatch(p);
        syncAllParams();
        return;
    }
    if (panelDirty())
    {
        std::memcpy(floatSnap_, cur, 133);
        floatValid_ = true;
    }
    p5::Patch p = engine.patch();
    std::memcpy(p.raw, srcRaw_, 133);
    engine.setPatch(p);
    syncAllParams();
}

// ---- events ------------------------------------------------------------------
static void dispatchNoteOn(Plugin *s, int key, int vel)
{
    if (key < 0 || key > 127) return;
    if (vel <= 0) vel = 1;
    if (vel > 127) vel = 127;
    s->engine.noteOn(key, vel);
}

static void dispatchEvent(Plugin *s, const clap_event_header_t *ev)
{
    switch (ev->type) {
    case CLAP_EVENT_NOTE_ON: {
        auto *e = reinterpret_cast<const clap_event_note *>(ev);
        dispatchNoteOn(s, e->key, (int)(e->velocity * 127.0 + 0.5));
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        auto *e = reinterpret_cast<const clap_event_note *>(ev);
        s->engine.noteOff(e->key);
        break;
    }
    case CLAP_EVENT_PARAM_VALUE:
    case CLAP_EVENT_PARAM_MOD: {
        auto *e = reinterpret_cast<const clap_event_param_value *>(ev);
        applyParam(s, e->param_id, e->value);
        break;
    }
    case CLAP_EVENT_MIDI: {
        auto *e = reinterpret_cast<const clap_event_midi *>(ev);
        const uint8_t st = e->data[0] & 0xF0u, d1 = e->data[1], d2 = e->data[2];
        switch (st) {
        case 0x90: if (d2) dispatchNoteOn(s, d1, d2); else s->engine.noteOff(d1); break;
        case 0x80: s->engine.noteOff(d1); break;
        case 0xE0: s->bendRaw = (int)(((d2 << 7) | d1) - 8192); s->engine.setBend(s->bendRaw); break;
        case 0xB0:
            if (d1 == 1) s->engine.setModWheel(d2);
            else if (d1 == 0) applyParam(s, PID_BANK, d2 / 8);      // CC0: bank cursor
            break;
        case 0xC0: s->programSlot(d1 < 8 ? s->bank * 8 + d1 : d1); break;  // legacy grid or flat
        case 0xD0: s->engine.setAftertouch(d2); break;
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
    std::fprintf(stderr, "[p5] plugin init: Muhammad build %s (gui-x11, 7-bit law, coalesced gestures, xembed)\n",
                 kDescriptor.version);
    return fromPlugin(p) != nullptr;
}

static void plugin_destroy(const clap_plugin_t *p)
{
    auto *s = fromPlugin(p);
    if (!s) return;
    delete s->self;
    delete s;
}

static bool plugin_activate(const clap_plugin_t *p, double sample_rate, uint32_t, uint32_t)
{
    auto *s = fromPlugin(p);
    s->sr = sample_rate;
    s->engine.activate(sample_rate);
    s->engine.setSmoothing(true);   // PC playability; the A/B harness keeps it off
    s->engine.setPcMode(true);      // LFO INITIAL = wheel state at patch load
    p5store::loadMap();             // bank/program retrieval map
    s->active = true;
    return true;
}

static void plugin_deactivate(const clap_plugin_t *p)
{
    auto *s = fromPlugin(p);
    s->active = false;
    if (s->latencyDirty && s->host && s->host->get_extension) {
        auto *l = (const clap_host_latency_t *)s->host->get_extension(s->host, CLAP_EXT_LATENCY);
        if (l && l->changed) l->changed(s->host);
    }
    s->latencyDirty = false;
}

static bool plugin_start(const clap_plugin_t *) { return true; }
static void plugin_stop(const clap_plugin_t *) {}

static void plugin_reset(const clap_plugin_t *p)
{
    auto *s = fromPlugin(p);
    s->engine.silenceReset();
    s->engine.setBend(0);
    s->engine.wheelReset();   // re-take the patch's wheel position (pc mode)
    s->engine.setAftertouch(0);
}

// render `frames` samples into the host buffers (mono engine duplicated to L/R)
static void renderChunk(Plugin *s, clap_audio_buffer_t *ab, uint32_t from, uint32_t frames)
{
    while (frames) {
        uint32_t k = frames > 32 ? 32 : frames;
        s->engine.process(s->scratch, k);
        float *L = ab->data32[0] + from, *R = ab->data32[1] + from;
        const float trim = s->trim * (1.0f / 32768.0f);
        for (uint32_t i = 0; i < k; i++) {
            float v = (float)s->scratch[i] * trim;
            L[i] = v;
            R[i] = v;
        }
        from += k;
        frames -= k;
    }
}

static clap_process_status plugin_process(const clap_plugin_t *p, const clap_process_t *pr)
{
    auto *s = fromPlugin(p);
    if (!s->active) return CLAP_PROCESS_ERROR;
    const uint32_t count = pr->frames_count;
    clap_audio_buffer_t *ab = &pr->audio_outputs[0];
    if (!ab || !ab->data32 || !ab->data32[0]) return CLAP_PROCESS_ERROR;

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
    return CLAP_PROCESS_CONTINUE;
}

static uint32_t lat_get(const clap_plugin_t *p)
{
    auto *s = fromPlugin(p);
    return s ? s->engine.latencySamples() : 0u;
}
const clap_plugin_latency_t kLatency = { lat_get };

static const void *plugin_get_extension(const clap_plugin_t *, const char *id)
{
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD)) return &kPresetLoad;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
#ifdef P5_GUI
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGuiX11;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimerSupport;
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
    if (index < 57) {
        id = index; info->min_value = 0; info->max_value = 127;
        if (const KnobClamp *k = knobClamp(id)) { info->min_value = k->lo; info->max_value = k->hi; }
    }
    else if (index < 57 + NUM_OVERLAYS) {
        int o = (int)(index - 57); id = (clap_id)(64 + o);
        info->min_value = kOverlayMin[o]; info->max_value = kOverlayMax[o];
    } else if (index < 57 + NUM_OVERLAYS + 1) { id = PID_TRIM; info->min_value = 0; info->max_value = 2; }
    else if (index < 57 + NUM_OVERLAYS + 2) { id = PID_OS; info->min_value = 0; info->max_value = 1; }
    else if (index < 57 + NUM_OVERLAYS + 3) { id = PID_TUNE; info->min_value = -100; info->max_value = 100; }
    else if (index < 57 + NUM_OVERLAYS + 4) { id = PID_PROGRAM; info->min_value = 0; info->max_value = PW_FACTORY_N - 1; }
    else { id = PID_BANK; info->min_value = 0; info->max_value = PW_FACTORY_N / 8 - 1; }
    info->id = id;
    const bool stepped = isRawParam(id) || id == PID_TUNE || id == PID_PROGRAM || id == PID_BANK;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE |
                  (stepped ? CLAP_PARAM_IS_STEPPED : 0);
    std::snprintf(info->name, sizeof info->name, "%s", Plugin::nameOf(id));
    std::snprintf(info->module, sizeof info->module,
                  isRawParam(id) ? "P5" : isOverlayParam(id) ? "EDIT" : "PERFORM");
    return true;
}
static bool par_get_value(const clap_plugin_t *p, clap_id id, double *out)
{
    auto *s = fromPlugin(p);
    if (!s) return false;
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
    if (id == PID_PROGRAM || id == PID_BANK) { std::snprintf(buf, cap, "%d", (int)(v + 0.5)); return true; }
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
    *out = v;
    return true;
}
static void par_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out)
{
    auto *s = fromPlugin(p);
    if (!s || !in) return;
    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; i++) {
        const clap_event_header_t *ev = in->get(in, i);
        if (ev) dispatchEvent(s, ev);
    }
    // drain GUI-initiated changes so the host's UI/automation sees them
    // (coalesced: at most one value per param, so a fast drag cannot leave a
    // catch-up trail behind the pointer)
    if (!out) return;
    for (clap_id id = 0; id < Plugin::PEND_IDS; id++) {
        if (!s->pendSet[id]) continue;
        s->pendSet[id] = false;
        clap_event_param_value e{};
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.header.time = 0;
        e.param_id = id;
        e.cookie = nullptr;
        e.value = s->pendVal[id];
        out->try_push(out, &e.header);
    }
}
const clap_plugin_params_t kParams = { par_count, par_get_info, par_get_value,
                                       par_value_to_text, par_text_to_value, par_flush };

// ---- state (versioned binary blob) --------------------------------------------
#define PW_STATE_MAGIC 0x5035434Cu            // "P5CL"
#define PW_STATE_VERSION 3u
static bool wr(const clap_ostream_t *o, const void *b, size_t n)
{ return o->write(o, b, (uint64_t)n) == (int64_t)n; }
static bool state_save(const clap_plugin_t *p, const clap_ostream_t *out)
{
    auto *s = fromPlugin(p);
    uint32_t head[2] = { PW_STATE_MAGIC, PW_STATE_VERSION };
    int16_t ov[8];
    for (int i = 0; i < 8; i++) ov[i] = (int16_t)s->engine.overlay(i);
    float trim = s->trim;
    int32_t os = s->engine.oversample();
    int16_t gui[3] = { (int16_t)s->engine.tuneCents(), (int16_t)s->program, (int16_t)s->bank };
    return wr(out, head, sizeof head) && wr(out, s->engine.patch().raw, 133) &&
           wr(out, ov, sizeof ov) && wr(out, &trim, sizeof trim) && wr(out, &os, sizeof os) &&
           wr(out, gui, sizeof gui);
}
static bool rd(const clap_istream_t *i, void *b, size_t n)
{ return i->read(i, b, (uint64_t)n) == (int64_t)n; }
static bool state_load(const clap_plugin_t *p, const clap_istream_t *in)
{
    auto *s = fromPlugin(p);
    uint32_t head[2];
    if (!rd(in, head, sizeof head)) return false;
    if (head[0] != PW_STATE_MAGIC || head[1] == 0u || head[1] > PW_STATE_VERSION) return false;
    uint8_t raw[133]; int16_t ov[8]; float trim; int32_t os = 1;
    if (head[1] == 1u) {
        struct StateBlobV1 { uint32_t magic, version; uint8_t raw[133]; int16_t overlay[8]; float trim; } b;
        b.magic = head[0]; b.version = head[1];
        if (!rd(in, &b.raw, sizeof b - offsetof(StateBlobV1, raw))) return false;   // includes v1 padding
        std::memcpy(raw, b.raw, 133);
        std::memcpy(ov, b.overlay, sizeof ov);
        trim = b.trim;
    } else {
        if (!rd(in, raw, sizeof raw) || !rd(in, ov, sizeof ov) ||
            !rd(in, &trim, sizeof trim) || !rd(in, &os, sizeof os)) return false;
        if (head[1] >= 3u) {
            int16_t gui[3] = {0, -1, 0};
            if (!rd(in, gui, sizeof gui)) return false;
            s->engine.setTuneCents(gui[0]);
            s->program = gui[1];
            s->bank = gui[2];
        }
    }
    p5::Patch patch;
    std::memset(&patch, 0, sizeof patch);
    std::memcpy(patch.raw, raw, 133);
    patch.size = 133; patch.model = 0x32; patch.command = 3;
    s->engine.setPatch(patch);
    s->markSource(0, s->program, s->srcFile_);   // restored session floats as saved
    if (s->program >= 0 && !std::memcmp(patch.raw, PW_FACTORY_RAW[s->program], 133))
    { s->curSrc_.kind = p5store::SourceKind::Factory; s->curSrc_.factory = s->program; }
    else if (!s->srcFile_.empty())
    { s->curSrc_.kind = p5store::SourceKind::File; s->curSrc_.file = s->srcFile_; }
    for (int i = 0; i < 8; i++) s->engine.setOverlay(i, ov[i]);
    s->trim = trim;
    s->engine.setOverSample((int)os);
    return true;
}
const clap_plugin_state_t kState = { state_save, state_load };

// ---- preset load (built-in factory programs) -----------------------------------
// load_key: "0" = INIT, "1".."200" = Sequential's v1.03 programs.
static bool preset_from_location(const clap_plugin_t *p, uint32_t location_kind,
                                 const char *location, const char *load_key)
{
    auto *s = fromPlugin(p);
    if (location_kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && load_key &&
        !std::strncmp(load_key, "user:", 5)) return s->loadUserPatch(load_key + 5);
    if (location_kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && load_key &&
        !std::strcmp(load_key, "init")) { s->recallBasicInit(); return true; }
    (void)location;
    if (location_kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || !load_key) return false;
    int k = atoi(load_key);
    if (k < 0 || k > (int)PW_FACTORY_N) return false;
    if (k == 0 && s->loadUserPatch("Peace Be Upon Him")) return true;
    if (k == 0) {
        p5::Patch i; p5::patchInit(i); p5::patchDefaultName(i);
        s->engine.setPatch(i); s->program = -1; s->markSource(0, 0, std::string());
        for (clap_id id = 0; id < 57; id++) s->markPending(id, paramValue(s, id));
        requestHostParamFlush(s);
        return true;
    }
    else s->engine.loadFactory((uint32_t)(k - 1));
    return true;
}
const clap_plugin_preset_load_t kPresetLoad = { preset_from_location };
// A brand-new instance shows the default patch (bare INIT content, curated
// name) so the first thing you see when loading is "Peace Be Upon Him".
static void seedDefaultPatch(Plugin *s)
{
    uint8_t raw[133];
    if (p5store::loadPatch("Peace Be Upon Him", raw))
    {   // the user recorded a real default: that file IS the default patch now
        p5::Patch p; std::memset(&p, 0, sizeof p); std::memcpy(p.raw, raw, 133);
        p.size = 133; p.model = 0x32; p.command = 3;
        s->engine.setPatch(p);
        s->markSource(1, -1, "Peace Be Upon Him");
        s->curSrc_.kind = p5store::SourceKind::File; s->curSrc_.file = "Peace Be Upon Him";
        return;
    }
    p5::Patch i; p5::patchInit(i); p5::patchDefaultName(i);
    s->engine.setPatch(i);
    s->markSource(0, 0, std::string());
    s->curSrc_.kind = p5store::SourceKind::Factory; s->curSrc_.factory = -2;
}

// ---- instance creation ----------------------------------------------------------
const clap_plugin_t *prophet_create(const clap_host_t *host)
{
    auto *data = new Plugin(host);
    seedDefaultPatch(data);
    data->self = new clap_plugin_t(kPluginTemplate);
    data->self->plugin_data = data;
    return data->self;
}


} // namespace pw
