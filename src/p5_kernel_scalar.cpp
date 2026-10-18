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
// audible-only deviation; the OS STRATEGY (what runs at 2x and where the
// decimator sits) is ours, not the firmware's. Decimation happens ONCE, at
// the end, over the summed voice mix - so no per-voice filter stands inside
// the FEEDBACK loop's hearing (that placement was the high-fb gremlins).
// Both kernels see the identical stream and identical arithmetic.
#include "p5_engine.hpp"
#include "p5_math.hpp"

namespace p5 {

namespace {

struct Hot {
    uint32_t ph[2];
    float z[4];
    float triangle, syncTail;
    int32_t noise;
    float fbPrev;                     // FEEDBACK: last main sample's VCA out
};

// p5_filter_core: four trapezoidal one-poles with an algebraic feedback
// solution. Curtis saturates the input; SSI saturates each stage.
// `tab` is the cutoff->g table for the current internal rate (Engine::tpt_).
inline float filterCore(Hot &s, float input, float cutoff, float k, float gain, int curtis,
                        const float *tab)
{
    cutoff = f_clamp(cutoff, 0.0f, (float)PW_TPT_TOP);
    uint32_t i = (uint32_t)cutoff;
    float g = tab[i];
    if (i < (uint32_t)PW_TPT_TOP) g += (tab[i + 1u] - g) * (cutoff - (float)i);
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
    // Patch-mode flags hoisted ONCE per block (integer control flow only -
    // no float law touched, bit-exactness by construction). The per-sample
    // array reads were the scalar fallback's cheap-to-kill overhead.
    const int fSawA = p[P5_SAW_A], fPulA = p[P5_PULSE_A], fSawB = p[P5_SAW_B],
              fPulB = p[P5_PULSE_B], fTriB = p[P5_TRI_B], fSync = p[P5_SYNC],
              fPolyFreq = p[P5_POLY_FREQ], fPolyPw = p[P5_POLY_PW],
              fPolyFilter = p[P5_POLY_FILTER], fWFreqB = p[P5_WHEEL_FREQ_B],
              fWFreqA = p[P5_WHEEL_FREQ_A], fWPwA = p[P5_WHEEL_PW_A],
              fWPwB = p[P5_WHEEL_PW_B], fWFilter = p[P5_WHEEL_FILTER];
    // OS-rate voice mix, staged in float and quantised once at the end.
    // Callers chop host blocks to <= CTL_ (the envelope-law time index
    // proves it), so 2*CTL_ sub-samples is the exact worst case.
    float stgL[2 * CTL_], stgR[2 * CTL_];
    const int total = (int)(n * os);
    for (int k = 0; k < total; k++) { stgL[k] = 0.0f; stgR[k] = 0.0f; }
    for (int v = 0; v < MAXV; v++) {
        if (!(mask >> v & 1u)) continue;
        Engine::Lane &l = e.lanes_[v];
        const LaneParams &c = l.p;
        Hot h;
        h.ph[0] = e.phaseA_[v]; h.ph[1] = e.phaseB_[v];
        h.z[0] = e.z_[0][v]; h.z[1] = e.z_[1][v]; h.z[2] = e.z_[2][v]; h.z[3] = e.z_[3][v];
        h.triangle = e.triangle_[v]; h.syncTail = e.syncTail_[v];
        h.noise = (int32_t)e.noise_[v];
        h.fbPrev = e.fbState_[v];      // = the lowpassed tap (see loop)
        const float gl0 = (float)l.panG[0] * (1.0f / 32768.0f);
        const float gl1 = (float)l.panG[1] * (1.0f / 32768.0f);
        int32_t last = 0;
        for (uint32_t i = 0; i < n; i++) {
            // per-sample control, materialized by Engine::process: envelopes,
            // VCA (onset ramp x amp drift), cutoff adds, pitch increments,
            // and the part-wide parameter row (knob math is per-sample too)
            const float fe = e.fenvS_[i][v], ae = e.aenvS_[i][v];
            const float va = e.vcaS_[i][v], fcAdd = e.fcAddS_[i][v];
            const uint32_t ia0 = e.iaS_[i][v], ib0 = e.ibS_[i][v];
            const float ppCut = e.ppS_[PP_CUT][i], ppK = e.ppS_[PP_K][i], ppFG = e.ppS_[PP_FG][i];
            const float ppEnvAmt = e.ppS_[PP_ENVAMT][i], ppPwA = e.ppS_[PP_PWMA][i], ppPwmB = e.ppS_[PP_PWMB][i];
            const float ppLa = e.ppS_[PP_LA][i], ppLb = e.ppS_[PP_LB][i], ppLn = e.ppS_[PP_LN][i];
            const float ppFb = e.ppS_[PP_FB][i], ppPaEnv = e.ppS_[PP_PAENV][i], ppPaB = e.ppS_[PP_PAB][i];
            const float ppPfEnv = e.ppS_[PP_PFENV][i], ppPfB = e.ppS_[PP_PFB][i];
            const float ppPwEnv = e.ppS_[PP_PWENV][i], ppPwB = e.ppS_[PP_PWB][i];
            for (uint32_t o = 0; o < os; o++) {          // the firmware's P5_OVERSAMPLE loop
                const float noise = (float)(fastNoise32(&h.noise) >> 8) * (1.0f / 16777216.0f) - 0.5f;
                const float lfo = e.wheel_[i * os + o], wheelPitch = e.wheelPitch_[i * os + o];
                const float gain = ae * c.ampVel;
            const float amp = va;
            const uint32_t db = fWFreqB ? wheelInc(ib0, wheelPitch) : ib0;
                const float b = oscWave(h.ph[1], db, pwOf(ppPwmB + (fWPwB ? lfo : 0.0f)),
                                        fSawB, fPulB, fTriB, &h.triangle);
                const float bn = b * 2.0f;
                uint32_t da = fPolyFreq ? incOf(ia0, fe * ppPaEnv + bn * ppPaB) : ia0;
                if (fWFreqA) da = wheelInc(da, wheelPitch);
                const uint32_t pw = pwOf(ppPwA + (fPolyPw ? fe * ppPwEnv + b * ppPwB : 0.0f)
                                         + (fWPwA ? lfo : 0.0f));
                float unused = 0.0f;
                float a = oscWave(h.ph[0], da, pw, fSawA, fPulA, 0, &unused) + h.syncTail;
                h.syncTail = 0.0f;
                const uint32_t nb = h.ph[1] + db, na = h.ph[0] + da;
                if (fSync && nb < h.ph[1] && db) {  // B resets A mid-sample, with the BLEP split
                    const float frac = syncFraction(0u - h.ph[1], db), remain = 1.0f - frac;
                    const uint32_t at = h.ph[0] + (uint32_t)((float)da * frac);
                    const float before = oscWave(at, da, pw, fSawA, fPulA, 0, &unused);
                    const float after = oscWave(0, da, pw, fSawA, fPulA, 0, &unused);
                    const float jump = (after - before) * 0.5f;
                    a += jump * remain * remain;
                    h.syncTail = -jump * frac * frac;
                    h.ph[0] = (uint32_t)((float)da * remain);
                } else {
                    h.ph[0] = na;
                }
                h.ph[1] = nb;
                const float x = a * ppLa + b * ppLb + noise * ppLn + ppFb * h.fbPrev;
                float fc = ppCut + fcAdd + fe * c.filterVel * ppEnvAmt;
                if (fPolyFilter) fc += fe * ppPfEnv + bn * ppPfB;
                if (fWFilter) fc += lfo * 112.0f;
                const float y = filterCore(h, fastSoftclip(x), fc, ppK, ppFG, curtis, tab);
                const float vcaF = (y * gain) * amp;    // VCA at the OS rate, so every
                h.fbPrev += kFbLpA * (vcaF - h.fbPrev); // sub-sample reaches the tap
                if (os == 1u)            // the firmware-identical int path, untouched
                {   last = (int32_t)(vcaF * (2.0f * VOICE_FS));
                    if (e.panOn_)
                    {   out[i] += (int32_t)(((int64_t)last * l.panG[0]) >> 15);
                        e.mixR_[i] += (int32_t)(((int64_t)last * l.panG[1]) >> 15); }
                    else out[i] += last;
                }
                else
                {   stgL[i * 2u + o] += vcaF * (e.panOn_ ? gl0 : 1.0f);
                    if (e.panOn_) stgR[i * 2u + o] += vcaF * gl1;
                    last = (int32_t)(vcaF * (2.0f * VOICE_FS));  // release-fade memory
                }
            }
        }
        l.lastPcm = last;
        e.phaseA_[v] = h.ph[0]; e.phaseB_[v] = h.ph[1];
        e.z_[0][v] = h.z[0]; e.z_[1][v] = h.z[1]; e.z_[2][v] = h.z[2]; e.z_[3][v] = h.z[3];
        e.triangle_[v] = h.triangle; e.syncTail_[v] = h.syncTail;
        e.noise_[v] = (uint32_t)h.noise;
        e.fbState_[v] = h.fbPrev;
    }
    if (os != 1u)      // THE decimator: once, right here, after the summed
    {   for (uint32_t i = 0; i < n; i++)       // mix at OS rate - the end
        {   out[i] += (int32_t)(hbDecim(e.hbMixL_, stgL[i * 2u], stgL[i * 2u + 1u])
                               * (2.0f * VOICE_FS));
            if (e.panOn_)
                e.mixR_[i] += (int32_t)(hbDecim(e.hbMixR_, stgR[i * 2u], stgR[i * 2u + 1u])
                                        * (2.0f * VOICE_FS));
        }
    }
}

} // namespace p5
