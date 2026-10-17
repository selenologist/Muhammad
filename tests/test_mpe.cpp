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
#include "p5_store.hpp"

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

    // 7) MEASURED validation: the engine really plays what the wheel depicts.
    //    Fine lock-in: |sum x[i] e^{-i 2 pi f i/fs}| over ~0.9 s; peak on a 1-cent grid.
    auto centsOf = [](const std::vector<float> &buf, double fRef, double lo, double hi)
    {
        double best = lo, bm = -1;
        for (double c = lo; c <= hi; c += 0.5)
        {
            const double w = 6.283185307179586 * fRef * (std::pow(2.0, c / 1200.0)) / 44100.0;
            const double cw = 2.0 * std::cos(w);
            double s1 = 0, s2 = 0;
            for (size_t i = 9000; i < 49000 && i < buf.size(); i++)
            {   const double s0 = buf[i] + cw * s1 - s2; s2 = s1; s1 = s0; }
            const double m = s1 * s1 + s2 * s2 - cw * s1 * s2;
            if (m > bm) { bm = m; best = c; }
        }
        return best;
    };
    auto renderTuned = [](const p5::Patch &p, const int16_t *dev, int note)
    {
        p5::Engine e(p5::KernelMode::Scalar); e.activate(44100); e.setPolyphony(8); e.setPatch(p);
        if (dev) e.setTuningDeviation(dev);
        e.noteOn(note, 100);
        return render(e, 1600);
    };
    {
        static const double pyth[13] = { 0, 113.69, 203.91, 294.13, 407.82, 498.04, 611.73,
                                         701.96, 815.64, 905.87, 996.09, 1109.78, 1200.0 };
        static int16_t devP[128];
        const bool okb = p5store::buildScaleDev(pyth, 12, 0x0fff, 0, devP);
        check("buildScaleDev pythagorean returns ok", okb);
        const auto et = renderTuned(p, nullptr, 61);
        const auto tp = renderTuned(p, devP, 61);
        const double cET = centsOf(et, 440.0 * std::pow(2.0, (61 - 69) / 12.0), -30, 30);
        const double cPY = centsOf(tp, 440.0 * std::pow(2.0, (61 - 69) / 12.0), -30, 30);
        std::printf("  measured C#: ET %+0.1f c, pythagorean %+0.1f c (table says %+0.1f)\n",
                    cET, cPY, devP[61] * 6.25);
        check("ET renders at ET (sanity)", std::fabs(cET) < 4.0);
        check("pythagorean C# really sounds +12.5c (depicted)", std::fabs(cPY - devP[61] * 6.25) < 4.0);

        // 8) quantizer: C major mask snaps C# DOWN to C (dev -100c), E stays put
        static const double maj[8] = { 0, 200, 400, 500, 700, 900, 1100, 1200 };
        static int16_t devM[128];
        check("buildScaleDev major quantizes", p5store::buildScaleDev(maj, 7, 0x0ab5, 0, devM));
        check("C major: C# key plays C (-100c)", devM[61] == -16);
        check("C major: white keys unshifted", devM[60] == 0 && devM[64] == 0 && devM[67] == 0);
        const auto q1 = renderTuned(p, devM, 61);
        const double cQ = centsOf(q1, 440.0 * std::pow(2.0, (61 - 69) / 12.0), -150, 150);
        std::printf("  measured quantized C#: %+0.1f c (expect -100 = C)\n", cQ);
        check("quantized C# sounds at C's pitch", std::fabs(cQ + 100.0) < 4.0);

        // 9) walk (>12 tones): 19-EDO key61 = +63.16c
        double e19[20]; for (int i = 1; i <= 19; i++) e19[i] = 1200.0 * i / 19.0;
        static int16_t devW[128];
        check("buildScaleDev 19-EDO walk", p5store::buildScaleDev(e19, 19, 0, 0, devW));
        // walk semantics: key 61 plays degree 2 of nineteen (63.16c) = ET minus 36.8c
        check("19-EDO key61 = -36.8c (-6 units)", devW[61] == -6);
        check("19-EDO key79 wraps to octave (+1200c over 19 ET keys = -700c)", devW[79] == -112);
    }

    // ---- wheel depth persistence (pc mode) ----
    // Saving must capture the wheel POSITION, not the stale pre-takeover
    // INITIAL byte: the old bug saved silent patches ("depth zeroed until I
    // touch the wheel again"). commitWheelToPatch fixes the file truth.
    check("wheel commit: taken writes position", [&]{
        p5::Engine en; en.setPcMode(true); en.editRaw(p5::P5_LFO_INITIAL, 0);
        en.setModWheel(90); en.commitWheelToPatch();
        return en.rawValue(p5::P5_LFO_INITIAL) == 90; }());
    check("wheel commit: untouched keeps patch byte", [&]{
        p5::Engine en; en.setPcMode(true); en.editRaw(p5::P5_LFO_INITIAL, 33);
        en.commitWheelToPatch();
        return en.rawValue(p5::P5_LFO_INITIAL) == 33; }());
    check("wheel commit: no pc mode never commits", [&]{
        p5::Engine en; en.editRaw(p5::P5_LFO_INITIAL, 40);
        en.setModWheel(90); en.commitWheelToPatch();
        return en.rawValue(p5::P5_LFO_INITIAL) == 40; }());

    // ---- drift now rides wire byte 37 (saved patches carry the knob) ----
    check("drift: setter writes byte 37", [&]{
        p5::Engine en; en.setDriftDepth(80);
        return en.rawValue(p5::P5_VINTAGE) == 80 && en.driftDepth() == 80; }());
    check("drift: patch load delivers depth", [&]{
        uint8_t raw[133]; std::memset(raw, 0, sizeof raw); raw[p5::P5_VINTAGE] = 66;
        p5::Patch p{}; std::memcpy(p.raw, raw, 133);
        p5::Engine en; en.setPatch(p);
        return en.driftDepth() == 66; }());
    check("drift: file round-trip keeps it", [&]{
        uint8_t raw[133]; std::memset(raw, 0, sizeof raw); raw[p5::P5_VINTAGE] = 100;
        p5::Patch p{}; std::memcpy(p.raw, raw, 133);
        p5::Engine en; en.setPatch(p);
        if (!p5store::savePatch("DRIFT RT", en.patch().raw)) return false;
        uint8_t back[133] = {}; if (!p5store::loadPatch("DRIFT RT", back)) return false;
        p5::Patch q{}; std::memcpy(q.raw, back, 133);
        p5::Engine e2; e2.setPatch(q);
        return e2.driftDepth() == 100; }());

    std::printf("%s\n", fails ? "MPE/TUNING: FAILED" : "mpe/tuning: all ok");
    return fails ? 1 : 0;
}
