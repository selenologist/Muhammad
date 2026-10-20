/* ThreadSanitizer stress modeling a SPEC-FOLLOWING host (review 4.1 / B12):
 *   - the audio thread runs process() with NOTES in in_events, and between
 *     blocks runs params.flush() and reset() THERE (the CLAP annotations:
 *     flush = [active ? audio : main], reset = [audio & active]);
 *   - the main thread does everything a host UI + the panel do concurrently:
 *     program loads, tuning/trim/polarity edits, guiSet/guiSetLocal/guiSync
 *     gestures, get_value reads, state save/load, timer-tick adoption.
 * The patch/patchMain contract claims main never writes the engine while
 * active and the audio thread never needs main's locks. This test is that
 * claim under -fsanitize=thread (CI job `tsan`). */
#include "muhammad_plugin.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include <clap/clap.h>
#include <clap/ext/params.h>

static std::atomic<bool> stopFlag{false};
static std::atomic<unsigned> outPushes{0}, hostFlushes{0}, rescans{0};

static void host_rescan(const clap_host_t *, clap_param_rescan_flags) { rescans.fetch_add(1); }
static void host_clear(const clap_host_t *, clap_id, clap_param_clear_flags) {}
static void host_request_flush(const clap_host_t *) { hostFlushes.fetch_add(1); }
static const clap_host_params_t kHostParams = { host_rescan, host_clear, host_request_flush };

static bool out_try(const clap_output_events_t *, const clap_event_header_t *)
{   outPushes.fetch_add(1); return true; }
static const clap_output_events_t kOut = { nullptr, out_try };

struct InList { std::vector<const clap_event_header_t *> evs; };
static uint32_t in_size(const clap_input_events_t *l)
{   return (uint32_t)((const InList *)l->ctx)->evs.size(); }
static const clap_event_header_t *in_get(const clap_input_events_t *l, uint32_t i)
{   return ((const InList *)l->ctx)->evs[i]; }

static const void *host_get_ext(const clap_host_t *, const char *id)
{   if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kHostParams;
    return nullptr; }

int main()
{
    setenv("P5_STORE_DIR", "/tmp/muh-tsan-store", 1);
    clap_host_t host{};
    host.clap_version = CLAP_VERSION_INIT;
    host.get_extension = host_get_ext;

    const clap_plugin_t *pl = pw::muhammad_create(&host);
    if (!pl->init(pl)) { std::fprintf(stderr, "init failed\n"); return 1; }
    if (!pl->activate(pl, 44100.0, 64, 1024)) { std::fprintf(stderr, "activate failed\n"); return 1; }
    if (!pl->start_processing(pl)) { std::fprintf(stderr, "start failed\n"); return 1; }
    const auto *prm = (const clap_plugin_params_t *)pl->get_extension(pl, CLAP_EXT_PARAMS);
    const auto *stt = (const clap_plugin_state_t *)pl->get_extension(pl, CLAP_EXT_STATE);

    static float bufL[256], bufR[256];
    static float *chans[2] = { bufL, bufR };
    clap_audio_buffer_t ab{};
    ab.data32 = chans; ab.channel_count = 2;

    std::atomic<unsigned> blocks{0}, flushesOnAudio{0}, resetsOnAudio{0};

    // ---- audio thread: process + flush + reset, all in this thread ----
    std::thread audio([&]
    {
        std::mt19937 rng(11);
        unsigned n = 0;
        while (!stopFlag.load(std::memory_order_relaxed))
        {   // in-band events: notes on/off every block, param automation stream
            std::vector<clap_event_note> notes(2);
            std::vector<clap_event_param_value> vals(3);
            std::vector<clap_event_param_mod> mods(2);
            InList il;
            const int key = 48 + (int)(rng() % 36);
            for (auto &e : notes)
            {   std::memset(&e, 0, sizeof e);
                e.header.type = CLAP_EVENT_NOTE_ON; e.header.size = sizeof e;
                e.port_index = -1; e.channel = (short)(rng() % 16); e.key = key;
                e.note_id = key; e.velocity = 0.7; }
            notes[1].header.type = CLAP_EVENT_NOTE_OFF;
            notes[1].velocity = 0.0;
            for (size_t i = 0; i < vals.size(); i++)
            {   std::memset(&vals[i], 0, sizeof vals[i]);
                vals[i].header.type = CLAP_EVENT_PARAM_VALUE; vals[i].header.size = sizeof vals[i];
                vals[i].param_id = (clap_id)(rng() % 58); vals[i].value = (double)(rng() % 128);
                vals[i].note_id = -1; vals[i].port_index = -1; vals[i].channel = -1; vals[i].key = -1; }
            for (size_t i = 0; i < mods.size(); i++)
            {   std::memset(&mods[i], 0, sizeof mods[i]);
                mods[i].header.type = CLAP_EVENT_PARAM_MOD; mods[i].header.size = sizeof mods[i];
                mods[i].param_id = (clap_id)(rng() % 58);
                mods[i].amount = (double)((int)(rng() % 200) - 100) / 10.0;
                mods[i].note_id = -1; mods[i].port_index = -1; mods[i].channel = -1; mods[i].key = -1; }
            for (auto &e : notes) il.evs.push_back(&e.header);
            for (auto &e : vals)  il.evs.push_back(&e.header);
            for (auto &e : mods)  il.evs.push_back(&e.header);
            const clap_input_events_t in = { &il, in_size, in_get };

            clap_process_t pr{};
            pr.frames_count = 64;
            pr.audio_outputs = &ab;
            pr.audio_outputs_count = 1;
            pr.in_events = &in;
            pr.out_events = &kOut;
            pl->process(pl, &pr);
            blocks.fetch_add(1, std::memory_order_relaxed);

            if ((n++ % 8u) == 0u)
            {   // the host's pending param traffic, on the AUDIO thread
                prm->flush(pl, &in, &kOut);
                flushesOnAudio.fetch_add(1, std::memory_order_relaxed); }
            if ((n % 13u) == 0u)
            {   pl->reset(pl);                     // [audio-thread & active]
                resetsOnAudio.fetch_add(1, std::memory_order_relaxed); }
        }
    });

    // ---- main thread: host UI + panel, spec-legal main-only calls ----
    auto *s = static_cast<pw::Plugin *>(pl->plugin_data);
    std::mt19937 rng(7);
    unsigned iters = 0;
    for (int round = 0; round < 1500 && !stopFlag.load(); round++)
    {
        s->togglePolarity(p5::P5_ENV_FILTER);
        s->togglePolarity(p5::P5_LFO_RATE);
        s->togglePolarity(p5::P5_FEEDBACK);
        if (round % 7 == 0) s->loadProgram((int)(rng() % 200));
        if (round % 5 == 0)
        {   int16_t dev[128]; for (int k = 0; k < 128; k++) dev[k] = (int16_t)(rng() % 9 - 4);
            s->setTuning(dev); }
        if (round % 3 == 0)
            s->setTrimPair((int)(rng() % 8), (int)(rng() % 2), (int)(rng() % 90), (int)(rng() % 30));
        s->guiSet(pw::PID_TRIM, (double)(rng() % 200) / 100.0);
        s->guiSet(pw::PID_OS, (double)(round % 2));
        s->guiSet(pw::PID_PAN_WIDTH, (double)(rng() % 128));
        // a full drag gesture on CUTOFF: BEGIN, motion, END
        s->guiSetLocal(17, (double)(rng() % 128));
        s->guiSetLocal(17, (double)(rng() % 128));
        s->guiSync(17);
        double v = 0;
        prm->get_value(pl, rng() % 58, &v);
        prm->get_value(pl, rng() % 58, &v);
        s->adoptPendingIdentity();
        s->maybePingLatency();
        if (round % 200 == 199) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (round % 20 == 0 && stt)
        {   std::vector<char> blob;
            auto wr = [](const clap_ostream_t *o, const void *b, uint64_t n) -> int64_t
                {   auto *vv = (std::vector<char> *)o->ctx;
                    vv->insert(vv->end(), (const char *)b, (const char *)b + n); return (int64_t)n; };
            auto rd = [](const clap_istream_t *o, void *b, uint64_t n) -> int64_t
                {   auto *vv = (std::vector<char> *)o->ctx;
                    if (vv->size() < n) return -1;
                    for (uint64_t i = 0; i < n; i++) ((char *)b)[i] = (*vv)[i];
                    vv->erase(vv->begin(), vv->begin() + (long)n); return (int64_t)n; };
            const clap_ostream_t os = { &blob, wr };
            if (stt->save(pl, &os))
            {   std::vector<char> copy = blob;
                const clap_istream_t is = { &copy, rd };
                stt->load(pl, &is); }
        }
        iters++ ;
    }
    stopFlag.store(true);
    audio.join();
    pl->stop_processing(pl);
    pl->deactivate(pl);
    const unsigned blk = blocks.load();
    std::printf("threadsan: %u main rounds, %u audio blocks, %u audio-thread flushes, "
                "%u resets, %u out events, %u host flush requests, %u rescan pings\n",
                iters, blk, flushesOnAudio.load(), resetsOnAudio.load(),
                outPushes.load(), hostFlushes.load(), rescans.load());
    if (flushesOnAudio == 0 || resetsOnAudio == 0)
    {   std::fprintf(stderr, "threadsan: host pattern never exercised\n"); return 1; }
    pl->destroy(pl);
    return blk > 0 ? 0 : 1;
}
