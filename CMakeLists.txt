# Muhammad CLAP: the Melodee engine port, vectorized with SIMDe.
cmake_minimum_required(VERSION 3.16)
project(MuhammadClap LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# SIMD width is chosen by SIMDe for the target; the SIMD kernel must not let the
# compiler fuse mul+add, or the scalar reference and the vector kernel drift.
if(MSVC)
    # Windows Kits has GL/gl.h but no GL/glext.h/wglext.h (C1083 in every MSVC
    # compile of the panel); these Khronos copies (see external/mingw-gl/README)
    # sit BEFORE the system so <GL/glext.h> resolves. MinGW never uses them.
    include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/external/mingw-gl)
endif()
include_directories(vendor/tuning-library/include)   # header-only, MIT: tuning-library
# the library needs C++20 concepts; grant them to the one TU that includes it
# compiler-id genex, not if(MSVC): the flag evaporates for cl.exe at build
# time regardless of scope timing (MSVC gets C++20 via CMAKE_CXX_STANDARD)
set_source_files_properties(src/p5_store.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-std=gnu++20>")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(PP_CXX_FLAGS "")   # see note below: flags are on p5_engine now
else()
    # MSVC: /O2 arrives via CMAKE_BUILD_TYPE=Release; /fp:precise (the default)
    # never forms FMA, which is exactly the no-contraction semantics the engine
    # bit-exactness contract needs. Feeding gcc flags to cl.exe only produced a
    # D9002 wall that hid real diagnostics (windows clapfirst CI).
    set(PP_CXX_FLAGS "")
endif()
# (review hygiene: -O3 used to be forced into EVERY build type, Debug
# included. It no longer needs to be: the engine's math contract rides the
# p5_engine target PUBLIC (see below), and Release brings its own -O3.
# C++ Debug builds now get real -O0 debuggability; the engine stays -O3
# because bit-exactness is not a build-type option.)

find_library(MATH_LIBRARY m)

# Every symbol was exported on Linux/macOS: nvg*, stb_*, p5::, pw:: sat in the
# dynamic symbol table where any host or neighbouring plugin shipping nanovg
# or stb could interpose them (review B6). clap_entry is CLAP_EXPORT-marked
# (visibility("default")) and stays visible.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Build stamp: version + git hash + dirty marker + time, shown in the panel
# corner and the descriptor, so a running binary identifies itself.
find_program(GIT_EXE git)
execute_process(COMMAND ${GIT_EXE} rev-parse --short HEAD OUTPUT_VARIABLE P5_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND ${GIT_EXE} status --porcelain OUTPUT_VARIABLE P5_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(TIMESTAMP P5_BUILDDATE "%Y-%m-%d %H:%M")
set(P5_VERSION "0.2.4")
set(P5_DIRTY_MARK "")
if(P5_DIRTY)
    set(P5_DIRTY_MARK "-d")
endif()
set(P5_BUILD_INFO "${P5_VERSION} ${P5_GIT}${P5_DIRTY_MARK} ${P5_BUILDDATE}")

set(ENGINE_SOURCES
    src/p5_engine.cpp
    src/p5_kernel_scalar.cpp
    src/p5_kernel_simd.cpp
)

# The engine's math contract (-O3 + no FMA contraction) rides the TARGET,
# not CMAKE_CXX_FLAGS: consumers that recompile these sources in their own
# project (tools/wrapper) must not be able to lose it (review B1: the VST3/AU
# builds fused FMAs in one kernel but not the other). PUBLIC: anything
# linking p5_engine inherits it.
add_library(p5_engine STATIC ${ENGINE_SOURCES})
target_include_directories(p5_engine PUBLIC src external)
target_compile_options(p5_engine PUBLIC
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3;-ffp-contract=off>)
if(MATH_LIBRARY)
    target_link_libraries(p5_engine PUBLIC ${MATH_LIBRARY})
endif()

    # x86 ships the wide kernel twice: portable baseline + true AVX2 (SIMDe maps
# to native instructions under -mavx2 -mfma). Engine::activate selects by
# cpuid; the contract stays OFF so both paths are bit-identical.
# Review B7: this block used to hide inside if(MUHAMMAD_GUI AND UNIX) -
# Windows never got the AVX2 kernel, and CI never A/B-tested the SSE2 kernel
# the Windows ship actually uses. 32-bit x86 is deliberately EXCLUDED: x87
# excess precision breaks bit-exactness. Apple is skipped for the same reason
# the universal slice would compile -mavx2 for arm64; single-arch x86_64 mac
# builds can opt in with -DP5_AVX2=ON.
set(P5_AVX2_OK OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"
   AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    if(NOT APPLE)
        set(P5_AVX2_OK ON)
    elseif(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        set(P5_AVX2_OK ON)
    endif()
endif()
option(P5_AVX2 "true-AVX2 twin of the wide kernel (default: auto by platform)" ${P5_AVX2_OK})
if(P5_AVX2)
    add_library(p5_simd_avx2 OBJECT src/p5_kernel_simd.cpp)
    target_include_directories(p5_simd_avx2 PRIVATE src external)
    target_compile_definitions(p5_simd_avx2 PRIVATE P5_SIMD_SYMBOL=p5KernelSimdAVX2 P5_HAVE_AVX2)
    target_compile_options(p5_simd_avx2 PRIVATE -mavx2 -mfma -ffp-contract=off)
    target_link_libraries(p5_engine PUBLIC p5_simd_avx2)   # objects propagate transitively
    target_compile_definitions(p5_engine PUBLIC P5_HAVE_AVX2)
endif()

# ---- GUI (NanoVG panel; Linux/X11 is the primary target) ---------------------
# draw_panel + layout are platform-neutral; the embedding shim is per-OS.
option(MUHAMMAD_GUI "Build the NanoVG panel (needs OpenGL; X11 embedding on Linux)" ON)
set(NANOVG_DIR external/nanovg)
if(MUHAMMAD_GUI)
    if(WIN32)
        # Windows: WGL context, opengl32 exports only GL 1.1 at link time -
        # gui_win32.cpp provides extern"C" thunks for the rest (loaded via
        # wglGetProcAddress before any nanovg call).
        find_package(OpenGL REQUIRED)
        set(MUHAMMAD_GL_LIBS opengl32 gdi32 user32 dwmapi)
    elseif(APPLE)
        add_compile_definitions(GL_SILENCE_DEPRECATION)   # NSOpenGL deprecated-but-
                                                           # exactly-what-we-use; 19
                                                           # warning scroll in CI
        # macOS: desktop GL and Cocoa arrive as FRAMEWORKS. A plain
        # find_library(EGL/GL) resolves NOTFOUND there and poisons the link
        # line - the CI generate step failed exactly on that (the shared
        # else() branch was X11/Mesa-shaped, as our primary target is Linux).
        enable_language(OBJCXX)
        find_library(COCOA_LIBRARY Cocoa REQUIRED)
        find_library(OPENGLF_LIBRARY OpenGL REQUIRED)
        set(MUHAMMAD_GL_LIBS ${COCOA_LIBRARY} ${OPENGLF_LIBRARY})
    else()
        find_library(EGL_LIBRARY EGL)
        find_library(GL_LIBRARY GL)
        find_package(X11 MODULE)
        set(MUHAMMAD_GL_LIBS ${GL_LIBRARY} ${EGL_LIBRARY} ${CMAKE_DL_LIBS})
    endif()
    add_library(nanovg STATIC ${NANOVG_DIR}/gl3_impl.cpp ${NANOVG_DIR}/src/nanovg.c)
    target_include_directories(nanovg PUBLIC ${NANOVG_DIR}/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(nanovg PRIVATE -w)
    elseif(MSVC)
        target_compile_options(nanovg PRIVATE /W0)
    endif()
    target_compile_definitions(nanovg PUBLIC GL_GLEXT_PROTOTYPES=1)

    add_library(p5_gui STATIC src/p5_gui.cpp)
    target_include_directories(p5_gui PUBLIC src ${NANOVG_DIR}/src)
    target_link_libraries(p5_gui PUBLIC nanovg)
    if(WIN32)
        # put opengl32/gdi32 AFTER libnanovg.a on the link line: ld is
        # single-pass and nanovg references the GL 1.1 import symbols.
        target_link_libraries(nanovg PUBLIC ${MUHAMMAD_GL_LIBS})
    endif()

    if(WIN32)
        set(GUI_PLUGIN_SOURCES src/gui_win32.cpp src/gui_core.cpp)
        set(GUI_DEFINES P5_GUI=1)
    elseif(APPLE)
        set(GUI_PLUGIN_SOURCES src/gui_mac.mm src/gui_core.cpp)
        set(GUI_DEFINES P5_GUI=1)
    elseif(X11_FOUND)
        set(GUI_PLUGIN_SOURCES src/gui_x11.cpp src/gui_core.cpp)
        set(GUI_DEFINES P5_GUI=1)
    endif()

    # headless golden-image test (see tools/check_goldens.sh)
if(UNIX AND NOT APPLE)   # goldens need Mesa/EGL surfaces: Linux only
# (macOS has no EGL at all - neither headers nor runtime)
# NOTE: the AVX2 twin used to live INSIDE this branch (review B7) - it is
# now a top-level option, so Windows/macOS/Linux all get it where valid.

add_executable(render_golden tests/render_golden.cpp)
    target_include_directories(render_golden PRIVATE src vendor/clap external ${NANOVG_DIR}/src
                                     external/stb ${X11_INCLUDE_DIR})
    target_compile_definitions(render_golden PRIVATE GL_GLEXT_PROTOTYPES=1 P5_BUILD_INFO="${P5_BUILD_INFO}")
    target_link_libraries(render_golden PRIVATE p5_engine p5_gui ${MUHAMMAD_GL_LIBS} ${MATH_LIBRARY})
endif()
    endif()

# macOS: NSOpenGL/AppKit deprecation notes scroll ~5 per build and drown real
# diagnostics in the runner log (and the wrapper tree gets them via its own
# scope, so set it for the repo build here). Harmless elsewhere.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
    # GL_SILENCE_DEPRECATION covers <OpenGL/*.h>, NOT AppKit's NSOpenGL*/
    # NSView legacy surface declarations (proven by the 2026-10-21 mac leg:
    # the warning wall survived it). The cocoa shim deliberately uses the
    # legacy NSOpenGLContext path until a Metal rewrite - silence THAT file's
    # deprecation noise specifically, nowhere else, so real diagnostics stay.
    if(GUI_PLUGIN_SOURCES)
        set_source_files_properties(src/gui_mac.mm PROPERTIES
            COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wno-deprecated-declarations>")
    endif()
endif()

# ---- the CLAP plugin ----------------------------------------------------------
add_library(muhammad_clap MODULE
    src/clap_entry.cpp
    src/muhammad_plugin.cpp
    src/muhammad_discovery.cpp
    src/p5_store.cpp
    ${GUI_PLUGIN_SOURCES}
)
target_link_libraries(muhammad_clap PRIVATE p5_engine)
target_include_directories(muhammad_clap PRIVATE src vendor/clap external ${X11_INCLUDE_DIR})
if(WIN32 OR (UNIX AND NOT APPLE))
    # ship a self-contained C++ runtime: the .clap must load on boxes without
    # a matching libstdc++ (review B2; the bare-Windows branch already did)
    target_link_options(muhammad_clap PRIVATE -static-libgcc -static-libstdc++)
endif()
target_compile_definitions(muhammad_clap PRIVATE P5_BUILD_INFO="${P5_BUILD_INFO}" P5_VERSION="${P5_VERSION}")
if(GUI_DEFINES)
    target_compile_definitions(muhammad_clap PRIVATE ${GUI_DEFINES})
    target_link_libraries(muhammad_clap PRIVATE p5_gui ${X11_LIBRARIES} ${MUHAMMAD_GL_LIBS})
endif()
if(WIN32)
# MinGW MODULE targets ignore SHARED_LINKER_FLAGS_INIT; bake the C++ runtime in so
# muhammad.clap loads on a bare Windows box (kernel32/msvcrt/advapi only).
target_link_options(muhammad_clap PRIVATE -static -static-libgcc -static-libstdc++)
endif()
set_target_properties(muhammad_clap PROPERTIES
    PREFIX ""
    OUTPUT_NAME "muhammad"
    SUFFIX ".clap"
)
if(MATH_LIBRARY)
    target_link_libraries(muhammad_clap PRIVATE ${MATH_LIBRARY})
endif()

# ---- engine A/B test (scalar reference vs the SIMD kernel) ---------------------
# ---- ThreadSanitizer stress (opt-in; CI job `tsan`) ----------------------------
option(MUHAMMAD_TSAN "build test_threadsan under ThreadSanitizer" OFF)
if(MUHAMMAD_TSAN AND NOT WIN32)
    add_executable(test_threadsan tests/test_threadsan.cpp
        src/muhammad_plugin.cpp src/p5_store.cpp)
    target_link_libraries(test_threadsan PRIVATE p5_engine)
    target_include_directories(test_threadsan PRIVATE src vendor/clap external)
    target_compile_definitions(test_threadsan PRIVATE P5_BUILD_INFO="tsan" P5_VERSION="tsan")
    target_compile_options(test_threadsan PRIVATE -fsanitize=thread)
    target_link_options(test_threadsan PRIVATE -fsanitize=thread)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(test_threadsan PRIVATE -no-pie)   # TSan shadow vs PIE mmaps
    endif()
endif()

add_executable(test_splitinv tests/test_splitinv.cpp)
target_link_libraries(test_splitinv PRIVATE p5_engine)
target_include_directories(test_splitinv PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(test_splitinv PRIVATE ${MATH_LIBRARY})
endif()

add_executable(test_engine_ab tests/test_engine_ab.cpp)
target_link_libraries(test_engine_ab PRIVATE p5_engine)
target_include_directories(test_engine_ab PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(test_engine_ab PRIVATE ${MATH_LIBRARY})
endif()

# ---- benchmark -----------------------------------------------------------------
add_executable(test_quantnoise tests/test_quantnoise.cpp)
target_link_libraries(test_quantnoise PRIVATE p5_engine)
target_include_directories(test_quantnoise PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(test_quantnoise PRIVATE ${MATH_LIBRARY})
endif()

if(NOT WIN32)   # bench uses POSIX clock_gettime(CLOCK_MONOTONIC)
add_executable(bench_engine tests/bench_engine.cpp)
target_link_libraries(bench_engine PRIVATE p5_engine)
target_include_directories(bench_engine PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(bench_engine PRIVATE ${MATH_LIBRARY})
endif()
endif()

# ---- firmware cross-check ------------------------------------------------------
# No firmware checkout is consulted by the build, ever. The firmware-comparison
# harness (ref_dump against melodee's hostsim) lives OUTSIDE this project:
# it is run from the melodee tree by hand during validation sessions, not
# configured here. The generated tables under src/ are committed artifacts.


add_executable(render_demo tests/render_demo.cpp)
target_link_libraries(render_demo PRIVATE p5_engine)
target_include_directories(render_demo PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(render_demo PRIVATE ${MATH_LIBRARY})
endif()

# ---- audio-thread allocation watchdog (LD_PRELOAD shim for clap_smoke) --------
if(NOT WIN32)
add_library(allocwatch SHARED tools/allocwatch.c)
set_target_properties(allocwatch PROPERTIES PREFIX ""
    C_VISIBILITY_PRESET default)   # it is LD_PRELOADED: aw_count/aw_blocks must
                                   # be dynamically visible despite the global
                                   # hidden default (B6)
endif()

find_library(X11_LIBRARY X11)
find_path(X11_PROBE_HEADER X11/Xlib.h)
if(X11_LIBRARY AND X11_PROBE_HEADER AND NOT APPLE)
    # gui_probe is the Linux X11 dev harness. The mac runners have an X11
    # dylib lying around (Homebrew/XQuartz) that fools find_library alone
    # while the headers stay invisible - CI arm64 died exactly there.
    add_executable(gui_probe tests/gui_probe.cpp)
    target_include_directories(gui_probe PRIVATE src vendor/clap)
    target_link_libraries(gui_probe PRIVATE ${X11_LIBRARY} ${CMAKE_DL_LIBS} m)
endif()

if(MUHAMMAD_GUI AND NOT WIN32)   # links the X11-era GL panel lib
add_executable(test_tune tests/test_tune.cpp src/p5_store.cpp)
target_include_directories(test_tune PRIVATE src external ${NANOVG_DIR}/src ${NANOVG_DIR})
target_link_libraries(test_tune PRIVATE p5_gui nanovg)
add_test(NAME tune COMMAND test_tune)   # registration lives WITH the target's
set_tests_properties(tune PROPERTIES    # guard - an unconditional add_test
    ENVIRONMENT "P5_STORE_DIR=${CMAKE_BINARY_DIR}/ctest-store")   # poisons every GUI=OFF ctest
endif()

add_executable(test_mpe tests/test_mpe.cpp ${ENGINE_SOURCES} src/p5_store.cpp)
target_link_libraries(test_mpe PRIVATE p5_engine)
target_include_directories(test_mpe PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(test_mpe PRIVATE ${MATH_LIBRARY})
endif()

add_executable(test_trimdrift tests/test_trimdrift.cpp)
target_link_libraries(test_trimdrift PRIVATE p5_engine)
target_include_directories(test_trimdrift PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(test_trimdrift PRIVATE ${MATH_LIBRARY})
endif()

# ---- patch store unit test (headless; P5_STORE_DIR override) -----------------
if(NOT WIN32)
add_executable(test_store tests/test_store.cpp src/p5_store.cpp)
target_include_directories(test_store PRIVATE src)
endif()

# ---- ctest registration (review B10: a new test previously only ran if
# someone hand-edited the workflow YAML) --------------------------------------
enable_testing()
add_test(NAME engine_ab COMMAND test_engine_ab)
add_test(NAME engine_ab_sse2 COMMAND test_engine_ab)     # the SHIPPED portable wide kernel
set_tests_properties(engine_ab_sse2 PROPERTIES ENVIRONMENT "P5_KERNEL=sse2")
add_test(NAME splitinv COMMAND test_splitinv)
add_test(NAME mpe COMMAND test_mpe)
add_test(NAME trimdrift COMMAND test_trimdrift)
add_test(NAME quantnoise COMMAND test_quantnoise)
add_test(NAME store COMMAND test_store)
set_tests_properties(store PROPERTIES ENVIRONMENT "P5_STORE_DIR=${CMAKE_BINARY_DIR}/ctest-store")
set_tests_properties(mpe PROPERTIES ENVIRONMENT "P5_STORE_DIR=${CMAKE_BINARY_DIR}/ctest-store")
if(NOT WIN32 AND MUHAMMAD_GUI)
# clap_smoke asserts the SHIPPED shape (GUI extension offered, X11/cocoa
# api strings); a MUHAMMAD_GUI=OFF build legitimately declines them, so
# register the smoke test only where it is the product.
add_test(NAME smoke COMMAND clap_smoke $<TARGET_FILE:muhammad_clap>)
set_tests_properties(smoke PROPERTIES
    ENVIRONMENT "P5_STORE_DIR=${CMAKE_BINARY_DIR}/ctest-store"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
if(MUHAMMAD_TSAN)
    add_test(NAME threadsan COMMAND test_threadsan)
    set_tests_properties(threadsan PROPERTIES ENVIRONMENT "P5_STORE_DIR=${CMAKE_BINARY_DIR}/ctest-store")
endif()
endif()

# ---- CLAP smoke test: dlopens the built plugin and drives its surface --------
if(NOT WIN32)
add_executable(clap_smoke tests/clap_smoke.cpp)
target_include_directories(clap_smoke PRIVATE vendor/clap)
target_link_libraries(clap_smoke PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(clap_smoke muhammad_clap)
endif()
