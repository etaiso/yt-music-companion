# The portable UI lives in ../../ui and is shared with the desktop simulator.
set(UI_DIR ${CMAKE_CURRENT_LIST_DIR}/../../ui)

idf_component_register(
    SRCS "main.c"
         "net_backend.c"
         "${UI_DIR}/now_playing_screen.c"
         "${UI_DIR}/ring_visualizer.c"
         "${UI_DIR}/ambient_glow.c"
         "${UI_DIR}/ambient_paint.c"
         "${UI_DIR}/palette.c"
         "${UI_DIR}/mock.c"
         "${UI_DIR}/mdi_solid.c"
         "${UI_DIR}/mdi_line.c"
         "${UI_DIR}/inter_extrabold_29.c"
         "${UI_DIR}/inter_extrabold_12.c"
         "${UI_DIR}/inter_semibold_17.c"
         "${UI_DIR}/inter_semibold_13.c"
         "${UI_DIR}/inter_semibold_12.c"
         "${UI_DIR}/inter_regular_12.c"
    INCLUDE_DIRS "." "${UI_DIR}"
    REQUIRES esp32_s3_touch_amoled_2_16
             esp_websocket_client esp_wifi mdns json nvs_flash esp_netif esp_event
)

# Generated fonts (ui/mdi_*.c, ui/inter_*.c) use the lv_font_conv include
# guard: with this defined they pull "lvgl.h" (the IDF lvgl component header)
# instead of the unresolvable "lvgl/lvgl.h".
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
