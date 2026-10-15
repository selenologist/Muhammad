/* ns per sample of the whole part (8 voices) on the host: scalar kernel vs
 * the SIMDe wide kernel, at full polyphony. */
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>
#include <string>
#include "p5_engine.hpp"

static double bench(bool simd, int voices)
{
    p5::Engine e(simd);
    e.activate(44100);
    e.setPolyphony(8);
    p5::Patch p;
    p5::patchInit(p);
    p.raw[p5::P5_SAW_B] = 1; p.raw[p5::P5_TRI_B] = 1; p.raw[p5::P5_PULSE_A] = 1;
    p.raw[p5::P5_POLY_ENV] = 90; p.raw[p5::P5_POLY_B] = 80; p.raw[p5::P5_POLY_FILTER] = 1;
    p.raw[p5::P5_RESONANCE] = 90;
    e.setPatch(p);
    for (int i = 0; i < voices; i++) e.noteOn(36 + i * 7, 100);
    int32_t out[32];
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int b = 0; b < 44100 * 5 / 32; b++) e.process(out, 32);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ns = (t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec);
    return ns / (44100.0 * 5.0);
}

int main()
{
    printf("8 voices scalar: %6.1f ns/sample\n", bench(false, 8));
    printf("8 voices simd  : %6.1f ns/sample\n", bench(true, 8));
    printf("1 voice  scalar: %6.1f ns/sample\n", bench(false, 1));
    printf("1 voice  simd  : %6.1f ns/sample\n", bench(true, 1));
    return 0;
}
