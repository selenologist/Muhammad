// SPDX-License-Identifier: GPL-3.0-only
// Surge XT's cascaded-allpass halfband decimator (the same structure and the
// same M=6 "soft" coefficients vasynth uses), 2x -> 1x, mono. Stopband ~150 dB
// past host Nyquist; group delay ~2.73 host samples at M=6.
#pragma once

namespace p5 {

namespace hbcoeff {
// even-branch and odd-branch allpass coefficients, M=6 (soft mode), from
// Surge XT's HalfbandIir / the vasynth reference implementation.
static constexpr float A[6] = {0.01677466677723562f, 0.13902148819717805f,
                               0.3325011117394731f,  0.53766105314488f,
                               0.7214184024215805f,  0.8821858402078155f};
static constexpr float B[6] = {0.06501319274445962f, 0.23094129990840923f,
                               0.4364942348420355f,  0.6329609551399348f,
                               0.80378086794111226f, 0.9599687404800694f};
constexpr int M = 6;
} // namespace hbcoeff

// One voice's decimator state, 2*6+2*6+1 floats.
struct HbState {
    float ax[hbcoeff::M], ay[hbcoeff::M], bx[hbcoeff::M], by[hbcoeff::M];
    float odd;
};

inline void hbReset(HbState &s)
{
    for (int j = 0; j < hbcoeff::M; j++)
        s.ax[j] = s.ay[j] = s.bx[j] = s.by[j] = 0.0f;
    s.odd = 0.0f;
}

// even/odd pair of oversampled samples -> one host-rate sample.
inline float hbDecim(HbState &s, float even, float odd)
{
    float a = even, b = s.odd;
    s.odd = odd;
    for (int j = 0; j < hbcoeff::M; j++) {
        float ya = s.ax[j] + hbcoeff::A[j] * (a - s.ay[j]);
        s.ax[j] = a; s.ay[j] = ya; a = ya;
        float yb = s.bx[j] + hbcoeff::B[j] * (b - s.by[j]);
        s.bx[j] = b; s.by[j] = yb; b = yb;
    }
    return (a + b) * 0.5f;
}

} // namespace p5
