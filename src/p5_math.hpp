// SPDX-License-Identifier: GPL-3.0-only
// Exact scalar helpers mirrored from melodee firmware (dsp.c / x0x/fastmath.h /
// eng_prophet_test.c). Same rounding, same truncations, bit-for-bit at 44.1 kHz.
#pragma once
#include <cstdint>
#include "p5_tables.h"

namespace p5 {

union Bits { uint32_t u; int32_t i; float f; };

inline float f_abs(float x) { Bits b; b.f = x; b.u &= 0x7FFFFFFFu; return b.f; }
inline float f_min(float a, float b) { return a < b ? a : b; }
inline float f_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
inline int32_t i_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

inline int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }   // dsp.c: truncating, no rounding
inline int32_t mulq16(int32_t a, uint32_t k)
{
    return (int32_t)(((a >> 16) * (int32_t)k) + (int32_t)(((uint32_t)(a & 0xFFFF) * k) >> 16));
}

// pitch_inc() of melodee_tables.h (440 Hz * 2^((p/16 - 69)/12) -> u32 phase step)
inline uint32_t pitchInc(uint32_t p)
{
    uint32_t i = p + 64u, o = (i * 21846u) >> 22;
    return PW_PITCH_OCT[i - o * 192u] >> (10u - o);
}

// cents_inc() of dsp.c (whole 1/16 st from the table, the residual as a factor)
inline uint32_t centsInc(int32_t pitch16, int32_t ct, int32_t fine)
{
    int32_t d16 = ct * 16 / 100, rem = ct * 16 - d16 * 100;
    uint32_t inc = pitchInc(i_clamp(pitch16 + d16, 0, 2047));
    return inc + (uint32_t)((int32_t)(inc >> 12) * (rem * 2367 / 16000 + fine));
}

// x0x/fastmath.h fm_clampf for the env tick (the firmware's clamp, same order)
// p5_fast_* oscillators and helpers, verbatim
inline int32_t fastClamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

inline float fastBlep(uint32_t ph, uint32_t inc)
{
    if (!inc) return 0.0f;
    if (ph < inc) {
        float x = (float)ph / (float)inc;
        return x + x - x * x - 1.0f;
    }
    if (ph > 0xFFFFFFFFu - inc) {
        float x = -(float)(0xFFFFFFFFu - ph) / (float)inc;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

inline float fastOscSaw(uint32_t ph, uint32_t inc)
{
    return (float)ph * (1.0f / 2147483648.0f) - 1.0f - fastBlep(ph, inc);
}

inline float fastSoftclip(float x)
{
    float a = f_abs(x) * 128.0f;
    if (a >= 1024.0f) return x < 0.0f ? -1.0f : 1.0f;
    uint32_t i = (uint32_t)a;
    float y = PW_P5_TANH[i] + (PW_P5_TANH[i+1u] - PW_P5_TANH[i]) * (a - (float)i);
    return x < 0.0f ? -y : y;
}

inline uint32_t fastNoise32(int32_t *st)
{
    uint32_t s = (uint32_t)*st;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *st = (int32_t)s;
    return s;
}

// p5_lfo_wave (triangle +-0.5, saw/pulse 0..1, mixed evenly)
inline float lfoWave(uint32_t phase, const uint8_t *p)
{
    float pos = (float)phase * (1.0f / 4294967296.0f), y = 0.0f;
    uint32_t n = 0;
    if (p[P5_LFO_SAW]) { y += pos; n++; }
    if (p[P5_LFO_TRI]) { y += phase < 0x80000000u ? pos * 2.0f - 0.5f : 1.5f - pos * 2.0f; n++; }
    if (p[P5_LFO_PULSE]) { y += phase < 0x80000000u ? 1.0f : 0.0f; n++; }
    return n ? y / (float)n : 0.0f;
}

// p5_pitch_ratio: float interpolation of one octave (x in [-8, 6])
inline float pitchRatio(float x)
{
    int32_t octave = (int32_t)x; if ((float)octave > x) octave--;
    float pos = (x - (float)octave) * 512.0f;
    uint32_t i = (uint32_t)pos;
    float ratio = PW_P5_EXP2[i] + (PW_P5_EXP2[i+1u] - PW_P5_EXP2[i]) * (pos - (float)i);
    Bits scale; scale.u = (uint32_t)(octave + 127) << 23;
    return ratio * scale.f;
}

inline float syncFraction(uint32_t distance, uint32_t inc)
{
    return inc ? f_min((float)distance / (float)inc, 1.0f) : 0.0f;
}

inline uint32_t pwOf(float value)
{
    return (uint32_t)(f_clamp(value, 0.03125f, 31743.0f / 32768.0f) * 4294967296.0f);
}

inline uint32_t incOf(uint32_t base, float x)
{
    if (!x) return base;
    float y = (float)base * pitchRatio(f_clamp(x, -8.0f, 6.0f));
    return (uint32_t)f_clamp(y, 1.0f, 1932735232.0f);
}

inline uint32_t wheelInc(uint32_t base, float ratio)
{
    if (ratio == 1.0f) return base;
    return (uint32_t)f_min((float)base * ratio, 1932735232.0f);
}

// p5_wave: saw/pulse/tri mix with the tracked triangle integrator
inline float oscWave(uint32_t phase, uint32_t inc, uint32_t pw, int saw, int pulse, int tri, float *triangle)
{
    float y = 0.0f, a = fastOscSaw(phase, inc), square = 0.0f;
    if (saw) y += a * 0.5f;
    if (pulse) { square = (a - fastOscSaw(phase + pw, inc)) * 0.5f; y += square; }
    if (tri) {
        if (!pulse || pw != 0x80000000u) square = (a - fastOscSaw(phase + 0x80000000u, inc)) * 0.5f;
        *triangle = f_clamp(*triangle * (32767.0f / 32768.0f) + square * (float)inc * (8.0f / 4294967296.0f),
                            -1.220703125f, 1.220703125f);
        y += *triangle * 0.5f;
    }
    return y;
}

} // namespace p5
