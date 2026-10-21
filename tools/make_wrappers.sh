#!/usr/bin/env bash
# Build the VST3 wrapper (+ standalone, + AUv2 on macOS) for Muhammad using
# free-audio/clap-wrapper's CLAPFIRST mode: the engine + panel are STATICALLY
# EMBEDDED in every wrapper, so a tester installs the .vst3/.component or runs
# the standalone with no muhammad.clap anywhere. (User call 2026-10-20: "users
# aren't going to populate a clap directory when they're not using clap".)
#
# Run from anywhere inside the repo on a native machine (Linux, macOS) or in
# CI including windows-latest under git-bash - the wrapper needs no compile
# of the engine itself, so no MinGW trickery and no MSVC port of the plugin.
#
# Artifacts land in dist/wrappers/: muhammad.vst3 (+.component on mac) and
# the standalone host executable, plus a zip per platform for CI upload.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${MUH_WRAPPER_WORK:-${RUNNER_TEMP:-/tmp}/muhammad-wrappers}"
DIST="$REPO/dist/wrappers"
mkdir -p "$WORK"
rm -rf "$DIST"; mkdir -p "$DIST"   # never trust leftovers: a stale (or
                                   # committed-by-accident!) dist here was
                                   # silently uploaded as a Windows artifact

say() { echo "[wrappers] $*"; }

# ---- fetch the wrapper + Steinberg SDK (pinned tag, shallow) --------------
# clap-wrapper PINNED by SHA (review B8: HEAD floats). The TAG pin attempt
# (v0.9.1, newest tag) broke every wrapper leg on CI: CLAP-first mode -
# cmake/make_clapfirst.cmake, the whole design of tools/wrapper - postdates
# every published clap-wrapper tag; upstream only tags releases that its own
# versioned workflow cuts, and this integration lives ahead of that. So:
# pin the master commit we build against, fetched by SHA (GitHub allows it),
# and bump deliberately. Layout guard: a stale other-pin cache self-heals.
WRAP_SHA=806bba0a5f792e3966b99c1561ab9952d3316d7c
if [ ! -d "$WORK/clap-wrapper" ] || [ ! -f "$WORK/clap-wrapper/.pinned-$WRAP_SHA" ]; then
    rm -rf "$WORK/clap-wrapper"
    git clone -q https://github.com/free-audio/clap-wrapper "$WORK/clap-wrapper"
    ( cd "$WORK/clap-wrapper" && git checkout -q "$WRAP_SHA" ) || {
        echo '[wrappers] pinned commit absent from clone; fetching by SHA'
        ( cd "$WORK/clap-wrapper" && git fetch -q origin "$WRAP_SHA" \
          && git checkout -q FETCH_HEAD ) ; }
    touch "$WORK/clap-wrapper/.pinned-$WRAP_SHA"
fi
if [ ! -d "$WORK/vst3sdk" ]; then
    git clone -q --depth 1 https://github.com/steinbergmedia/vst3sdk "$WORK/vst3sdk"
    git -C "$WORK/vst3sdk" fetch -q --depth 1 origin tag v3.8.0_build_66
    git -C "$WORK/vst3sdk" checkout -q v3.8.0_build_66
    git -C "$WORK/vst3sdk" submodule update -q --init --depth 1
fi

# ---- stage the vendored CLAP headers into the layout the wrapper expects --
# (vendor/clap holds `clap/*.h` at its root; base_sdks wants <root>/include
#  plus a minimal CMakeLists exporting an INTERFACE target `clap`.)
STAGE="$WORK/clap-sdk-stage"
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -R "$REPO/vendor/clap" "$STAGE/include"
cat > "$STAGE/CMakeLists.txt" <<'EOF'
# Minimal stand-in for upstream free-audio/clap's CMakeLists: an INTERFACE
# target over the vendored headers (the wrapper build only needs include/).
cmake_minimum_required(VERSION 3.16)
project(clap-sdk-staging NONE)
add_library(clap INTERFACE)
add_library(clap::clap ALIAS clap)
target_include_directories(clap INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
EOF

# ---- platform extras -------------------------------------------------------
EXTRA=()
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # VS 18-era MSVC (14.5x) hard-errors (C2338) on the deprecated
        # <experimental/coroutine> WIL pulls in for the standalone; the
        # compiler's own message names the opt-out.
        #
        # Delivery matters, twice proven by CI: (a) a value beginning with a
        # SLASH (/D_SILENCE...) gets rewritten by git-bash's MSYS layer into
        # C:/Program Files/Git/... and reaches cl.exe as a source FILE; (b)
        # setting CMAKE_CXX_FLAGS at all REPLACES CMake's MSVC default seed,
        # killing /EHsc /GR - which silently broke WIL's exception-dependent
        # com_ptr the same way; (c) an _CL_ environment variable poisoned the
        # compiler-ID discovery in the very next run. So: -D leading value
        # (MSYS-safe), CMake's documented default flags RESTATED verbatim,
        # and the opt-out appended. Both language seeds set; CMake's MSVC
        # init string is "/DWIN32 /D_WINDOWS /W3 /GR /EHsc".
        EXTRA+=(-DCMAKE_CXX_FLAGS="-DWIN32 /D_WINDOWS /W3 /GR /EHsc -D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS"
                -DCMAKE_C_FLAGS="-DWIN32 /D_WINDOWS /W3")
        ;;
esac
if [ "$(uname)" = "Darwin" ]; then
    # AUv2 wrapper: Apple's AudioUnitSDK (BSD-3), cloned next to the others
    # Pinned AudioUnitSDK-1.3.0 - the newest tag the macos-14 runner's
    # Xcode-15.4 toolchain can compile. Tag forensics (local clones, real
    # greps): 1.0.0 keeps sources in Source/ (fails base_sdks' src/ probe);
    # 1.1.0/1.2.0 are src/ + C++17-clean; 1.3.0 adds std::span/concepts
    # (C++20 - granted below); 1.4.0 adds std::expected (C++23, absent from
    # clang 15's libc++ - the CI error) . So: 1.3.0 + CLAP_WRAPPER_CXX_STANDARD=20.
    # Upgrading the job to macos-15/Xcode16 would let the pin move forward.
    # Layout guard + pin marker: a stale wrong-shape OR wrong-tag cache self-heals.
    if [ ! -d "$WORK/AudioUnitSDK/src/AudioUnitSDK" ] || \
       [ ! -f "$WORK/AudioUnitSDK/.pinned-AudioUnitSDK-1.3.0" ]; then
        rm -rf "$WORK/AudioUnitSDK"
        git clone -q --depth 1 --branch AudioUnitSDK-1.3.0 \
            https://github.com/apple/AudioUnitSDK "$WORK/AudioUnitSDK"
        touch "$WORK/AudioUnitSDK/.pinned-AudioUnitSDK-1.3.0"
    fi
    # C++20 for the whole mac wrapper build (1.3.0 needs span/concepts;
    # vst3sdk and the wrapper support >=17 by their own guard), deployment
    # 12.0 for the span-era availability annotations. OSX_DEPLOYMENT_TARGET is
# clap-wrapper's OWN cache variable (unset -> it announces 10.13 and span
# availability bites anyway; proven on CI 2026-10-18); CMAKE_ prefixed is
# also set so the toolchain default agrees with it. A 2026-10-21 review
# called the bare name a typo and the 'fix' would have silently reverted
# the wrapper to 10.13 - trust the proven skill, not the reviewer.
    EXTRA+=(-DCLAP_WRAPPER_BUILD_AUV2=ON -DAUDIOUNIT_SDK_ROOT="$WORK/AudioUnitSDK"
            -DCLAP_WRAPPER_CXX_STANDARD=20 -DOSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0)
fi

VER="$(grep -m1 'set(P5_VERSION' "$REPO/CMakeLists.txt" | sed 's/.*"\(.*\)".*/\1/')"
TUID="$(cat "$REPO/tools/muhammad_vst3_tuid.txt")"

# ---- configure + build (clapfirst: embedded wrappers) ---------------------
# The TUID pins the VST3 identity; clapfirst derives it from the CLAP id when
# unset, so pass it explicitly to keep host plugin databases stable.
# Configure does NETWORK WORK (CPM.cmake release download, then rtaudio/
# rtmidi/Gulrak fetches) and clap-wrapper's bootstrap file(DOWNLOAD) has no
# retry — a runner timeout once killed macOS configure outright (flake of
# 2026-10-20: "file DOWNLOAD cannot compute hash on failed download",
# status [28 Timeout], cascading into "CPMAddPackage: unknown command").
# Configure is idempotent, so retry the WHOLE invocation: transient network
# faults clear in seconds; genuine CMake errors just fail three times in a
# row (with logs) and still stop the build.
CFG=(cmake -B "$WORK/clapfirst-build" "$REPO/tools/wrapper"
      -DCMAKE_BUILD_TYPE=Release
      -DMUH_REPO="$REPO"
      -DCLAP_WRAPPER_ROOT="$WORK/clap-wrapper"
      -DCLAP_SDK_ROOT="$STAGE"
      -DVST3_SDK_ROOT="$WORK/vst3sdk"
      -DCLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=ON
      -DCLAP_VST3_TUID_STRING="$TUID"
      "${EXTRA[@]}")
for attempt in 1 2 3; do
    if "${CFG[@]}" > "$WORK/clapfirst-configure.log" 2>&1; then
        cat "$WORK/clapfirst-configure.log"; break
    fi
    echo "[wrappers] configure attempt $attempt/3 failed (tail below)"
    tail -15 "$WORK/clapfirst-configure.log"
    [ "$attempt" = 3 ] && { echo "[wrappers] configure failed after 3 attempts"; exit 1; }
    rm -rf "$WORK/clapfirst-build/CMakeCache.txt" "$WORK/clapfirst-build/CMakeFiles"
    sleep 5
done
# --config Release matters on the VS generator (windows): it is
# multi-config, ignores CMAKE_BUILD_TYPE, and DEFAULTS TO DEBUG.
cmake --build "$WORK/clapfirst-build" --config Release --target muhammadw_all \
      -j"$( (nproc 2>/dev/null || sysctl -n hw.ncpu) | head -1 )"

# ---- collect from the assets tree -----------------------------------------
# ASSET_OUTPUT_DIRECTORY layout: per-format dirs holding muhammad.vst3 (dir),
# muhammad.component (dir, mac), muhammad.app (dir, mac standalone) or the
# flat muhammad / muhammad.exe host. The CLAP asset itself is NOT collected —
# the release ships muhammad.clap from the main build (version stamp!).
ASSETS="$WORK/clapfirst-build/assets"
[ -d "$ASSETS" ] || { echo "[wrappers] ERROR: no assets tree"; exit 1; }
FOUND=""
while IFS= read -r -d '' item; do
    base=$(basename "$item")
    case "$base" in
        muhammad.vst3|muhammad.component|muhammad.app)
            cp -R "$item" "$DIST/" && FOUND="$FOUND [$base]";;
        muhammad|muhammad.exe)
            [ -d "$item" ] && continue
            [ -x "$item" ] && { cp "$item" "$DIST/" && FOUND="$FOUND [$base]"; };;
    esac
done < <(find "$ASSETS" -mindepth 1 \
           \( -name 'muhammad.vst3' -o -name 'muhammad.component' \) -print0 -o \
           \( \( -name 'muhammad' -o -name 'muhammad.exe' -o -name 'muhammad.app' \) -print0 \) 2>/dev/null)
say "collected:${FOUND:- nothing}"
# Fail LOUDLY on missing pieces (the silent mac .app drop of the first
# wrappers release must never repeat, in either direction).
case " $FOUND " in *" [muhammad.vst3] "*) :;;
    *) echo "[wrappers] ERROR: no muhammad.vst3 collected"; ls -R "$ASSETS" | head -40; exit 1;;
esac
# same uname -s axis as the platform branch at the top: git-bash sets OSTYPE
# to msys/mingw..., NEVER "windows*" - an OSTYPE test here demanded a flat
# Linux muhammad and FAIL LOUDLY'd a good Windows build whose exe had, in
# fact, been collected two lines earlier.
case "$(uname -s)" in
    Darwin)                       pat=" [muhammad.app] ";;
    MINGW*|MSYS*|CYGWIN*)         pat=" [muhammad.exe] ";;
    *)                            pat=" [muhammad] ";;
esac
case " $FOUND " in *"$pat"*) :;;
    *) echo "[wrappers] ERROR: no standalone host collected (wanted$pat)"; ls -R "$ASSETS" | head -40; exit 1;;
esac
# Keep dist/wrappers LOOSE: actions/upload-artifact always wraps artifacts in
# one zip of its own - a self-made zip inside produced a zip-of-a-zip (user-
# observed). The release job stages explicit per-platform zips instead.
say "done: $(ls -1 "$DIST")"
