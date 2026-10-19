/* ThreadSanitizer stress: one audio thread in process(), one main thread
 * doing everything a host+panel can do CONCURRENTLY: flushed param events,
 * polarity toggles, program loads, tuning/trim edits, state save+load. The
 * ring/view discipline claims the engine has exactly one writer at a time -
 * this test is that claim, run under -fsanitize=thread (CI job `tsan`). */
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
static std::atomic<unsigned> outPushes{0}, hostFlushes{0};

static void host_rescan(const clap_host_t *, clap_param_rescan_flags) {}
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
    host.host_data = nullptr;
    host.get_extension = host_get_ext;

    const clap_plugin_t *pl = pw::muhammad_create(&host);
    if (!pl->init(pl)) { std::fprintf(stderr, "init failed\n"); return 1; }
    if (!pl->activate(pl, 44100.0, 16, 0)) { std::fprintf(stderr, "activate failed\n"); return 1; }
    if (!pl->start_processing(pl)) { std::fprintf(stderr, "start failed\n"); return 1; }
    const auto *prm = (const clap_plugin_params_t *)pl->get_extension(pl, CLAP_EXT_PARAMS);
    const auto *stt = (const clap_plugin_state_t *)pl->get_extension(pl, CLAP_EXT_STATE);

    static float bufL[256], bufR[256];
    static float *chans[2] = { bufL, bufR };
    clap_audio_buffer_t ab{};
    ab.data32 = chans; ab.channel_count = 2;

    std::atomic<unsigned> blocks{0};
    std::thread audio([&]
    {
        while (!stopFlag.load(std::memory_order_relaxed))
        {   clap_process_t pr{};
            pr.frames_count = 64;
            pr.audio_outputs = &ab;
            pr.audio_outputs_count = 1;
            InList none;
            const clap_input_events_t in = { &none, in_size, in_get };
            pr.in_events = &in;
            pr.out_events = &kOut;
            pl->process(pl, &pr);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::mt19937 rng(7);
    unsigned iters = 0;
    for (int round = 0; round < 800; round++)
    {
        // flushed param events (the host's own writes)
        clap_event_param_value pv[4];
        InList inl;
        for (int i = 0; i < 4; i++)
        {   std::memset(&pv[i], 0, sizeof pv[i]);
            pv[i].header.type = (i & 1) ? CLAP_EVENT_PARAM_VALUE : CLAP_EVENT_PARAM_MOD;
            pv[i].header.size = sizeof pv[i];
            pv[i].param_id = rng() % 58;
            pv[i].value = (double)(rng() % 128);
            inl.evs.push_back(&pv[i].header); }
        const clap_input_events_t pin = { &inl, in_size, in_get };
        prm->flush(pl, &pin, &kOut);

        // panel-thread mutations + reads
        auto *s = static_cast<pw::Plugin *>(pl->plugin_data);
        s->togglePolarity(p5::P5_ENV_FILTER);
        s->togglePolarity(p5::P5_LFO_RATE);
        s->togglePolarity(p5::P5_FEEDBACK);
        if (round % 7 == 0) s->loadProgram((int)(rng() % 200));
        if (round % 5 == 0)
        {   int16_t dev[128]; for (int k = 0; k < 128; k++) dev[k] = (int16_t)(rng() % 9 - 4);
            s->setTuning(dev); }
        if (round % 3 == 0)
            s->setTrimPair((int)(rng() % 8), (int)(rng() % 2), (int)(rng() % 90), (int)(rng() % 30));
        s->guiSet(80, (double)(rng() % 200) / 100.0);            // TRIM
        s->guiSet(81, (double)(round % 2));                        // OS toggle
        s->guiSet(85, (double)(rng() % 128));                     // PAN WIDTH
        double v = 0;
        prm->get_value(pl, rng() % 58, &v);
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
                stt->load(pl, &is);
            }
        }
        iters++;
    }
    stopFlag.store(true);
    audio.join();
    pl->stop_processing(pl);
    pl->deactivate(pl);
    pl->destroy(pl);
    const unsigned blk = blocks.load();
    std::printf("threadsan stress: %u flush rounds, %u audio blocks, %u out events, %u host flush requests\n",
                iters, blk, outPushes.load(), hostFlushes.load());
    return blk > 0 ? 0 : 1;
}
