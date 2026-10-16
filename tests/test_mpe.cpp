/* SPDX-License-Identifier: GPL-3.0-only
 * Units for the v1.12 MPE + microtuning engine surface: per-channel bend
 * isolation, asymmetric ranges, channel-keyed release, reuse semantics, and
 * the per-key tuning deviation table. Deterministic int-domain renders. */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "p5_common.hpp"
#include "p5_engine.hpp"

static int fails = 0;
static void check(const char *name, bool ok)
{
    std::printf("%-48s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static p5::Patch loneSaw()
{
    p5::Patch p;
    std::memset(&p, 0, sizeof p);
    p.size = 133; p.model = 0x32; p.command = 3;
    p.raw[p5::P5_FREQ_A] = 24; p.raw[p5::P5_FREQ_B] = 24;
    p.raw[p5::P5_SAW_A] = 1;
    p.raw[p5::P5_LEVEL_A] = 127;
    p.raw[p5::P5_CUTOFF] = 127;
    p.raw[p5::P5_ATTACK_AMP] = 4;
    p.raw[p5::P5_SUSTAIN_AMP] = 127;
    p.raw[p5::P5_RELEASE_ON] = 1;
    p.raw[p5::P5_RELEASE_AMP] = 16;         // dies ~32 ms after gate-off at the -84 cut... fast
    return p;
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
{
    const int n = 4096;
    double best = -1e300; int bestLag = (int)guess;
    for (int lag = (int)(guess * 0.75); lag <= (int)(guess * 1.25); lag++)
    {
        double acc = 0;
        for (int i = 0; i < n; i++) acc += (double)buf[start + i] * buf[start + i + lag];
        if (acc > best) { best = acc; bestLag = lag; }
    }
    return bestLag;
}

static long zeroX(const std::vector<float> &buf, size_t from, size_t to)
{   // one crossing per cycle for a saw: immune to harmonic autocorrelation peaks
    long n = 0;
    for (size_t i = from + 1; i < to && i < buf.size(); i++)
        if ((buf[i - 1] < 0.0f) != (buf[i] < 0.0f)) n++;
    return n;
}

static double peakOf(const std::vector<float> &buf, size_t from, size_t to)
{
    double pk = 0;
    for (size_t i = from; i < to && i < buf.size(); i++) { double a = std::fabs(buf[i]); if (a > pk) pk = a; }
    return pk;
}

int main()
{
    /* NOTE: mk() by value would need move semantics; simpler: fresh engines inline. */
    p5::Patch p = loneSaw();

    // 1) bend on an UNINVOLVED channel must be inaudible to a ch1 voice
    p5::Engine A(p5::KernelMode::Scalar); A.activate(44100); A.setPolyphony(8); A.setPatch(p);
    A.noteOn(60, 100, 1);
    auto aRef = render(A, 600);
    p5::Engine B(p5::KernelMode::Scalar); B.activate(44100); B.setPolyphony(8); B.setPatch(p);
    B.setBendChannel(0, 8191);
    B.noteOn(60, 100, 1);
    auto bOther = render(B, 600);
    check("bend on ch0 does not touch a ch1 voice", aRef == bOther);

    p5::Engine C(p5::KernelMode::Scalar); C.activate(44100); C.setPolyphony(8); C.setPatch(p);
    C.setBendChannel(1, 8191);
    C.noteOn(60, 100, 1);
    auto cOwn = render(C, 600);
    check("bend on the voice's own channel moves it", cOwn != aRef);

    // 2) asymmetric ranges: editor default up=3, down=7 semitones
    const double pBase = periodAt(aRef, 169.0);
    p5::Engine D(p5::KernelMode::Scalar); D.activate(44100); D.setPolyphony(8); D.setPatch(p);
    D.setBendRangeDefault(3.0, 7.0);
    D.setBendChannel(1, 8191);
    D.noteOn(60, 100, 1);
    auto dUp = render(D, 900);
    double wantUp = pBase / std::pow(2.0, 3.0 / 12.0);
    double gotUp = periodAt(dUp, wantUp, 20480);
    std::printf("  up: base %.1f want %.1f got %.1f\n", pBase, wantUp, gotUp);
    check("bend UP honours range 3", std::fabs(gotUp / wantUp - 1.0) < 0.02);

    p5::Engine E(p5::KernelMode::Scalar); E.activate(44100); E.setPolyphony(8); E.setPatch(p);
    E.setBendRangeDefault(3.0, 7.0);
    E.setBendChannel(1, -8192);
    E.noteOn(60, 100, 1);
    auto dDn = render(E, 900);
    double wantDn = pBase / std::pow(2.0, -7.0 / 12.0);
    double gotDn = periodAt(dDn, wantDn, 20480);
    std::printf("  down: want %.1f got %.1f\n", wantDn, gotDn);
    check("bend DOWN honours range 7 (asymmetric)", std::fabs(gotDn / wantDn - 1.0) < 0.02);

    // 3) per-channel RPN override: ch1 ±12 while ch0 keeps default
    p5::Engine F(p5::KernelMode::Scalar); F.activate(44100); F.setPolyphony(8); F.setPatch(p);
    F.setBendRangeDefault(3.0, 3.0);
    F.setBendRangeSemitones(1, 12.0, 12.0);
    F.setBendChannel(1, 8191);
    F.noteOn(60, 100, 1);
    auto fRpn = render(F, 900);
    double want12 = pBase / std::pow(2.0, 12.0 / 12.0);
    double got12 = periodAt(fRpn, want12, 20480);
    check("per-channel RPN range beats the default", std::fabs(got12 / want12 - 1.0) < 0.02);

    // 4) same key, two channels = two voices; release one, the other sustains
    p5::Engine G(p5::KernelMode::Scalar); G.activate(44100); G.setPolyphony(8); G.setPatch(p);
    G.noteOn(60, 100, 0);
    G.noteOn(60, 100, 1);
    render(G, 1);                        // live_ refreshes only inside process()
    check("same key different channel = separate voices", G.liveVoices() == 2);
    G.noteOff(60, 0);
    auto g1 = render(G, 300);          // 200 ms: ch0 must be dead, ch1 full sustain
    G.noteOff(60, 1);
    auto g2 = render(G, 300);
    check("ch0 release leaves ch1 sounding", peakOf(g1, 100 * 32, g1.size()) > 0.05);
    check("ch1 release then dies", peakOf(g2, 100 * 32, g2.size()) < 0.005);

    // 5) same key same channel reuses the lane (firmware allocation kept)
    p5::Engine H(p5::KernelMode::Scalar); H.activate(44100); H.setPolyphony(8); H.setPatch(p);
    H.noteOn(60, 100, 2);
    H.noteOn(60, 110, 2);
    render(H, 1);
    check("same key same channel reuses one voice", H.liveVoices() == 1);

    // 6) tuning: all-zero deviation is bit-identical to never setting it
    p5::Engine I(p5::KernelMode::Scalar); I.activate(44100); I.setPolyphony(8); I.setPatch(p);
    static int16_t zero[128]; std::memset(zero, 0, sizeof zero);
    I.setTuningDeviation(zero);
    I.noteOn(60, 100, 0);
    auto iZero = render(I, 600);
    check("zero tuning table == untouched path", iZero == aRef);

    int16_t dev[128]; std::memset(dev, 0, sizeof dev);
    dev[60] = 16;                                   // +1 semitone on C4 only
    p5::Engine J(p5::KernelMode::Scalar); J.activate(44100); J.setPolyphony(8); J.setPatch(p);
    J.setTuningDeviation(dev);
    J.noteOn(60, 100, 0);
    auto jDev = render(J, 600);
    double wantT = pBase / std::pow(2.0, 1.0 / 12.0);
    double gotT = periodAt(jDev, wantT);
    check("tuning dev +16 units == +1 semitone at key 60", std::fabs(gotT / wantT - 1.0) < 0.02);

    dev[60] = 0; dev[72] = -8;                      // quarter-tone flat at C6
    p5::Engine K(p5::KernelMode::Scalar); K.activate(44100); K.setPolyphony(8); K.setPatch(p);
    K.setTuningDeviation(dev);
    K.noteOn(72, 100, 0);
    auto kDev = render(K, 600);
    p5::Engine L(p5::KernelMode::Scalar); L.activate(44100); L.setPolyphony(8); L.setPatch(p);
    L.noteOn(72, 100, 0);
    auto kNeu = render(L, 600);
    const long zN = zeroX(kNeu, 12000, 19000), zD = zeroX(kDev, 12000, 19000);
    const double ratio = (double)zD / (double)zN;
    const double want = std::pow(2.0, -8.0 / 16.0 / 12.0);   // dev -8 xics = -0.5 semitone
    std::printf("  qtone: zc neutral %ld dev %ld ratio %.4f want %.4f\n", zN, zD, ratio, want);
    check("quarter-tone dev shifts by 50 cents", std::fabs(ratio / want - 1.0) < 0.015);

    std::printf("%s\n", fails ? "MPE/TUNING: FAILED" : "mpe/tuning: all ok");
    return fails ? 1 : 0;
}
