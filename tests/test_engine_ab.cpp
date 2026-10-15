/* Renders the shared note scripts through the two Prophet kernels (scalar
 * reference vs the SIMDe wide kernel) and, when present, against the melodee
 * firmware reference dump. Prints exact-match statistics. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include "p5_engine.hpp"

#include "scenario.h"

static void runScenario(const Scenario &sc, p5::Engine &e, std::vector<int32_t> &pcm)
{
    e.silenceReset();
    if (sc.preset == 0) {
        p5::Patch p;
        p5::patchInit(p);
        e.setPatch(p);
    } else {
        e.loadFactory((uint32_t)(sc.preset - 1));
    }
    if (sc.patchRes > 0) {                    // self-oscillation: closed mixer, hot filter
        p5::Patch p = e.patch();
        p.raw[p5::P5_RESONANCE] = (uint8_t)sc.patchRes;
        p.raw[p5::P5_CUTOFF] = (uint8_t)sc.patchCut;
        p.raw[p5::P5_LEVEL_A] = p.raw[p5::P5_LEVEL_B] = p.raw[p5::P5_NOISE] = 0;
        e.setPatch(p);
    }
    for (int i = 0; i < sc.n; i++) {
        int kind = sc.ev[i].kind, a = sc.ev[i].a, b = sc.ev[i].b;
        unsigned blk = sc.ev[i].block;
        (void)blk; (void)kind; (void)a; (void)b;
    }
    // events interleave with blocks
    unsigned nextEv = 0;
    for (unsigned blk = 0; blk < BLOCKS; blk++) {
        while (nextEv < sc.n && sc.ev[nextEv].block == blk) {
            if (sc.ev[nextEv].kind == 0) e.noteOn(sc.ev[nextEv].a, sc.ev[nextEv].b);
            else e.noteOff(sc.ev[nextEv].a);
            nextEv++;
        }
        int32_t out[p5::CTL_];
        e.process(out, p5::CTL_);
        for (int i = 0; i < p5::CTL_; i++) pcm.push_back(out[i]);
    }
}

static void writeWav(const char *path, const std::vector<int32_t> &pcm)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t ns = (uint32_t)pcm.size();
    uint32_t dataBytes = ns * 2 * 4;
    uint32_t riffBytes = 36 + dataBytes;          // RIFF chunk: file minus its own 8-byte header
    uint32_t fmtBytes = 16;                        // canonical PCM 'fmt ' subchunk size
    uint32_t rate = 44100;
    uint16_t ch = 2, bits = 32, fmt = 3;           // 3 = IEEE float
    uint32_t byteRate = rate * ch * 4;
    uint16_t block = ch * 4;
    fwrite("RIFF", 1, 4, f); fwrite(&riffBytes, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtBytes, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&block, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    for (size_t i = 0; i < pcm.size(); i++) {
        float v = (float)pcm[i] * (1.0f / 32768.0f) * 0.8f;
        fwrite(&v, 4, 1, f);
        fwrite(&v, 4, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *refPath = argc > 1 ? argv[1] : nullptr;
    int failures = 0;
    for (int s = 0; s < NSCEN; s++) {
        std::vector<int32_t> a, b;
        p5::Engine scalar(p5::KernelMode::Scalar), simd(p5::KernelMode::Wide);
        scalar.setPolyphony(5);
        simd.setPolyphony(5);
        scalar.activate(44100);
        simd.activate(44100);
        runScenario(scenarios[s], scalar, a);
        runScenario(scenarios[s], simd, b);

        long diffCount = 0, maxDiff = 0;
        { for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i] && getenv("FIRSTDIFF")) {
            printf("first s-d diff at %zu: scalar %d simd %d\n", i, a[i], b[i]); break; } }
        double signal = 0, noise = 0;
        for (size_t i = 0; i < a.size(); i++) {
            long d = labs(a[i] - b[i]);
            if (d) { diffCount++; if (d > maxDiff) maxDiff = d; }
            signal += (double)a[i] * a[i];
            noise += (double)d * d;
        }
        double snr = noise > 0 ? 10.0 * log10(signal / noise) : 1e9;
        printf("scenario %d: %zu samples, scalar-vs-simd mismatches %ld, max |dlsb| %ld, SNR %.1f dB\n",
               s + 1, a.size(), diffCount, maxDiff, snr);
        if (diffCount != 0) failures++;

        if (refPath) {
            static std::vector<int32_t> ref[NSCEN];
            if (ref[0].empty()) {
                FILE *f = fopen(refPath, "rb");
                if (!f) { printf("ref file missing\n"); return 2; }
                for (int q = 0; q < NSCEN; q++)
                    for (int i = 0; i < BLOCKS * 32; i++) {
                        int32_t v;
                        if (fread(&v, 4, 1, f) != 1) { printf("ref truncated\n"); return 2; }
                        ref[q].push_back(v);
                    }
                fclose(f);
            }
            long rc = 0, rm = 0;
            double rs = 0, rn = 0;
            for (size_t i = 0; i < a.size(); i++) {
                long d = labs(a[i] - ref[s][i]);
                if (d) { rc++; if (d > rm) rm = d; }
                rs += (double)ref[s][i] * ref[s][i];
                rn += (double)d * d;
            }
            double rsnr = rn > 0 ? 10.0 * log10(rs / rn) : 1e9;
            printf("scenario %d: port-vs-firmware mismatches %ld, max |dlsb| %ld, SNR %.1f dB\n",
                   s + 1, rc, rm, rsnr);
            if (rc != 0) failures++;
        }

        if (s == 0) writeWav("prophet_ab_simd.wav", b);
    }
    printf("%s\n", failures ? "A/B: DIFFERENCES FOUND" : "A/B: bit-exact");
    return failures ? 1 : 0;
}
