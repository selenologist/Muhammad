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
include_directories(vendor/tuning-library/include)   # header-only, MIT: tuning-library
# the library needs C++20 concepts; grant them to the one TU that includes it
set_source_files_properties(src/p5_store.cpp PROPERTIES COMPILE_OPTIONS "-std=gnu++20")
set(PP_CXX_FLAGS "-O3 -ffp-contract=off")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${PP_CXX_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${PP_CXX_FLAGS}")

find_library(MATH_LIBRARY m)

# Build stamp: version + git hash + dirty marker + time, shown in the panel
# corner and the descriptor, so a running binary identifies itself.
find_program(GIT_EXE git)
execute_process(COMMAND ${GIT_EXE} rev-parse --short HEAD OUTPUT_VARIABLE P5_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND ${GIT_EXE} status --porcelain OUTPUT_VARIABLE P5_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(TIMESTAMP P5_BUILDDATE "%Y-%m-%d %H:%M")
set(P5_VERSION "0.2.1")
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
    target_compile_options(nanovg PRIVATE -w)
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
    # x86 ships the wide kernel twice: portable baseline + true AVX2 (SIMDe maps to
# native instructions under -mavx2 -mfma). Engine::activate selects by cpuid;
# contract stays OFF so both paths are bit-identical to the firmware reference.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i386|i686")
    add_library(p5_simd_avx2 OBJECT src/p5_kernel_simd.cpp)
    target_include_directories(p5_simd_avx2 PRIVATE src external)
    target_compile_definitions(p5_simd_avx2 PRIVATE P5_SIMD_SYMBOL=p5KernelSimdAVX2)
    target_compile_options(p5_simd_avx2 PRIVATE -mavx2 -mfma)
    add_compile_definitions(P5_HAVE_AVX2)
    list(APPEND ENGINE_SOURCES $<TARGET_OBJECTS:p5_simd_avx2>)
endif()

add_executable(render_golden tests/render_golden.cpp ${ENGINE_SOURCES})
    target_include_directories(render_golden PRIVATE src vendor/clap external ${NANOVG_DIR}/src
                                     external/stb ${X11_INCLUDE_DIR})
    target_compile_definitions(render_golden PRIVATE GL_GLEXT_PROTOTYPES=1 P5_BUILD_INFO="${P5_BUILD_INFO}")
    target_link_libraries(render_golden PRIVATE p5_gui ${MUHAMMAD_GL_LIBS} ${MATH_LIBRARY})
endif()
    endif()

# ---- the CLAP plugin ----------------------------------------------------------
add_library(muhammad_clap MODULE
    src/clap_entry.cpp
    src/muhammad_plugin.cpp
    src/muhammad_discovery.cpp
    src/p5_store.cpp
    ${GUI_PLUGIN_SOURCES}
    ${ENGINE_SOURCES}
)
target_include_directories(muhammad_clap PRIVATE src vendor/clap external ${X11_INCLUDE_DIR})
target_compile_definitions(muhammad_clap PRIVATE P5_BUILD_INFO="${P5_BUILD_INFO}" P5_VERSION="${P5_VERSION}")
if(GUI_DEFINES)
    target_compile_definitions(muhammad_clap PRIVATE ${GUI_DEFINES} P5_BUILD_INFO="${P5_BUILD_INFO}" P5_VERSION="${P5_VERSION}")
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
add_executable(test_splitinv tests/test_splitinv.cpp ${ENGINE_SOURCES})
target_include_directories(test_splitinv PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(test_splitinv PRIVATE ${MATH_LIBRARY})
endif()

add_executable(test_engine_ab tests/test_engine_ab.cpp ${ENGINE_SOURCES})
target_include_directories(test_engine_ab PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(test_engine_ab PRIVATE ${MATH_LIBRARY})
endif()

# ---- benchmark -----------------------------------------------------------------
add_executable(test_quantnoise tests/test_quantnoise.cpp ${ENGINE_SOURCES})
target_include_directories(test_quantnoise PRIVATE src external)
if(TARGET m)
    target_link_libraries(test_quantnoise PRIVATE m)
endif()

if(NOT WIN32)   # bench uses POSIX clock_gettime(CLOCK_MONOTONIC)
add_executable(bench_engine tests/bench_engine.cpp ${ENGINE_SOURCES})
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


add_executable(render_demo tests/render_demo.cpp ${ENGINE_SOURCES})
target_include_directories(render_demo PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(render_demo PRIVATE ${MATH_LIBRARY})
endif()

# ---- audio-thread allocation watchdog (LD_PRELOAD shim for clap_smoke) --------
if(NOT WIN32)
add_library(allocwatch SHARED tools/allocwatch.c)
set_target_properties(allocwatch PROPERTIES PREFIX "")
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
endif()

add_executable(test_mpe tests/test_mpe.cpp ${ENGINE_SOURCES} src/p5_store.cpp)
target_include_directories(test_mpe PRIVATE src external)
if(TARGET m)
    target_link_libraries(test_mpe PRIVATE m)
endif()

add_executable(test_trimdrift tests/test_trimdrift.cpp ${ENGINE_SOURCES})
target_include_directories(test_trimdrift PRIVATE src external)
if(TARGET m)
    target_link_libraries(test_trimdrift PRIVATE m)
endif()

# ---- patch store unit test (headless; P5_STORE_DIR override) -----------------
if(NOT WIN32)
add_executable(test_store tests/test_store.cpp src/p5_store.cpp)
target_include_directories(test_store PRIVATE src)
endif()

# ---- CLAP smoke test: dlopens the built plugin and drives its surface --------
if(NOT WIN32)
add_executable(clap_smoke tests/clap_smoke.cpp)
target_include_directories(clap_smoke PRIVATE vendor/clap)
target_link_libraries(clap_smoke PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(clap_smoke muhammad_clap)
endif()
