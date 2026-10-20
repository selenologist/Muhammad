/* SPDX-License-Identifier: GPL-3.0-only
 * Quantization-noise probe: render a lone saw with a long release down to the
 * voice-kill point, then FFT the LAST SECOND of the tail and report energy
 * where the signal has no harmonics. A per-voice int32 quantiser (LSB = 1/32768
 * of the int domain, ~ -90 dBFS) shows up here as an off-harmonic floor and
 * idle tones once the tail is below ~ -80 dB; a native-float tail should not.
 * The same binary measures whichever codepath it links against, so int-vs-float
 * noise floors are directly comparable. */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "p5_common.hpp"
#include "p5_engine.hpp"

static void fft(std::vector<double> &re, std::vector<double> &im)
{
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; i++)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * M_PI / (double)len;
        const double wr = cos(ang), wi = sin(ang);
        for (size_t i = 0; i < n; i += len)
        {
            double cwr = 1.0, cwi = 0.0;
            for (size_t k = 0; k < len / 2; k++)
            {
                const double ur = re[i + k], ui = im[i + k];
                const double vr = re[i + k + len / 2] * cwr - im[i + k + len / 2] * cwi;
                const double vi = re[i + k + len / 2] * cwi + im[i + k + len / 2] * cwr;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                const double nwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr; cwr = nwr;
            }
        }
    }
}

int main(int argc, char **argv)
{
    p5::Engine e(p5::KernelMode::Auto);
    e.activate(44100);
    e.setPolyphony(8);
    e.setReleaseCut(1.0f / 16384.0f);            // the shipped -84 dBFS cut

    p5::Patch p;
    p5::patchInit(p);
    std::memset(p.raw, 0, 133);
    p.raw[p5::P5_SAW_A] = 1;                     // lone saw A, direct to mix
    p.raw[p5::P5_LEVEL_A] = 127;
    p.raw[p5::P5_CUTOFF] = 127;                  // filter wide open, no colouring
    p.raw[p5::P5_ATTACK_AMP] = 8;
    p.raw[p5::P5_SUSTAIN_AMP] = 127;
    p.raw[p5::P5_RELEASE_ON] = 1;
    p.raw[p5::P5_RELEASE_AMP] = argc > 1 ? (uint8_t)atoi(argv[1]) : 64;
    printf("release byte %d\n", p.raw[p5::P5_RELEASE_AMP]);
    e.setPatch(p);

    e.noteOn(45, 100);                           // A2 = 110 Hz, exact harmonic grid
    std::vector<float> buf;
    int32_t out[32];
    uint32_t ks = 0;                             // kill sample
    bool off = false;
    for (uint32_t s = 0; s < 44100u * 40u; s += 32)
    {
        if (!off && s >= 44100u) { e.noteOff(45); off = true; }
        e.process(out, 32);
        for (int i = 0; i < 32; i++)
#ifdef P5_FLOATOUT
            buf.push_back(out[i] < -1e30f ? 0.0f : (float)out[i]);
#else
            buf.push_back((float)out[i] * (1.0f / 32768.0f));
#endif
        if (!ks && s > 44100u && e.liveVoices() == 0) ks = s;
        if (s && (s % (44128u * 5u)) == 0u)
        {   float pk = 0; for (int i = 32; i--;) { float a = fabsf((float)out[i] * (1.0f/32768.0f)); if (a > pk) pk = a; }
            printf("  t=%2ds peak %.3f dBFS\n", (int)(s / 44100u), 20.0 * log10(pk + 1e-30)); }
    }
    if (!ks) { printf("voice never died in 40 s\n"); return 2; }
    printf("release killed the voice at %.3f s after note-off\n", (float)(ks - 44100u) / 44100.0f);

    const size_t N = 65536;
    if (buf.size() < ks + 1 || ks < N + 1024) { printf("tail shorter than FFT window\n"); return 2; }
    std::vector<double> re(N), im(N, 0.0);
    const size_t base = ks - N;
    double rms = 0;
    for (size_t i = 0; i < N; i++)
    {
        const double w = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)N);
        const double v = (double)buf[base + i] * w;
        rms += v * v;
        re[i] = v;
    }
    rms = sqrt(rms / (double)N);
    fft(re, im);

    const double binHz = 44100.0 / (double)N, gain = 2.0 / (double)N;
    double offPeak = 0, offSum = 0; size_t offBins = 0; double atFreq = 0;
    size_t firstOff = 100.0 / binHz, lastOff = 9000.0 / binHz;
    for (size_t b = firstOff; b <= lastOff; b++)
    {
        const double f = (double)b * binHz;
        const double h = f / 110.0;
        const long nearest = lround(h);
        if (nearest >= 1 && fabs(h - (double)nearest) * 110.0 < 3.0) continue;   // harmonic grid
        const double m = hypot(re[b], im[b]) * gain;
        offSum += m * m; offBins++;
        if (m > offPeak) { offPeak = m; atFreq = f; }
    }
    const double sigPeak = [&]{
        double mx = 0;
        for (size_t b = 1; b < N / 2; b++) { const double m = hypot(re[b], im[b]) * gain; if (m > mx) mx = m; }
        return mx; }();
    const double spurDb = 20.0 * log10(offPeak + 1e-300);
    printf("tail-window: peak-bin %.1f dBFS, off-harmonic floor %.1f dB, biggest off-harmonic spur "
           "%.1f dBFS @ %.0f Hz\n",
           20.0 * log10(sigPeak + 1e-300),
           10.0 * log10(offSum / (double)offBins + 1e-300),
           spurDb, atFreq);

    // B10: this probe used to print and always exit 0 - a silent test is no
    // test. The thresholds are deliberately loose: they do NOT certify a
    // tight noise floor (window skirt dominates the numbers), they catch
    // GROSS regressions the float path cannot produce: a re-introduced
    // integer quantiser's idle tones, a stuck voice that never reaches the
    // kill point, or a tail that never silences.
    int fails = 0;
    const double floorDb = 10.0 * log10(offSum / (double)offBins + 1e-300);
    const double killSec = (double)(ks - 44100u) / 44100.0;
    if (!(floorDb < -55.0)) { printf("FAIL: off-harmonic floor %.1f dB above -55\n", floorDb); ++fails; }
    if (!(spurDb < -28.0))   { printf("FAIL: idle spur %.1f dBFS above -28\n", spurDb); ++fails; }
    if (!(killSec > 0.3 && killSec < 2.5))
        { printf("FAIL: voice kill at %.3f s outside 0.3..2.5 s (fade/kill law broken)\n", killSec); ++fails; }
    if (!(sigPeak > 0.0))    { printf("FAIL: silent render - probe degenerate\n"); ++fails; }
    if (fails) { printf("test_quantnoise: %d assertion(s) FAILED\n", fails); return 1; }
    printf("test_quantnoise: assertions PASS\n");
    return 0;
}
