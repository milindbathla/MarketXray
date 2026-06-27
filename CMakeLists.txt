cmake_minimum_required(VERSION 3.20)
project(MarketXray_Engine VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------
# HFT Optimization Flags
# ---------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -O3              # Maximum optimization
        -march=native    # Use ALL CPU SIMD/AVX instructions
        -flto            # Link-Time Optimization
        -funroll-loops   # Unroll loops for speed
        -Wall -Wextra
    )
elseif(MSVC)
    add_compile_options(
        /O2              # Max speed
        /arch:AVX2       # AVX2 SIMD
        /GL              # Whole-program optimization (LTO)
        /W4
    )
    add_link_options(/LTCG)
endif()

# ---------------------------------------------------------------
# Include Directories
# ---------------------------------------------------------------
include_directories(src)

# ---------------------------------------------------------------
# Main Engine Executable
# ---------------------------------------------------------------
add_executable(market_xray
    src/main.cpp
)

# ---------------------------------------------------------------
# Windows: kernel32 for CreateFileMapping/MapViewOfFile (Shared Memory)
# Linux:   rt for shm_open/mmap, pthread for threading
# ---------------------------------------------------------------
if(WIN32)
    target_link_libraries(market_xray PRIVATE kernel32)
elseif(UNIX)
    target_link_libraries(market_xray PRIVATE rt pthread)
endif()

# ---------------------------------------------------------------
# OPTIONAL: Eigen3 (SIMD-accelerated math, e.g., for advanced OFI)
#   Windows: vcpkg install eigen3
#   Linux:   sudo apt-get install libeigen3-dev
# ---------------------------------------------------------------
# find_package(Eigen3 3.4 REQUIRED)
# target_link_libraries(market_xray PRIVATE Eigen3::Eigen)

# ---------------------------------------------------------------
# Build & Run Instructions:
#
#   mkdir build && cd build
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   cmake --build . --config Release
#
#   Benchmark mode (no Rust needed, pure speed test):
#     .\Release\market_xray.exe --benchmark        (Windows)
#     ./market_xray --benchmark                    (Linux)
#
#   Daemon mode (Rust ingestor must be running first):
#     .\Release\market_xray.exe --daemon           (Windows)
#     ./market_xray --daemon                       (Linux)
#     ./market_xray --daemon --shm /marketxray_shm (custom SHM name)
# ---------------------------------------------------------------
