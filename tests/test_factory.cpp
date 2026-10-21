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

int main()
{
    int fails = 0;
    // ---- A: authoritative miss -> graceful emptiness ----
    setenv("P5_FACTORY", "/nonexistent/muhammad-factory.p5fac", 1);
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

    // ---- B: sidecar present (module-dir candidate, next to this binary) ----
    unsetenv("P5_FACTORY");
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
