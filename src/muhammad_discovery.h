// SPDX-License-Identifier: GPL-3.0-only
// Preset discovery for the Prophet-5 bank: INIT (ours) + Sequential's 200
// v1.03 programs - which are NOT embedded: they live in the strippable
// sidecar (p5_factory) and the factory rows vanish when it is deleted.
// Follows the vasynth CLAP provider pattern.
#pragma once
#include <clap/clap.h>
#ifdef __cplusplus
extern "C" const clap_preset_discovery_factory_t *muhammadDiscoveryFactory();
#endif
