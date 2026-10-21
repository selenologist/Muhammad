/* Renders the shared note scripts through the two engine kernels (scalar
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
        e.editRaw(p5::P5_VINTAGE, 0);   // byte 37 retired by design (Muhammad reads
                                        // drift from param 86): zero it so A/B certifies
                                        // every OTHER byte bit-exact against firmware
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
        float v = (float)pcm[i] * (1.0f / 32768.0f) * 0.5f;  // the plugin's default trim
        fwrite(&v, 4, 1, f);
        fwrite(&v, 4, 1, f);
    }
    fclose(f);
}

// Hann-windowed Goertzel. The audit compares inter-harmonic floors BETWEEN
// laws/modes; unwindowed Goertzel at half-integer bins is dominated by
// fundamental/harmonic LEAKAGE (several dB of measurement luck per window
// position, verified on the old engine), which buried the true -120 dB floor
// entirely. With the window the numbers are offset-stable and the 1x-vs-2x
// comparison measures aliasing, not leakage phase.
static double goertzel(const std::vector<int32_t> &x, double f, int N)
{
    const double PI = 3.14159265358979323846;
    double w = 2.0 * PI * f / 44100.0, cw = cos(w), coeff = 2.0 * cw, s1 = 0, s2 = 0;
    const double w0 = 2.0 * PI / N;
    for (int i = 0; i < N; i++) {
        const double win = 0.5 * (1.0 - cos(w0 * i));
        double s0 = win * (double)x[i] / 32768.0 + coeff * s1 - s2; s2 = s1; s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
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

        if (s == 0) writeWav("muhammad_ab_simd.wav", b);
    }

    // ---- 2x oversample: the halfband path must agree scalar<->SIMD too ----
    {
        p5::Engine A(p5::KernelMode::Scalar), B(p5::KernelMode::Wide);
        A.setPolyphony(5); B.setPolyphony(5);
        A.activate(44100); B.activate(44100);
        A.setOverSample(2); B.setOverSample(2);
        for (int s = 0; s < NSCEN; s++) {
            std::vector<int32_t> a, b;
            runScenario(scenarios[s], A, a);
            runScenario(scenarios[s], B, b);
            long d = 0;
            for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) d++;
            printf("os2 scenario %d: scalar-vs-simd mismatches %ld\n", s + 1, d);
            if (d) failures++;
        }
    }

    // ---- 48 kHz: kernels must agree, output sane (pitch/env/LFO/cutoff all
    //      track the host rate; the cutoff table rebuilds for it) ----
    {
        p5::Engine A(p5::KernelMode::Scalar), B(p5::KernelMode::Wide);
        A.setPolyphony(5); B.setPolyphony(5);
        A.activate(48000); B.activate(48000);
        for (int s = 0; s < NSCEN; s++) {
            std::vector<int32_t> a, b;
            runScenario(scenarios[s], A, a);
            runScenario(scenarios[s], B, b);
            long d = 0; double peak = 0; bool bad = false;
            for (size_t i = 0; i < a.size(); i++) {
                if (a[i] != b[i]) d++;
                double v = (double)a[i] / 32768.0;
                if (!std::isfinite(v)) bad = true;
                if (std::fabs(v) > peak) peak = std::fabs(v);
            }
            printf("48k scenario %d: mismatches %ld, peak %.1f%s\n", s + 1, d, peak, bad ? " NONFINITE" : "");
            if (d || bad || peak < 0.05) failures++;
        }
    }

    // ---- 48 kHz + 2x: same agreement on the oversampled path ----
    {
        p5::Engine A(p5::KernelMode::Scalar), B(p5::KernelMode::Wide);
        A.setPolyphony(5); B.setPolyphony(5);
        A.activate(48000); B.activate(48000);
        A.setOverSample(2); B.setOverSample(2);
        for (int s = 0; s < NSCEN; s++) {
            std::vector<int32_t> a, b;
            runScenario(scenarios[s], A, a);
            runScenario(scenarios[s], B, b);
            long d = 0;
            if (a.size() != b.size()) { printf("size mismatch\n"); failures++; continue; }
            for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) d++;
            printf("48k+os2 scenario %d: mismatches %ld\n", s + 1, d);
            if (d) failures++;
        }
    }

    // ---- aliasing / oversampling audit: hard-sync dual saws at note 96
    //      (f0 = 2093 Hz). The engine's BLEP keeps inter-harmonic bins at a
    //      deep floor already; 2x + halfband must not raise it and must lower
    //      it (the fold-back and nonlinear products move above the transition
    //      band). Assert: both windowed floors below -100 dB vs the
    //      fundamental, and the 2x floor no higher than 1x. ----
    {
        double v1[8], v2[8];
        const double f0 = 440.0 * pow(2.0, (96 - 69) / 12.0);
        double bins[6];
        for (int k = 0; k < 6; k++) {
            double band = 0.35 + 0.6 * k / 5.0;
            double m = std::floor(22050.0 * band / f0);
            bins[k] = (m + 0.5) * f0;
        }
        for (int oi = 0; oi < 2; oi++) {
            const int os = oi == 0 ? 1 : 2;
            p5::Engine e(p5::KernelMode::Auto);
            e.activate(44100); e.setOverSample(os);
            p5::Patch p; p5::patchInit(p);
            p.raw[p5::P5_LEVEL_A] = 110; p.raw[p5::P5_LEVEL_B] = 110; p.raw[p5::P5_NOISE] = 0;
            p.raw[p5::P5_SAW_A] = 1; p.raw[p5::P5_SAW_B] = 1; p.raw[p5::P5_SYNC] = 2;
            p.raw[p5::P5_CUTOFF] = 127; p.raw[p5::P5_RESONANCE] = 20; p.raw[p5::P5_ENV_FILTER] = 0;
            p.raw[p5::P5_KEY_FILTER] = 0; p.raw[p5::P5_POLY_FILTER] = 0; p.raw[p5::P5_WHEEL_FILTER] = 0;
            e.setPatch(p); e.noteOn(96, 110);
            std::vector<int32_t> pcm; int32_t out[32];
            for (int blk = 0; blk < 2048; blk++) { e.process(out, 32); for (int i = 0; i < 32; i++) pcm.push_back(out[i]); }
            double *v = oi == 0 ? v1 : v2;
            const int N = 32768;
            for (int k = 0; k < 6; k++) v[k] = goertzel(pcm, bins[k], N);
            v[6] = goertzel(pcm, f0, N);
        }
        double p1 = 0, p2 = 0;
        for (int k = 0; k < 6; k++) { p1 += v1[k]*v1[k]; p2 += v2[k]*v2[k]; }
        double f1 = 10*log10(p1/(v1[6]*v1[6])), f2 = 10*log10(p2/(v1[6]*v1[6]));
        printf("alias floor (sync saws note 96, inter-harmonic bins): 1x %.1f dB, 2x %.1f dB (%.1f dB cleaner)\n",
               f1, f2, f1 - f2);
        if (!(f1 < -100.0 && f2 < -100.0 && f2 <= f1 + 0.5)) failures++;
    }

    printf("%s\n", failures ? "A/B: DIFFERENCES FOUND" : "A/B: bit-exact");
    // ---- FULL factory sweep: all 200 programs, scalar vs wide, bit-exact --
    // the bank is the strippable sidecar now: a sweep that silently sees zero
    // programs would "pass" against nothing - load it, or fail loudly.
    if (!p5fac::load() || p5fac::count() != 200)
    {   std::fprintf(stderr, "factory sidecar missing (P5_FACTORY/module dir) - sweep cannot certify\n");
        return 1; }
    // The old gate certified only INIT + program 0 while 61 programs route
    // the (at-rest) wheel/poly-mod path - exactly where the scalar kernel's
    // shortcut (`if (!x) return base`) diverges the wide kernel's float
    // round-trip. Sweeping every program makes the claim-check true.
    {
        p5::Engine A(p5::KernelMode::Scalar), B(p5::KernelMode::Wide);
        A.setPolyphony(5); B.setPolyphony(5);
        A.activate(44100); B.activate(44100);
        long badPrograms = 0, badSamples = 0;
        int32_t ba[2048], bb[2048];
        for (uint32_t prog = 0; prog < 200; prog++)
        {
            A.silenceReset(); B.silenceReset();
            A.loadFactory(prog); B.loadFactory(prog);
            A.editRaw(p5::P5_VINTAGE, 0); B.editRaw(p5::P5_VINTAGE, 0);
            A.noteOn(52, 100, 0); B.noteOn(52, 100, 0);
            A.noteOn(59, 96, 0);  B.noteOn(59, 96, 0);
            A.noteOn(64, 92, 0);  B.noteOn(64, 92, 0);
            A.process(ba, 1024); A.process(ba, 1024);
            B.process(bb, 1024); B.process(bb, 1024);
            A.noteOff(52, 0); B.noteOff(52, 0);
            A.noteOff(59, 0); B.noteOff(59, 0);
            A.noteOff(64, 0); B.noteOff(64, 0);
            A.process(ba, 1024); A.process(ba, 1024);
            B.process(bb, 1024); B.process(bb, 1024);
            long d = 0;
            for (int i = 0; i < 2048; i++) if (ba[i] != bb[i]) d++;
            if (d) { badPrograms++; badSamples += d;
                     if (getenv("FIRSTDIFF")) printf("program %u: %ld mismatched samples\n", prog, d); }
        }
        printf("factory sweep: 200 programs, %ld mismatching, %ld mismatched samples\n",
               badPrograms, badSamples);
        if (badPrograms) failures++;
    }

    return failures ? 1 : 0;
}
