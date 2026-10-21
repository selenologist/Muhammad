// SPDX-License-Identifier: GPL-3.0-only
// The GPL-purity contract, asserted both ways IN ONE PROCESS:
//  A) STRIPPED (what a distributor ships when they delete the sidecar):
//     P5_FACTORY pointing at nothing is AUTHORITATIVE - no fallbacks - so
//     count()==0, every factory load degrades to the GPL boot default, and
//     nothing crashes. Runs FIRST because load() is idempotent-once-loaded.
//  B) PRESENT: the sidecar beside this test binary (CMake copies it next to
//     the build outputs) loads all 200 wire programs, names come from the
//     data itself, and Engine::loadFactory reproduces program bytes exactly.
#include "p5_engine.hpp"
#include "p5_factory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// env control with a minGW shim: no setenv/unsetenv there. An EMPTY value is
// equivalent by contract - p5fac::load() treats set-but-empty as unset on
// every platform.
#ifdef _WIN32
static void setEnv(const char *k, const char *v) { _putenv_s(k, v); }
static void clrEnv(const char *k)                 { _putenv_s(k, ""); }
#else
static void setEnv(const char *k, const char *v)  { setenv(k, v, 1); }
static void clrEnv(const char *k)                 { unsetenv(k); }
#endif

static void writeBlob(const char *path, uint32_t rec, uint32_t n,
                      long payload_bytes, const char *magic, uint8_t fill)
{   FILE *f = fopen(path, "wb");
    fwrite(magic, 1, 4, f);
    fwrite(&rec, 4, 1, f); fwrite(&n, 4, 1, f);
    for (long i = 0; i < payload_bytes; i++) fputc((int)fill, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    int fails = 0;
    const int hostile = argc > 1 && !std::strcmp(argv[1], "hostile");
    // ---- A: authoritative miss -> graceful emptiness ----
    setEnv("P5_FACTORY", "/nonexistent/muhammad-factory.p5fac");
    if (p5fac::load()) { std::printf("stripped: load() unexpectedly true\n"); fails++; }
    if (p5fac::loaded() || p5fac::count() != 0)
    { std::printf("stripped: count != 0 (%d)\n", p5fac::count()); fails++; }
    {   p5::Engine e;
        e.activate(44100.0);
        e.loadFactory(7);                       // must NOT touch absent storage
        const p5::Patch &p = e.patch();
        p5::Patch init; p5::patchInit(init);
        if (std::memcmp(p.raw, init.raw, 133) != 0)
        { std::printf("stripped: loadFactory(7) is not the boot default\n"); fails++; }
        int32_t out[32];
        for (int i = 0; i < 64; i++) e.process(out, 32);   // silence must be finite, not a crash
        p5::Patch nm; p5::patchInit(nm);
        char name[64];
        p5::Engine::patchNameAt(9, name);       // factory row -> graceful label
        if (!std::strstr(name, "absent")) { std::printf("stripped: name='%s'\n", name); fails++; }
        (void)init;
    }
    std::printf("stripped mode: graceful %s\n", fails ? "FAIL" : "ok");

    if (hostile)
    {   // attacker controls the file: shapes must be REJECTED, content must
        // be inert data. No attacker number ever sizes an allocation or picks
        // a read offset - tryFile reads a fixed 26,612-byte layout into .bss.
        setEnv("P5_FACTORY", "p5fac_probe.tmp");
        struct { const char *magic; uint32_t rec, n; long payload; uint8_t fill; } bad[] = {
            { "P5FD", 133, 200, 133*200, 0xFF },          // wrong magic
            { "P5FC", 133, 200, 133*199, 0 },             // truncated body
            { "P5FC", 133, 0xFFFFFFFFu, 0, 0 },           // absurd count (pre-read reject)
            { "P5FC", 4, 200, 0, 0 },                     // absurd record size
        };
        for (auto &b : bad)
        {   writeBlob("p5fac_probe.tmp", b.rec, b.n, b.payload, b.magic, b.fill);
            if (p5fac::load() || p5fac::count() != 0)
            { std::printf("hostile: shape accepted (%s %u %u)\n", b.magic, b.rec, b.n); fails++; } }
        // fully hostile CONTENT in a VALID shape: every byte 0xFF is outside
        // every wire field's legal set. Loader accepts (data, not code); the
        // engine must treat it like any SysEx byte stream: bounded render.
        writeBlob("p5fac_probe.tmp", 133, 200, 133*200, "P5FC", 0xFF);
        if (!p5fac::load() || p5fac::count() != 200)
        { std::printf("hostile: 0xFF-shaped blob not accepted\n"); return 1; }
        {   p5::Engine e; e.activate(44100.0);
            for (uint32_t prog = 0; prog < 200; prog++)
            {   e.loadFactory(prog);
                e.noteOn(36, 127, 0); e.noteOn(60, 100, 0); e.noteOn(95, 127, 0);
                int32_t out[32];
                for (int i = 0; i < 1024; i++)
                {   e.process(out, 32);
                    for (int j = 0; j < 32; j++)
                        if (out[j] < -200000000 || out[j] > 200000000)
                        { std::printf("hostile: prog %u sample %+d runaway\n", prog, out[j]);
                          prog = 200; break; } }
                e.noteOff(36); e.noteOff(60); e.noteOff(95); } }
        {   char nm[64]; bool over = false;
            for (uint32_t i = 1; i <= 200; i++)
            {   p5::Engine::patchNameAt(i, nm);
                if (std::strlen(nm) > 20) { std::printf("hostile: name overflow @%u\n", i); over = true; break; } }
            if (over) fails++;
            p5::Engine::patchNameAt(1, nm);
            if (nm[0] != '\0') { std::printf("hostile: 0xFF name not all-space ('%s')\n", nm); fails++; } }
        remove("p5fac_probe.tmp");
        std::printf("hostile blob: %s\n", fails ? "FAILED" : "rejected shapes, inert content");
        return fails ? 1 : 0;
    }

    // ---- B: sidecar present (module-dir candidate, next to this binary) ----
    clrEnv("P5_FACTORY");
    if (!p5fac::load() || p5fac::count() != 200)
    {   std::printf("present: sidecar not found beside the test binary (build-dir copy missing?)\n");
        return 1; }
    {   const uint8_t *r0 = p5fac::raw(0);
        if (!(r0[0] == 0x19 && r0[1] == 0x19 && r0[2] == 0x18))
        { std::printf("present: program 0 wire head wrong\n"); fails++; }
        char name[64];
        p5::Engine::patchNameAt(1, name);       // factory index 0 -> row 1 (row 0 is INIT)
        if (std::strcmp(name, "It's a Prophet 5") != 0)
        { std::printf("present: program 0 name='%s'\n", name); fails++; }
        p5::Engine e; e.activate(44100.0);
        e.loadFactory(4);
        const p5::Patch &p = e.patch();
        if (std::memcmp(p.raw, p5fac::raw(4), 133) != 0)
        { std::printf("present: loadFactory(4) byte mismatch\n"); fails++; }
        int32_t out[32];
        for (int i = 0; i < 2048; i++) e.process(out, 32);   // and it actually renders
    }
    std::printf("factory sidecar: %s (%s)\n", fails ? "FAILED" : "all ok", p5fac::pathUsed());
    return fails ? 1 : 0;
}
