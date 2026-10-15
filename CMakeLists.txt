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

set(ENGINE_SOURCES
    src/p5_engine.cpp
    src/p5_kernel_scalar.cpp
    src/p5_kernel_simd.cpp
)

# ---- the CLAP plugin ----------------------------------------------------------
add_library(prophet_clap MODULE
    src/clap_entry.cpp
    src/prophet_plugin.cpp
    src/p5_prophet_discovery.cpp
    ${ENGINE_SOURCES}
)
target_include_directories(prophet_clap PRIVATE src vendor/clap external)
set_target_properties(prophet_clap PROPERTIES
    PREFIX ""
    OUTPUT_NAME "prophet"
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
