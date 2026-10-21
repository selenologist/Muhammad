// SPDX-License-Identifier: GPL-3.0-only
#include <clap/clap.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "muhammad_discovery.h"
#include "p5_engine.hpp"
#include "p5_store.hpp"
#include "p5_factory.hpp"
#include <string>
#include <vector>

namespace {

constexpr const char *kProviderId = "muhammad.unresolvable.factory";
static uint32_t presetCount() { return 1u + (uint32_t)p5fac::count(); }   // INIT + (0|200) factory

struct Provider {
    const clap_preset_discovery_indexer_t *indexer = nullptr;
};

bool provider_init(const clap_preset_discovery_provider *p)
{
    auto *d = static_cast<Provider *>(p->provider_data);
    clap_preset_discovery_location_t loc{};
    loc.kind = CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
    loc.name = "Muhammad factory presets";
    loc.flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT;
    loc.location = nullptr;
    if (!d->indexer || !d->indexer->declare_location(d->indexer, &loc)) return false;
    // User files live in a FILE location (review 5.8): hosts watch the
    // folder, so a patch saved from the panel appears without a re-index;
    // under a PLUGIN location they never could.
    static std::string userDir;
    userDir = p5store::baseDir();
    clap_preset_discovery_location_t uloc{};
    uloc.kind = CLAP_PRESET_DISCOVERY_LOCATION_FILE;
    uloc.name = "Muhammad user patches";
    uloc.flags = 0;
    uloc.location = userDir.c_str();
    d->indexer->declare_location(d->indexer, &uloc);
    clap_preset_discovery_soundpack_t pack{};
    pack.id = "muhammad.unresolvable.factory";
    pack.name = "P5 clone (v1.03 factory programs)";
    pack.flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT;
    d->indexer->declare_soundpack(d->indexer, &pack);
    pack.id = "muhammad.unresolvable.user";
    pack.name = "Muhammad user patches";
    pack.flags = 0;
    d->indexer->declare_soundpack(d->indexer, &pack);
    return true;
}

void provider_destroy(const clap_preset_discovery_provider *p)
{
    delete static_cast<Provider *>(p->provider_data);
    delete p;
}

// Report all built-in presets to the receiver (one location = the plugin itself).
bool provider_get_metadata_impl(uint32_t location_kind, const char *location,
                               const clap_preset_discovery_metadata_receiver_t *receiver)
{
    const std::string udir = p5store::baseDir();
    const bool userLoc = location_kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE &&
                         location && udir == location;
    if (!userLoc &&
        (location_kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location != nullptr)) return false;
    if (userLoc)
    {   for (const std::string &name : p5store::listPatches())
        {   if (!receiver->begin_preset(receiver, name.c_str(), ("user:" + name).c_str())) return false;
            clap_universal_plugin_id_t upid{};
            upid.abi = "clap";
            upid.id = "muhammad.unresolvable";
            receiver->add_plugin_id(receiver, &upid);
            receiver->set_soundpack_id(receiver, "muhammad.unresolvable.user");
            receiver->set_flags(receiver, CLAP_PRESET_DISCOVERY_IS_USER_CONTENT);
            receiver->add_feature(receiver, "synthesizer");
        }
        return true;
    }
    for (uint32_t i = 0; i < presetCount(); i++) {
        char name[33];
        p5::Engine::patchNameAt(i, name);
        char key[16];   // runtime bound (presetCount) - gcc can no longer prove 8 fits
        std::snprintf(key, sizeof key, "%u", i);
        if (!receiver->begin_preset(receiver, name, key)) return false;
        clap_universal_plugin_id_t upid{};
        upid.abi = "clap";
        upid.id = "muhammad.unresolvable";
        receiver->add_plugin_id(receiver, &upid);
        receiver->set_soundpack_id(receiver, "muhammad.unresolvable.factory");
        receiver->set_flags(receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
        receiver->add_feature(receiver, "synthesizer");
    }
    {   // the bare INIT patch, loadable straight from the browser
        if (!receiver->begin_preset(receiver, "INIT", "init")) return false;
        clap_universal_plugin_id_t upid{};
        upid.abi = "clap";
        upid.id = "muhammad.unresolvable";
        receiver->add_plugin_id(receiver, &upid);
        receiver->set_soundpack_id(receiver, "muhammad.unresolvable.factory");
        receiver->set_flags(receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
        receiver->add_feature(receiver, "synthesizer");
    }
    return true;
}

bool provider_get_metadata(const clap_preset_discovery_provider *p, uint32_t location_kind,
                           const char *location,
                           const clap_preset_discovery_metadata_receiver_t *receiver)
{   // S3: this runs as a direct host callback - an escaping exception
    // terminates the host. The scan is error_code-based, but string ops
    // and the store layer keep a wall.
    (void)p;
    try { return provider_get_metadata_impl(location_kind, location, receiver); }
    catch (...) { return false; }
}

const void *provider_get_extension(const clap_preset_discovery_provider *, const char *)
{
    return nullptr;
}

const clap_preset_discovery_provider_descriptor_t kProviderDesc = {
    CLAP_VERSION_INIT, kProviderId, "Muhammad presets (factory + user patches)", "selenologist"
};

uint32_t factory_count(const clap_preset_discovery_factory *) { return 1; }

const clap_preset_discovery_provider_descriptor_t *factory_get_descriptor(
    const clap_preset_discovery_factory *, uint32_t index)
{
    return index == 0 ? &kProviderDesc : nullptr;
}

const clap_preset_discovery_provider_t *factory_create(const clap_preset_discovery_factory *,
                                                       const clap_preset_discovery_indexer_t *indexer,
                                                       const char *provider_id)
{
    if (std::strcmp(provider_id, kProviderId) != 0) return nullptr;
    auto *data = new Provider();
    data->indexer = indexer;
    auto *prov = new clap_preset_discovery_provider_t();
    prov->desc = &kProviderDesc;
    prov->provider_data = data;
    prov->init = provider_init;
    prov->destroy = provider_destroy;
    prov->get_metadata = provider_get_metadata;
    prov->get_extension = provider_get_extension;
    return prov;
}

const clap_preset_discovery_factory_t kFactory = {
    factory_count, factory_get_descriptor, factory_create
};

} // namespace

extern "C" const clap_preset_discovery_factory_t *muhammadDiscoveryFactory()
{
    return &kFactory;
}
