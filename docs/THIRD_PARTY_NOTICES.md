# Third-party lineage and notices

Muhammad is distributed under **GPL-3.0-only** (see `LICENSE`). This project
contains or is derived from the following third-party work. Items 1-3 are the
"things we got from Surge"; the rest are listed for completeness since they
also ship in the tree.

## From the Surge ecosystem

1. **Surge Synth Team tuning-library** — **MIT**, © Paul Walker 2019-2020.
   - Vendored verbatim: `vendor/tuning-library/` (`include/Tunings.h`,
     `include/TuningsImpl.h`; upstream
     https://github.com/surge-synthesizer/tuning-library). `LICENSE.md`
     retained in the vendor directory.
   - Used by `src/p5_store.cpp` for all `.scl`/`.kbm` parsing and equal /
     KBM retuning math (`Tunings::parseSCLData`, `Tuning`,
     `retuningFromEqualInSemitonesForMidiNote`).
   - MIT is BSD-style: compatible with GPL-3.0-only provided the notice is
     kept — it is, in-file and in the vendor LICENSE.

2. **sst-filters `HalfRateFilter.h`** — **GPL v3** (upstream LICENSE.md is
   the GPLv3 text; repo https://github.com/surge-synthesizer/sst-filters),
   reached via the Surge XT bundle (`libs/sst/sst-filters` submodule).
   - Derived: `src/p5_halfband.hpp` — the even/odd cascaded-allpass chain
     structure and the *verbatim* "soft, order==12" coefficient tables
     (six A + six B allpass coefficients, upstream
     `include/sst/filters/HalfRateFilter.h` ≈ line 716, class
     `sst::filters::HalfRateFilter`).
   - This is a transcription, not a line-for-line copy: the upstream class
     is a stereo multi-stage object; ours is a per-voice mono 2x→1x
     decimator. The coefficient data itself comes straight from upstream.
   - GPL v3 ↔ GPL-3.0-only: the same license family; where upstream is
     "v3 or later", we exercise the recipient's option to take the v3 terms
     only, which is compatible by construction.

3. **Surge XT GUI — `RadialScaleGraph` in
   `src/surge-xt/gui/overlays/TuningOverlays.cpp`** — **GPL-3.0-or-later**
   (https://github.com/surge-synthesizer/surge).
   - No source lines were copied (upstream is JUCE/Skia; ours is NanoVG/
     OpenGL). The microtuning dial in `src/p5_gui.{hpp,cpp}` and
     `src/gui_x11.cpp` **reimplements the design**: node radius as deviation
     from equal temperament in ET units
     (`r = R + (c_i − ET·i)/ET · ringScale`), the auto-ranged ring spacing
     with Surge's 0.2-ET floor, blue-below / amber-above node semantics,
     double-click snap-to-equal, and the Shift = ×0.1 fine-drag factor are
     taken from Surge's behaviour after studying that implementation.
   - Distribution terms: same-family GPL; the "or later" option is likewise
     exercised to v3-only. Where our dial constitutes an adaptation of
     upstream GPL code, that is intentional and this notice records it.

   Related, reference-only (no code or data taken): the JUCE table model in
   https://github.com/surge-synthesizer/surgesynthteam_tuningui informed the
   KEY/FREQUENCY/CENTS table layout, and Surge's overall tuning editor
   workflow informed the TUNING page structure.

## Other third parties in this tree

- **CLAP headers** — `vendor/clap/`, © Alexandre Bique / free-audio.org,
  **MIT** (notice in each header). The old draft ABI we vendored is
  deliberate: it matches the Bitwig generation we target.
- **NanoVG** — `external/nanovg/`, © Mikko Mononen, **zlib**
  (notice in headers; vendored from https://github.com/memonnick/nanovg).
- **SIMDe** — `external/simde/`, © 2018 David Michael Taschner and
  contributors, **MIT**; SPDX + permission notice preserved in every
  vendored header. We chose it because Surge vectorizes the same way, but
  the code is upstream SIMDe, not Surge.
- **stb** — `external/stb/` (`stb_truetype`, `stb_image_write`), **MIT /
  public domain**, notices in files.
- **Melodee firmware** — © keremimo, **GPL-3.0**: the Prophet-5 engine
  ported here, the generated DSP tables (kept verbatim via the dump tools),
  and the scale catalogue from `firmware/src/scales.h` (generated into
  `src/p5_stock_scales.hpp` by `tools/gen_muhammad_scales.py`). Built
  against a local checkout; no melodee source files are copied into this
  repo besides generated data.
- **Factory patch tables** — `assets/`/generated patch data decoded from
  Sequential Circuits' public Prophet-5 V1.03 (Rev-4) wire files, stored
  unmodified as data.
- **Fonts embedded in the binary** (`src/p5_fontdata.h` via
  `tools/embed_font.py`): **Inter Tight Bold** © Rasmus Andersson and
  **VT323** © The VT323 Project Authors — both **SIL OFL 1.1**; license
  texts kept in `assets/` (`OFL-InterTight.txt`, `VT323-OFL.txt`). The OFL
  does not infect the work; unmodified embedded fonts are permitted.

## Wrapper toolchain (build-time only, output ships)

- **clap-wrapper** — https://github.com/free-audio/clap-wrapper , © Surge
  Synth Team contributors, **MIT**. Not vendored into this repo; the VST3
  build (see README "VST3 via clap-wrapper") consumes it from a sibling
  checkout. The produced `muhammad.vst3` is a wrapper that dynamically loads
  our `muhammad.clap`; treat the wrapper's MIT notice as an attribution for
  any distributed VST3 binary, alongside the Steinberg SDK notice below.
- **VST3 SDK** — https://github.com/steinbergmedia/vst3sdk , © Steinberg
  Media Technologies, **MIT** (use of the VST trademark per Steinberg's
  guidelines; pinned to tag `v3.8.0_build_66`, the version clap-wrapper
  itself downloads — master breaks the wrapper with an `iid` ambiguity).
- AAX/AU targets are not built; AAX would additionally involve Avid's dual
  license terms if ever attempted.

## Compatibility summary

GPL-3.0-or-later sources (Surge XT, sst-filters) are combined into a
GPL-3.0-only work by exercising the "or later" option down to v3 — legal,
and recorded here. MIT/zlib/OFL/public-domain components are permissive and
ship with their notices preserved. Nothing here is under GPLv2-only, AGPL,
or non-GPL copyleft, so the combined distribution under GPL-3.0-only stands.

If you remove or vendor-bump any of the above, update this file in the same
commit.

## Khronos GL extension headers (external/mingw-gl)

`GL/glext.h` + `GL/wglext.h` copied verbatim from the mingw-w64 sysroot; the
original Khronos permissive notice is preserved in each file. Used only for
MSVC builds on Windows (Windows Kits does not ship them); MinGW and POSIX
builds never read this directory.
