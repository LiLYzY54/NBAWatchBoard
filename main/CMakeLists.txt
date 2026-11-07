# NBA 看板应用的源文件列表。
#
# 基线硬件自检 demo(demo_*.c、ui_pixel*.c)保留在 main/ 下作为 BSP 用法参考,
# 但不编进本应用:本固件开机直接进入看板。它们仍由 tests/ 下的主机测试单独编译。
idf_component_register(
    SRCS "main.c"
         "nba_app.c"
         "nba_api.c"
         "nba_box.c"
         "nba_console.c"
         "nba_format.c"
         "nba_json.c"
         "nba_parse.c"
         "nba_prov.c"
         "nba_ui.c"
         "wifi_mgr.c"
    INCLUDE_DIRS "."
    REQUIRES bsp console esp_event esp_http_client esp_http_server esp_netif esp_timer esp_wifi nvs_flash
)

# 中文字体子集(由 assets/fonts/app_font_cjk_16.c 提供,见 assets/README.md)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_cjk_16.c"
)
