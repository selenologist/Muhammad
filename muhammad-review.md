---
title: "Muhammad 0.2.3 — Code Review"
subtitle: "Safety, architecture, functional correctness and project overview"
date: "2026-10-20"
---

# About this review

| | |
|---|---|
| **Project** | Muhammad — polyphonic subtractive CLAP synth (Prophet-style engine ported from the Melodee firmware, 8 voices as the 8 lanes of a SIMDe float32x8) |
| **Revision** | `main` @ `9e29269` (version 0.2.3) |
| **Method** | Read-only. Nothing was compiled or run. Four parallel reviewers each took one area: engine/DSP, CLAP plugin + state + presets, GUI, and build/CI/tests. Each reviewer traced its findings through the code. The lead reviewer then re-read the source for every Critical finding and most High ones (marked **✔**) before including them. |
| **Line numbers** | Anchored to `9e29269`; they will drift. |

## The request

The request that produced this review, quoted as given apart from the redaction:

> This is an audio plug in / synthesizer a collaborator is writing. I don't want to compile it currently but would like to do a top to bottom review for
>
> 1. Safety / Gross errors / etc.. (like out of bounds reads and writes)
> 2. Architectural decisions and problems (like audio thread allocations)
> 3. functional bugs (like preset loads or param syncs can fail)
> 4. General 'overview'
>
> Feel free to spin a few agents. Final result will be a PDF and Markdown of the review results I can share. Feel free to use the skills I've built for other synths to execute the review and my memories from other projects, but don't mention the projects explicitly unless they are remoted surge synth team or baconpaul ones (so like 'six sines' and 'shortcircuit' yes but 'redacted' no). Share this prompt in the resulting document but replace 'redacted' with redacted when you do

## Severity and confidence

- **Critical**: loses or corrupts the user's sound or project in ordinary use, or crashes or hangs a host.
- **High**: likely to be hit in real hosts, or a real-time-safety or thread-safety violation of the CLAP contract.
- **Medium**: wrong behaviour in less common paths, or a robustness gap on hostile or unusual input.
- **Low / Note**: polish, latent hazards, documentation drift.
- **Confirmed**: the failure path was traced end to end in the code.
- **Plausible**: the code path is real, but whether it triggers depends on the host, the platform or memory contents.

---

# 1. Executive summary

This is a careful, ambitious port with an unusually strong determinism test culture. The **DSP core is in good shape**:
- no allocation, locks, logging or I/O on the process path;
- table indexing that was chosen with the edge cases in mind;
- bounded filter states;
- a state blob parser that is allocation-free and bounds-checked.

**Memory safety is not where the risk is.** Across ~8,000 lines of first-party code the reviewers found only a handful of out-of-bounds hazards, all latent or reachable only from unusual inputs.

**The risk is at the boundaries.** Three areas carry it:
1. **The CLAP host contract.** The threading model assigns thread roles by the `active` flag rather than by CLAP's thread annotations. As a result, `params.flush` and `reset`, which hosts call **on the audio thread** while the plugin is active, run main-thread code there. That code takes a mutex, writes main-private state, and becomes a second producer on a single-producer ring. The README's "strict single-writer, verified under ThreadSanitizer" claim does not hold against real hosts. The TSan test calls `flush` from the main thread concurrently with `process()`, a pattern the spec forbids, so it never sees the real one.
2. **Lifecycle and state.** `activate()` and `reset()` wipe state the user considers part of the project: microtuning, bend defaults, per-voice trims, and, with PAN WIDTH on, the output itself. The next save then makes the loss permanent.
3. **Host parameter conventions.** Modulation is applied as an absolute value, there are no gesture events, no parameter defaults, and malformed output events. Hosts will misbehave in ways that look like plugin bugs to users.

## Top items to fix first

| # | Sev | Finding | Where |
|---|---|---|---|
| 1 | Critical ✔ | `params.flush` / `reset` on the audio thread run main-thread code: they block on a mutex, race the GUI as a second SPSC producer, and write main-private state | `muhammad_plugin.cpp:1196-1219, 1003-1007, 409-453` |
| 2 | Critical ✔ | `CLAP_EVENT_PARAM_MOD` is applied as an absolute value. Any host modulator overwrites the patch, and removing it leaves the knob at ~0. PROGRAM, BANK and OS are flagged modulatable too | `muhammad_plugin.cpp:857-861, 1152, 1209` |
| 3 | Critical ✔ | `Engine::activate()` wipes microtuning and bend defaults. A state load before activate, or any re-activate, plays 12-TET while the GUI shows the scale; the next save persists 12-TET | `p5_engine.cpp:127-130` |
| 4 | Critical ✔ | A host `reset()` zeroes every voice's pan gains. With PAN WIDTH > 0 the plugin is **silent** until PAN WIDTH is changed | `p5_engine.cpp:250-253, 641-656` |
| 5 | High ✔ | `reset()` also resets per-voice V/oct trims to neutral and zeroes the knob slew state, so every knob sweeps up from 0 for ~100 ms | `p5_engine.cpp:262, 268` |
| 6 | High ✔ | A saved TRIM above 1.0 reloads as 0, so the session reopens **silent** (the param range is 0..2) | `muhammad_plugin.cpp:1375` |
| 7 | High ✔ | Output param events have `header.size == 0` and note, port, channel and key set to 0 rather than -1. `process()` never writes `out_events`, and no gesture begin/end events are sent | `muhammad_plugin.cpp:1227-1233`, `plugin_process` |
| 8 | High ✔ | The Win32 timer pump drains the **host's** entire thread message queue (`PeekMessage(nullptr…)`) | `gui_win32.cpp:314-316` |
| 9 | High | Uninitialised per-sample SoA rows feed the 8-lane kernel's dead lanes. Heap garbage that is NaN can reach a signed-min tanh index and read `PW_P5_TANH[2^31]` | `p5_engine.hpp:298-301`, `p5_kernel_simd.cpp` |
| 10 | High | The Linux and Windows panels only repaint on input. Host automation, preset loads and MIDI do not show until the user clicks | `gui_x11.cpp:426`, `gui_win32.cpp` |
| 11 | High | Multi-instance and multi-format globals: the X11 error handler and clipboard are per-process, and the ObjC class `P5PanelView` has a fixed name | `gui_x11.cpp:37-43, 445-456`; `gui_mac.mm:27` |
| 12 | High | The wrapper builds (VST3, AU, standalone) drop `-ffp-contract=off`. The Linux `.clap` needs glibc 2.38+. The macOS build has no deployment target | `tools/wrapper/CMakeLists.txt`, `nightly.yml` |

---

# 2. Architectural overview

## 2.1 Engine and DSP

**Signal and control flow.** `plugin_process` drains the main→audio command ring, then walks host events in time order. `renderChunk` renders up to each event in slices of at most 32 samples, calling `Engine::process(int32* out, n)`, and dispatches the event between slices. Inside a slice, every control quantity advances **per sample**, in four layers:

1. **Part-wide knob state.** `fval_` slews per sample. `partSample` writes broadcast rows `ppS_[PP_*][i]` and the envelope-rate rows.
2. **Per-channel bend and the shared LFO/wheel.** Bend history is kept per MIDI channel, which is what makes MPE work. The LFO/wheel signal is computed per sub-sample.
3. **Per-lane control.** Ornstein–Uhlenbeck drift walks, glide, two envelopes, a VCA onset ramp, and the firmware's integer pitch law producing phase increments. These are written as struct-of-arrays columns `[sample][lane]`.
4. **Render kernel.** Either the scalar reference kernel or the SIMD kernel, which puts all 8 voices in one float32x8. Each runs two BLEP oscillators with hard sync and poly-mod, a tanh-table softclip, a 4-pole TPT ladder with algebraic feedback, the VCA and a feedback tap. At 1x the output goes straight to int32. At 2x it is decimated once, over the whole mix, by a 12th-order allpass halfband (the Surge design). An integer ~5 Hz DC-coupling HPF finishes the mix.

**Voices.** There are 8 fixed slots.
- **Poly allocation:** first reuse a voice with the same note and channel, then take the free voice with the nearest last pitch, then steal the oldest released voice, then the oldest held voice.
- **Mono/unison:** an 8-entry note stack with LOW or LAST priority.
- **Voice end:** when the amp envelope falls below about −84 dBFS, followed by a nominal 32-sample fade.

**Kernel dispatch.** "Auto" picks the scalar kernel for a single voice and the wide kernel otherwise. An AVX2 copy of the wide kernel is selected at runtime via `__builtin_cpu_supports`, but only in Linux GUI builds (§6).

## 2.2 Plugin threading model

The design intent, stated in `muhammad_plugin.hpp:120-124`:

- **While inactive**, the main thread owns the engine and every setter applies directly.
- **While active**, the audio thread is the engine's only writer:
  - Main-thread mutations update a main-private `View` optimistically and push a 360-byte `Cmd` into a 256-entry SPSC ring, which `process()` drains at the top of each block.
  - At the end of each block the audio thread `try_lock`s a mutex and copies a full engine snapshot into `pub_`. The main thread reads `pub_` under the same mutex and replays any not-yet-drained ring entries to get a "current" view.
- **MIDI program change** uses a double-buffered slot cache built on the main thread, so the audio thread only `memcpy`s precomputed patch bytes.
- **Identity bookkeeping** (which source is loaded) returns to the main thread through an `idSync_` flag that the GUI tick adopts.

This is sound in outline, and in some ways it is nicer than per-parameter audio→main messaging: CC and automation changes appear in the GUI for free, and `state_save` never reads live audio memory. Compared with the **patch / patchMain** model used in Six Sines and ShortCircuit, three things are weaker:

1. **There is no authoritative main-thread copy of the patch.** Main's truth is "the last snapshot the audio thread managed to publish, plus replayed pending commands". Persistent values that live only inside the engine (tuning, trims, bend defaults) are therefore at the mercy of the engine's own reset paths (§5).
2. **Thread role is decided by `active`, not by CLAP's thread annotations.** CLAP calls `flush` and `reset` on the audio thread while active, so these entry points run "main-thread" code on the audio thread (§4.1).
3. **There are two parallel parameter paths.** `applyParam` (main) and `applyParamNow` (audio, or main while inactive) duplicate logic and have already diverged: DRIFT, and the PROGRAM echo guard.

In the patch/patchMain model, main owns a full copy of persistent state and re-pushes it after any activate or reset. Host-facing main-thread calls read that copy, and the audio thread is the sole reader of its own patch. Moving toward that model would close findings C1, C3, H1, H2, H5 and several Mediums in one structural change.

## 2.3 State and presets

**State format.** A native-endian, fixed-layout binary blob:
- magic `P5CL` and version 11;
- the 134 wire-patch bytes, where bit 7 of five bytes carries the polarity flag;
- overlays, trim, oversampling, tune, program, bank and pan;
- 32 per-osc trims, a 128-entry tuning deviation table and the bend defaults;
- the tuning name, up to 64 scale tones, and root/mask/walk.

The loader steps through v1 to v11 with explicit migrations.

**Presets come from three layers:**
1. 200 embedded Sequential v1.03 programs, plus INIT and an embedded default patch.
2. User `.muhammad` files (8-byte magic plus 134 bytes) in `~/Documents/Muhammad Synth`.
3. A text `bank-program.map` that binds 200 slots to factory or file sources, for the program buttons and MIDI program change.

A preset-discovery provider exposes all of these under one PLUGIN location.

## 2.4 GUI

The GUI has three layers:
- **Core** (`gui_core.*`): platform-neutral and single-threaded. It owns all interaction state.
- **Renderer** (`p5_gui.*` and the generated `panel_layout.h`): a nanovg drawer for a fixed 1560×430 layout. It is driven by a plain `State` struct and has no side effects. The headless golden-image test drives the same `draw()`.
- **Per-OS shims**: X11 with GLX and XEmbed plus an optional floating window, Win32 with WGL, and Cocoa with `NSOpenGLContext`.

There are no GUI threads. Everything runs on the host's main thread via `timer-support` and native event dispatch. The GUI never touches the engine directly: reads come from the published View, and writes are posted to the ring.

## 2.5 Build, test and release

- **Build:** one top-level CMake file that forces `-O3 -ffp-contract=off`.
- **Wrappers:** VST3, AUv2 and standalone builds come from clap-wrapper in "clap-first" mode with the engine compiled in, via `tools/make_wrappers.sh`.
- **CI** (`nightly.yml`):
  - Linux builds and runs 8 test executables plus golden images, and builds the wrappers.
  - A TSan job runs `test_threadsan`.
  - Windows has a MinGW build and an MSVC wrappers job.
  - macOS runs `test_mpe` and `clap_smoke`, then bundles the plugin.
  - A `v*` tag publishes a release.
- **Tests:** hand-rolled executables. There is no ctest, no ASan or UBSan, and no fuzzing. Nothing is code-signed or notarized.

---

# 3. Safety and gross errors

Overall the codebase is **memory-safe in ordinary operation**:
- The state parser, the SysEx codec (`patchDecode`), picker and tuning indexing, the param-id→index maps and the string formatting (`snprintf` with `sizeof` throughout) all held up under end-to-end tracing.
- Table extents were chosen with edge cases in mind: the tanh table has 1025 entries guarded by `<1024`, and the exp2 table has 514 entries to cover `pos == 512` rounding.

The items below are the exceptions.

### S1 — Uninitialised SoA rows in the SIMD kernel's dead lanes · High · Plausible

- **Where:** `p5_engine.hpp:298-301`, `p5_engine.cpp:791-811`, `p5_kernel_simd.cpp:24, 251-253`.
- **Issue:** `fenvS_`, `aenvS_`, `vcaS_`, `fcAddS_`, `iaS_` and `ibS_` have no initialiser. `Plugin` is created with `new Plugin(host)`, which is default-initialisation, so these arrays hold whatever was in the heap. The rows are written only for active lanes. The wide kernel loads all 8 lanes and silences dead lanes by **multiplying** by `laneLive`, and NaN × 0 = NaN.
- **Failure scenario:**
  1. A never-used lane holds a NaN bit pattern.
  2. Its `fbPrev` becomes NaN.
  3. Next sample, `softclipV(NaN)` → `cvtf2u` gives `0x80000000`.
  4. `simde_mm256_min_epi32` is a *signed* min, so the index stays `0x80000000` and the gather reads `PW_P5_TANH[2^31]`. That is a segfault in the host.
  5. Even without the crash, `cvttps(NaN)` gives INT_MIN in the output sum, which is a full-scale burst.
- **Fix:**
  - Value-initialise the rows (`= {}`).
  - Mask dead lanes with a select, not a multiply.
  - Use an unsigned min (`min_epu32`) in `softclipV` so a NaN index clamps to 1023.

### S2 — `loadMap` reads into `char kind[8]` without a width · Medium · Plausible

- **Where:** `p5_store.cpp:170-172`, `while (f >> sl >> kind)`.
- **Issue:** The target builds as C++17, where `operator>>(istream&, char*)` is unbounded. libstdc++ bounds it via `__builtin_object_size`; libc++ in pre-C++20 mode does not.
- **Failure scenario:** A hand-edited or corrupt `bank-program.map` on macOS overflows the stack during `plugin_activate`.
- **Fix:** `std::string kind;`.

### S3 — Exceptions can cross the C ABI · Medium · Plausible

- **Where:** `p5_store.cpp:123-136`, called from the GUI (`gui_core.cpp:266, 384`) and from the preset-discovery `get_metadata`.
- **Issue:** `listPatches` uses a range-for over `directory_iterator`, whose `operator++` throws, and also calls `is_regular_file()` and `path::string()`. On Windows, `path::string()` throws for names the ANSI code page cannot represent, e.g. `Läuft ✓.muhammad`.
- **Failure scenario:** One unusual filename or a permission error terminates the host, from a WndProc or a CLAP callback.
- **Fix:**
  - Use the `error_code` overloads and `increment(ec)`.
  - Use `u8string()`.
  - Wrap every C entry point in `try/catch(...)`.

### S4 — Debug text buffer over-read · Low · Confirmed

- **Where:** `gui_core.cpp:933-940`.
- **Issue:** `n += vsnprintf(...)` can push `n` past `sizeof c.dbg`, and `c.dbg[n-1]` then reads past the end.
- **Fix:** Clamp `n` to `sizeof c.dbg - 1`.

### S5 — X11 clipboard globals point at a closed `Display` · Medium · Confirmed

- **Where:** `gui_x11.cpp:37-43`; teardown at `460-467` never clears them.
- **Issue:** `clip::dpy` and `clip::win` are process-wide, and are set by whichever GUI was created last.
- **Failure scenario:** Close that GUI while another instance is open, then COPY on the debug page, or receive a `SelectionRequest`. The code uses the freed `Display*` and crashes.
- **Fix:** Keep the clipboard state per `Ctx`.

### S6 — X11 error handler protects only the last-created instance · High · Confirmed path

- **Where:** `gui_x11.cpp:445-456, 702-703, 718-720`.
- **Issue:** `gActiveCtx` is whichever instance called `gui_create` last.
- **Failure scenario:**
  1. Open GUI A, then GUI B.
  2. The host destroys A's parent window, then calls `gui_destroy(A)`.
  3. BadWindow and GLXBadDrawable errors chain to the host's handler, often Xlib's default, which calls `exit()`.

  This is the very device-delete crash the handler was written to prevent. Destroying B first restores the host handler and leaves A unprotected.
- **Fix:** Swallow errors for any `Display*` belonging to a live Ctx. Install the handler once, and remove it with the last instance.

### S7 — Fixed Objective-C class name `P5PanelView` · High · Confirmed design

- **Where:** `gui_mac.mm:27`.
- **Issue:** The engine and panel are statically embedded in the CLAP, VST3, AU and standalone builds. A host loading two of them (CLAP + VST3, or two versions) gets "Class P5PanelView is implemented in both…". Which class wins is undefined.
- **Failure scenario:** One module's views run the other module's methods against a different `Ctx` layout. That corrupts memory, or jumps into unmapped code after an unload.
- **Fix:** Give the class a per-build unique name via a macro (version + git hash), or register it at runtime with a random suffix.

### S8 — macOS view keeps a raw `core_` pointer after teardown · Medium · Plausible

- **Where:** `gui_mac.mm:87-101, 369-370`.
- **Issue:** `teardown()` releases the view but never sets `core_ = nullptr`, and the view can outlive its `Ctx`.
- **Failure scenario:** `keyDown` calls `[super keyDown:]`; if the host closes the editor synchronously from that call, `core_->consumeFocus()` is a use-after-free.
- **Fix:** Null `core_` in teardown, and re-check it after the `super` call.

### S9 — Win32: second `SetPixelFormat` on the same window · High · Plausible (driver-dependent)

- **Where:** `gui_win32.cpp:432-451`.
- **Issue:** A window's pixel format can be set only once. When `wglChoosePixelFormatARB` returns a different format index from `ChoosePixelFormat`, the second call fails and the GUI never opens.
- **Fix:** Create the bootstrap context on a hidden dummy window.

### S10 — X11 floating-window failure leaves a null `Display` the timer still uses · Low · Confirmed

- **Where:** `gui_x11.cpp:529-565, 296-300, 317`.
- **Issue:** If `setupFloating` fails, `c.dpy` is null but the timer stays registered.
- **Failure scenario:** The next `pump` crashes in `XGetWindowAttributes`.
- **Fix:** Return early from `pump` when `!c.dpy`.

### S11 — X11 GL teardown order · Low · Confirmed

- **Where:** `gui_x11.cpp:462`.
- **Issue:** `glXMakeCurrent(dpy, None, nullptr)` runs *before* `nvgDeleteGL3`, so nanovg deletes its GL objects with no context current. Some drivers crash on this. The Win32 and mac teardowns get the order right.

### S12 — Engine hazards outside the normal call contract · Low · Confirmed

- **Uninitialised tables before `activate()`:**
  - `tpt_` points at a 128-entry table that `filterCore` indexes up to 159 (`p5_engine.hpp:286`).
  - `rUp_` and `rDn_` are uninitialised.

  Hosts must not process before activate, but call `buildTpt()` in the constructor anyway.
- **Low sample rates:** below about 13 kHz host rate, `(uint32_t)((double)ia * srScale)` overflows, which is UB (`p5_engine.cpp:529`). Clamp it the way `incOf` already does.
- **Silent truncation:** `Engine::process` truncates `n > 32` and leaves the tail unwritten (`:637`). The plugin chops correctly, but the engine API should loop or assert.

### S13 — Smaller input-validation gaps · Low

- **Event space:** `dispatchEvent` never checks `space_id`, so a non-core event whose type collides with a core type is read as the wrong struct.
- **`user:` preset key fallback:** a `user:` key falls back to opening an arbitrary path (`p5_store.cpp:111-112`). The magic check limits the damage; drop the fallback.
- **Stereo assumption:** `renderChunk` writes `data32[1]` without checking `channel_count >= 2`.
- **Masked index:** `applyViewCmd` writes `v.raw[c.i0 & 255]` into a 134-byte array. It is safe only because `postRaw` checks bounds first; use `< RAW_MAX`.

---

# 4. Architecture, threading and real-time safety

**Engine side: solid.** The engine's own process path is clean: nothing reachable from `Engine::process` allocates, locks, logs or makes syscalls. Every SIMD load and store is unaligned-safe. FTZ/DAZ is set at the top of `process()`.

**Plugin side: problems.** Everything in this section is in the plugin layer around the engine.

### 4.1 — C1 · `params.flush` and `reset` run main-thread code on the audio thread · Critical · Confirmed ✔

- **Spec:** `params.flush` is `[active ? audio-thread : main-thread]` (`params.h:303`). `reset` is `[audio-thread & active]` (`plugin.h:89`).
- **What the code does:** while active, both go through the main-thread posting helpers (`muhammad_plugin.cpp:1196-1219`, `1003-1007`). `par_flush` → `applyParam` → `refreshView()` / `postRaw()` / `postCmd()`, and `plugin_reset` → `postReset()` → `postCmd()`.
- **Consequences:**
  - **Blocking lock on the audio thread:** `refreshView()` takes `std::lock_guard<std::mutex> viewMx_`.
  - **Second producer on an SPSC ring:** if the GUI posts at the same moment, both read the same `rw_`, both write `ring_[w]` (a 360-byte struct) and both publish. A command is lost, or worse, a torn `OP_LOAD` payload applies a corrupt patch.
  - **Main-private state written from audio:** `view_`, `pendVal[]`/`pendSet[]`, `program`, `curSrc_`, `srcRaw_`.
  - **Possible audio-thread stall:** if the ring is full, `postCmd` spins and yields for up to 50 ms waiting for itself to drain.
  - **Wrong-thread host call:** `PID_PROGRAM` → `loadProgram` → `request_flush`, which is `[!audio-thread]`.
- **Why the TSan test misses it:** `tests/test_threadsan.cpp:93` calls `flush` from the main thread *concurrently with* `process()`. That is spec-illegal, and it never exercises the legal audio-thread flush.
- **Fix:**
  - While active, `flush` should behave like a mini-`process()`: `dispatchEvent` / `applyParamNow` directly.
  - `reset` should call `silenceReset` directly (after fixing §5).
  - Main-thread view updates should flow audio→main through a second queue.
  - Rewrite the TSan test to call `flush` and `reset` on the audio thread, between `process()` calls.

### 4.2 — H1 · Host notification relies on `flush` alone, and the output events are malformed · High · Confirmed ✔

- **`process()` never writes `out_events`:** `plugin_process` ignores `pr->out_events`; GUI edits drain only in `par_flush`. Per `params.h:56-57, 298-299`, a host that is processing answers `request_flush` with a `process()` call, not `flush()`. Panel edits during playback therefore never reach the host on a spec-following host, and automation recording from the UI doesn't work.
- **No gesture events:** `CLAP_EVENT_PARAM_GESTURE_BEGIN/END` is never emitted (no matches in `src/`). A drag changes the engine locally and sends one value on release. The consequences:
  - touch and latch automation cannot record;
  - host automation fights the user mid-drag;
  - the host's own UI is stale during a drag;
  - host undo is not grouped.
- **Malformed events:** `muhammad_plugin.cpp:1227-1233` builds the output events with:
  - `header.size` left at 0;
  - `note_id`, `port_index`, `channel` and `key` = 0 rather than -1, so each event claims to target note 0 / channel 0 / key 0, not the global parameter;
  - `space_id` never set;
  - the `try_push` result ignored, while `pendSet` is cleared anyway.
- **Changes the host never hears about:** MIDI-CC parameter changes, CC48 trim, audio-thread PROGRAM loads and MIDI program change emit no output events at all, so the host's cached values drift from `get_value`.
- **Fix:**
  - Drain pending GUI values into `out_events` in **both** `process()` and `flush()`, via a lock-free main→audio queue rather than shared arrays.
  - Fill in the events correctly.
  - Wrap drags in gesture begin/end.
  - Tag CC-driven changes with `CLAP_EVENT_DONT_RECORD`.

### 4.3 — H2 · The 50 ms "stalled consumer" fallback writes the engine from main while active · Medium · Confirmed

- **Where:** `muhammad_plugin.cpp:165-170`.
- **Trigger:** a host keeps the plugin active but stops calling `process` (bypass, sleep, a stopped device), and the user keeps editing.
- **Failure scenario:**
  1. After 255 posts the ring is full.
  2. Every further post blocks the GUI thread for 50 ms, then calls `execCmd(copy)` **on main, against the engine, while `active` is true**. When processing resumes on the audio thread, nothing hands the engine back, so the two threads can race.
  3. The newest command is applied first, and the 255 older ones are replayed after it, so the user's final edit is lost.
- **Fix:**
  - Coalesce by parameter id (latest value wins) instead of queueing every motion event.
  - Never touch the engine from main while active.

### 4.4 — Memory ordering and smaller races · Low · Confirmed

- **Ring release ordering:** `drainCmds` publishes `rr_` with `memory_order_relaxed` (`:178`). It must be `release`; otherwise, on ARM (Apple Silicon), the producer can overwrite a slot that is still being read.
- **Stale view:** `refreshView` copies `pub_` and only *then* loads `rr_` (`:136-144`). A command drained but not yet published is in neither the copy nor the replay, so the view is stale for one block. The polarity read-modify-write in `applyParam` (`:420`) can then rebuild bit 7 from that stale value. Record the drain position inside `pub_` at publish time.
- **Slot cache:** the double-buffered slot cache (`:597-618`) has no handshake. Two rebuilds in a row can write the buffer that `applyPcFromCache` is reading.
- **Non-atomic shared int:** `idSyncSlot_` is a plain `int` written by the audio thread and read by main.
- **Wrong-thread host call:** `applyPcFromCache` (`:660`) and the audio PROGRAM path (`:376`) call `request_flush` from the audio thread, which the spec marks `[!audio-thread]`.
- **Stale comments:** the comment on `plugin_start` says "apply residue on MAIN", but `start_processing` runs on the audio thread. The header calls the view a "seqlock"; it is a mutex.

### 4.5 — GUI repaint and CPU cost · High / Medium · Confirmed

- **Linux and Windows never repaint for changes that don't come from the panel (High).** `dirty` is set only by native input or expose events (`gui_x11.cpp:426`, `gui_win32.cpp:279, 380`). The panel stays stale after host automation, `params.flush`, MIDI CC, program change, host preset loads and state load, until the user clicks or resizes.
- **Host sync can wait forever.** `adoptPendingIdentity()` is the host parameter sync after a MIDI program change. It runs only inside `fillState`, i.e. only when painting, and never when the GUI is closed.
- **macOS has the opposite problem (Medium).** It redraws unconditionally every 16 ms with swap interval 1, so `flushBuffer` can block the host's main thread for up to a frame per open instance.
- **Cost of every paint, on every platform:**
  - `fillState` calls `refreshView()` about 120 times; each call locks, copies ~600 bytes and replays the ring.
  - While the USER tab is empty, each paint also does a directory scan.
  - On X11, each 16 ms tick makes a synchronous `XGetWindowAttributes` round-trip.
- **Fix:**
  - A change counter, bumped by `publishView`, flush, posts and identity adoption, so all three platforms repaint only when it moves.
  - One `refreshView()` per frame.
  - Rescan the directory on tab switch, not on every paint.
  - Run `adoptPendingIdentity` from a main-thread timer that is independent of the GUI.

### 4.6 — Win32 timer pump drains the host's message queue · High · Confirmed ✔

- **Where:** `gui_win32.cpp:314-316`: `while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }`.
- **Issue:** This runs inside the host's timer callback, on the host's UI thread. The null HWND pulls **every** message for every window on that thread.
- **Failure scenarios:**
  - **Hang on exit:** `WM_QUIT` is swallowed, so the host can hang on exit.
  - **Re-entrant timer:** the host's own `WM_TIMER` is dispatched from inside our callback, which re-enters our timer.
  - **Broken host input:** host accelerators, modal loops and `IsDialogMessage` handling are bypassed.
- **Fix:** Delete the loop. The child HWND already receives its messages from the host's loop.

### 4.7 — X11 event loop is timer-only · Note

- **README claim** (README:270-273): repaint is "driven by the input event itself".
- **Reality:** X events are read only inside `pump()`, which runs only from the host timer. `posix-fd-support` is not used, so a host that never fires the timer delivers no input to the panel.
- **Fix:** Register the X connection's fd with `clap.posix-fd-support`.

### 4.8 — Plugin resizes the host's own window; float mode is learned globally · Medium · Confirmed

- **Resizing the host's window:** X11 (`gui_x11.cpp:239-287`) and Win32 (`gui_win32.cpp:333-361`) climb to the host's top-level window and resize it, up to 10 times. In a host that docks the editor inside its main window and refuses `request_resize`, the plugin shrinks the host's main window.
- **Float mode learned globally:** `autoLearnFloat()` writes `mode=float` to a shared `gui.conf` after one session in one host. From then on, every host gets a floating window plus an empty embedded slot (`gui_x11.cpp:189-202, 699`). Key the setting by host name.

### 4.9 — Allocation and file I/O on the main thread during activate · Note

- **What happens:** `plugin_activate` does file I/O (`p5store::loadMap()`). That is legal, because activate is a main-thread call.
- **Weakness:** the engine's `activate()` resets persistent state (§5, C3).
- **Not a bug, but:** the audio path is genuinely allocation-free, and MIDI program change is only a precomputed `memcpy`.

---

# 5. Functional bugs

### 5.1 — C2 · Parameter modulation is applied as an absolute value · Critical · Confirmed ✔

- **Where:** `muhammad_plugin.cpp:857-861` (and `1209` in flush):
  ```cpp
  case CLAP_EVENT_PARAM_VALUE:
  case CLAP_EVENT_PARAM_MOD: { auto *e = (const clap_event_param_value *)ev; applyParamNow(s, e->param_id, e->value);
  ```
  `clap_event_param_mod::amount` sits at the same offset as `value`, so a modulation offset becomes the new base value. Every parameter is flagged `CLAP_PARAM_IS_MODULATABLE` (`:1152`).
- **User-visible effect:**
  - In a host with per-parameter modulators (Bitwig, the README's test host), assigning an LFO to CUTOFF slams the cutoff to about 0 plus the LFO amount.
  - Removing the modulator sends `amount = 0`, which leaves the cutoff at 0.
  - The damage is saved with the project.
  - PROGRAM and BANK are modulatable too, so modulation triggers factory loads at audio rate.
- **Fix:** Remove `IS_MODULATABLE` everywhere for now. Later, implement a separate per-parameter modulation offset that is not persisted. Mark PROGRAM, BANK and OS non-automatable as well (see 5.9).

### 5.2 — C3 · `activate()` wipes microtuning and bend defaults; the next save makes it permanent · Critical · Confirmed ✔

- **Where:** `p5_engine.cpp:127-130`:
  ```cpp
  rUpDef_ = rDnDef_ = -1;
  std::memset(tuneDev_, 0, sizeof tuneDev_);
  tuningActive_ = false;
  ```
- **When it happens:** `plugin_activate` calls this on every activation.
  - Hosts commonly load state **before** activate, and `state_load` while inactive writes the tuning straight into the engine. Activate then wipes it.
  - It is wiped again on every later deactivate/activate: a sample-rate change, a buffer change, a device toggle, the restart after toggling 2x.
- **Why it becomes permanent:** `tuneName_` and the scale-wheel tones live in the plugin and survive, so the GUI still shows the scale while 12-TET plays. `state_save` reads the (zeroed) `view_.dev`, so the next save writes 12-TET.
- **Missed by tests:** `clap_smoke` only loads state while already active.
- **Fix:** Keep only rate-dependent state in `Engine::activate()`. Keep tuning, bend defaults and trims in plugin-owned main state, and re-apply them after activate.

### 5.3 — C4 · Host `reset()` silences the plugin when PAN WIDTH > 0 · Critical · Confirmed ✔

- **Where:** `silenceReset()` `memset`s each `Lane` (`p5_engine.cpp:250-253`), which zeroes `panG` (default `{32768, 32768}`, `p5_engine.hpp:206`).
- **Why it stays silent:** `panDirty_` is not set, so `process()` never recomputes the gains (`:641-656`). `panOn_` stays true, and every kernel scales each voice by `panG = 0`. `setPanWidth` early-returns when the width is unchanged, so re-sending the same value does not help.
- **When it happens:** hosts call reset on transport jumps and before offline renders, and AU hosts (through the wrapper) call Reset frequently.
- **Fix:** Set `panDirty_ = true` in `silenceReset()`.

### 5.4 — H3 · `reset()` also wipes V/oct trims and zeroes the knob slews · High · Confirmed ✔

- **Where:** `p5_engine.cpp:262, 268`.
- **Trims:** reset to neutral (64), although they are project state (state blob v6). The View mirrors the engine, so the next save persists the loss.
- **Knob slews:** `fval_[k] = 0` with smoothing on means every knob — cutoff, levels, envelope rates, coarse tuning — slews up from 0 over ~100 ms. The first notes after a transport jump get a filter/level sweep and a semitone glissando.
- **Fix:**
  - Reset only voices, filter, oscillator and DC state.
  - Leave trims alone.
  - Snap `fval_[k] = valueOf(k)`.

### 5.5 — H4 · A session saved with TRIM > 1.0 reopens silent · High · Confirmed ✔

- **Where:** `muhammad_plugin.cpp:1375`: `if (!(trim >= 0.0f && trim <= 1.0f)) trim = 0.0f;`.
- **Issue:** The parameter range is 0..2 (`:1139`), and CC48 can set 2 (`:908`). `p5_common.hpp:105` says 0..1, so three different ranges are in play.
- **Fix:** Clamp to [0, 2], and send NaN to the default 0.5, not to 0.

### 5.6 — H5 · State stream I/O is single-shot and non-transactional · Medium · Confirmed

- **Where:** `muhammad_plugin.cpp:1242-1243, 1275-1276`: `return i->read(i, b, n) == (int64_t)n;`.
- **Single-shot:** `stream.h` explicitly allows short reads and writes and requires the plugin to loop. A host that chunks its streams makes save or load fail.
- **Non-transactional:** a failure midway leaves state half-applied, because tune, program, bank, pan and trims are posted before a later short read returns false.
- **Fix:** Loop the reads and writes. Parse into a temporary struct and apply it only on success.

### 5.7 — Parameter info problems · Medium · Confirmed

- **No defaults:** `par_get_info` `memset`s the struct and never sets `default_value`. Host "reset to default" sends TRIM to 0 (silence), CUTOFF to 0 and coarse tuning to 0 (−12 semitones). UNISON CNT has min 1 and default 0, outside its own range.
- **Duplicate params:** VINTAGE (param 37) and DRIFT (PID 86) write the same wire byte, so automating both fights. After a preset load only ids 0..57 are synced, so DRIFT stays stale.
- **Wrong return:** `par_get_value` returns true for unknown ids.

### 5.8 — Preset loading · Medium · Confirmed

- **Host factory loads skip bookkeeping.** `from_location` with a factory key calls `postLoad` directly (`:1411`) instead of `loadProgram`. As a result:
  - identity (`program`, `programA_`, `curSrc_`, `srcRaw_`) is not updated;
  - no param rescan is sent and `preset_load->loaded()` is not called;
  - `atoi("garbage")` loads program 0 and reports success.

  The host's knobs go stale, PRESET recall returns the previous patch, and the picker highlights the wrong row.
- **User presets are listed by raw filename but loaded by sanitized name** (`p5_store.cpp:63-79, 108-136`). Any file with `( ) , & ' #`, non-ASCII characters, or a stem over 24 characters appears in both the host browser and the USER picker. Clicking it silently loads **INIT** and marks it as the current source.
- **Program echo guard missing after state load.** `state_load` sets `program` but leaves `programA_ = -1` (`:1306, 1369`). A host that replays PROGRAM=17 after loading a session saved on program 17 with edits re-loads factory 17 and destroys the edits.
- **No compat preset-load ID.** `get_extension` does not answer `CLAP_EXT_PRESET_LOAD_COMPAT`, although the discovery factory is exposed under its compat ID. Older hosts can index presets but cannot load them.
- **User files under a PLUGIN location.** Discovery lists user files under a PLUGIN location rather than a FILE location, so hosts can't watch the folder and newly saved patches may not appear until a full re-index.
- **Saving cuts sound.** `savePatchNamed` re-`postLoad`s the whole patch just to set the name (`:715`), which kills every sounding voice.
- **Fix:**
  - Route host factory loads through `loadProgram`, and parse keys with `strtol` plus an end-pointer check.
  - Call `params->rescan(CLAP_PARAM_RESCAN_VALUES)` and `loaded()`.
  - Load user patches by their exact filename inside `baseDir()`; reject separators and `..`, but don't re-sanitize.
  - Set `programA_` in `state_load`.
  - Answer the compat ID.

### 5.9 — Latency handling against the spec · Low-Medium · Confirmed

- **Wrong-time notification:** `latency->changed()` is called whenever the plugin is inactive, including from `state_load`. The spec allows it only during `activate` (`latency.h:21`).
- **Unreported change while active:** toggling 2x oversampling changes `get_latency` without `request_restart()`, so host delay compensation is 3 samples off. `latencyDirty` is set but never used.
- **Fix:** While active, call `request_restart()` and apply the change at the next activate. Make the OS parameter non-automatable.

### 5.10 — Note handling · Medium · Confirmed

- **Wildcard channel:** `channel == -1` is a wildcard in CLAP, but `e->channel & 15` maps it to 15 (`:844, 849`). A wildcard note-off releases only channel-15 voices.
- **Wildcard key:** `key == -1` is ignored, so a host "all notes off" via a wildcard does nothing.
- **Choke:** `NOTE_CHOKE` is a gated release, not a kill.
- **Sustain:**
  - `dispatchNoteOn` never clears `heldFlag_`, so a key re-pressed under the pedal is cut at pedal-up while it is still held.
  - Sustain is global, not per channel (it matters for MPE).
- **All Sound Off:** CC120 only releases notes; it doesn't silence them.
- **Stuck notes across activation:** voices survive deactivate/activate. A key held across a host deactivate with no note-off drones after reactivation.

### 5.11 — Engine behaviour bugs · Medium / Low · Confirmed

| ID | Sev | Issue | Where |
|---|---|---|---|
| E1 | Medium | **The cutoff table goes unstable below 32 kHz at 1x.** `tan(atan(t)·scale)` passes π/2 for fc > sr/2 and turns negative, while the guard only catches large positive values. At 22.05k/24k, indices ≈120-127 get g between about 1.7 and 7, which is unstable, so INIT (CUTOFF 127) oscillates at Nyquist. ✔ | `p5_engine.cpp:146-152` |
| E2 | Medium | **Patch or program change while smoothing is on: the new sound morphs from the old one for ~100 ms.** `setPatch` never snaps `fval_`. A sequenced PC with a note at the same timestamp starts that note with the old cutoff and levels, plus a coarse-pitch glissando. | `p5_engine.cpp:208-222, 681-691` |
| E3 | Medium | **Unison: falling back to a held note drops its velocity to 50% (or 36%).** The fallback re-uses the *scaled* `v.vel` as `mvel`, which drives VEL>AMP and VEL>FLT. | `p5_engine.cpp:410-411, 455-456, 422` |
| E4 | Medium | **Unison level compensation is dead code.** The scaled `v.vel` never reaches audio, so N unison voices sum at full gain (+14-18 dB). The phase seed is applied after `synthNoteOn`, so the first unison note is phase-coherent. | `p5_engine.cpp:410-416` |
| E5 | Low-Med | **Mono/unison: re-striking the same held key doesn't retrigger.** `if (sounding && want == monoNote_) return;` | `p5_engine.cpp:398-400` |
| E6 | Low | **Program change hard-cuts sounding voices in one sample (click).** | `p5_engine.cpp:218-220` |
| E7 | Low | **DC-blocker state is dropped on the block where the last voice dies** (possible click with low-frequency content). | `p5_engine.cpp:840` |
| E8 | Low | **The death fade is not continuous across short chunks,** contrary to its comment. | `p5_engine.cpp:610-621, 761` |
| E9 | Low | **Not sample-rate independent:** drift rate, VCA onset ramp, death fade and glide floor are fixed per sample (2× faster at 96k, 4× at 192k). | `p5_engine.cpp:593, 618, 754-755, 794` |
| E10 | Low | **Bend target goes stale when the bend range changes while bent.** | `p5_engine.cpp:705-711` |
| E11 | Low | **Real-time controllers are not smoothed:** aftertouch, brightness, mod wheel, FEEDBACK and polarity flips step per message (zipper noise). | various |

### 5.12 — GUI functional bugs · Medium / Low · Confirmed

- **Right-click polarity is missing on macOS (Medium ✔).** `rightMouseDown` calls `onPointerPress(…P5_MOD_RIGHT…)` and never `onRightPress` (`gui_mac.mm:325-331`). The README's headline feature does not exist on mac. Ctrl-click isn't mapped either.
- **Right-click flips polarity on knobs hidden under an overlay (Medium).** `onRightPress` hit-tests with no `c.modal` check (`gui_core.cpp:442-455`). On the trim page, a right-click meant to centre a trim silently inverts the FIL ENV or FEEDBACK knob underneath it.
- **Panel edits never mark host state dirty (Medium-Low).** `clap_host_state.mark_dirty` is never called, so hosts don't prompt to save after polarity, trim, tuning or GUI preset changes.
- **A lost mouse-up leaves a "hover drag" (Low-Med).** `WM_CAPTURECHANGED` is not handled, so after Alt-Tab mid-drag the knob follows plain mouse motion, and the final host value is never sent.
- **macOS trackpad scrolling (Low-Med).** `deltaY == 0` is treated as "up", so horizontal swipes and momentum events change values, and a flick pins knobs at max (`gui_mac.mm:337`).
- **Keyboard (Low):**
  - `onChar` accepts characters in non-text modals, so Space (the host's play key) is swallowed.
  - Win32 returns 0 for every `WM_KEYDOWN`.
  - `WM_CHAR` is narrowed with `(char)ch`, which mangles non-ASCII characters.
- **Smaller items (Low):**
  - `GetTickCount` is cast to a 32-bit `long`, which stalls the throttles after 24.8 days of uptime.
  - The picker's ">" button doesn't clamp the page.
  - Pressing Shift mid-drag jumps the knob.
  - There is no double-click-to-default on knobs.
  - GUI preset loads don't mark PROGRAM/BANK pending or call `preset_load->loaded()`.
  - The macOS view doesn't set `wantsBestResolutionOpenGLSurface`, which may draw a quarter-size panel on Retina (Plausible).

---

# 6. Build, CI, tests and repo hygiene

### High

- **B1 · The wrapper builds drop `-ffp-contract=off` (Confirmed).** `CMakeLists.txt:26-36` sets the flag via `CMAKE_CXX_FLAGS` in the top directory's scope only. `tools/wrapper/CMakeLists.txt:96-130` recompiles the engine (including an `-mavx2 -mfma` copy) without it. GCC defaults to `fp-contract=fast` and AppleClang to `on`, so the VST3, AU and standalone builds fuse FMAs in one kernel but not the other. The kernels stop being bit-identical, which undercuts the "switching kernels mid-stream is inaudible" argument. No test runs against a wrapper build. Fix: `target_compile_options` on the engine targets, or an engine OBJECT/INTERFACE library that carries its own flags.
- **B2 · The Linux `.clap` is built on ubuntu-24.04 (Plausible, high confidence).** It needs glibc 2.38+ (`__isoc23_*` symbols from `sscanf`/`strtol`) and GCC 13's libstdc++, so it won't load on Ubuntu 22.04, Debian 12 or RHEL 9. Fix: build in an older container, link `-static-libstdc++ -static-libgcc`, and check the GLIBC symbol versions in CI.
- **B3 · No macOS deployment target (Plausible, high confidence).** The build doesn't set `CMAKE_OSX_DEPLOYMENT_TARGET`, but `make_mac_bundle.sh` writes `LSMinimumSystemVersion 11.0`. The binary's `minos` will be the runner's OS (14.x). The wrapper passes `-DOSX_DEPLOYMENT_TARGET=12.0`, which is not a CMake variable.
- **B4 · Release zips lose execute bits (Plausible).** `actions/upload-artifact@v4` drops Unix modes, and the release job re-zips the downloaded files. The macOS `.app` and the Linux standalone arrive non-executable. Fix: tar, or `ditto -c -k`, inside each job before uploading.

### Medium

- **B5 · Nothing is signed or notarized.** Downloads are quarantined by Gatekeeper; at minimum, ad-hoc sign the bundle and document `xattr -dr com.apple.quarantine`.
- **B6 · Every symbol is exported on Linux and macOS.** There is no `-fvisibility=hidden`. `nvg*`, `stbi_*`, `stbtt_*`, `p5::` and `pw::` all land in the dynamic symbol table, which risks interposition by hosts or other plugins that ship nanovg or stb. Fix: the CMake visibility presets, `-Wl,--exclude-libs,ALL`, and an exported-symbols list containing `_clap_entry` on mac.
- **B7 · The AVX2 kernel only exists in Linux GUI builds.** The AVX2 block is nested in `if(MUHAMMAD_GUI)` and `if(UNIX AND NOT APPLE)` (`CMakeLists.txt:118-130`). As a result:
  - Windows and Intel-mac users get the SSE2 path at roughly half the speed;
  - on CI's AVX2 runners, the SSE2 kernel, which is what Windows ships, is **never A/B tested**.

  A namespace-scope `const V ALL1 = …` in the AVX2 translation unit (`p5_kernel_simd.cpp:24`) could run VEX code at load time in an -O0 build. Fix: move the AVX2 block to the top level, add a test hook to force each kernel, and make `ALL1` function-local.
- **B8 · The wrapper toolchain is unpinned.** `make_wrappers.sh` clones clap-wrapper HEAD, and CPM then pulls rtaudio and rtmidi at whatever versions that HEAD pins. Release builds are not reproducible.
- **B9 · The macOS wrappers are probably arm64-only.** No `CMAKE_OSX_ARCHITECTURES` is passed, while the `.clap` itself is universal.
- **B10 · Tests aren't registered with ctest, and TSan is the only sanitizer.** A new test only runs if it is added to the YAML by hand. Nothing catches memory errors in the blob, patch and tuning parsers, or signed-overflow UB in the fixed-point engine. Fix: `add_test` for each binary, plus an ASan+UBSan CI job.
- **B11 · The allocation watchdog never runs in CI.** `tools/allocwatch.c` is a working `LD_PRELOAD` malloc counter that `clap_smoke` brackets `process()` with. But CI runs `clap_smoke` without the preload, so the test prints "skipped" and passes.
- **B12 · `test_threadsan` doesn't model a real host.** It:
  - sends no notes, so voice paths are idle;
  - calls `flush` on main concurrently with `process` (§4.1);
  - activates with `max_frames = 0`, then processes 64 frames;
  - runs no GUI thread, timer or preset discovery;
  - uses a single instance.

  The README's "can ever corrupt" wording overclaims.
- **B13 · macOS CI runs only `test_mpe` and `clap_smoke`.** README:475-476 says more tests run there. The x86_64 slice and the NEON bit-exactness are not exercised in CI.
- **B14 · The release job doesn't wait for TSan.** `needs:` omits `tsan`.
- **B15 · The release notes contradict the install story.** They tell users to put `muhammad.clap` next to the wrappers, but 0.2.x embeds the engine. The same stale text is in `THIRD_PARTY_NOTICES.md` and `tools/vst3_smoke.cpp`.
- **B16 · Factory data rights.** `p5_tables.h` embeds Sequential's 200 factory programs in a GPL-3 binary. The GPL cannot grant redistribution rights for that data, and nothing in the repo records Sequential's permission. Worth confirming. (This is a legal question, flagged here, not legal advice.)

### Low / hygiene

- **SIMDe merge-conflict leftovers.** `external/simde/wasm/relaxed-simd_{BACKUP,BASE,LOCAL,REMOTE}_746082.h` (~2.6 MB, with live conflict markers) are checked in. All of SIMDe (31 MB) is vendored, though only `x86/avx2.h` is used.
- **Test output committed at the repo root.** `muhammad_ab_simd.wav` is test output; running the test rewrites it and dirties the tree. `muhammad_panel.svg` is generator output that belongs in `docs/`. Add `*.wav` to `.gitignore`.
- **README font claim is wrong.** It says Liberation Sans Narrow, embedded at build time. The panel actually uses Inter Tight and VT323, embedded into a checked-in 1.3 MB header. The Liberation font files are unused.
- **`THIRD_PARTY_NOTICES.md` errors.**
  - It says "old draft ABI" for what is CLAP 1.2.10.
  - The nanovg URL is wrong.
  - It gives the wrong path for stb_truetype.
  - Entries for AudioUnitSDK, rtaudio, rtmidi and WIL are missing.
  - Upstream copyright is credited inconsistently.
- **Version regex.** The wrapper's version regex (`"0\\.[0-9]+…"`) breaks at 1.0.
- **Tests that don't assert.**
  - `test_quantnoise` always returns 0.
  - `check_goldens.sh` ignores its own `TOL` variable.
  - `clap_smoke` calls `get_extension`, `state->save` and `flush` before `init()`.
- **Dead or stale tools.**
  - `tools/vst3_smoke.cpp` is dead and doesn't compile.
  - The `experiments/` patch references a base commit that doesn't exist in the repo.
  - `gen_tables.sh` and `gen_default_hdr.py` depend on developer-local paths.
  - `dev_install.sh` greps for an old version pattern.
- **CMake details.**
  - `-O3` is forced on every build type, Debug included.
  - `P5_BUILD_INFO` is defined twice.
  - `if(TARGET m)` is never true.
  - The engine is recompiled into ~10 targets; an OBJECT library would build it once.
  - The top level sets C++17, but the tuning library needs C++20 on MSVC.
  - The processor regex includes `i386`/`i686`, where x87 excess precision would break bit-exactness.
- **CI details.**
  - Actions are pinned by major tag, not SHA, including `softprops/action-gh-release`, which runs with `contents: write`.
  - There is no top-level `permissions:` block.
  - `CFBundlePackageType` is `APPL` and should be `BNDL`.

---

# 7. README claims vs. what the code and tests do

| Claim | Status |
|---|---|
| Scalar vs wide kernels are bit-exact, 44.1k/48k × 1x/2x | **True for the tested patches**, Linux CLAP, AVX2 kernel. Not checked for SSE2, arm64 or wrapper builds (B1, B7, B13). Scalar-only shortcuts (`if (ratio == 1.0f) return base;` in `wheelInc`, `if (!x) return base;` in `incOf`) diverge in low bits for wheel-freq routings with the wheel at 0, which covers 61 of the 200 factory programs. The A/B tests use only INIT and factory 0. |
| Split-invariance (`test_splitinv`) | **True and well built.** Linux CI only. |
| "Strict single-writer threading model, verified under ThreadSanitizer — no knob drag, program change or session load can ever corrupt a sound mid-block" | **Does not hold against spec-following hosts** (C1, H2). The TSan test models a host that cannot exist. |
| Sample-rate independence ("everything scales by 44100/sr") | **Partly.** Drift, VCA ramp, death fade and glide floor are per sample (E9), and the cutoff table breaks below 32 kHz (E1). |
| Death fade is "continuous across call bounds" | **False** for chunks shorter than 32 samples (E8). |
| Right-click invert | **Missing on macOS** (5.12). |
| Wrappers need no separate `.clap` | **True in code.** The release notes say the opposite (B15). |
| AVX2 on x86 | **Linux GUI builds only** (B7). |
| Repaint is driven by input events on X11 | **The timer drives everything** (4.7). |

---

# 8. What is done well

- **Determinism engineering.**
  - The scalar-vs-wide A/B runs across sample rates and oversampling, and exits nonzero on any mismatch.
  - `test_splitinv` checks that host chunking cannot change the output, and has already caught a real bug.
  - An alias-floor check uses a windowed Goertzel.
  - The build uses `-ffp-contract=off`, and `uint32→float` conversion is correctly rounded.

  These are rare and valuable in plugin projects.
- **Real-time discipline in the engine.**
  - No allocation, lock, log or syscall is reachable from `Engine::process`.
  - MIDI program change is a precomputed `memcpy`.
  - The audio thread never blocks to publish (`try_lock`).
  - FTZ/DAZ is set.
- **Bounded DSP.**
  - Ladder states are clamped and flushed, and the input is softclipped.
  - The DC blocker uses int64 intermediates.
  - The feedback path is lowpassed and kept outside the decimator.
  - No NaN source was found at self-oscillation or with maximum feedback (S1 is about *uninitialised* memory, not the DSP).
- **Hostile-input care in the codecs.**
  - The state parser has fixed-size fields, magic and version gates, bounded counts, clamped tables and NaN rejection.
  - `patchDecode` checks exact lengths.
  - Every patch byte that steers DSP is masked or clamped.
  - `sanitize` prevents path traversal on save, and tuning files are capped at 1 MB.
- **Polarity flag design.** Bit 7 is preserved through automation, CC, `get_value` masking and the save round-trip, and old files load invert-off.
- **Clean GUI layering.**
  - The renderer has no side effects and is shared with a headless golden-image test.
  - The layout is generated from the same source as the SVG design document.
  - The GUI never touches the engine directly.
- **MPE.** Per-channel bend lanes and per-channel note matching are handled cleanly.
- **Packaging care.**
  - The Windows build uses a static runtime with an export check.
  - VST3 TUID and AU IDs are stable.
  - CI fails loudly.
  - Vendored versions are recorded.
  - The VST3 SDK is 3.8 (MIT), so the GPL-3 wrapper distribution is licence-clean.

---

# 9. Suggested order of work

1. **Thread roles by CLAP annotation, not by `active`** (C1, H1, H2, 4.4).
   - Make `flush` and `reset` audio-side operations while active.
   - Emit GUI→host values, with proper event headers and gestures, from both `process()` and `flush()`.
   - Coalesce posts per id instead of running the 50 ms fallback.
   - Make `rr_` a release store.
   - Fix `test_threadsan` to call `flush` and `reset` on the audio thread, and send it notes.
2. **Stop the engine's lifecycle calls from wiping project state** (C3, C4, H3).
   - `activate()` touches only rate-dependent state.
   - `silenceReset()` touches only voices and filters, and sets `panDirty_`.
   - `fval_` snaps rather than zeroing.

   The most robust form of this is a main-owned persistent copy (the patch/patchMain pattern) that is re-pushed after activate or reset.
3. **One-line fixes with large user impact.**
   - Drop `IS_MODULATABLE` (C2).
   - Clamp trim to [0, 2] on load (H4).
   - Set `default_value` for every parameter.
   - Initialise the SoA rows and use an unsigned min in `softclipV` (S1).
   - Fix the cutoff-table angle clamp (E1).
4. **GUI host-citizenship.**
   - Delete the Win32 `PeekMessage` loop.
   - Make the X11 error handler and clipboard per-instance.
   - Give the ObjC class a unique name.
   - Add change-counter repaint on all platforms.
   - Add right-click on mac.
   - Check `c.modal` in `onRightPress`.
5. **Preset path.**
   - Route host factory loads through `loadProgram`.
   - Load user files by their exact name.
   - Call rescan and `loaded()`.
   - Make writes atomic (write a temp file, then rename).
   - Use the `error_code` filesystem calls and catch at the C boundary.
6. **Build and release.**
   - Engine compile flags on a target.
   - An older Linux toolchain, or static libstdc++.
   - A macOS deployment target, plus universal wrappers.
   - tar before upload.
   - Hidden visibility.
   - AVX2 on all x86 platforms, with an SSE2 A/B test.
   - ctest, ASan/UBSan, and allocwatch in CI.
7. **Tidy-up.** Repo hygiene and README/notices corrections.
