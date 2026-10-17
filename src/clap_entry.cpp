// SPDX-License-Identifier: GPL-3.0-only
// CLAP entry point for Muhammad (structure follows the vasynth CLAP reference:
// one shared library, plugin factory + preset discovery factory).
#include <clap/clap.h>
#include <cstring>
#include "muhammad_plugin.hpp"
#include "muhammad_discovery.h"

namespace {

uint32_t factory_get_plugin_count(const clap_plugin_factory *) { return 1; }

const clap_plugin_descriptor_t *factory_get_plugin_descriptor(const clap_plugin_factory *, uint32_t index)
{
    return index == 0 ? &pw::kDescriptor : nullptr;
}

const clap_plugin_t *factory_create_plugin(const clap_plugin_factory *, const clap_host_t *host,
                                           const char *plugin_id)
{
    if (!clap_version_is_compatible(host->clap_version)) return nullptr;
    if (std::strcmp(plugin_id, pw::kDescriptor.id) != 0) return nullptr;
    return pw::muhammad_create(host);
}

const clap_plugin_factory_t gFactory = {
    factory_get_plugin_count,
    factory_get_plugin_descriptor,
    factory_create_plugin,
};

bool entry_init(const char *) { return true; }
void entry_deinit(void) {}

const void *entry_get_factory(const char *factory_id)
{
    if (!std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &gFactory;
    if (!std::strcmp(factory_id, CLAP_PRESET_DISCOVERY_FACTORY_ID) ||
        !std::strcmp(factory_id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT))
        return muhammadDiscoveryFactory();
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entry_init,
    entry_deinit,
    entry_get_factory,
};
