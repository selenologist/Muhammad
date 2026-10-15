// SPDX-License-Identifier: GPL-3.0-only
// Preset discovery for the built-in Prophet-5 factory bank: INIT + Sequential's
// 200 v1.03 programs, embedded in the plugin (PW_FACTORY_RAW), following the
// vasynth CLAP provider pattern.
#pragma once
#include <clap/clap.h>
#ifdef __cplusplus
extern "C" const clap_preset_discovery_factory_t *p5ProphetDiscoveryFactory();
#endif
