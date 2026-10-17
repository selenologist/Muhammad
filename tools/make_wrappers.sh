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

# ---- macOS: AUv2 needs Apple's AudioUnitSDK (BSD-3) ----------------------
EXTRA=()
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # VS "18"-era MSVC (14.5x) turned the deprecated <experimental/coroutine>
        # - pulled in by WIL for the standalone host - into a hard C2338. The
        # compiler's own message prescribes this opt-out define; it silences a
        # deprecation gate, nothing else.
                # -D, NOT /D: git-bash's MSYS layer rewrites arguments that begin with a
        # slash into C:/Program Files/Git/... - the previous /D form arrived at
        # cl.exe as a (nonexistent) SOURCE FILE and broke the compiler check.
        EXTRA+=(-DCMAKE_CXX_FLAGS="-D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS")
        ;;
esac
if [ "$(uname)" = "Darwin" ]; then
    if [ ! -d "$WORK/AudioUnitSDK" ]; then
        git clone -q --depth 1 https://github.com/apple/AudioUnitSDK "$WORK/AudioUnitSDK"
    fi
    EXTRA+=(-DCLAP_WRAPPER_BUILD_AUV2=ON -DAUDIOUNIT_SDK_ROOT="$WORK/AudioUnitSDK")
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
