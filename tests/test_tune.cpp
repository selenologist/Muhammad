/* SPDX-License-Identifier: GPL-3.0-only
 * Units for the polar-wheel math (p5_gui) and the store's scale round trip. */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "p5_gui.hpp"
#include "p5_store.hpp"

static int fails = 0;
static void check(const char *name, bool ok)
{   std::printf("%-52s %s\n", name, ok ? "ok" : "FAIL"); if (!ok) fails++; }

int main()
{
    using namespace p5ui;
    float et[64];
    for (int i = 0; i < 12; i++) et[i] = 100.f * (i + 1);

    // equal temperament: every node exactly on the reference circle
    float r0 = 0; bool okr = true;
    for (int i = 1; i <= 12; i++)
    {   float x, y; tune::nodePos(et, 12, i, x, y);
        const float r = std::hypot(x - tune::wheelCX, y - tune::wheelCY);
        if (i == 1) r0 = r; else if (std::fabs(r - r0) > 0.01f) okr = false; }
    check("equal wheel: all nodes on one circle", okr && std::fabs(r0 - tune::wheelR) < 0.01f);

    // pythagorean chromatic: tone 1 sits just INSIDE (90.2c < 100c ET), tone 6 outside
    float py[12] = { 90.2f, 203.9f, 294.2f, 407.8f, 498.0f, 612.1f,
                     702.0f, 792.2f, 905.9f, 996.1f, 1109.9f, 1200.0f };
    float x1, y1; tune::nodePos(py, 12, 1, x1, y1);
    const float r1 = std::hypot(x1 - tune::wheelCX, y1 - tune::wheelCY);
    float x6, y6; tune::nodePos(py, 12, 6, x6, y6);
    const float r6 = std::hypot(x6 - tune::wheelCX, y6 - tune::wheelCY);
    check("pythagorean tone 1 inside the ring", r1 < tune::wheelR);
    check("pythagorean tone 6 outside the ring", r6 > tune::wheelR);
    check("ring scale auto-ranges", tune::ringScale(py, 12) > 1.f);

    // nodeAt round-trips positions to tone indices
    bool hit7 = false, hit1 = false;
    {   float x, y; tune::nodePos(py, 12, 7, x, y); hit7 = tune::nodeAt(py, 12, x, y) == 7;
        hit1 = tune::nodeAt(py, 12, x1, y1) == 1; }
    check("nodeAt round-trips nodes 1 and 7", hit1 && hit7);

    // monotone clamp: between neighbours; octave keeps >= one step and >= 100c
    float seq[3] = { 100.f, 300.f, 1200.f };
    check("clamp keeps interior tone between neighbours",
          tune::clampTone(seq, 3, 2, 50.f) == 101.f &&            // above tone 1
          tune::clampTone(seq, 3, 2, 999.f) == 999.f &&           // free below tone 3
          tune::clampTone(seq, 3, 2, 5000.f) == 1199.f);          // just under the octave
    check("clamp keeps octave above its neighbours",
          tune::clampTone(seq, 3, 3, 50.f) == 301.f);

    // store round trip: equal tones -> zero dev; pythagorean wheel == loaded file
    setenv("P5_STORE_DIR", "/tmp/muh-wheel-test-store", 1);
    std::system("mkdir -p /tmp/muh-wheel-test-store/Tunings && "
        "printf 'Example Pythagorean tuning\\n 12\\n!\\n 90.2\\n 203.9\\n 294.2\\n 407.8\\n "
        "498.0\\n 612.1\\n 702.0\\n 792.2\\n 905.9\\n 996.1\\n 1109.9\\n 1200.0\\n' "
        "> '/tmp/muh-wheel-test-store/Tunings/Example Pythagorean.scl'");
    static int16_t devA[128], devB[128];
    double cum[64]; for (int i = 0; i < 12; i++) cum[i] = py[i];
    const bool bld = p5store::buildScaleTones(cum, 12, devA);
    bool zeroDev = false;
    {   double e[64]; for (int i = 0; i < 12; i++) e[i] = 100.0 * (i + 1);
        int16_t d[128];
        zeroDev = p5store::buildScaleTones(e, 12, d);
        if (zeroDev) { zeroDev = true; for (int k = 0; k < 128; k++) if (d[k]) zeroDev = false; } }
    check("equal tones through buildScaleTones -> zero dev", zeroDev);
    char nm[40]; int tnn = 0; double tn2[64];
    const bool ld = p5store::loadTuningDev("Example Pythagorean.scl", devB, nm, sizeof nm, tn2, &tnn);
    check("loadTuningDev hands back editable tones", ld && tnn == 12 && tn2[0] == 90.2);
    bool same = bld && ld;
    for (int k = 0; same && k < 128; k++) same = devA[k] == devB[k];
    check("wheel-rebuilt scale == loaded-file scale (dev table)", same);
    int16_t devB2[128]; char nm2[40];
    const bool ld2 = p5store::loadTuningDev("Example Pythagorean.scl", devB2, nm2, sizeof nm2);
    check("legacy 4-arg loadTuningDev call still works", ld2 && !std::memcmp(devB2, devB, sizeof devB));

    std::printf("%s\n", fails ? "WHEEL/TUNE: FAILED" : "wheel/tune: all ok");
    return fails ? 1 : 0;
}
