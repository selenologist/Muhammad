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

static double periodAt(const std::vector<float> &buf, double guess, int start = 8192)
{   // autocorrelation around a guessed period on a steady window
    const int n = 4096;
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
    std::memset(&p, 0, sizeof p);
    p.size = 133; p.model = 0x32; p.command = 3;
    p.raw[p5::P5_FREQ_A] = 24;                  // coarse knobs at centre (0 offset)
    p.raw[p5::P5_FREQ_B] = 24;
    p.raw[p5::P5_SAW_A] = 1;                    // lone saw A: each osc measured cleanly
    p.raw[p5::P5_LEVEL_A] = 127;
    p.raw[p5::P5_CUTOFF] = 127;
    p.raw[p5::P5_ATTACK_AMP] = 8;
    p.raw[p5::P5_SUSTAIN_AMP] = 127;
    p.raw[p5::P5_RELEASE_ON] = 1;
    p.raw[p5::P5_RELEASE_AMP] = 24;

    p5::Engine A(p5::KernelMode::Scalar);       // neutral
    A.activate(44100); A.setPolyphony(8); A.setPatch(p);
    A.noteOn(60, 100);
    auto bufA = render(A, 600);

    p5::Engine B(p5::KernelMode::Scalar);       // OSC A of voice 1: offset +1 semitone
    B.activate(44100); B.setPolyphony(8); B.setPatch(p);
    B.setVoiceTrim(0, 0, 64, 80);
    B.noteOn(60, 100);
    auto bufB = render(B, 600);

    const double pA = periodAt(bufA, 44100.0 / 261.63);
    const double pB = periodAt(bufB, 44100.0 / 261.63);
    const double ratio = pA / pB;
    std::printf("  periods: neutral %.1f oscA-trimmed %.1f ratio %.4f (want %.4f)\n",
                pA, pB, ratio, std::pow(2.0, 1.0 / 12.0));
    check("OSC A offset +16 == +1 semitone", std::fabs(ratio - std::pow(2.0, 1.0 / 12.0)) < 0.015);

    p5::Engine E(p5::KernelMode::Scalar);       // OSC B trimmed, but B is silent
    E.activate(44100); E.setPolyphony(8); E.setPatch(p);
    E.setVoiceTrim(0, 1, 64, 96);
    E.noteOn(60, 100);
    auto bufE = render(E, 600);
    check("OSC B trim silent when B mixed out", bufE == bufA);

    p5::Engine F(p5::KernelMode::Scalar);       // B audible, only B trimmed
    p5::Patch p2 = p; p2.raw[p5::P5_SAW_B] = 1; p2.raw[p5::P5_LEVEL_B] = 0;  // B runs, lb=0? -> use noiseless level
    p2.raw[p5::P5_LEVEL_B] = 127;
    p5::Engine G2(p5::KernelMode::Scalar);
    G2.activate(44100); G2.setPolyphony(8); G2.setPatch(p2);
    G2.noteOn(60, 100);
    auto bufBoth0 = render(G2, 600);
    p5::Engine G3(p5::KernelMode::Scalar);
    G3.activate(44100); G3.setPolyphony(8); G3.setPatch(p2);
    G3.setVoiceTrim(0, 1, 64, 80);              // +1 semitone on B only
    G3.noteOn(60, 100);
    auto bufBoth1 = render(G3, 600);
    check("per-osc trim changes the mix (B audible)", bufBoth1 != bufBoth0);

    // slope trim: max pot, C8 -> sharper
    p5::Engine C(p5::KernelMode::Scalar);
    C.activate(44100); C.setPolyphony(8); C.setPatch(p);
    C.setVoiceTrim(0, 0, 127, 64);
    C.noteOn(96, 100);
    auto bufC = render(C, 600);
    p5::Engine D(p5::KernelMode::Scalar);
    D.activate(44100); D.setPolyphony(8); D.setPatch(p);
    D.noteOn(96, 100);
    auto bufD = render(D, 600);
    check("slope trim makes top octaves sharp", periodAt(bufC, 44100.0 / 2093.0) <
                                               periodAt(bufD, 44100.0 / 2093.0));

    // DRIFT: deterministic, moves, and UNBIASED (the v1.11 fixed-detune bug)
    p5::Engine N1(p5::KernelMode::Scalar), N2(p5::KernelMode::Scalar);
    N1.activate(44100); N1.setPolyphony(8); N1.setPatch(p); N1.noteOn(60, 100);
    N2.activate(44100); N2.setPolyphony(8); N2.setPatch(p); N2.noteOn(60, 100);
    auto n1 = render(N1, 4200), n2 = render(N2, 4200);
    check("drift 0 == deterministic", n1 == n2);

    p5::Engine H(p5::KernelMode::Scalar);
    H.activate(44100); H.setPolyphony(8); H.setPatch(p);
    H.setDriftDepth(127);
    H.noteOn(60, 100);
    auto h1 = render(H, 4200);
    check("drift max deterministic", [&]{
        p5::Engine H2(p5::KernelMode::Scalar);
        H2.activate(44100); H2.setPolyphony(8); H2.setPatch(p);
        H2.setDriftDepth(127);
        H2.noteOn(60, 100);
        return render(H2, 4200) == h1; }());
    check("drift max actually moves", h1 != n1);

    const double pEarly = periodAt(h1, 44100.0 / 261.63, 8192);
    const double pLate  = periodAt(h1, 44100.0 / 261.63, 110000);
    std::printf("  drift periods: early %.1f late %.1f neutral %.1f\n", pEarly, pLate, pA);
    check("drift unbiased (no fixed-detune bias)",
          std::fabs(pEarly / pA - 1.0) < 0.025 && std::fabs(pLate / pA - 1.0) < 0.025);

    std::printf("%s\n", fails ? "TRIM/DRIFT: FAILED" : "trim/drift: all ok");
    return fails ? 1 : 0;
}
