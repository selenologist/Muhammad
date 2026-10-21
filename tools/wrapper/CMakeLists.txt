# SPDX-License-Identifier: GPL-3.0-only
# clapfirst wrapper project: SELF-CONTAINED muhammad wrappers (VST3, AUv2 on
# macOS, standalone) with engine + panel statically embedded — hosts and
# standalone users never need a muhammad.clap on a CLAP search path.
#
# Driven by tools/make_wrappers.sh:
#   cmake -B build tools/wrapper -DCLAP_WRAPPER_ROOT=... -DVST3_SDK_ROOT=... \
#        [-DAUDIOUNIT_SDK_ROOT=...] [-DCLAP_SDK_ROOT=...] -DMUH_REPO=...
#
# The repo is pulled in as a subproject ONLY for its GUI targets (p5_gui/
# nanovg with correct platform libs); muhammad-impl compiles the same sources
# statically and mirrors the dual-kernel AVX2 arrangement where the compiler
# supports GCC flags (portable SIMDe-only otherwise). clap_entry.cpp is
# clapfirst's ENTRY_SOURCE: it defines clap_entry + factory glue and reaches
# the synth exclusively through pw:: symbols.

cmake_minimum_required(VERSION 3.16)
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/../../CMakeLists.txt MUH_VER_LINE LIMIT_COUNT 1 REGEX "set\\(P5_VERSION")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" MUH_VER "${MUH_VER_LINE}")   # was 0\\.x: breaks at 1.0
project(muhammad-wrappers VERSION ${MUH_VER} LANGUAGES C CXX)

# clap-wrapper sets this in its own directory scope; the SDK/wrapper static
# libs here get created from THIS directory (via make_clapfirst), so PIC must
# be on at this level or the shared-module link fails with R_X86_64_PC32.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# clap-wrapper's CMAKE_CXX_STANDARD handling is gated on PROJECT_IS_TOP_LEVEL
# (its own CMakeLists) — as a subproject it inherits whatever WE set. Apple
# clang's default is C++98, and the CLAP headers (1LL, trailing enumerator
# commas) then die under its -Werror extension warnings; MSVC defaults to
# pre-C++20 designated-initializer support in our code paths. 20 matches the
# value the old top-level mac configure passed as CLAP_WRAPPER_CXX_STANDARD.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # clap-wrapper statically links the MSVC CRT into every wrapper (its own
    # CMakeLists sets this, but only in ITS directory scope — and the entries
    # clapfirst compiles for us live in OURS). Mixed MT libs + MD objects is
    # LNK2038 RuntimeLibrary mismatch + a multiply-defined-std flood (first
    # muhammadw_vst3 link did exactly that). Match it at this scope so ALL
    # targets here — ours and every subproject's — get /MT. This also gives
    # the Windows wrappers the same self-contained-CRT property the MinGW
    # -static .clap has: no VC++ redist dependency on tester machines.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # C4996 'unsafe strcpy/sscanf/strncpy' is an MSVC-only deprecation the
    # CRT honors via this switch; our code and Steinberg's both use plain,
    # portable CRT calls deliberately (strncpy_s & co. exist only on MSVC).
    # 4244/4267 stay ENABLED: every muhammad-side site was fixed at source, and
    # the remaining third-party chatter is quieter than a rule that hides our own.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

set(MUH_REPO "" CACHE PATH "muhammad repo root")
set(CLAP_WRAPPER_ROOT "" CACHE PATH "clap-wrapper checkout")
if(NOT EXISTS ${MUH_REPO}/CMakeLists.txt)
    message(FATAL_ERROR "MUH_REPO missing")
endif()
if(NOT EXISTS ${CLAP_WRAPPER_ROOT}/cmake/make_clapfirst.cmake)
    message(FATAL_ERROR "CLAP_WRAPPER_ROOT missing")
endif()

add_subdirectory(${CLAP_WRAPPER_ROOT} clap-wrapper-build EXCLUDE_FROM_ALL)
set(MUHAMMAD_GUI ON)         # we link p5_gui from it
add_subdirectory(${MUH_REPO} muhammad-repo-build EXCLUDE_FROM_ALL)

# ---- platform GL/OS libs (repo-scope variables are invisible here) ---------
if(WIN32)
    set(MUH_GL_LIBS opengl32 gdi32 user32 dwmapi)
    set(MUH_GUI_SHIM ${MUH_REPO}/src/gui_win32.cpp)
elseif(APPLE)
    enable_language(OBJCXX)
    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(OPENGLF_LIBRARY OpenGL REQUIRED)
    set(MUH_GL_LIBS ${COCOA_LIBRARY} ${OPENGLF_LIBRARY})
    set(MUH_GUI_SHIM ${MUH_REPO}/src/gui_mac.mm)
else()
    find_library(EGL_LIBRARY EGL)
    find_library(GL_LIBRARY GL)
    find_package(X11 MODULE)
    set(MUH_GL_LIBS ${GL_LIBRARY} ${EGL_LIBRARY} ${CMAKE_DL_LIBS} ${X11_LIBRARIES})
    set(MUH_GUI_SHIM ${MUH_REPO}/src/gui_x11.cpp)
endif()

# ---- impl static library: everything but the entry cpp --------------------
set(MUH_IMPL_SRCS
    ${MUH_REPO}/src/muhammad_plugin.cpp
    ${MUH_REPO}/src/muhammad_discovery.cpp
    ${MUH_REPO}/src/p5_store.cpp
    ${MUH_REPO}/src/p5_factory.cpp
    ${MUH_REPO}/src/p5_engine.cpp
    ${MUH_REPO}/src/p5_kernel_scalar.cpp
    ${MUH_REPO}/src/p5_kernel_simd.cpp
    ${MUH_REPO}/src/gui_core.cpp
    ${MUH_GUI_SHIM})

add_library(muhammad-impl STATIC ${MUH_IMPL_SRCS})
# tuning-library needs concepts: the repo pins gnu++20 on p5_store.cpp alone
# and the rest stays 17 — mirror that exactly (and C++17 as the project floor).
set_source_files_properties(${MUH_REPO}/src/p5_store.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-std=gnu++20>")
target_compile_features(muhammad-impl PRIVATE cxx_std_17)
# The engine math contract rides the TARGET here too (review B1): this
# project re-compiles the engine sources; inheriting nothing from the repo's
# top level, it silently built WITHOUT -ffp-contract=off — FMA fused in the
# SIMD copy but not the scalar reference, so the shipped wrappers' kernels
# were not bit-identical. MSVC /fp:precise (default) never forms FMA.
target_compile_options(muhammad-impl PUBLIC
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3;-ffp-contract=off>)
# clapfirst links this static impl into SHARED modules (the .clap asset and
# the VST3 bundle): whole-archive PIC, and the PIC'd objects win over the
# non-PIC repo subproject copies that arrive via the p5_gui dependency edge.
set_property(TARGET muhammad-impl PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(muhammad-impl PUBLIC
    ${MUH_REPO}/src ${MUH_REPO}/vendor/clap ${MUH_REPO}/external
    ${MUH_REPO}/vendor/tuning-library/include
    ${MUH_REPO}/external/stb ${X11_INCLUDE_DIR})
if(MSVC)
    target_include_directories(muhammad-impl BEFORE PRIVATE ${MUH_REPO}/external/mingw-gl)
endif()
target_link_libraries(muhammad-impl PUBLIC clap clap-wrapper-extensions p5_gui ${MUH_GL_LIBS})
target_compile_definitions(muhammad-impl PRIVATE P5_GUI=1 P5_VERSION="${PROJECT_VERSION}")
if(APPLE)
    target_compile_definitions(muhammad-impl PRIVATE GL_SILENCE_DEPRECATION)
endif()

# dual wide kernel, matching the repo layout when the compiler speaks GCC
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"
   AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    add_library(muhammad-impl-avx2 OBJECT ${MUH_REPO}/src/p5_kernel_simd.cpp)
    target_include_directories(muhammad-impl-avx2 PRIVATE ${MUH_REPO}/src
        ${MUH_REPO}/external ${MUH_REPO}/vendor/clap ${MUH_REPO}/vendor/tuning-library/include)
    target_compile_definitions(muhammad-impl-avx2 PRIVATE P5_SIMD_SYMBOL=p5KernelSimdAVX2 P5_HAVE_AVX2)
    set_property(TARGET muhammad-impl-avx2 PROPERTY POSITION_INDEPENDENT_CODE ON)
    target_compile_options(muhammad-impl-avx2 PRIVATE -mavx2 -mfma -ffp-contract=off)
    target_sources(muhammad-impl PRIVATE $<TARGET_OBJECTS:muhammad-impl-avx2>)
    target_compile_definitions(muhammad-impl PRIVATE P5_HAVE_AVX2)
endif()

# P5_BUILD_INFO carries the same version stamp the panel shows
target_compile_definitions(muhammad-impl PRIVATE
    P5_BUILD_INFO="embedded ${PROJECT_VERSION} wrapper")

# Pin the VST3 identity to the same TUID the dynamic wrappers shipped with,
# so hosts keep their plugin databases across the static-embedding switch.
# make_clapfirst does not forward SINGLE_PLUGIN_TUID, but the compile
# definition wrap_vst3 uses is directory-inheritable for targets created
# after this point (the wrapper entry targets).
file(STRINGS ${MUH_REPO}/tools/muhammad_vst3_tuid.txt MUH_TUID LIMIT_COUNT 1)
string(STRIP "${MUH_TUID}" MUH_TUID)
add_compile_definitions(CLAP_VST3_TUID_STRING="${MUH_TUID}")

set(MUH_FORMATS CLAP VST3)
if(APPLE)
    list(APPEND MUH_FORMATS AUV2)
endif()

# TARGET_NAME muhammadw (not muhammad!): clapfirst derives its internal
# target names as <TARGET_NAME>_clap/_vst3/..., which would collide with the
# repo subproject's own muhammad_clap MODULE target. OUTPUT_NAME keeps every
# shipped artifact named muhammad.* as before.
make_clapfirst_plugins(
        TARGET_NAME muhammadw
        IMPL_TARGET muhammad-impl
        OUTPUT_NAME muhammad
        ENTRY_SOURCE ${MUH_REPO}/src/clap_entry.cpp
        BUNDLE_IDENTIFIER muhammad.unresolvable
        BUNDLE_VERSION ${PROJECT_VERSION}
        COPY_AFTER_BUILD FALSE
        PLUGIN_FORMATS ${MUH_FORMATS}
        STANDALONE_CONFIGURATIONS "standalone;muhammad;muhammad.unresolvable"
        # AUv2's build helper refuses to invent a manufacturer: without these it
        # probes the clap for a clap.plugin-factory-info-for-auv2 extension we
        # do not implement (mac CI: "No manufacturer provider"). Name/code come
        # from the plugin-id namespace; the AU subtype self-derives from a hash
        # of the clap id, so it is stable by construction.
        AUV2_MANUFACTURER_NAME "unresolvable"
        AUV2_MANUFACTURER_CODE "UNRS"
        # The cmake-configuration branch of wrap_auv2 (the one clapfirst hits)
        # needs the subtype explicitly — the id-hash fallback lives only in
        # the build-helper binary path. mham = muhammad, fixed for host
        # identity stability; aumu = AU Music Device, stated to silence the
        # default warning and pin intent.
        AUV2_SUBTYPE_CODE "mham"
        AUV2_INSTRUMENT_TYPE "aumu"
        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/assets)
