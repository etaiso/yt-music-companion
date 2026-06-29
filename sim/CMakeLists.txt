cmake_minimum_required(VERSION 3.16)
project(ytm_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Use our sim lv_conf.h (placed on the include path; LVGL finds it by name).
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)

# Build-time theme: DARK (default) or LIGHT. Dark is also the fallback if unset
# (styles.h resolves to Dark unless YTM_THEME_LIGHT is defined). Build Light with
#   cmake -B build -DYTM_THEME=LIGHT
set(YTM_THEME "DARK" CACHE STRING "UI theme: DARK or LIGHT")
string(TOUPPER "${YTM_THEME}" YTM_THEME)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/mock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/mdi_solid.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui/mdi_line.c
)

# lv_conf.h lives here; both our code and lvgl must see it on the include path.
target_include_directories(ytm_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui
)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(YTM_THEME STREQUAL "LIGHT")
    target_compile_definitions(ytm_sim PRIVATE YTM_THEME_LIGHT)
    message(STATUS "ytm_sim theme: Light")
else()
    message(STATUS "ytm_sim theme: Dark (default)")
endif()

# lvgl compiles its own SDL driver, so its target also needs the SDL2 headers.
# Modern SDL2 ships a CMake config that exposes the imported target SDL2::SDL2
# (which carries include dirs) instead of populating ${SDL2_INCLUDE_DIRS}.
target_link_libraries(lvgl PUBLIC SDL2::SDL2)

target_link_libraries(ytm_sim PRIVATE lvgl SDL2::SDL2 m)
