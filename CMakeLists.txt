cmake_minimum_required(VERSION 3.18)

# RTX 4090 = SM 89 (Ada Lovelace), native compilation with CUDA 12.8
# Must be set before project() so CMake doesn't override with defaults
set(CMAKE_CUDA_ARCHITECTURES 89)

project(SAT_Parallel LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------- build type ----------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -fsanitize=address")

message(STATUS "CUDA architectures: ${CMAKE_CUDA_ARCHITECTURES}")

# ---------- threads ----------
find_package(Threads REQUIRED)

# ---------- CUDA ----------
find_package(CUDAToolkit REQUIRED)
message(STATUS "CUDA version: ${CUDAToolkit_VERSION}")
message(STATUS "CUDA include: ${CUDAToolkit_INCLUDE_DIRS}")

# ---------- yaml-cpp (from deps/local) ----------
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/deps/local")
find_package(yaml-cpp REQUIRED)
message(STATUS "yaml-cpp found: ${yaml-cpp_DIR}")

# ---------- CaDiCaL (Painless-modified, supports clause import) ----------
set(CADICAL_DIR "${CMAKE_SOURCE_DIR}/deps/painless/solvers/cadical")
set(CADICAL_INCLUDE "${CADICAL_DIR}/src")
set(CADICAL_LIB "${CADICAL_DIR}/build/libcadical.a")

if(NOT EXISTS "${CADICAL_LIB}")
    message(FATAL_ERROR "CaDiCaL library not found at ${CADICAL_LIB}\n"
                        "Run: bash scripts/setup_env.sh")
endif()
message(STATUS "CaDiCaL (Painless): ${CADICAL_LIB}")

add_library(cadical STATIC IMPORTED)
set_target_properties(cadical PROPERTIES
    IMPORTED_LOCATION "${CADICAL_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${CADICAL_INCLUDE}"
)

# ---------- dsrg_core static library ----------
file(GLOB_RECURSE CORE_SRC src/core/*.cpp)
if(CORE_SRC)
    add_library(dsrg_core STATIC ${CORE_SRC})
    target_include_directories(dsrg_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(dsrg_core PUBLIC yaml-cpp::yaml-cpp)
    message(STATUS "dsrg_core sources: ${CORE_SRC}")
else()
    message(STATUS "No core sources yet — dsrg_core skipped")
endif()

# ---------- gpu_lib static library ----------
file(GLOB_RECURSE GPU_LIB_SRC src/gpu/*.cu)
if(GPU_LIB_SRC AND TARGET dsrg_core)
    add_library(gpu_lib STATIC ${GPU_LIB_SRC})
    target_include_directories(gpu_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(gpu_lib PUBLIC dsrg_core CUDA::cudart CUDA::curand Threads::Threads yaml-cpp::yaml-cpp)
    # OFF = no device link step needed when linking with g++ (e.g. Painless Makefile)
    set_target_properties(gpu_lib PROPERTIES CUDA_SEPARABLE_COMPILATION OFF)
    message(STATUS "gpu_lib sources: ${GPU_LIB_SRC}")
endif()

# ---------- testing ----------
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

if(TARGET dsrg_core)
    add_executable(dsrg_test tests/core/dsrg_test.cpp)
    target_link_libraries(dsrg_test PRIVATE dsrg_core GTest::gtest_main)
    set_target_properties(dsrg_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(NAME DSRGUnitTests COMMAND dsrg_test)

    add_executable(centrality_test tests/core/centrality_test.cpp)
    target_link_libraries(centrality_test PRIVATE dsrg_core GTest::gtest_main)
    set_target_properties(centrality_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    add_test(NAME CentralityUnitTests COMMAND centrality_test)

    add_executable(dsrg_bench tests/core/dsrg_bench.cpp)
    target_link_libraries(dsrg_bench PRIVATE dsrg_core)
    set_target_properties(dsrg_bench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    if(TARGET gpu_lib)
        add_executable(gpu_test tests/gpu/gpu_test.cu)
        target_include_directories(gpu_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(gpu_test PRIVATE gpu_lib GTest::gtest_main)
        set_target_properties(gpu_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            CUDA_SEPARABLE_COMPILATION ON
        )
        add_test(NAME GPUUnitTests COMMAND gpu_test)
    endif()
endif()
