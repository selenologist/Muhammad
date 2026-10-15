/* dlopen the built prophet.clap and drive it through the CLAP surface the way
 * a host would: entry, factory, init, activate, events, process, params,
 * preset-load, state save/load. Fails loudly on any contract violation. */
#include <clap/clap.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

static int failures = 0;
static void check(const char *what, bool ok)
{
    printf("clap: %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

struct EventList {
    std::vector<const clap_event_header_t *> evs;
};
static uint32_t EL_size(const clap_input_events_t *s) { return (uint32_t)((const EventList *)s->ctx)->evs.size(); }
static const clap_event_header_t *EL_get(const clap_input_events_t *s, uint32_t i)
{
    const auto *l = (const EventList *)s->ctx;
    return i < l->evs.size() ? l->evs[i] : nullptr;
}
static bool EL_try_out(const clap_output_events_t *, const clap_event_header_t *) { return true; }

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char *path = argc > 1 ? argv[1] : "./prophet.clap";
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("dlopen %s: %s\n", path, dlerror()); return 2; }
    auto *entry = (const clap_plugin_entry_t *)dlsym(lib, "clap_entry");
    check("clap_entry exported", entry && clap_version_is_compatible(entry->clap_version));
    if (!entry) return 2;
    check("entry.init", entry->init(path));
    auto *getf = entry->get_factory;
    auto *factory = (const clap_plugin_factory_t *)getf(CLAP_PLUGIN_FACTORY_ID);
    check("plugin factory", factory && factory->get_plugin_count(factory) == 1);
    auto *desc = factory->get_plugin_descriptor(factory, 0);
    check("descriptor", desc && !strcmp(desc->id, "muhammad.unresolvable"));
    check("preset discovery factory", getf(CLAP_PRESET_DISCOVERY_FACTORY_ID) != nullptr);

    clap_host_t host{};
    host.clap_version = CLAP_VERSION_INIT;
    host.host_data = nullptr;
    host.get_extension = [](const clap_host_t *, const char *) -> const void * { return nullptr; };
    const clap_plugin_t *plugin = factory->create_plugin(factory, &host, desc->id);
    check("create_plugin + init", plugin && plugin->init(plugin));
    if (!plugin) { printf("clap smoke: FAILED (no plugin)\n"); return 1; }
    const void *ap = plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS);
    const void *np = plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS);
    const void *pp = plugin->get_extension(plugin, CLAP_EXT_PARAMS);
    const void *st = plugin->get_extension(plugin, CLAP_EXT_STATE);
    const void *pl = plugin->get_extension(plugin, CLAP_EXT_PRESET_LOAD);
    check("extensions (audio/note/params/state/preset-load)", ap && np && pp && st && pl);
    if (ap) check("one output port", ((const clap_plugin_audio_ports_t *)ap)->count(plugin, false) == 1);
    if (pp) check("param count >= 60", ((const clap_plugin_params_t *)pp)->count(plugin) >= 60);

    check("activate 44100", plugin->activate(plugin, 44100.0, 32, 1024));

    // events: three notes over one second
    std::vector<clap_event_note> notes;
    auto pushNote = [&](uint32_t frame, uint16_t type, int16_t key, double vel) {
        notes.emplace_back();
        auto &e = notes.back();
        memset(&e, 0, sizeof e);
        e.header.time = frame; e.header.type = type;
        e.channel = 0; e.key = key; e.velocity = vel;
    };
    pushNote(0, CLAP_EVENT_NOTE_ON, 60, 127.0 / 127.0);
    pushNote(22050, CLAP_EVENT_NOTE_ON, 64, 0.8);
    pushNote(22050, CLAP_EVENT_NOTE_ON, 67, 0.9);

    EventList all;
    for (auto &n : notes) all.evs.push_back(&n.header);
    size_t hePos = 0;

    float L[512], R[512];
    float *chan[2] = { L, R };
    clap_audio_buffer_t ab{};
    ab.channel_count = 2; ab.data32 = chan;
    clap_output_events_t out{ &all, EL_try_out };
    plugin->start_processing(plugin);
    double peak = 0;
    bool nonzero = false, lrEqual = true, procOk = true;
    const uint32_t total = 44100;
    for (uint32_t done = 0; done < total; done += 512) {
        uint32_t n = total - done < 512 ? total - done : 512;
        EventList due;
        while (hePos < all.evs.size() && all.evs[hePos]->time <= done) {
            // rewrite time to block-relative 0 for this delivery
            auto *ev = const_cast<clap_event_header_t *>(all.evs[hePos]);
            ev->time = 0;
            due.evs.push_back(ev);
            hePos++;
        }
        clap_input_events_t in{ &due, EL_size, EL_get };
        clap_process_t pr{};
        pr.in_events = &in; pr.out_events = &out;
        pr.audio_outputs = &ab; pr.frames_count = n;
        if (plugin->process(plugin, &pr) != CLAP_PROCESS_CONTINUE) procOk = false;
        for (uint32_t i = 0; i < n; i++) {
            if (std::fabs(L[i]) > 1e-6) nonzero = true;
            if (std::fabs(L[i]) > peak) peak = std::fabs(L[i]);
            if (L[i] != R[i]) lrEqual = false;
        }
    }
    plugin->stop_processing(plugin);
    check("process status", procOk);
    check("audio is non-silent", nonzero);
    check("peak in a sane band (0.01..2.0)", peak > 0.01 && peak <= 2.0);
    check("mono engine duplicated to L/R", lrEqual);
    printf("clap: peak %.3f\n", peak);

    auto *params = (const clap_plugin_params_t *)pp;

    if (pl) {
        auto *load = (const clap_plugin_preset_load_t *)pl;
        check("load factory program 1", load->from_location(plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN,
                                                            nullptr, "1"));
    }

    clap_event_param_value pv{};
    pv.header.type = CLAP_EVENT_PARAM_VALUE; pv.header.time = 0;
    pv.param_id = 17; pv.value = 42;
    EventList pe{ { &pv.header } };
    clap_input_events_t pin{ &pe, EL_size, EL_get };
    params->flush(plugin, &pin, &out);
    double got = -1;
    check("param CUTOFF readback after flush", params->get_value(plugin, 17, &got) && got == 42);

    if (st) {
        auto *state = (const clap_plugin_state_t *)st;
        std::vector<char> blob;
        auto wr = [](const clap_ostream_t *s, const void *b, uint64_t n) -> int64_t {
            auto *v = (std::vector<char> *)s->ctx;
            v->insert(v->end(), (const char *)b, (const char *)b + n); return (int64_t)n; };
        clap_ostream_t os{ &blob, wr };
        check("state save", state->save(plugin, &os));
        pv.value = 7;
        params->flush(plugin, &pin, &out);          // same list object, new value already applied on flush
        double mid = -1; params->get_value(plugin, 17, &mid);
        std::vector<char> copy = blob;
        auto rd = [](const clap_istream_t *s, void *b, uint64_t n) -> int64_t {
            auto *v = (std::vector<char> *)s->ctx;
            if (v->size() < n) return -1;
            for (uint64_t i = 0; i < n; i++) ((char *)b)[i] = (*v)[i];
            v->erase(v->begin(), v->begin() + (long)n); return (int64_t)n; };
        clap_istream_t is{ &copy, rd };
        check("state load", state->load(plugin, &is));
        double v17 = -1; params->get_value(plugin, 17, &v17);
        check("state restored the patch (42)", v17 == 42);
        (void)mid;
    }

    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    dlclose(lib);
    printf("clap smoke: %s\n", failures ? "FAILED" : "all checks ok");
    return failures ? 1 : 0;
}
