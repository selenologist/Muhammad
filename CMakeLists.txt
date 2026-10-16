# Prophet CLAP: the melodee Prophet-5 engine, vectorized with SIMDe.
cmake_minimum_required(VERSION 3.16)
project(ProphetClap LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# SIMD width is chosen by SIMDe for the target; the SIMD kernel must not let the
# compiler fuse mul+add, or the scalar reference and the vector kernel drift.
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
set(P5_VERSION "1.9.4-panwidth")
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
option(PROPHET_GUI "Build the NanoVG panel (needs OpenGL; X11 embedding on Linux)" ON)
set(NANOVG_DIR external/nanovg)
if(PROPHET_GUI)
    find_library(EGL_LIBRARY EGL)
    find_library(GL_LIBRARY GL)
    find_package(X11 MODULE)
    add_library(nanovg STATIC ${NANOVG_DIR}/gl3_impl.cpp ${NANOVG_DIR}/src/nanovg.c)
    target_include_directories(nanovg PUBLIC ${NANOVG_DIR}/src)
    target_compile_options(nanovg PRIVATE -w)
    target_compile_definitions(nanovg PUBLIC GL_GLEXT_PROTOTYPES=1)
    set(PROPHET_GL_LIBS ${GL_LIBRARY} ${EGL_LIBRARY} ${CMAKE_DL_LIBS})

    add_library(p5_gui STATIC src/p5_gui.cpp)
    target_include_directories(p5_gui PUBLIC src ${NANOVG_DIR}/src)
    target_link_libraries(p5_gui PUBLIC nanovg)

    if(X11_FOUND)
        set(GUI_PLUGIN_SOURCES src/gui_x11.cpp)
        set(GUI_DEFINES P5_GUI=1)
    endif()

    # headless golden-image test (see tools/check_goldens.sh)
    add_executable(render_golden tests/render_golden.cpp ${ENGINE_SOURCES})
    target_include_directories(render_golden PRIVATE src vendor/clap external ${NANOVG_DIR}/src
                                     external/stb ${X11_INCLUDE_DIR})
    target_compile_definitions(render_golden PRIVATE GL_GLEXT_PROTOTYPES=1 P5_BUILD_INFO="${P5_BUILD_INFO}")
    target_link_libraries(render_golden PRIVATE p5_gui ${PROPHET_GL_LIBS} ${MATH_LIBRARY})
endif()

# ---- the CLAP plugin ----------------------------------------------------------
add_library(prophet_clap MODULE
    src/clap_entry.cpp
    src/prophet_plugin.cpp
    src/p5_prophet_discovery.cpp
    src/p5_store.cpp
    ${GUI_PLUGIN_SOURCES}
    ${ENGINE_SOURCES}
)
target_include_directories(prophet_clap PRIVATE src vendor/clap external ${X11_INCLUDE_DIR})
if(GUI_DEFINES)
    target_compile_definitions(prophet_clap PRIVATE ${GUI_DEFINES} P5_BUILD_INFO="${P5_BUILD_INFO}")
    target_link_libraries(prophet_clap PRIVATE p5_gui ${X11_LIBRARIES} ${PROPHET_GL_LIBS})
endif()
set_target_properties(prophet_clap PROPERTIES
    PREFIX ""
    OUTPUT_NAME "muhammad"
    SUFFIX ".clap"
)
if(MATH_LIBRARY)
    target_link_libraries(prophet_clap PRIVATE ${MATH_LIBRARY})
endif()

# ---- engine A/B test (scalar reference vs the SIMD kernel) ---------------------
add_executable(test_engine_ab tests/test_engine_ab.cpp ${ENGINE_SOURCES})
target_include_directories(test_engine_ab PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(test_engine_ab PRIVATE ${MATH_LIBRARY})
endif()

# ---- benchmark -----------------------------------------------------------------
add_executable(bench_engine tests/bench_engine.cpp ${ENGINE_SOURCES})
target_include_directories(bench_engine PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(bench_engine PRIVATE ${MATH_LIBRARY})
endif()

# ---- firmware cross-check ------------------------------------------------------
# ref_dump renders the same scripts through melodee's hostsim (the firmware's own
# host build). Built only when a melodee checkout is given:
#   cmake -DMELODEE_DIR=/path/to/melodee ...
if(DEFINED MELODEE_DIR AND EXISTS "${MELODEE_DIR}/tests/hostsim.c")
    add_executable(ref_dump tests/ref_dump.c)
    target_include_directories(ref_dump PRIVATE
        "${MELODEE_DIR}/build/gen" "${MELODEE_DIR}/firmware/src")
    target_compile_definitions(ref_dump PRIVATE
        MELODEE_HOSTSIM_C="${MELODEE_DIR}/tests/hostsim.c")
    target_compile_options(ref_dump PRIVATE -w)
    if(MATH_LIBRARY)
        target_link_libraries(ref_dump PRIVATE ${MATH_LIBRARY})
    endif()
endif()

add_executable(render_demo tests/render_demo.cpp ${ENGINE_SOURCES})
target_include_directories(render_demo PRIVATE src external)
if(MATH_LIBRARY)
    target_link_libraries(render_demo PRIVATE ${MATH_LIBRARY})
endif()

# ---- audio-thread allocation watchdog (LD_PRELOAD shim for clap_smoke) --------
add_library(allocwatch SHARED tools/allocwatch.c)
set_target_properties(allocwatch PROPERTIES PREFIX "")

# ---- patch store unit test (headless; P5_STORE_DIR override) -----------------
add_executable(test_store tests/test_store.cpp src/p5_store.cpp)
target_include_directories(test_store PRIVATE src)

# ---- CLAP smoke test: dlopens the built plugin and drives its surface --------
add_executable(clap_smoke tests/clap_smoke.cpp)
target_include_directories(clap_smoke PRIVATE vendor/clap)
target_link_libraries(clap_smoke PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(clap_smoke prophet_clap)
