/* Renders a multi-preset demo through the engine's Auto kernel path at the
 * plugin's default trim, writing a valid stereo float32 WAV.
 * Timing: one control block = 32 samples = 0.726 ms at 44.1k; B = 450 blocks
 * ~ 0.33 s, so 1*B spacing is eighth notes around 180 BPM. */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "p5_engine.hpp"

struct N { unsigned blk; int kind, note, vel; };   // kind 0 on, 1 off

static void renderWav(const char *path, const std::vector<int32_t> &pcm)
{
    FILE *f = fopen(path, "wb");
    uint32_t dataBytes = (uint32_t)pcm.size() * 8, riff = 36 + dataBytes, fmtsz = 16;
    uint32_t rate = 44100, byteRate = rate * 8;
    uint16_t fmt = 3, ch = 2, block = 8, bits = 32;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtsz, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&block, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    const float trim = 0.5f * (1.0f / 32768.0f);
    for (int32_t v : pcm) { float x = (float)v * trim; fwrite(&x, 4, 1, f); fwrite(&x, 4, 1, f); }
    fclose(f);
}

int main()
{
    const unsigned B = 450;                    // one eighth
    p5::Engine e(p5::KernelMode::Auto);
    e.setPolyphony(8);                         // the PC build's dual-core-equivalent cap
    e.activate(44100);
    std::vector<int32_t> pcm;
    int32_t out[32];

    auto section = [&](uint32_t preset, const N *script, size_t n, unsigned blocks) {
        if (preset == 0) { p5::Patch p; p5::patchInit(p); e.setPatch(p); }
        else e.loadFactory(preset - 1);
        size_t ev = 0;
        for (unsigned b = 0; b < blocks; b++) {
            while (ev < n && script[ev].blk == b) {
                if (script[ev].kind == 0) e.noteOn(script[ev].note, script[ev].vel);
                else e.noteOff(script[ev].note);
                ev++;
            }
            e.process(out, 32);
            for (int i = 0; i < 32; i++) pcm.push_back(out[i]);
        }
    };

    // 1) INIT PROPHET: stacked chord, release, accented re-press (the A/B script)
    static const N s1[] = {
        {  0*B,0,60,100},{  2*B,0,64,100},{  4*B,0,67,120},{  6*B,1,60},
        {  8*B,0,55, 80},{ 10*B,1,64},{ 12*B,0,60,111},{ 16*B,1,67},
        { 18*B,1,55},{ 20*B,1,60} };
    section(0, s1, 10, 30 * B);
    // 2) "It's a Prophet 5" (factory 0): held pad chord
    static const N s2[] = {
        {  0*B,0,45,110},{  0*B,0,52,100},{  1*B,0,57, 96},{  1*B,0,64,110},
        {  2*B,0,69,100},{ 12*B,1,45},{ 12*B,1,52},{ 13*B,1,57},{ 13*B,1,64},
        { 14*B,1,69} };
    section(1, s2, 10, 24 * B);
    // 3) "Forrevrer Keys" (factory 2): the arpeggio line
    static const N s3[] = {
        {  0*B,0,60,100},{  1*B,0,67,100},{  2*B,0,72,100},{  3*B,0,67,100},
        {  4*B,0,64,100},{  5*B,0,71,100},{  6*B,0,76,100},{  7*B,0,71,100},
        {  8*B,0,60,100},{  9*B,0,67,100},{ 10*B,0,72,100},{ 11*B,0,79,100},
        { 12*B,1,60},{ 13*B,1,67},{ 14*B,1,72},{ 15*B,1,79} };
    section(3, s3, 16, 24 * B);
    // 4) "Pickle Pincher" (factory 32): the filter-bent bass
    static const N s4[] = {
        {  0*B,0,36,127},{  2*B,0,36,127},{  4*B,0,41,127},{  6*B,0,36,127},
        {  8*B,0,39,127},{ 10*B,0,43,127},{ 12*B,0,36,127},{ 14*B,1,36},
        { 14*B,1,39},{ 14*B,1,41},{ 14*B,1,43} };
    section(33, s4, 11, 26 * B);

    renderWav("prophet-demo.wav", pcm);
    printf("prophet-demo.wav: %.1f s, %zu mono frames\n", pcm.size() / 44100.0, pcm.size());
    return 0;
}
