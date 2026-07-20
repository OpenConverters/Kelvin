cmake_minimum_required(VERSION 3.15)
project(Kelvin VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Match Kirchhoff's WASM exception recipe so libKelvin can share a worker with kirchhoff.wasm
# (native wasm EH, NOT the legacy JS -fexceptions — the two cannot be mixed in one module).
if (EMSCRIPTEN)
    add_compile_options(-fwasm-exceptions)
    add_link_options(-fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
endif()

# Top-level vs embedded (add_subdirectory from Kirchhoff). When embedded, the parent already
# provides nlohmann_json / Catch2 / pybind11, and only the `kelvin` static lib is wanted.
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(KELVIN_TOP TRUE)
else()
    set(KELVIN_TOP FALSE)
endif()

include(FetchContent)
if (NOT TARGET nlohmann_json::nlohmann_json)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ---- core static lib -------------------------------------------------------
# KelvinApi.cpp is compiled separately into the pybind/embind targets (mirrors Kirchhoff:
# it can also go into a hidden-visibility shared lib without a duplicate symbol) — but it has
# no MAS-type clash, so we keep it in the core lib too for the native test binaries.
# PEAS provides the canonical resolve_dimensional_values (json overload). Header-only; a git
# submodule under deps/PEAS at deploy time. Overridable with -DKELVIN_PEAS_DIR for co-dev.
set(KELVIN_PEAS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/PEAS/src" CACHE PATH "PEAS headers (DimensionJson.hpp)")
if (NOT EXISTS "${KELVIN_PEAS_DIR}/DimensionJson.hpp")
    foreach(_cand "/home/alf/PSMA/PEAS/src" "${CMAKE_CURRENT_SOURCE_DIR}/../Kirchhoff/deps/PEAS/src")
        if (EXISTS "${_cand}/DimensionJson.hpp")
            set(KELVIN_PEAS_DIR "${_cand}")
            break()
        endif()
    endforeach()
endif()
if (NOT EXISTS "${KELVIN_PEAS_DIR}/DimensionJson.hpp")
    message(FATAL_ERROR "PEAS headers not found (need DimensionJson.hpp). Init deps/PEAS or set -DKELVIN_PEAS_DIR.")
endif()

add_library(kelvin STATIC
    src/Views.cpp
    src/Index.cpp
    src/Requirements.cpp
    src/Select.cpp
    src/RecordFetcher.cpp
    src/KelvinApi.cpp)
target_include_directories(kelvin PUBLIC src)
target_include_directories(kelvin SYSTEM PUBLIC "${KELVIN_PEAS_DIR}")
target_link_libraries(kelvin PUBLIC nlohmann_json::nlohmann_json)
target_compile_definitions(kelvin PUBLIC _USE_MATH_DEFINES)
if (NOT MSVC)
    target_compile_options(kelvin PRIVATE -Wall -Wextra)
endif()

# ---- kelvin-index CLI (offline / nightly index builder) --------------------
if (NOT EMSCRIPTEN AND KELVIN_TOP)
    add_executable(kelvin-index tools/kelvin_index_main.cpp)
    target_link_libraries(kelvin-index PRIVATE kelvin)
endif()

# ---- Catch2 tests ----------------------------------------------------------
option(KELVIN_BUILD_TESTS "Build the Catch2 test suite" ON)
if (KELVIN_BUILD_TESTS AND NOT EMSCRIPTEN AND KELVIN_TOP)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1)
    FetchContent_MakeAvailable(Catch2)

    add_executable(test_kelvin
        tests/test_index.cpp
        tests/test_select.cpp
        tests/test_requirements.cpp
        tests/test_parity.cpp
        tests/test_determinism.cpp
        tests/test_components.cpp
        tests/test_phase5.cpp
        tests/test_crossref_score.cpp
        tests/test_crossref_rank.cpp
        tests/test_crossref_dimensions.cpp
        tests/test_crossref_classes.cpp
        tests/test_crossref_fuzz.cpp
        tests/test_crossref_ranking_golden.cpp
        tests/test_crossref_golden.cpp
        tests/test_mfr_policy.cpp
        tests/test_browse.cpp)
    target_compile_definitions(test_kelvin PRIVATE
        KELVIN_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(test_kelvin PRIVATE kelvin Catch2::Catch2WithMain)
    if (NOT MSVC)
        target_compile_options(test_kelvin PRIVATE -Wall -Wextra)
    endif()
endif()

# ---- PyKelvin: pybind11 module (Heaviside consumes this) -------------------
option(KELVIN_BUILD_PYBIND "Build the PyKelvin pybind11 module" ON)
if (KELVIN_BUILD_PYBIND AND NOT EMSCRIPTEN AND KELVIN_TOP)
    FetchContent_Declare(pybind11
        GIT_REPOSITORY https://github.com/pybind/pybind11.git GIT_TAG v2.13.6)
    FetchContent_Declare(pybind11_json
        GIT_REPOSITORY https://github.com/pybind/pybind11_json.git GIT_TAG 0.2.15)
    FetchContent_MakeAvailable(pybind11 pybind11_json)
    pybind11_add_module(PyKelvin src/bindings.cpp)
    target_include_directories(PyKelvin PRIVATE "${pybind11_json_SOURCE_DIR}/include")
    target_link_libraries(PyKelvin PRIVATE kelvin)
endif()

# ---- libKelvin: the Emscripten/embind WASM module --------------------------
if (EMSCRIPTEN)
    add_executable(libKelvin src/libKelvin.cpp)
    target_link_libraries(libKelvin PRIVATE kelvin)
    set_target_properties(libKelvin PROPERTIES OUTPUT_NAME "kelvin")
    target_link_options(libKelvin PRIVATE
        "--bind"
        "-sMODULARIZE=1" "-sEXPORT_NAME=createKelvinModule"
        "-sENVIRONMENT=web,worker,node" "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORT_ES6=1" "-sSINGLE_FILE=1")
endif()
