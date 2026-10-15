# Prophet — the Melodee Prophet-5 engine as a CLAP plugin

A port of the **PROPHET** engine from the [Melodee](https://github.com/keremimo/melodee)
firmware (`eng_prophet_test.c`, the Rev-4 wire-format Prophet-5) to PC-like
platforms, packaged as a [CLAP](https://clap.audio) plugin. The per-sample
kernel processes the **eight voices as the eight lanes of a float32x8 vector**
through [SIMDe](https://github.com/simd-everywhere/simde) — the same portable
vectorization layer Surge uses — so one source emits AVX2 on x86, NEON on ARM,
and SSE2 as the fallback.

## Verification (the point of the exercise)

    tests/test_engine_ab: 5 scripted scenarios (poly chords, releases with
    re-presses, self-oscillation, the factory bank) rendered three ways:

      * the melodee firmware's own hostsim (built in-tree via tests/ref_dump.c)
      * the ported scalar reference kernel
      * the SIMDe wide kernel

    Result: **bit-exact at all three, every sample** — the SIMD path equals the
    scalar port equals the firmware, at 44.1 kHz. (SIMDe maps every float op to
    the exact SSE2/AVX2 instruction; `-ffp-contract=off` forbids FMA fusion;
    the BLEP saw uses correctly-rounded uint32→float via a 16+16-bit split.)

    Machine: x86-64 with AVX2, gcc 13, -O3.

    tests/clap_smoke.cpp dlopens the built prophet.clap and drives the CLAP
    surface: factory, extensions, process, params, preset-load, state round-trip.

## Layout

    src/p5_engine.hpp/.cpp      the control path, transcribed from voice.c
                                (allocation, envelopes, block prep, p5_post)
    src/p5_kernel_scalar.cpp    bit-exact reference kernel (p5_samples)
    src/p5_kernel_simd.cpp      the wide kernel: eight voices, one float32x8
    src/p5_math.hpp             the firmware's float helpers (clamp/abs/BLEP...)
    src/p5_tables.h             DSP tables generated verbatim from melodee's
                                build/gen headers (tools/gen_tables.sh)
    src/p5_common.hpp           the 133-byte wire patch + SyX frame codec
    src/prophet_plugin.*        the CLAP plugin (params 1:1 with the wire
                                patch bytes, state, factory preset-load)
    src/p5_prophet_discovery.*  preset-discovery provider: INIT + the 200
                                Sequential v1.03 programs, built in
    external/simde/             SIMDe 0.8.2 amalgamated (vendored, MIT)
    vendor/clap/                the CLAP SDK headers as vendored by the
                                vasynth reference
    tests/scenario.h            the shared note scripts

## Build

    cmake -B build -DMELODEE_DIR=/path/to/melodee
    cmake --build build -j
    build/prophet.clap                    # the plugin (a single shared object)
    build/test_engine_ab build/ref.bin    # the A/B, after ./build/ref_dump > build/ref.bin
    build/clap_smoke build/prophet.clap
    build/bench_engine

`MELODEE_DIR` only gates `ref_dump` (the firmware cross-check); the plugin and
its A/B build standalone.

## Design notes

* **Voices wide, samples serial.** The engine keeps one float32x8 register
  per value: each lane is a voice slot, hot state (phases, filter z, noise,
  triangle, sync tail) lives struct-of-arrays. Table lookups spill the lane
  indices to memory (L1-hot tables) rather than needing gather instructions,
  which keeps NEON/SSE2 fast.
* **Uniform branching.** Patch flags (wave shapes, sync, poly-mod routes) are
  part-wide, so they branch the whole vector identically instead of selecting.
* **Adaptive dispatch** (Auto mode): below two live voices the scalar kernel
  runs — the wide kernel's fixed cost dominates for a lone note. Both kernels
  are bit-identical (verified), so switching mid-stream is inaudible.
* 8 voices by default (the firmware's dual-core Prophet cap); `setPolyphony(5)`
  reproduces the single-core chip's five.
* Sample-rate independence: phase increments, envelope steps, the LFO and the
  DC coupling capacitor scale by 44100/sr; everything is exact at 44.1k.
* **The >110-velocity filter accent is a no-op on this engine** — the firmware
  computes `m.cutoff += (m.envq15*24)>>7` while `envq15` is still the forced-
  zero common-envelope value (p5 overwrites it *after* the accent line, in
  track_render_audio). The port transcribes that order exactly.
* Mono/UNISON/MONO/LEGATO modes come from the patch's UNISON byte, and the
  unison phase-seed LCG is the plugin's own counter (the firmware's is a
  process-static; a fresh engine restarts the sequence, the firmware's does not).
* Output: mono, copied to both ports; default trim 0.7 (a stacked full-velocity
  chord peaks near 1.8 raw), param 80.

## License

GPL-3.0-only, same as the melodee firmware (the engine files are a derivative
work of `firmware/src/eng_prophet_test.c`, `voice.c` and `prophet_patch.c`,
Copyright Leo Kuroshita / Hügelton Instruments). SIMDe is MIT (vendored under
external/), the CLAP headers are their own BSD-style license.

## References

* melodee firmware: `eng_prophet_test.c` — the engine; `voice.c` — the render
  path; `prophet_patch.c` — the wire format.
* The CLAP plumbing follows the vasynth recovered reference
  (`/home/user/code/c/vasynth-workspace-34307107-.../vasynth/`): single shared
  object as `.clap`, entry → plugin factory + preset-discovery factory,
  the same event-dispatch shape.
