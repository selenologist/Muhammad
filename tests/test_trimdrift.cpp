/* SPDX-License-Identifier: GPL-3.0-only
 * Engine units for the two v1.11 features: V/oct trim law + DRIFT behaviour.
 * Everything here must be deterministic: walk LCGs seed from lane index only. */
#include <cmath>
#include <cstdio>
#include <vector>
#include "p5_engine.hpp"

static int fails = 0;
static void check(const char *name, bool ok)
{
    std::printf("%-44s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static std::vector<float> render(p5::Engine &e, int blocks)
{
    std::vector<float> buf;
    int32_t out[32];
    for (int b = 0; b < blocks; b++)
    {
        e.process(out, 32);
        for (int i = 0; i < 32; i++) buf.push_back((float)out[i] * (1.0f / 32768.0f));
    }
    return buf;
}

static double periodAt(const std::vector<float> &buf, double guess)
{   // autocorrelation around a guessed period on a steady window (buffer >= 16k)
    const int n = 4096, start = 8192;
    double best = -1e300; int bestLag = (int)guess;
    for (int lag = (int)(guess * 0.80); lag <= (int)(guess * 1.20) && lag < 2000; lag++)
    {
        double acc = 0;
        for (int i = 0; i < n; i++) acc += (double)buf[start + i] * buf[start + i + lag];
        if (acc > best) { best = acc; bestLag = lag; }
    }
    return bestLag;
}

int main()
{
    p5::Patch p;
    p5::patchInit(p);

    p5::Engine A(p5::KernelMode::Scalar);   // neutral
    A.activate(44100); A.setPolyphony(8); A.setPatch(p);
    A.noteOn(60, 100);
    auto bufA = render(A, 600);   // ~13.7k samples: window fits

    p5::Engine B(p5::KernelMode::Scalar);   // voice 0 offset +16 units = +1 semitone
    B.activate(44100); B.setPolyphony(8); B.setPatch(p);
    B.setVoiceTrim(0, 64, 80);
    B.noteOn(60, 100);
    auto bufB = render(B, 600);

    const double pA = periodAt(bufA, 44100.0 / 261.63);   // C4 = 168.6 samples
    const double pB = periodAt(bufB, 44100.0 / 261.63);
    std::printf("  periods: neutral %.1f trimmed %.1f ratio %.4f (want %.4f)\n",
                pA, pB, pA / pB, std::pow(2.0, 1.0 / 12.0));
    const double ratio = pA / pB;                          // +1 semitone => 2^(1/12)
    check("offset trim +16 == +1 semitone", std::fabs(ratio - std::pow(2.0, 1.0 / 12.0)) < 0.015);

    p5::Engine C(p5::KernelMode::Scalar);   // slope +12: high notes sharp
    C.activate(44100); C.setPolyphony(8); C.setPatch(p);
    C.setVoiceTrim(0, 127, 64);   // max slope error
    C.noteOn(96, 100);
    auto bufC = render(C, 600);
    const double pC = periodAt(bufC, 44100.0 / 2093.0);    // C8
    std::printf("  slope: neutral %.1f trimmed %.1f\n", 0.0, pC);
    p5::Engine D(p5::KernelMode::Scalar);
    D.activate(44100); D.setPolyphony(8); D.setPatch(p);
    D.noteOn(96, 100);
    auto bufD = render(D, 600);
    const double pD = periodAt(bufD, 44100.0 / 2093.0);
    check("slope trim makes top octaves sharp", pC < pD);

    // DRIFT off: renders are exactly reproducible; DRIFT max: the pitch wanders.
    p5::Engine E(p5::KernelMode::Scalar);
    E.activate(44100); E.setPolyphony(8); E.setPatch(p);
    E.noteOn(60, 100);
    auto e1 = render(E, 40);
    p5::Engine F(p5::KernelMode::Scalar);
    F.activate(44100); F.setPolyphony(8); F.setPatch(p);
    F.noteOn(60, 100);
    auto f1 = render(F, 40);
    check("drift 0 == deterministic", e1 == f1);

    p5::Engine G(p5::KernelMode::Scalar);
    G.activate(44100); G.setPolyphony(8); G.setPatch(p);
    G.setDriftDepth(127);
    G.noteOn(60, 100);
    auto g1 = render(G, 40);
    p5::Engine H(p5::KernelMode::Scalar);
    H.activate(44100); H.setPolyphony(8); H.setPatch(p);
    H.setDriftDepth(127);
    H.noteOn(60, 100);
    auto h1 = render(H, 40);
    check("drift max deterministic", g1 == h1);
    check("drift max actually moves", g1 != e1);

    std::printf("%s\n", fails ? "TRIM/DRIFT: FAILED" : "trim/drift: all ok");
    return fails ? 1 : 0;
}
