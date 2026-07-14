cmake_minimum_required(VERSION 3.20)
project(MarketXray_Engine VERSION 2.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

include_directories(src)

add_executable(market_xray
    src/main.cpp
)

add_executable(market_xray_ws
    src/main_ws.cpp
)

if(BUILD_TESTING)
    add_executable(engine_regression_tests
        tests/engine_regression_tests.cpp
    )
    add_test(NAME engine_regression_tests COMMAND engine_regression_tests)
endif()

if(WIN32)
    target_link_libraries(market_xray    PRIVATE kernel32)
    target_link_libraries(market_xray_ws PRIVATE kernel32 ws2_32)
elseif(UNIX)
    target_link_libraries(market_xray    PRIVATE rt pthread)
    target_link_libraries(market_xray_ws PRIVATE rt pthread)
    if(BUILD_TESTING)
        target_link_libraries(engine_regression_tests PRIVATE rt pthread)
    endif()
endif()
