cmake_minimum_required(VERSION 3.18)
project(SAT_Parallel LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------- build type ----------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -fsanitize=address")

# ---------- CUDA architecture ----------
# RTX 4090 = SM 89 (Ada Lovelace)
# Falls back to SM 86 PTX if older toolkit
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 86 89)
endif()
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

# ---------- CaDiCaL ----------
set(CADICAL_DIR "${CMAKE_SOURCE_DIR}/deps/cadical")
set(CADICAL_INCLUDE "${CADICAL_DIR}/src")
set(CADICAL_LIB "${CADICAL_DIR}/build/libcadical.a")

if(NOT EXISTS "${CADICAL_LIB}")
    message(FATAL_ERROR "CaDiCaL library not found at ${CADICAL_LIB}\n"
                        "Run: bash scripts/setup_env.sh")
endif()
message(STATUS "CaDiCaL: ${CADICAL_LIB}")

add_library(cadical STATIC IMPORTED)
set_target_properties(cadical PROPERTIES
    IMPORTED_LOCATION "${CADICAL_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${CADICAL_INCLUDE}"
)

# ---------- source groups ----------
file(GLOB_RECURSE CORE_SRC    src/core/*.cpp src/core/*.h)
file(GLOB_RECURSE MASTER_SRC  src/master/*.cpp src/master/*.h)
file(GLOB_RECURSE WORKER_SRC  src/worker/*.cpp src/worker/*.h)
file(GLOB_RECURSE COMM_SRC    src/comm/*.cpp src/comm/*.h)
file(GLOB_RECURSE SOLVER_SRC  src/solver/*.cpp src/solver/*.h)
file(GLOB_RECURSE GPU_SRC     src/gpu/*.cu src/gpu/*.cuh src/gpu/*.h)

set(ALL_CPP_SRC
    ${CORE_SRC} ${MASTER_SRC} ${WORKER_SRC}
    ${COMM_SRC} ${SOLVER_SRC}
)

# ---------- main executable ----------
if(ALL_CPP_SRC OR GPU_SRC)
    add_executable(sat_parallel
        ${ALL_CPP_SRC}
        ${GPU_SRC}
    )

    target_include_directories(sat_parallel PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CUDAToolkit_INCLUDE_DIRS}
    )

    target_link_libraries(sat_parallel PRIVATE
        cadical
        yaml-cpp::yaml-cpp
        Threads::Threads
        CUDA::cudart
        CUDA::curand
    )

    set_target_properties(sat_parallel PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
else()
    message(STATUS "No source files yet — skipping target (add .cpp/.cu files to src/)")
endif()
