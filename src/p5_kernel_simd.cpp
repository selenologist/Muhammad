// SPDX-License-Identifier: GPL-3.0-only
// The wide render kernel: p5_samples of melodee firmware/src/eng_prophet_test.c
// with the eight voices as the eight lanes of a float32x8 vector, through SIMDe,
// so one source emits AVX2 on x86 and NEON/SSE2 fallbacks elsewhere - the same
// portable-vectorization layer Surge uses.
//
// Bit-parity with the scalar kernel: keep the firmware's float expression order
// (no FMA contraction - build with -ffp-contract=off), and branch only on flags
// that are patch-wide, hence uniform across lanes.
#ifndef P5_SIMD_SYMBOL
#define P5_SIMD_SYMBOL p5KernelSimdSSE2      // recompiled as p5KernelSimdAVX2 by CMake
#endif
#include "p5_engine.hpp"
#include "p5_math.hpp"

#include <simde/x86/avx2.h>

namespace p5 {
namespace {

using V = simde__m256;
using VI = simde__m256i;

const V ALL1 = simde_mm256_castsi256_ps(simde_mm256_set1_epi32(-1));

inline V ld(const float *p) { return simde_mm256_loadu_ps(p); }
inline void st(V v, float *p) { simde_mm256_storeu_ps(p, v); }
inline VI ldu(const uint32_t *p) { return simde_mm256_loadu_si256((const VI *)p); }
inline void stu(VI v, uint32_t *p) { simde_mm256_storeu_si256((VI *)p, v); }
inline V spl(float x) { return simde_mm256_set1_ps(x); }
inline VI spli(int32_t x) { return simde_mm256_set1_epi32(x); }

inline V add(V a, V b) { return simde_mm256_add_ps(a, b); }
inline V sub(V a, V b) { return simde_mm256_sub_ps(a, b); }
inline V mul(V a, V b) { return simde_mm256_mul_ps(a, b); }
inline V div(V a, V b) { return simde_mm256_div_ps(a, b); }
inline V minv(V a, V b) { return simde_mm256_min_ps(a, b); }
inline V maxv(V a, V b) { return simde_mm256_max_ps(a, b); }
inline VI iadd(VI a, VI b) { return simde_mm256_add_epi32(a, b); }
inline VI isub(VI a, VI b) { return simde_mm256_sub_epi32(a, b); }
inline VI iand(VI a, VI b) { return simde_mm256_and_si256(a, b); }
inline VI ixor(VI a, VI b) { return simde_mm256_xor_si256(a, b); }
inline VI iieq(VI a, VI b) { return simde_mm256_cmpeq_epi32(a, b); }
// Shift counts are TEMPLATE CONSTANTS: x86 emits the immediate psll/psrl as
// before, and clang/ARM's vshlq_n_* NEON macros (which SIMDe maps to on
// arm64) demand a compile-time constant - a runtime `int c` there was the
// "must be a constant integer" wall in the macOS CI leg. Every call site
// already passes a literal.
template <int C> inline VI isll(VI a)  { return simde_mm256_slli_epi32(a, C); }
template <int C> inline VI isrli(VI a) { return simde_mm256_srli_epi32(a, C); }
inline V andf(V a, V b) { return simde_mm256_and_ps(a, b); }
inline V andnotf(V a, V b) { return simde_mm256_andnot_ps(a, b); }   // ~a & b
inline V xorf(V a, V b) { return simde_mm256_xor_ps(a, b); }
inline V castsi(VI a) { return simde_mm256_castsi256_ps(a); }
inline VI castis(V a) { return simde_mm256_castps_si256(a); }
inline V cmplt(V a, V b) { return simde_mm256_cmp_ps(b, a, SIMDE_CMP_GT_OQ); }   // a < b
inline VI cmt(VI a, VI b) { return simde_mm256_cmpgt_epi32(a, b); }         // signed a > b
inline V sel(V m, V a, V b) { return simde_mm256_blendv_ps(a, b, m); }     // b where m set
inline VI seli(V m, VI a, VI b) { return castis(simde_mm256_blendv_ps(castsi(a), castsi(b), m)); }
inline V fabsfV(V a) { return andnotf(spl(-0.0f), a); }                    // clear the sign bit
inline V negf(V a) { return xorf(a, spl(-0.0f)); }

// uint32 -> float like the C cast (split at 2^31)
// uint32 -> float with the same single correct rounding as the C cast: split into
// two 16-bit halves (each exact as float) and round once at the end.
inline V u2f(VI u)
{
    V hi = simde_mm256_cvtepi32_ps(isrli<16>(u));         // exact, < 2^16
    V lo = simde_mm256_cvtepi32_ps(iand(u, spli(0xFFFF)));
    return add(mul(hi, spl(65536.0f)), lo);
}
// float -> uint32 like the C cast (values below 2^32)
inline VI cvtf2u(V f)
{
    V m = cmplt(spl(2147483648.0f), f);                 // f >= 2^31
    VI lo = simde_mm256_cvttps_epi32(sel(m, f, sub(f, spl(2147483648.0f))));
    return seli(m, lo, ixor(lo, spli((int32_t)0x80000000)));
}
inline V cvti2f(VI i) { return simde_mm256_cvtepi32_ps(i); }    // values in int32 range
// unsigned a < b: bias both into the signed domain, then signed b' > a'
inline V iltu(VI a, VI b) { return castsi(cmt(ixor(b, spli((int32_t)0x80000000)),
                                               ixor(a, spli((int32_t)0x80000000)))); }

// per-lane table lookup: 8 lanes spill their indices once; the tables stay hot in L1
inline V gather(const float *tbl, VI idx)
{
    alignas(32) uint32_t ix[8];
    alignas(32) float val[8];
    stu(idx, ix);
    for (int j = 0; j < 8; j++) val[j] = tbl[ix[j]];
    return ld(val);
}

struct HotV {
    VI phA, phB;
    V z0, z1, z2, z3;
    V tri, syncTail;
    VI noise;
    V fbPrev;                        // FEEDBACK per-voice tap (same law as scalar)
};

// p5_fast_blep: the BLEP correction at a saw's wrap and at start
inline V blep(VI ph, VI inc)
{
    V mInc = castsi(iieq(inc, spli(0)));                       // a stopped oscillator has no edge
    V fPh = u2f(ph), fInc = u2f(inc);
    V x = div(fPh, fInc);
    V r1 = add(sub(add(x, x), mul(x, x)), spl(-1.0f));         // x + x - x*x - 1
    V m1 = iltu(ph, inc);                                      // ph < inc
    V m2 = iltu(isub(spli(-1), inc), ph);                      // 0xFFFFFFFF - inc < ph
    V xn = negf(div(u2f(ixor(ph, spli(-1))), fInc));           // -(float)(0xFFFFFFFF - ph) / inc
    V r2 = add(add(add(mul(xn, xn), xn), xn), spl(1.0f));
    return sel(mInc, sel(m2, sel(m1, spl(0.0f), r1), r2), spl(0.0f));
}

inline V oscSaw(VI ph, VI inc)
{
    return sub(sub(mul(u2f(ph), spl(1.0f / 2147483648.0f)), spl(1.0f)), blep(ph, inc));
}

// p5_fast_softclip: interpolate the 1025-entry tanh table, saturate beyond it
inline V softclipV(V x)
{
    V a = mul(fabsfV(x), spl(128.0f));
    VI i = cvtf2u(a);
    VI i0 = simde_mm256_min_epi32(i, spli(1023));               // clamp so the +1 lane stays in the table
    VI i1 = simde_mm256_min_epi32(iadd(i, spli(1)), spli(1024));
    V t0 = gather(PW_P5_TANH, i0), t1 = gather(PW_P5_TANH, i1);
    V y = add(t0, mul(sub(t1, t0), sub(a, cvti2f(i0))));
    V neg = cmplt(x, spl(0.0f));
    V signedY = sel(neg, y, negf(y));
    V big = andnotf(cmplt(a, spl(1024.0f)), ALL1);              // a >= 1024
    return sel(big, signedY, sel(neg, spl(1.0f), spl(-1.0f)));
}

inline V pitchRatioV(V x)                                      // p5_pitch_ratio, x in [-8, 6]
{
    VI o = simde_mm256_cvttps_epi32(x);
    V of = cvti2f(o);
    V m = cmplt(x, of);                                        // (float)o > x: floor down
    o = seli(m, o, iadd(o, spli(-1)));
    of = cvti2f(o);
    V pos = mul(sub(x, of), spl(512.0f));
    VI i = cvtf2u(pos);
    V t0 = gather(PW_P5_EXP2, i), t1 = gather(PW_P5_EXP2, iadd(i, spli(1)));
    V ratio = add(t0, mul(sub(t1, t0), sub(pos, cvti2f(i))));
    V scale = castsi(isll<23>(iadd(o, spli(127))));            // 2^octave through the exponent
    return mul(ratio, scale);
}

inline VI wheelIncV(VI base, V ratio)                          // p5_wheel_inc
{
    return cvtf2u(minv(mul(u2f(base), ratio), spl(1932735232.0f)));
}

inline VI incOfV(VI base, V x)                                 // p5_inc
{
    V r = pitchRatioV(minv(maxv(x, spl(-8.0f)), spl(6.0f)));
    return cvtf2u(minv(maxv(mul(u2f(base), r), spl(1.0f)), spl(1932735232.0f)));
}

inline VI pwOfV(V value)                                       // p5_pw
{
    return cvtf2u(mul(minv(maxv(value, spl(0.03125f)), spl(31743.0f / 32768.0f)), spl(4294967296.0f)));
}

// p5_filter_core over lanes: four trapezoidal one-poles, algebraic feedback.
// Curtis saturates only the input and output; SSI saturates every stage.
inline V filterCoreV(HotV &h, V input, V cutoff, V k, V gain, int curtis, const float *tab)
{
    V cc = minv(maxv(cutoff, spl(0.0f)), spl((float)PW_TPT_TOP));
    VI i = cvtf2u(cc);
    V g0 = gather(tab, i);
    V g1 = gather(tab, cvtf2u(minv(cc + spl(1.0f), spl((float)PW_TPT_TOP))));   // clamp, not iand: 160-entry table
    V g = sel(castsi(cmt(spli(PW_TPT_TOP), i)), g0, add(g0, mul(sub(g1, g0), sub(cc, cvti2f(i)))));  // interp when i < TOP
    V g2 = mul(g, g), g3 = mul(g2, g), g4 = mul(g3, g);
    V recip = div(spl(1.0f), add(spl(1.0f), mul(k, g4)));
    V sigma = mul(sub(spl(1.0f), g), add(add(add(mul(g3, h.z0), mul(g2, h.z1)), mul(g, h.z2)), h.z3));
    V u = softclipV(mul(sub(mul(input, gain), mul(k, sigma)), recip));
#define STAGE(jm, clip) do { \
    V delta = mul(g, sub(u, jm)); \
    V y = add(jm, delta); \
    jm = minv(maxv(add(y, delta), spl(-1.83105469f)), spl(1.83105469f)); \
    jm = sel(cmplt(fabsfV(jm), spl(1e-20f)), jm, spl(0.0f)); \
    u = clip; \
} while (0)
#define LINC(y) (y)
    if (curtis) { STAGE(h.z0, LINC(y)); STAGE(h.z1, LINC(y)); STAGE(h.z2, LINC(y)); STAGE(h.z3, LINC(y)); }
    else { STAGE(h.z0, softclipV(y)); STAGE(h.z1, softclipV(y)); STAGE(h.z2, softclipV(y)); STAGE(h.z3, softclipV(y)); }
#undef LINC
#undef STAGE
    return softclipV(u);
}

} // namespace

void P5_SIMD_SYMBOL(Engine &e, uint32_t mask, int32_t *out, uint32_t n)
{
    const uint8_t *p = e.patch_.raw;
    const int curtis = !!p[P5_FILTER_REV];
    const uint32_t os = (uint32_t)e.os_;
    const float *tab = e.tpt_;

    // transpose the per-lane block parameters (LaneParams: 24 floats + 2 u32)
    V parF[24];
    for (int f = 0; f < 24; f++) {
        alignas(32) float t[8];
        for (int j = 0; j < 8; j++) t[j] = ((const float *)&e.lanes_[j].p)[f];
        parF[f] = ld(t);
    }
    enum { F_F0, F_A0, F_F1, F_A1, F_CUT, F_K, F_FG, F_FVEL, F_AVEL, F_ENVAMT,
           F_PAENV, F_PAB, F_PFENV, F_PFB, F_PWENV, F_PWB, F_PWMA, F_PWMB,
           F_LA, F_LB, F_LN, F_AMP0, F_AMP1, F_FB };
    VI ia, ib;
    {
        alignas(32) uint32_t t[8];
        for (int j = 0; j < 8; j++) t[j] = e.lanes_[j].p.ia;
        ia = ldu(t);
        for (int j = 0; j < 8; j++) t[j] = e.lanes_[j].p.ib;
        ib = ldu(t);
    }
    V laneLive = spl(0.0f);
    {
        alignas(32) float t[8];
        for (int j = 0; j < 8; j++) t[j] = (float)((mask >> j) & 1u);
        laneLive = ld(t);
    }
    V ampDriftN = spl(0.0f);
    {
        alignas(32) float t[8];
        for (int j = 0; j < 8; j++) t[j] = (float)e.lanes_[j].ampDrift;
        ampDriftN = ld(t);
    }

    HotV h;
    h.phA = ldu(e.phaseA_); h.phB = ldu(e.phaseB_);
    h.z0 = ld(e.z_[0]); h.z1 = ld(e.z_[1]); h.z2 = ld(e.z_[2]); h.z3 = ld(e.z_[3]);
    h.tri = ld(e.triangle_); h.syncTail = ld(e.syncTail_);
    h.noise = ldu(e.noise_);
    h.fbPrev = ld(e.fbState_);

    const V mSawA = spl(p[P5_SAW_A] ? 0.5f : 0.0f);
    const V mPulA = spl(p[P5_PULSE_A] ? 1.0f : 0.0f);
    const V mSawB = spl(p[P5_SAW_B] ? 0.5f : 0.0f);
    const V mPulB = spl(p[P5_PULSE_B] ? 1.0f : 0.0f);
    const int pulseBHalfCapable = p[P5_PULSE_B];

    alignas(32) int32_t lastArr[8];

    // OS-rate voice mix staging (float, quantised once after the summing).
    // One decimator for the whole mix now: no per-lane halfband state.
    float stgL[2 * CTL_], stgR[2 * CTL_];
    {   const int total = (int)(n * os);
        for (int k = 0; k < total; k++) { stgL[k] = 0.0f; stgR[k] = 0.0f; }
    }
    V gl0V = spl(1.0f), gl1V = spl(1.0f);
    if (e.panOn_)
    {   alignas(32) float g0[8], g1[8];
        for (int j = 0; j < 8; j++)
        {   g0[j] = (float)e.lanes_[j].panG[0] * (1.0f / 32768.0f);
            g1[j] = (float)e.lanes_[j].panG[1] * (1.0f / 32768.0f); }
        gl0V = ld(g0); gl1V = ld(g1);
    }

    for (uint32_t i = 0; i < n; i++) {
        const V time = spl((float)i * (1.0f / CTL_));
        const V fe = add(parF[F_F0], mul(sub(parF[F_F1], parF[F_F0]), time));
        const V ae = add(parF[F_A0], mul(sub(parF[F_A1], parF[F_A0]), time));
        // the VCA ramp and the int32 quantisation, identical to p5_samples
        const V gain = mul(mul(ae, parF[F_AVEL]), add(spl(1.0f), mul(ampDriftN, spl(1.0f / 32768.0f))));
        const V amp = mul(mul(add(parF[F_AMP0], mul(sub(parF[F_AMP1], parF[F_AMP0]), time)),
                              spl(1.0f / 32768.0f)), laneLive);
        for (uint32_t o = 0; o < os; o++) {          // the firmware's P5_OVERSAMPLE loop
        const V lfo = spl(e.wheel_[i * os + o]), wheelPitch = spl(e.wheelPitch_[i * os + o]);

        // p5_fast_noise32 across lanes
        {
            VI s = h.noise;
            s = ixor(s, isll<13>(s));
            s = ixor(s, isrli<17>(s));
            s = ixor(s, isll<5>(s));
            h.noise = s;
        }
        const V noise = sub(mul(cvti2f(isrli<8>(h.noise)), spl(1.0f / 16777216.0f)), spl(0.5f));

        const VI db = p[P5_WHEEL_FREQ_B] ? wheelIncV(ib, wheelPitch) : ib;
        const VI pwB = pwOfV(p[P5_WHEEL_PW_B] ? add(parF[F_PWMB], lfo) : parF[F_PWMB]);

        // oscillator B with the triangle integrator (p5_wave)
        V bSaw = oscSaw(h.phB, db);
        V bSq = mul(sub(bSaw, oscSaw(iadd(h.phB, pwB), db)), spl(0.5f));
        if (p[P5_TRI_B]) {
            // p5_wave: reuse the pulse square only when pw is exactly half (identical read)
            V triSq = mul(sub(bSaw, oscSaw(iadd(h.phB, spli((int32_t)0x80000000)), db)), spl(0.5f));
            if (pulseBHalfCapable)
                triSq = sel(castsi(iieq(pwB, spli((int32_t)0x80000000))), triSq, bSq);
            V tSq = triSq;
            h.tri = minv(maxv(add(mul(h.tri, spl(32767.0f / 32768.0f)),
                                  mul(tSq, mul(u2f(db), spl(8.0f / 4294967296.0f)))),
                         spl(-1.220703125f)), spl(1.220703125f));
        }
        const V b = add(add(mul(bSaw, mSawB), mul(bSq, mPulB)),
                        p[P5_TRI_B] ? mul(h.tri, spl(0.5f)) : spl(0.0f));
        const V bn = mul(b, spl(2.0f));

        // oscillator A: poly-mod, pulse width, sync
        VI da = p[P5_POLY_FREQ] ? incOfV(ia, add(mul(fe, parF[F_PAENV]), mul(bn, parF[F_PAB]))) : ia;
        if (p[P5_WHEEL_FREQ_A]) da = wheelIncV(da, wheelPitch);
        const VI pwA = pwOfV(add(add(parF[F_PWMA],
                                    p[P5_POLY_PW] ? add(mul(fe, parF[F_PWENV]), mul(b, parF[F_PWB])) : spl(0.0f)),
                                p[P5_WHEEL_PW_A] ? lfo : spl(0.0f)));
        V aSaw = oscSaw(h.phA, da);
        V a = add(add(mul(aSaw, mSawA),
                      mul(mul(sub(aSaw, oscSaw(iadd(h.phA, pwA), da)), spl(0.5f)), mPulA)),
                  h.syncTail);
        h.syncTail = spl(0.0f);

        const VI nb = iadd(h.phB, db), na = iadd(h.phA, da);
        if (p[P5_SYNC]) {
            V m = andf(iltu(nb, h.phB), andnotf(castsi(iieq(db, spli(0))), ALL1));   // wrapped and running
            V frac = minv(div(u2f(isub(spli(0), h.phB)), u2f(db)), spl(1.0f));
            V remain = sub(spl(1.0f), frac);
            VI at = iadd(h.phA, cvtf2u(mul(frac, u2f(da))));
            V sAt = oscSaw(at, da);
            V before = add(mul(sAt, mSawA),
                           mul(mul(sub(sAt, oscSaw(iadd(at, pwA), da)), spl(0.5f)), mPulA));
            V s0 = oscSaw(spli(0), da);
            V after = add(mul(s0, mSawA),
                          mul(mul(sub(s0, oscSaw(pwA, da)), spl(0.5f)), mPulA));
            V jump = mul(sub(after, before), spl(0.5f));
            a = sel(m, a, add(a, mul(mul(jump, remain), remain)));
            h.syncTail = sel(m, h.syncTail, negf(mul(mul(jump, frac), frac)));
            h.phA = seli(m, na, cvtf2u(mul(remain, u2f(da))));
        } else {
            h.phA = na;
        }
        h.phB = nb;

        const V x = add(add(add(mul(a, parF[F_LA]), mul(b, parF[F_LB])),
                            mul(noise, parF[F_LN])),
                        mul(parF[F_FB], h.fbPrev));

        V fc = add(parF[F_CUT], mul(mul(fe, parF[F_FVEL]), parF[F_ENVAMT]));
        if (p[P5_POLY_FILTER]) fc = add(fc, add(mul(fe, parF[F_PFENV]), mul(bn, parF[F_PFB])));
        if (p[P5_WHEEL_FILTER]) fc = add(fc, mul(lfo, spl(112.0f)));

        const V y = filterCoreV(h, softclipV(x), fc, parF[F_K], parF[F_FG], curtis, tab);
        const V vcaF = mul(mul(y, gain), amp);
        h.fbPrev = add(h.fbPrev, mul(spl(kFbLpA), sub(vcaF, h.fbPrev)));  // 6 dB/oct pole at fs/4
        if (os == 1u)                    // the firmware-identical path, untouched
        {   VI last = simde_mm256_cvttps_epi32(mul(vcaF, spl(2.0f * VOICE_FS)));
            simde_mm256_storeu_si256((simde__m256i *)lastArr, last);
            if (e.panOn_)
                for (int j = 0; j < 8; j++)
                {   const int64_t sv = lastArr[j];
                    out[i] += (int32_t)((sv * e.lanes_[j].panG[0]) >> 15);
                    e.mixR_[i] += (int32_t)((sv * e.lanes_[j].panG[1]) >> 15); }
            else
                for (int j = 0; j < 8; j++) out[i] += lastArr[j];
        }
        else
        {   alignas(32) float vb[8];
            if (e.panOn_)
            {   st(mul(vcaF, gl0V), vb);
                for (int j = 0; j < 8; j++) stgL[i * 2u + o] += vb[j];
                st(mul(vcaF, gl1V), vb);
                for (int j = 0; j < 8; j++) stgR[i * 2u + o] += vb[j];
            }
            else
            {   st(vcaF, vb);
                for (int j = 0; j < 8; j++) stgL[i * 2u + o] += vb[j];
            }
            VI last = simde_mm256_cvttps_epi32(mul(vcaF, spl(2.0f * VOICE_FS)));
            simde_mm256_storeu_si256((simde__m256i *)lastArr, last);  // release-fade memory
        }
        }
    }

    stu(h.phA, e.phaseA_); stu(h.phB, e.phaseB_);
    st(h.z0, e.z_[0]); st(h.z1, e.z_[1]); st(h.z2, e.z_[2]); st(h.z3, e.z_[3]);
    st(h.tri, e.triangle_); st(h.syncTail, e.syncTail_);
    stu(h.noise, e.noise_);
    st(h.fbPrev, e.fbState_);

    for (int j = 0; j < 8; j++)
        if ((mask >> j) & 1u) e.lanes_[j].lastPcm = lastArr[j];

    if (os != 1u)      // THE decimator: once, over the summed mix at OS rate
    {   for (uint32_t i = 0; i < n; i++)
        {   out[i] += (int32_t)(p5::hbDecim(e.hbMixL_, stgL[i * 2u], stgL[i * 2u + 1u])
                               * (2.0f * VOICE_FS));
            if (e.panOn_)
                e.mixR_[i] += (int32_t)(p5::hbDecim(e.hbMixR_, stgR[i * 2u], stgR[i * 2u + 1u])
                                        * (2.0f * VOICE_FS));
        }
    }
}

} // namespace p5
