# The portable UI lives in ../../ui and is shared with the desktop simulator.
set(UI_DIR ${CMAKE_CURRENT_LIST_DIR}/../../ui)

idf_component_register(
    SRCS "main.c"
         "${UI_DIR}/now_playing_screen.c"
         "${UI_DIR}/ring_visualizer.c"
         "${UI_DIR}/mock.c"
         "${UI_DIR}/mdi_solid.c"
         "${UI_DIR}/mdi_line.c"
    INCLUDE_DIRS "." "${UI_DIR}"
    REQUIRES esp32_s3_touch_amoled_2_16
)
