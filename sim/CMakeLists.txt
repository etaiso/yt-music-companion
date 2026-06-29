cmake_minimum_required(VERSION 3.16)
project(ytm_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Use our sim lv_conf.h (placed on the include path; LVGL finds it by name).
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)

include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.2.2          # v9 line, matches the device target
)
FetchContent_MakeAvailable(lvgl)

find_package(SDL2 REQUIRED)

add_executable(ytm_sim
    main_sim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/now_playing_screen.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/ring_visualizer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/palette.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/mock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/mdi_solid.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/mdi_line.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/inter_extrabold_29.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/inter_extrabold_12.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/inter_semibold_17.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/inter_semibold_13.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/inter_semibold_12.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/inter_regular_12.c
)

# lv_conf.h lives here; both our code and lvgl must see it on the include path.
target_include_directories(ytm_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui
)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# lvgl compiles its own SDL driver, so its target also needs the SDL2 headers.
# Modern SDL2 ships a CMake config that exposes the imported target SDL2::SDL2
# (which carries include dirs) instead of populating ${SDL2_INCLUDE_DIRS}.
target_link_libraries(lvgl PUBLIC SDL2::SDL2)

target_link_libraries(ytm_sim PRIVATE lvgl SDL2::SDL2 m)
