// SPDX-License-Identifier: GPL-3.0-only
// CLAP plugin wrapper for the Prophet engine: params, events, state, preset load.
#pragma once
#include <clap/clap.h>
#include "p5_engine.hpp"

namespace pw {

// param ids: 0..56 = native wire patch knob bytes (0..255, exact raw semantics)
//            64..71 = the engine editor overlays P_E0..P_E7 (signed)
//            80     = output trim
constexpr int NUM_OVERLAYS = 8;
constexpr clap_id PID_TRIM = 80;

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

    void applyRaw(int idx, int v);
    int rawOf(int idx) const;
    static const char *nameOf(clap_id id);
};

extern const clap_plugin_audio_ports_t kAudioPorts;
extern const clap_plugin_note_ports_t kNotePorts;
extern const clap_plugin_params_t kParams;
extern const clap_plugin_state_t kState;
extern const clap_plugin_preset_load_t kPresetLoad;
extern const clap_plugin_descriptor_t kDescriptor;

const clap_plugin_t *prophet_create(const clap_host_t *host);

} // namespace pw
