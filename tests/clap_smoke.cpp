/* dlopen the built prophet.clap and drive it through the CLAP surface the way
 * a host would: entry, factory, init, activate, events, process, params,
 * preset-load, state save/load. Fails loudly on any contract violation. */
#include <clap/clap.h>
#include <clap/ext/latency.h>
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
    setenv("P5_STORE_DIR", "/tmp/muh-smoke-store", 1);   // never touch the user's patch store
    const char *path = argc > 1 ? argv[1] : "./muhammad.clap";
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
    {   // A brand-new instance must show the default patch: INIT content, raw
        // name "Peace Be Upon Him" (the bare patch itself stays "INIT").
        struct O { static int64_t w(const clap_ostream_t *o, const void *d, uint64_t n) {
            auto *v = (std::vector<char> *)o->ctx; const char *p2 = (const char *)d;
            v->insert(v->end(), p2, p2 + n); return (int64_t)n; } };
        std::vector<char> fresh;
        clap_ostream_t osf{ &fresh, O::w };
        const auto *st0 = (const clap_plugin_state_t *)plugin->get_extension(plugin, CLAP_EXT_STATE);
        bool okSave = st0 && st0->save(plugin, &osf);
        bool okName = okSave && fresh.size() > 8 + 65 + 17 &&
                      !std::memcmp(fresh.data() + 8 + 65, "Peace Be Upon Him", 17);
        check("fresh instance shows default patch", okName);
        {   // REGRESSION: host echoes the placeholder PROGRAM=0 back after the
            // fresh PBEUH seed (flush/restore resend). Must NOT hijack the patch.
            const clap_plugin_params_t *prm = (const clap_plugin_params_t *)plugin->get_extension(plugin, CLAP_EXT_PARAMS);
            const clap_plugin_state_t *stt = (const clap_plugin_state_t *)plugin->get_extension(plugin, CLAP_EXT_STATE);
            double reported = -7; prm->get_value(plugin, 83, &reported);
            clap_event_param_value echo{};
            echo.header.type = CLAP_EVENT_PARAM_VALUE; echo.header.time = 0;
            echo.param_id = 83; echo.value = reported;
            EventList el{ { &echo.header } };
            clap_input_events_t in{ &el, EL_size, EL_get };
            prm->flush(plugin, &in, nullptr);
            prm->flush(plugin, &in, nullptr);              // twice: guard must be stateless
            std::vector<char> after;
            struct O2 { static int64_t w2(const clap_ostream_t *o, const void *d, uint64_t n) {
                auto *v = (std::vector<char> *)o->ctx; const char *p2 = (const char *)d;
                v->insert(v->end(), p2, p2 + n); return (int64_t)n; } };
            clap_ostream_t os2{ &after, O2::w2 };
            bool ok2 = stt->save(plugin, &os2) && after.size() > 8 + 65 + 17 &&
                       !std::memcmp(after.data() + 8 + 65, "Peace Be Upon Him", 17);
            check("host PROGRAM=0 echo keeps default patch", ok2);
        }
    }
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

    std::vector<clap_event_midi_t> mids;
    mids.emplace_back();                       // MIDI PC 1 on the AUDIO thread:
    auto &pc = mids.back();                    // must be a cache memcpy, no file I/O
    std::memset(&pc, 0, sizeof pc);
    pc.header.type = CLAP_EVENT_MIDI; pc.header.size = sizeof pc; pc.header.time = 0;
    pc.data[0] = 0xC0; pc.data[1] = 1; pc.data[2] = 0;

    EventList all;
    all.evs.push_back(&pc.header);
    for (auto &n : notes) all.evs.push_back(&n.header);
    size_t hePos = 0;

    float L[512], R[512];
    typedef void (*awsetf)(int);
    typedef long (*awcntf)(void);
    auto aw_set = (awsetf)dlsym(RTLD_DEFAULT, "allocwatch_set");
    auto aw_count = (awcntf)dlsym(RTLD_DEFAULT, "allocwatch_count");
    long aw_blocks = 0;
    if (!aw_set) std::printf("clap: (allocwatch not preloaded — audio-alloc check skipped)\n");

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
        if (aw_set) aw_set(1);                                   // count only plugin work
        const auto st = plugin->process(plugin, &pr);
        if (aw_set) { aw_set(0); ++aw_blocks; }
        if (st != CLAP_PROCESS_CONTINUE) procOk = false;
        for (uint32_t i = 0; i < n; i++) {
            if (std::fabs(L[i]) > 1e-6) nonzero = true;
            if (std::fabs(L[i]) > peak) peak = std::fabs(L[i]);
            if (L[i] != R[i]) lrEqual = false;
        }
    }
    plugin->stop_processing(plugin);
    check("process status", procOk);
    if (aw_count && aw_blocks)
    {   // audio-thread contract: process() must not touch the allocator
        const long allocs = aw_count();
        std::printf("clap: audio-thread allocs during %d process blocks: %ld\n", (int)aw_blocks, allocs);
        check("zero allocations on audio thread", allocs == 0);
    }
    {   // the audio-thread PC must have switched to slot 1's default (factory "After Ringer")
        const clap_plugin_state_t *st9 = (const clap_plugin_state_t *)plugin->get_extension(plugin, CLAP_EXT_STATE);
        std::vector<char> blob9;
        struct O9 { static int64_t w9(const clap_ostream_t *o, const void *d, uint64_t n) {
            auto *v = (std::vector<char> *)o->ctx; const char *p2 = (const char *)d;
            v->insert(v->end(), p2, p2 + n); return (int64_t)n; } };
        clap_ostream_t os9{ &blob9, O9::w9 };
        bool ok9 = st9->save(plugin, &os9) && blob9.size() > 8 + 65 + 12 &&
                   !std::memcmp(blob9.data() + 8 + 65, "After Ringer", 12);
        check("audio MIDI PC applied from cache (no I/O)", ok9);
    }
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

    // unison knobs: effective ranges and read-back must match the behaviour
    {
        clap_param_info_t info{};
        bool ok53 = params->get_info(plugin, 53, &info) && info.id == 53 &&
                    info.min_value == 1 && info.max_value == 8;
        check("UNISON CNT range is 1..8 (not 0..255)", ok53);
    {   // KEYBD>FLT: three honest detents with names, over-range clamps to FULL
        const clap_plugin_params_t *pp = (const clap_plugin_params_t *)plugin->get_extension(plugin, CLAP_EXT_PARAMS);
        clap_param_info_t i19{};
        bool ok19 = pp && pp->get_info(plugin, 19, &i19) &&
                    i19.min_value == 0.0 && i19.max_value == 2.0;
        check("KEYBD>FLT range is 0..2", ok19);
        char tx[32];
        bool texts = pp->value_to_text(plugin, 19, 1.0, tx, sizeof tx) && !strcmp(tx, "HALF") &&
                     pp->value_to_text(plugin, 19, 2.0, tx, sizeof tx) && !strcmp(tx, "FULL") &&
                     pp->value_to_text(plugin, 19, 0.0, tx, sizeof tx) && !strcmp(tx, "OFF");
        check("KEYBD>FLT detent text OFF/HALF/FULL", texts);
    }
        clap_event_param_value pc{};
        pc.header.type = CLAP_EVENT_PARAM_VALUE; pc.header.time = 0;
        pc.param_id = 53; pc.value = 2;
        EventList pcl{ { &pc.header } };
        clap_input_events_t pl{ &pcl, EL_size, EL_get };
        params->flush(plugin, &pl, &out);
        double v53 = -1; params->get_value(plugin, 53, &v53);
        check("UNISON CNT=2 readback", v53 == 2);
        pc.value = 250;                       // over-range write must saturate to 8
        params->flush(plugin, &pl, &out);
        params->get_value(plugin, 53, &v53);
        check("UNISON CNT over-range clamps to 8", v53 == 8);
        char txt[64] = {0};
        check("PRIORITY value text", params->value_to_text(plugin, 56, 1, txt, sizeof txt) && !std::strcmp(txt, "LOW RET"));
        check("FILTER MODEL value text", params->value_to_text(plugin, 20, 0, txt, sizeof txt) && !std::strcmp(txt, "SSI"));

        // programmer params + gui surface
        uint32_t cnt = params->count(plugin);
        check("param count 70 (57 raw + 8 ov + trim + os + tune + program + bank)", cnt == 70);
        bool tuneInfo = false;
        for (uint32_t i = 0; i < cnt; i++) {
            clap_param_info_t inf{};
            if (params->get_info(plugin, i, &inf) && inf.id == 82)
                tuneInfo = inf.min_value == -100 && inf.max_value == 100;
        }
        check("MASTER TUNE range -100..100", tuneInfo);
        double cutBefore = -1; params->get_value(plugin, 17, &cutBefore);
        pc.param_id = 83; pc.value = 5;
        params->flush(plugin, &pl, &out);
        double v83 = -1; params->get_value(plugin, 83, &v83);
        double cutAfter = -1; params->get_value(plugin, 17, &cutAfter);
        check("PROGRAM readback 5", v83 == 5);
        check("PROGRAM load changed the patch", cutAfter != cutBefore);

        const void *gp = plugin->get_extension(plugin, CLAP_EXT_GUI);
        check("clap-gui extension present", gp != nullptr);
        if (gp) {
            auto *gui = (const clap_plugin_gui_t *)gp;
            check("gui: x11 embedded supported", gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false));
            check("gui: cocoa not claimed", !gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false));
            const char *api = nullptr; bool floating = true;
            check("gui: preferred is x11 non-floating", gui->get_preferred_api(plugin, &api, &floating) &&
                  !std::strcmp(api, CLAP_WINDOW_API_X11) && !floating);
            check("gui: create", gui->create(plugin, CLAP_WINDOW_API_X11, false));
            uint32_t gw = 0, gh = 0;
            check("gui: get_size sane", gui->get_size(plugin, &gw, &gh) && gw > 400 && gh > 100);
            clap_gui_resize_hints_t hints{};
            check("gui: resize hints preserve 1560x430 aspect", gui->get_resize_hints(plugin, &hints) &&
                  hints.preserve_aspect_ratio && hints.aspect_ratio_width == 1560 && hints.aspect_ratio_height == 430);
            uint32_t aw = 900, ah = 900;
            check("gui: adjust_size fixes aspect", gui->adjust_size(plugin, &aw, &ah) && aw == 900 && ah == 248);
            gui->destroy(plugin);
        }

        // restore the CUTOFF==42 assumption for the state round-trip below
        pc.param_id = 17; pc.value = 42;
        params->flush(plugin, &pl, &out);
    }

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

    // ---- oversample toggle + latency contract ----
    {
        auto *params = (const clap_plugin_params_t *)pp;
        clap_event_param_value po{};
        po.header.type = CLAP_EVENT_PARAM_VALUE; po.header.time = 0;
        po.param_id = 81; po.value = 1;
        EventList pe{ { &po.header } };
        clap_input_events_t pin{ &pe, EL_size, EL_get };
        params->flush(plugin, &pin, &out);
        double ov = -1;
        check("oversample param readback", params->get_value(plugin, 81, &ov) && ov == 1);
        const void *lat = plugin->get_extension(plugin, CLAP_EXT_LATENCY);
        check("latency extension present", lat != nullptr);
        if (lat) check("latency reports 3 with 2x on",
                       ((const clap_plugin_latency_t *)lat)->get(plugin) == 3u);
        // hold a chord through the 2x stretch (patch loads above reset the part)
        std::vector<clap_event_note> nn2;
        auto push2 = [&](uint32_t frame, uint16_t type, int16_t key, double vel) {
            nn2.emplace_back(); auto &e = nn2.back(); memset(&e, 0, sizeof e);
            e.header.time = frame; e.header.type = type; e.channel = 0; e.key = key; e.velocity = vel; };
        push2(0, CLAP_EVENT_NOTE_ON, 60, 127.0 / 127.0);
        push2(0, CLAP_EVENT_NOTE_ON, 64, 0.9);
        push2(0, CLAP_EVENT_NOTE_ON, 67, 0.8);
        EventList all2; for (auto &e : nn2) all2.evs.push_back(&e.header);
        size_t p2 = 0;
        float L2[512], R2[512]; float *ch2[2] = { L2, R2 };
        clap_audio_buffer_t ab2{}; ab2.channel_count = 2; ab2.data32 = ch2;
        double peak2 = 0; bool fin2 = true, nz2 = false;
        for (uint32_t done = 0; done < 44100; done += 512) {
            EventList due2;
            while (p2 < all2.evs.size() && all2.evs[p2]->time <= done) {
                auto *ev = const_cast<clap_event_header_t *>(all2.evs[p2]); ev->time = 0;
                due2.evs.push_back(ev); p2++;
            }
            clap_input_events_t din{ &due2, EL_size, EL_get };
            clap_process_t pr2{};
            pr2.in_events = &din; pr2.out_events = &out;
            pr2.audio_outputs = &ab2; pr2.frames_count = 512;
            plugin->process(plugin, &pr2);
            for (uint32_t i = 0; i < 512; i++) {
                if (!std::isfinite(L2[i]) || !std::isfinite(R2[i])) fin2 = false;
                if (std::fabs(L2[i]) > 1e-6) nz2 = true;
                if (std::fabs(L2[i]) > peak2) peak2 = std::fabs(L2[i]);
            }
        }
        check("2x renders finite non-silent audio", fin2 && nz2 && peak2 > 0.01 && peak2 <= 2.0);
        printf("clap: 2x peak %.3f\n", peak2);
        po.value = 0; params->flush(plugin, &pin, &out);
        if (lat) check("latency back to 0 with 2x off",
                       ((const clap_plugin_latency_t *)lat)->get(plugin) == 0u);

        // state round-trip must carry the oversample flag
        if (st) {
            auto *state = (const clap_plugin_state_t *)st;
            po.value = 1; params->flush(plugin, &pin, &out);       // 2x on
            std::vector<char> blob2;
            auto w2 = [](const clap_ostream_t *s, const void *b, uint64_t n) -> int64_t {
                auto *v = (std::vector<char> *)s->ctx; v->insert(v->end(), (const char *)b, (const char *)b + n); return (int64_t)n; };
            clap_ostream_t os2{ &blob2, w2 };
            state->save(plugin, &os2);
            po.value = 0; params->flush(plugin, &pin, &out);       // 2x off
            auto r2 = [](const clap_istream_t *s, void *b, uint64_t n) -> int64_t {
                auto *v = (std::vector<char> *)s->ctx;
                if (v->size() < (size_t)n) return -1;
                for (uint64_t i = 0; i < n; i++) ((char *)b)[i] = (*v)[i];
                v->erase(v->begin(), v->begin() + (long)n); return (int64_t)n; };
            std::vector<char> copy2 = blob2;
            clap_istream_t is2{ &copy2, r2 };
            state->load(plugin, &is2);
            double ov2 = -1; params->get_value(plugin, 81, &ov2);
            check("state round-trip carries oversample=on", ov2 == 1);
            po.value = 0; params->flush(plugin, &pin, &out);
        }
    }

    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    dlclose(lib);
    printf("clap smoke: %s\n", failures ? "FAILED" : "all checks ok");
    return failures ? 1 : 0;
}
