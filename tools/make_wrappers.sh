#!/usr/bin/env bash
# Build the VST3 wrapper (+ standalone, + AUv2 on macOS) for Muhammad using
# free-audio/clap-wrapper's DYNAMIC-LOAD mode: the wrapper binaries host the
# clap named "muhammad" found at runtime (~/.clap or the platform default),
# so they pair with the muhammad.clap artifact of the same version.
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
mkdir -p "$WORK" "$DIST"

say() { echo "[wrappers] $*"; }

# ---- fetch the wrapper + Steinberg SDK (pinned tag, shallow) --------------
if [ ! -d "$WORK/clap-wrapper" ]; then
    git clone -q --depth 1 https://github.com/free-audio/clap-wrapper "$WORK/clap-wrapper"
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
    if [ ! -d "$WORK/AudioUnitSDK" ]; then
        # pinned: CI's unpinned HEAD hit AudioUnitSDK's C++20 migration
        # (std::span, concept/requires) while the wrapper defaulted to C++17.
        git clone -q --depth 1 --branch AudioUnitSDK-1.0.0 \
            https://github.com/apple/AudioUnitSDK "$WORK/AudioUnitSDK"
    fi
    # C++20 for the whole wrapper build on mac (the SDK needs it; vst3sdk and
    # the wrapper both support >=17 per their own guard) + deployment 12.0 so
    # libc++'s availability-marked std::span compiles under 10.13 default.
    EXTRA+=(-DCLAP_WRAPPER_BUILD_AUV2=ON -DAUDIOUNIT_SDK_ROOT="$WORK/AudioUnitSDK"
            -DCLAP_WRAPPER_CXX_STANDARD=20 -DOSX_DEPLOYMENT_TARGET=12.0)
fi

VER="$(grep -m1 'set(P5_VERSION' "$REPO/CMakeLists.txt" | sed 's/.*"\(.*\)".*/\1/')"
TUID="$(cat "$REPO/tools/muhammad_vst3_tuid.txt")"

# ---- configure + build ----------------------------------------------------
cd "$WORK/clap-wrapper"
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCLAP_SDK_ROOT="$STAGE" \
      -DVST3_SDK_ROOT="$WORK/vst3sdk" \
      -DCLAP_WRAPPER_OUTPUT_NAME=muhammad \
      -DCLAP_VST3_TUID_STRING="$TUID" \
      -DCLAP_WRAPPER_DOWNLOAD_DEPENDENCIES=ON \
      -DCLAP_WRAPPER_BUILD_STANDALONE=ON \
      "${EXTRA[@]}"
cmake --build build -j"$( (nproc 2>/dev/null || sysctl -n hw.ncpu) | head -1 )"

# ---- collect -------------------------------------------------------------
cd "$WORK/clap-wrapper/build"
for cand in Release/muhammad.vst3 muhammad.vst3 Release/muhammad.component muhammad.component; do
    [ -e "$cand" ] && cp -R "$cand" "$DIST/" && say "collected $cand"
done
for cand in muhammad muhammad.exe Release/muhammad Release/muhammad.exe \
             Release/muhammad.app muhammad.app Debug/muhammad; do
    [ -x "$cand" ] && [ ! -d "$cand" ] && cp "$cand" "$DIST/" && say "collected standalone $cand"
done
# Keep dist/wrappers LOOSE: actions/upload-artifact always wraps artifacts in
# one zip of its own - a self-made zip inside produced a zip-of-a-zip (user-
# observed). The release job stages explicit per-platform zips instead.
say "done: $(ls -1 "$DIST")"
