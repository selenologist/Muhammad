# Muhammad

*A P5 clone — the PROPHET engine ported from the Melodee firmware to CLAP.*

A port of the **PROPHET** engine from [Melodee](https://keremimo.github.io/melodee/)
(`eng_prophet_test.c`, bespoke to Melodee) to PC-like platforms, packaged as a [CLAP](https://clap.audio) plugin. The per-sample
kernel processes the **eight voices as the eight lanes of a float32x8 vector**
through [SIMDe](https://github.com/simd-everywhere/simde) — the same portable
vectorization layer Surge uses — so one source emits AVX2 on x86, NEON on ARM,
and SSE2 as the fallback.

## Attribution & licensing

- Engine port: **GPL-3.0-only**, from the Melodee firmware
  (`eng_prophet_test.c`, `voice.c`, `dsp.c`, `prophet_patch.c`). The PROPHET
  engine is bespoke Melodee work; Melodee's license text is inherited verbatim
  in `LICENSE`. Author of the upstream engine work: **keremimo** (Melodee).
- Factory patch data (`p5_tables.h`, 200 programs): **Sequential's** 2025 v1.03
  Prophet-5 Rev-4 SysEx program files, carried as raw wire bytes unmodified —
  they are *not* part of the engine's authorship claim.
- "Prophet-5" / "PROPHET" appear only as statements of fact about wire formats
  and the upstream engine. This plugin is **Muhammad**; it is not a Prophet
  product and is not affiliated with Sequential.
- This port: **selenologist**. Plugin identifier: `muhammad.unresolvable`
  (no domain is claimed; CLAP identifiers require uniqueness, not
  resolvability).

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

    The same scripts also run 44.1k/48k × 1x/2x × scalar/wide: every pairing
    is bit-identical between the two kernels (the 2x path can only be
    kernel-vs-kernel checked — the firmware has no 48k reference).

    Width independence: rebuilt with -DSIMDE_NO_NATIVE (the portable
    two-128-bit decomposition of every __m256 op — structurally what NEON
    does on ARM) the whole suite is still bit-identical.

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
    build/muhammad.clap                    # the plugin (a single shared object)
    build/test_engine_ab build/ref.bin    # the A/B, after ./build/ref_dump > build/ref.bin
    build/clap_smoke build/muhammad.clap
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
* Sample-rate independence: oscillator increments, envelope steps, the LFO,
  glide, and the DC coupling capacitor all scale by 44100/sr; the filter
  cutoff table is rebuilt for the host rate with the generator's own formula
  (`g = tan(pi fc/fs)/(1+...)` inverted and re-derived per index), so knob
  positions keep their Hertz at any sample rate. At exactly 44.1 kHz/1x the
  dumped firmware table is used verbatim, keeping the bit-exact path intact.
  (One benign leftover: the bend slew and per-block noise wheel advance per
  control block, so their rates track the host clock ~8% at 48k.)
* **2x oversampling toggle** (param 81 "OVERSAMPLE 2X"): the firmware's own
  P5_OVERSAMPLE=2 design — oscillators/noise/filter integrate at twice the
  host rate with halved increments, the CTL-wheel covers 2 entries per
  sub-sample, the cutoff table rebuilds at the internal rate — but each
  voice's 2x stream is decimated by the **Surge XT cascaded-allpass halfband**
  (M=6 soft, the same coefficients vasynth uses; ~82 dB into the stopband)
  instead of the firmware's 2-tap box average. Not bit-exact by design (the
  decimator IS the difference); the 1x path remains bit-exact. Reports 3
  samples via the CLAP latency extension; state blob v2 carries the flag.
  Cost: 8 voices at 690 ns/sample (~3% of one core at 44.1k), and measured
  on hard-sync saws it cleans the inter-harmonic alias floor from -74 dB to
  -78 dB — modest because the firmware's BLEP already does the heavy lifting.
* **The >110-velocity filter accent is a no-op on this engine** — the firmware
  computes `m.cutoff += (m.envq15*24)>>7` while `envq15` is still the forced-
  zero common-envelope value (p5 overwrites it *after* the accent line, in
  track_render_audio). The port transcribes that order exactly.
* Mono/UNISON/MONO/LEGATO modes come from the patch's UNISON byte, and the
  unison phase-seed LCG is the plugin's own counter (the firmware's is a
  process-static; a fresh engine restarts the sequence, the firmware's does not).
* Output: mono, copied to both ports; default trim 0.5 (a stacked full-velocity
  chord peaks near 1.7 raw, so the port stays inside 0 dBFS), param 80.

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

## GUI (Linux-first NanoVG panel)

`prophet5_panel_gen.py` emits **both** `prophet5_panel.svg` (the design doc) and
`src/panel_layout.h` (the machine layout) from one set of emitter calls — they
cannot drift. The renderer (`src/p5_gui.cpp`) is a pure function of
(layout, state): the embedded plugin and the headless golden test call the
same `p5ui::draw()`.

- Backend: vendored [NanoVG](https://github.com/memononen/nanovg) at
  `external/nanovg` (pinned `ce3bf745`), GL3 via Mesa's `glext` prototypes —
  no loader library. Unmaintained upstream is fine: pinned + self-maintained.
- Embedding: `src/gui_x11.cpp` implements `clap-gui` for X11 (physical px,
  aspect-preserved resize 1560:416), reparents a GLX child window into the
  host, and pumps via `clap.timer-support` (16 ms) + X events. Mouse
  gestures go through `Plugin::guiSet` → drained into the host's output
  event queue at the next `params.flush`, so automation stays authoritative.
- **Pitch knob ranges match the engine's law**, not the raw byte: OSC A/B
  FREQUENCY dials span the live 0..96 (keyboard pitch at raw 24 = pointer at
  ~25% — matching the firmware's `semis = raw/2 − 12`; raws above 96 are dead
  on the engine, so they read back clamped), OSC B FINE spans 0..127 (0..+100
  cents). No more right-half-dead knobs.
- Programmer display: **VT323** (SIL OFL retro-terminal font, embedded) —
  patch name + bank-program, auto-shrinking to fit the (now larger) screen.
- Programmer row shifted right; the box starts after the new UNISON COUNT
  knob; program buttons spread into the freed space. The whole window is
  painted with the panel colour, so a host-forced tall container shows no
  black letterbox bars.
- Programmer knobs: `MASTER TUNE` (id 82, cents; applied on the control path,
  0 keeps bit-exact firmware behaviour), `PROGRAM` (83, writes load factory
  patches and sync all params to the host), `BANK` (84, BANK SELECT cycles).
  The patch-name display reads the live patch bytes.
- Panel font (Liberation Sans Narrow Bold, OFL) is **embedded** at build-time
  via `tools/embed_font.py` — no runtime font lookup, deterministic goldens.

**Golden test:** `tools/check_goldens.sh` renders `init/pad/bass` through
EGL (surfaceless → device → display fallback) and pixel-compares against
`tests/golden/*.png` (≤0.5 % of pixels beyond 2 % fuzz). It runs headless.

Other platforms: Windows (`win32` HWND reparenting) is next when a display is
available; macOS would add an NSView+GL3 shim (~150 lines, CI-built). The
draw code is platform-neutral; only the embedding file changes.

Disable with `-DPROPHET_GUI=OFF` for headless engine-only builds.

## Patch store & retrieval

Patches really live as files: `~/Documents/Muhammad Synth/<NAME>.muhammad`
(133 raw wire bytes; name inside). The PROGRAMMER grid is only a retrieval map:

- **RECORD** (orange) → in-window text-entry modal → saves the current panel as
  `<NAME>.muhammad`.
- **PRESET** (programmer) → modal assigns the current bank/program slot to a
  user patch by name, or `FAC:<n>` / `<n>` for a factory program. Persisted in
  `bank-program.map`. Program buttons 1–8 and MIDI resolve through this map:
  **PC** = `bank*8 + pc` (pc<8) or flat 0..199; **CC0** moves the bank cursor.
- **PRESET (far left)** recalls the loaded source and un-floats the panel —
  the original P5 button's function; its LED is lit while the panel floats
  away from the loaded patch.
- clap-preset-discovery reports factory programs *and* the user folder, so
  Bitwig's browser sees both. `P5_STORE_DIR` env overrides the directory
  (tests/CI).

New panel knobs: VEL>AMP (left of VOLUME), VEL>FLT (filter top row). The filter
KEYBOARD switch is now a 3-detent KEYBD knob (0..2 = OFF/HALF/FULL — the engine
quantises there), the LFO box is VIBRATO with its SAW/TRI/PULSE SHAPE buttons
(the engine's waveform law is exactly these three flags averaged), and every
one of raw params 0..56 now has a panel control. Host-only by necessity:
PRESS>× need aftertouch, LFO INIT is wheel-state, BEND RANGE/PRIORITY were
panel globals on the original, EDIT overlays ride params 64..71.

## Preset picker & recall (v1.3-picker)

- **PRESET (programmer)** opens a mouse picker — FACTORY | USER tabs, paged
  rows (default INIT, bare INIT, then all 200 Sequential programs / your saved
  files). **Clicking a row body = load only** — quickload slots untouched,
  picker stays open for auditioning (X / click outside closes). **Clicking a
  row's ASSIGN chip** arms a bind, closes the picker and shows "ASSIGN ARMED -
  PRESS PROGRAM 1..8" under the readout; the next quickload button binds that
  preset to the slot (and loads it). Pressing the same chip again cancels. The
  row matching the loaded patch is highlighted.
- **Bank display = binding truth.** The digits show the quickload slot the
  current patch is *bound to* (reverse lookup through the retrieval map,
  falling back to a factory patch's implicit default slot) — never an index
  into the factory list. Remap a slot and the readout follows; a user patch
  shows its bound slot too; an unbound patch shows none. The lit program
  button comes from the same lookup. "(pending bank)" renders in a fixed LEFT
  column so the digits never shift.
- **BANK** steps forward on click; **SHIFT+click or right-click steps down**.
- Picker is space-filling: 7 fat rows, 30px faux-bold VT323 names, 16px tabs,
  14px header/page text, full-size ASSIGN chips — legible even in a 1052x280
  Bitwig strip (the panel labels themselves stay panel-sized: no room there).
- **PRESET (far left)** recalls the loaded patch; press it again — having
  touched nothing else — and your floating edits come back.

- **Bare INIT**: picker row 2 ("INIT (bare patch)"), or a Bitwig *browser*
  entry named INIT (preset-discovery lets the DAW's own browser list our
  factory bank; the panel picker above is the in-plugin equivalent), or
  assigning the text `INIT` to a slot.
- **"Peace Be Upon Him" is a slot, not a fixed sound.** By default it is the
  bare INIT content. RECORD it under exactly that name and the file
  `Documents/Muhammad Synth/Peace Be Upon Him.muhammad` overrides the seed
  everywhere: fresh instances, the picker row, preset key 0. Re-record or
  delete the file to retarget it.
- **KEYBD** (filter key tracking) is a 3-state button: LED off = OFF,
  amber = HALF (x sqrt2/oct), red = FULL (x2/oct) — matching the engine's
  quantisation exactly.

## Voice trim, DRIFT, and Rev4 MIDI CC (v1.11)

**EDIT -> V/OCT TRIM page.** The EDIT button opens a modal calibration page:
**per oscillator** — 8 voice rows x 4 rails (A SCALE, A OFFSET, B SCALE,
B OFFSET): slope error pivoted at C4 and 1/16-semitone offset, exactly the
trimmers under an analog lid. Trims apply at each oscillator's increment read,
AFTER the shared glide/CV bus — the hardware topology, so per-osc detune and
glide interact the real way. Persistence: state blob v6 = the DAW PROJECT
(the song carries the instance's calibration); they are deliberately NOT
patch bytes — switching presets never re-trims the machine. v5 blobs expand
the old one-per-voice value to both oscillators. Neutral = 64 = bit-identical.
Drift and trim are live: held notes keep re-shaping, no retrigger needed.

**VINTAGE is now DRIFT** (param 86, depth 0..127): per-voice leaky random
walks (OU processes, ~1.5 s tau) wander pitch (A and B oppositely), cutoff,
and amplitude — continuous across notes, like a hot board. The firmware's old
static per-note roulette is retired, and raw byte 37 is left opaque:
Sequential's factory wire files own that slot (137 of 200 carry non-zero
data), so Muhammad never reads it any more. Measured cost of max drift:
+1.1% (wide 8 voices), within noise (1 v) — block-rate arithmetic, ~0.15
ops/sample. (First cut had a DC-biased noise draw that parked the walk at
about -110 cents — a "fixed detune", caught by the unit suite; the drift
checks now assert unbiasedness directly from early-vs-neutral pitch.)

**Rev4-compatible MIDI CC** (per Sequential's Prophet-5 MIDI Implementation
1.4 — its parameter numbering is identical to the wire enum): CC1 mod wheel,
CC7 volume * CC13 expression (multiply into output), CC32 bank, CC64 damper
(notes released under the pedal hold until it lifts; choke bypasses), CC74
brightness (added to cutoff), CC85 vintage->DRIFT, CC120/123 panic, CC121
reset, and the full CC->parameter map from the chart (CC3 OSC A FREQ, CC26
GLIDE, CC59/60/61/62/63 poly-mod, CC70 bend range, CC71 retrigger, CC73
cutoff, CC103-110 envelopes, CC112-114 unison, CC116-119 wave switches,
...). Unassigned CCs are ignored. NRPN uses the same numbers, if wanted.

## Measurements log

* v1.11: trim offset +16 measured +1.009 semitones; drift depth 127 walks
  audibly (engine determinism preserved: same seed -> same render); A/B stays
  bit-exact (reference regenerated with byte 37 excepted); damper hold/decay
  asserted in clap_smoke (`held 0.1.. / lift < 0.001`).
