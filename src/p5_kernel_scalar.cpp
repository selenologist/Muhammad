// SPDX-License-Identifier: GPL-3.0-only
// The bit-exact reference kernel: p5_samples and p5_filter_core of melodee
// firmware/src/eng_prophet_test.c, transcribed lane by lane over the engine's
// struct-of-arrays hot state. Used for A/B against the SIMD kernel and against
// the firmware's own hostsim renders.
//
// The engine's P5_OVERSAMPLE knob is runtime here: os_ == 1 is the firmware's
// base path (bit-exact to the firmware's hostsim), os_ == 2 runs the sample
// body twice per host sample at half increments (exactly the firmware's own
// P5_OVERSAMPLE=2 build) but decimates each voice's 2x stream with the Surge
// halfband instead of the firmware's 2-tap box average - a deliberate,
// audible-only deviation. Both kernels see the identical decimated stream.
#include "p5_engine.hpp"
#include "p5_math.hpp"

namespace p5 {

namespace {

struct Hot {
    uint32_t ph[2];
    float z[4];
    float triangle, syncTail;
    int32_t noise;
};

// p5_filter_core: four trapezoidal one-poles with an algebraic feedback
// solution. Curtis saturates the input; SSI saturates each stage.
// `tab` is the cutoff->g table for the current internal rate (Engine::tpt_).
inline float filterCore(Hot &s, float input, float cutoff, float k, float gain, int curtis,
                        const float *tab)
{
    cutoff = f_clamp(cutoff, 0.0f, 127.0f);
    uint32_t i = (uint32_t)cutoff;
    float g = tab[i];
    if (i < 127u) g += (tab[i + 1u] - g) * (cutoff - (float)i);
    const float g2 = g * g, g3 = g2 * g, g4 = g3 * g;
    const float reciprocal = 1.0f / (1.0f + k * g4);
    const float sigma = (1.0f - g) * (g3 * s.z[0] + g2 * s.z[1] + g * s.z[2] + s.z[3]);
    float u = fastSoftclip((input * gain - k * sigma) * reciprocal);
#define STAGE(j, clip) do { \
    const float delta = g * (u - s.z[j]); \
    const float y = s.z[j] + delta; \
    s.z[j] = f_clamp(y + delta, -1.83105469f, 1.83105469f); \
    if (f_abs(s.z[j]) < 1e-20f) s.z[j] = 0.0f; \
    u = clip(y); \
} while (0)
#define LINEAR(y) (y)
    if (curtis) { STAGE(0, LINEAR); STAGE(1, LINEAR); STAGE(2, LINEAR); STAGE(3, LINEAR); }
    else        { STAGE(0, fastSoftclip); STAGE(1, fastSoftclip); STAGE(2, fastSoftclip); STAGE(3, fastSoftclip); }
#undef LINEAR
#undef STAGE
    return fastSoftclip(u);
}

} // namespace

void p5KernelScalar(Engine &e, uint32_t mask, int32_t *out, uint32_t n)
{
    const uint8_t *p = e.patch_.raw;
    const int curtis = !!p[P5_FILTER_REV];
    const uint32_t os = (uint32_t)e.os_;
    const float *tab = e.tpt_;
    for (int v = 0; v < MAXV; v++) {
        if (!(mask >> v & 1u)) continue;
        Engine::Lane &l = e.lanes_[v];
        const LaneParams &c = l.p;
        Hot h;
        h.ph[0] = e.phaseA_[v]; h.ph[1] = e.phaseB_[v];
        h.z[0] = e.z_[0][v]; h.z[1] = e.z_[1][v]; h.z[2] = e.z_[2][v]; h.z[3] = e.z_[3][v];
        h.triangle = e.triangle_[v]; h.syncTail = e.syncTail_[v];
        h.noise = (int32_t)e.noise_[v];
        int32_t last = 0;
        for (uint32_t i = 0; i < n; i++) {
            const float time = (float)i * (1.0f / CTL_);
            const float fe = c.f0 + (c.f1 - c.f0) * time, ae = c.a0 + (c.a1 - c.a0) * time;
            float y0 = 0.0f, y1 = 0.0f;
            for (uint32_t o = 0; o < os; o++) {          // the firmware's P5_OVERSAMPLE loop
                const float noise = (float)(fastNoise32(&h.noise) >> 8) * (1.0f / 16777216.0f) - 0.5f;
                const float lfo = e.wheel_[i * os + o], wheelPitch = e.wheelPitch_[i * os + o];
                const uint32_t db = p[P5_WHEEL_FREQ_B] ? wheelInc(c.ib, wheelPitch) : c.ib;
                const float b = oscWave(h.ph[1], db, pwOf(c.pwmB + (p[P5_WHEEL_PW_B] ? lfo : 0.0f)),
                                        p[P5_SAW_B], p[P5_PULSE_B], p[P5_TRI_B], &h.triangle);
                const float bn = b * 2.0f;
                uint32_t da = p[P5_POLY_FREQ] ? incOf(c.ia, fe * c.paEnv + bn * c.paB) : c.ia;
                if (p[P5_WHEEL_FREQ_A]) da = wheelInc(da, wheelPitch);
                const uint32_t pw = pwOf(c.pwmA + (p[P5_POLY_PW] ? fe * c.pwEnv + b * c.pwB : 0.0f)
                                         + (p[P5_WHEEL_PW_A] ? lfo : 0.0f));
                float unused = 0.0f;
                float a = oscWave(h.ph[0], da, pw, p[P5_SAW_A], p[P5_PULSE_A], 0, &unused) + h.syncTail;
                h.syncTail = 0.0f;
                const uint32_t nb = h.ph[1] + db, na = h.ph[0] + da;
                if (p[P5_SYNC] && nb < h.ph[1] && db) {  // B resets A mid-sample, with the BLEP split
                    const float frac = syncFraction(0u - h.ph[1], db), remain = 1.0f - frac;
                    const uint32_t at = h.ph[0] + (uint32_t)((float)da * frac);
                    const float before = oscWave(at, da, pw, p[P5_SAW_A], p[P5_PULSE_A], 0, &unused);
                    const float after = oscWave(0, da, pw, p[P5_SAW_A], p[P5_PULSE_A], 0, &unused);
                    const float jump = (after - before) * 0.5f;
                    a += jump * remain * remain;
                    h.syncTail = -jump * frac * frac;
                    h.ph[0] = (uint32_t)((float)da * remain);
                } else {
                    h.ph[0] = na;
                }
                h.ph[1] = nb;
                const float x = a * c.la + b * c.lb + noise * c.ln;
                float fc = c.cut + fe * c.filterVel * c.envAmount;
                if (p[P5_POLY_FILTER]) fc += fe * c.pfEnv + bn * c.pfB;
                if (p[P5_WHEEL_FILTER]) fc += lfo * 112.0f;
                const float y = filterCore(h, fastSoftclip(x), fc, c.k, c.filterGain, curtis, tab);
                if (o == 0u) y0 = y; else y1 = y;
            }
            const float sum = (os == 1u) ? y0 : hbDecim(e.hb_[v], y0, y1);
            const float gain = ae * c.ampVel * (1.0f + (float)l.ampDrift / 32768.0f);
            const float amp = (c.amp0 + (c.amp1 - c.amp0) * time) * (1.0f / 32768.0f);
            last = (int32_t)(sum * gain * amp * (2.0f * VOICE_FS));
            out[i] += last;
        }
        l.lastPcm = last;
        e.phaseA_[v] = h.ph[0]; e.phaseB_[v] = h.ph[1];
        e.z_[0][v] = h.z[0]; e.z_[1][v] = h.z[1]; e.z_[2][v] = h.z[2]; e.z_[3][v] = h.z[3];
        e.triangle_[v] = h.triangle; e.syncTail_[v] = h.syncTail;
        e.noise_[v] = (uint32_t)h.noise;
    }
}

} // namespace p5
