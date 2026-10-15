// SPDX-License-Identifier: GPL-3.0-only
// CLAP plugin wrapper for the Prophet engine (see prophet_plugin.hpp).
#include "prophet_plugin.hpp"

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

constexpr uint32_t PARAM_COUNT = 57 + NUM_OVERLAYS + 2;   // + trim, oversample

static bool isRawParam(clap_id id) { return id < 57; }
static bool isOverlayParam(clap_id id) { return id >= 64 && id < 64 + NUM_OVERLAYS; }

const char *Plugin::nameOf(clap_id id)
{
    if (isRawParam(id)) return kRawNames[id];
    if (isOverlayParam(id)) return kOverlayNames[id - 64];
    if (id == PID_TRIM) return "OUTPUT";
    if (id == PID_OS) return "OVERSAMPLE 2X";
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
    "Prophet",
    "Melodee port",
    "https://github.com/keremimo/melodee",
    nullptr, nullptr,
    "0.1",
    "Prophet-5 (Rev-4 wire) engine ported from the Melodee firmware",
    kFeatures
};

// ---- params <-> engine -------------------------------------------------------
static void applyParam(Plugin *s, clap_id id, double value)
{
    if (isRawParam(id)) s->applyRaw(kRawIndex[id], (int)(value < 0 ? 0 : value + 0.5));
    else if (isOverlayParam(id))
        s->engine.setOverlay((int)(id - 64), (int)(value < 0 ? value - 0.5 : value + 0.5));
    else if (id == PID_TRIM) s->trim = (float)value;
    else if (id == PID_OS) {
        s->engine.setOverSample(value > 0.5 ? 2 : 1);
        if (s->host && s->host->get_extension) {           // latency changed with the decimator
            auto *l = (const clap_host_latency_t *)s->host->get_extension(s->host, CLAP_EXT_LATENCY);
            if (l && l->changed) l->changed(s->host);
        }
    }
}

static double paramValue(Plugin *s, clap_id id)
{
    if (isRawParam(id)) return (double)s->rawOf(kRawIndex[id]);
    if (isOverlayParam(id)) return (double)s->engine.overlay((int)(id - 64));
    if (id == PID_TRIM) return s->trim;
    if (id == PID_OS) return (double)(s->engine.oversample() - 1);
    return 0;
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
        case 0xB0: if (d1 == 1) s->engine.setModWheel(d2); break;
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

static bool plugin_init(const clap_plugin_t *p) { return fromPlugin(p) != nullptr; }

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
    s->active = true;
    return true;
}

static void plugin_deactivate(const clap_plugin_t *p) { fromPlugin(p)->active = false; }

static bool plugin_start(const clap_plugin_t *) { return true; }
static void plugin_stop(const clap_plugin_t *) {}

static void plugin_reset(const clap_plugin_t *p)
{
    auto *s = fromPlugin(p);
    s->engine.silenceReset();
    s->engine.setBend(0);
    s->engine.setModWheel(0);
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
    std::snprintf(info->name, sizeof info->name, "Prophet Keys");
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
    if (index < 57) { id = index; info->min_value = 0; info->max_value = 255; }
    else if (index < 57 + NUM_OVERLAYS) {
        int o = (int)(index - 57); id = (clap_id)(64 + o);
        info->min_value = kOverlayMin[o]; info->max_value = kOverlayMax[o];
    } else if (index < 57 + NUM_OVERLAYS + 1) { id = PID_TRIM; info->min_value = 0; info->max_value = 2; }
    else { id = PID_OS; info->min_value = 0; info->max_value = 1; }
    info->id = id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE |
                  (isRawParam(id) ? CLAP_PARAM_IS_STEPPED : 0);
    std::snprintf(info->name, sizeof info->name, "%s", Plugin::nameOf(id));
    std::snprintf(info->module, sizeof info->module,
                  isRawParam(id) ? "PROPHET" : isOverlayParam(id) ? "EDIT" : "PERFORM");
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
    if (id == 4) { std::snprintf(buf, cap, "%s", v < 1.0 ? "SSI" : "CURTIS"); return true; }
    if (isOverlayParam(id)) std::snprintf(buf, cap, "%+g", v);
    else std::snprintf(buf, cap, "%g", v);
    return true;
}
static bool par_text_to_value(const clap_plugin_t *, clap_id id, const char *text, double *out)
{
    double v = 0;
    if (std::sscanf(text, "%lf", &v) != 1) return false;
    if (isRawParam(id)) v = v < 0 ? 0 : v > 255 ? 255 : v;
    else if (id == PID_TRIM) v = v < 0 ? 0 : v > 2 ? 2 : v;
    else if (id == PID_OS) v = v < 0 ? 0 : v > 1 ? 1 : v;
    *out = v;
    return true;
}
static void par_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *)
{
    auto *s = fromPlugin(p);
    if (!s || !in) return;
    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; i++) {
        const clap_event_header_t *ev = in->get(in, i);
        if (ev) dispatchEvent(s, ev);
    }
}
const clap_plugin_params_t kParams = { par_count, par_get_info, par_get_value,
                                       par_value_to_text, par_text_to_value, par_flush };

// ---- state (versioned binary blob) --------------------------------------------
#define PW_STATE_MAGIC 0x5035434Cu            // "P5CL"
#define PW_STATE_VERSION 2u
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
    return wr(out, head, sizeof head) && wr(out, s->engine.patch().raw, 133) &&
           wr(out, ov, sizeof ov) && wr(out, &trim, sizeof trim) && wr(out, &os, sizeof os);
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
    }
    p5::Patch patch;
    std::memset(&patch, 0, sizeof patch);
    std::memcpy(patch.raw, raw, 133);
    patch.size = 133; patch.model = 0x32; patch.command = 3;
    s->engine.setPatch(patch);
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
    (void)location;
    if (location_kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || !load_key) return false;
    int k = atoi(load_key);
    if (k < 0 || k > (int)PW_FACTORY_N) return false;
    if (k == 0) { p5::Patch i; p5::patchInit(i); s->engine.setPatch(i); }
    else s->engine.loadFactory((uint32_t)(k - 1));
    return true;
}
const clap_plugin_preset_load_t kPresetLoad = { preset_from_location };

// ---- instance creation ----------------------------------------------------------
const clap_plugin_t *prophet_create(const clap_host_t *host)
{
    auto *data = new Plugin(host);
    data->self = new clap_plugin_t(kPluginTemplate);
    data->self->plugin_data = data;
    return data->self;
}

} // namespace pw
