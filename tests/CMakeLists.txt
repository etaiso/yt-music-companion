# Host unit tests — the palette extractor is pure C, so this compiles the module
# standalone with NO LVGL, SDL, or panel. Build & run:
#
#   cmake -B build tests   &&   cmake --build build   &&   ctest --test-dir build
# or directly:
#   cc -std=c11 -I../ui tests/test_palette.c ../ui/palette.c -o test_palette && ./test_palette
cmake_minimum_required(VERSION 3.16)
project(ytm_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

add_executable(test_palette
    test_palette.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/palette.c
)
target_include_directories(test_palette PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../ui)

if(NOT MSVC)
    target_compile_options(test_palette PRIVATE -Wall -Wextra)
endif()

add_test(NAME palette COMMAND test_palette)

# Ambient-glow painter: also pure C (no LVGL), compiled standalone with palette.c.
add_executable(test_ambient
    test_ambient.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ambient_paint.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/palette.c
)
target_include_directories(test_ambient PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../ui)

if(NOT MSVC)
    target_compile_options(test_ambient PRIVATE -Wall -Wextra)
    target_link_libraries(test_ambient PRIVATE m)
endif()

add_test(NAME ambient COMMAND test_ambient)

# AXP2101 decode: pure C (no LVGL/ESP), compiled standalone.
add_executable(test_battery
    test_battery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/axp2101_decode.c
)
target_include_directories(test_battery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)

if(NOT MSVC)
    target_compile_options(test_battery PRIVATE -Wall -Wextra)
endif()

add_test(NAME battery COMMAND test_battery)
