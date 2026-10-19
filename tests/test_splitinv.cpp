/* Split-invariance. The host chooses its block size and the plugin splits
 * rendering at event offsets; identical sample-time event streams must
 * produce IDENTICAL PCM no matter where the process() call boundaries fall.
 *
 * This is the property whose loss caused the occasional note-on clicks: the
 * old 32-sample control tick charged a full tick of envelope movement per
 * call, so a partial block truncated the ramp and the boundary seam ticked.
 * The control path is per host sample now - assert the invariance directly,
 * over every scripted scenario, against adversarial chunk patterns, in 1x
 * and 2x oversampling and both kernels. */
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "p5_engine.hpp"
#include "scenario.h"

static void setup(p5::Engine &e, const Scenario &sc)
{
    e.silenceReset();
    if (sc.preset == 0)
    {   p5::Patch p; p5::patchInit(p); e.setPatch(p); }
    else
    {   e.loadFactory((uint32_t)(sc.preset - 1));
        e.editRaw(p5::P5_VINTAGE, 0); }
    if (sc.patchRes > 0)
    {   p5::Patch p = e.patch();
        p.raw[p5::P5_RESONANCE] = (uint8_t)sc.patchRes;
        p.raw[p5::P5_CUTOFF] = (uint8_t)sc.patchCut;
        p.raw[p5::P5_LEVEL_A] = p.raw[p5::P5_LEVEL_B] = p.raw[p5::P5_NOISE] = 0;
        e.setPatch(p); }
}

/* Uniform reference: the old firmware cadence, 32 per call. Events with
 * .block == b fire exactly at sample b*32. */
static void renderUniform(p5::Engine &e, const Scenario &sc, std::vector<int32_t> &pcm)
{
    setup(e, sc);
    unsigned nextEv = 0;
    for (unsigned blk = 0; blk < BLOCKS; blk++)
    {
        while (nextEv < sc.n && sc.ev[nextEv].block == blk)
        {   if (sc.ev[nextEv].kind == 0) e.noteOn(sc.ev[nextEv].a, sc.ev[nextEv].b);
            else e.noteOff(sc.ev[nextEv].a);
            nextEv++; }
        int32_t out[p5::CTL_];
        e.process(out, p5::CTL_);
        for (int i = 0; i < p5::CTL_; i++) pcm.push_back(out[i]);
    }
}

/* Arbitrary host cadence: the same events fired at the same absolute sample
 * offsets, delivered through process() calls of the given chunk pattern. */
static void renderSplit(p5::Engine &e, const Scenario &sc, const int *pat, int patLen,
                        std::vector<int32_t> &pcm)
{
    setup(e, sc);
    const unsigned total = BLOCKS * p5::CTL_;
    unsigned nextEv = 0, cursor = 0, pi = 0;
    while (cursor < total)
    {
        while (nextEv < sc.n && sc.ev[nextEv].block * p5::CTL_ <= cursor)
        {   if (sc.ev[nextEv].kind == 0) e.noteOn(sc.ev[nextEv].a, sc.ev[nextEv].b);
            else e.noteOff(sc.ev[nextEv].a);
            nextEv++; }
        unsigned n = (unsigned)pat[pi % patLen]; pi++;
        if (cursor + n > total) n = total - cursor;
        // the plugin's real contract: a render chunk NEVER straddles an event -
        // muhammad_plugin splits exactly at event offsets. Enforce the same.
        if (nextEv < sc.n)
        {   const unsigned evOff = sc.ev[nextEv].block * p5::CTL_;
            if (evOff > cursor && evOff < cursor + n) n = evOff - cursor; }
        std::vector<int32_t> out(n);
        e.process(out.data(), n);
        pcm.insert(pcm.end(), out.begin(), out.end());
        cursor += n;
    }
}

static long compare(const std::vector<int32_t> &a, const std::vector<int32_t> &b, long *maxAbs)
{
    long diffs = 0;
    if (a.size() != b.size()) { *maxAbs = -1; return -1; }
    for (size_t i = 0; i < a.size(); i++)
        if (a[i] != b[i])
        {   diffs++;
            const long d = std::abs((long)a[i] - (long)b[i]);
            if (d > *maxAbs) *maxAbs = d; }
    return diffs;
}

int main()
{
    static const int PAT[6][8] = {
        {13, 19},                 // straddles the old 32 seam
        {1, 31},
        {31, 1},
        {7, 7, 7, 7, 4},
        {5, 27},
        {2, 3, 5, 7, 11, 13},
    };
    static const int PATLEN[6] = {2, 2, 2, 5, 2, 6};
    int failures = 0;
    for (int s = 0; s < NSCEN; s++)
    {
        for (int mode = 0; mode < 2; mode++)          // scalar and wide
        for (int os = 1; os <= 2; os++)               // 1x and 2x oversampling
        {
            p5::Engine ref(mode ? p5::KernelMode::Wide : p5::KernelMode::Scalar);
            p5::Engine tst(mode ? p5::KernelMode::Wide : p5::KernelMode::Scalar);
            ref.setPolyphony(5); tst.setPolyphony(5);
            ref.activate(44100); tst.activate(44100);
            ref.setOverSample(os); tst.setOverSample(os);
            std::vector<int32_t> pcm;
            renderUniform(ref, scenarios[s], pcm);
            for (size_t p = 0; p < 6; p++)
            {
                std::vector<int32_t> sp;
                renderSplit(tst, scenarios[s], PAT[p], PATLEN[p], sp);
                long maxAbs = 0;
                const long d = compare(pcm, sp, &maxAbs);
                if (d)
                {   size_t fd = 0; while (fd < pcm.size() && pcm[fd] == sp[fd]) fd++;
                    printf("SPLIT DIFF: scen %d %s os%d pattern %zu: %ld samples (max %ld), first at %zu (block %zu i %zu)\n",
                           s, mode ? "wide" : "scalar", os, p, d, maxAbs, fd, fd / 32, fd % 32);
                    failures++; }
            }
        }
    }
    printf("%s\n", failures ? "split-invariance: DIFFERENCES FOUND"
                           : "split-invariance: bit-identical (all scenarios, kernels, os, patterns)");
    return failures ? 1 : 0;
}
