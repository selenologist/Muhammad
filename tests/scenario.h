/* Shared note scripts for the engine A/B and the firmware cross-check.
 * One entry per control block (32 samples at 44.1 kHz): fire events at the
 * START of that block, before the block is rendered (as the firmware's
 * events_block does). */
#ifndef P5_SCENARIO_H
#define P5_SCENARIO_H

struct Event { unsigned block; int kind; int a; int b; };  /* kind: 0 note_on(a=note,b=vel), 1 note_off */

/* A: INIT patch, poly, a running chord with a release and a re-press */
static const struct Event scenA[] = {
    {  0, 0, 60, 100 },
    {  8, 0, 64, 100 },
    { 16, 0, 67, 120 },
    { 24, 1, 60, 0 },
    { 40, 0, 55, 80 },
    { 48, 1, 64, 0 },
    { 56, 0, 60, 111 },     /* accent: vel > 110 opens the filter */
    { 80, 1, 67, 0 },
    { 96, 1, 55, 0 },
    { 104, 1, 60, 0 },
};

/* B: a resonant self-oscillation sweep (levels 0 is patched in by the host of the scenario) */
static const struct Event scenB[] = {
    {  0, 0, 48, 100 },
    { 64, 1, 48, 0 },
    { 96, 0, 53, 100 },
    { 120, 1, 53, 0 },
};

/* C: the fifth voice must not be stolen and the sixth must steal the oldest released */
static const struct Event scenC[] = {
    {  0, 0, 36, 100 },
    {  4, 0, 48, 100 },
    {  8, 0, 60, 100 },
    { 12, 0, 64, 100 },
    { 16, 0, 67, 100 },
    { 20, 1, 48, 0 },
    { 30, 0, 72, 100 },
    { 40, 1, 60, 0 },
    { 50, 0, 60, 100 },
    { 70, 1, 36, 0 },
    { 80, 1, 64, 0 },
    { 90, 1, 67, 0 },
    { 95, 1, 72, 0 },
};

struct Scenario { const struct Event *ev; unsigned n; int preset; int patchRes; int patchCut; };

/* preset: 0 = INIT (via host_preset k=0), k = factory program k-1. patchRes/patchCut
 * override RESONANCE/CUTOFF (and silence the oscillator levels) for scenario B when
 * patchRes > 0. */
static const struct Scenario scenarios[] = {
    { scenA, sizeof scenA / sizeof scenA[0], 0, 0, -1 },
    { scenB, sizeof scenB / sizeof scenB[0], 0, 127, 30 },
    { scenC, sizeof scenC / sizeof scenC[0], 1, 0, -1 },
};
#define NSCEN (int)(sizeof scenarios / sizeof scenarios[0])
#define BLOCKS 160

#endif
