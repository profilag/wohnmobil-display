idf_component_register(
    SRCS "main.c" "water_level.c" "waste_level.c"
         "batt_history.c" "ble_host.c" "ble_scan.c" "led_ble.c" "ble_probe.c" "victron_ble.c" "web.c"
         "fonts/font_sun_20.c" "fonts/font_waste_28.c" "fonts/font_ui_14.c" "fonts/font_ui_20.c" "fonts/font_ui_28.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES bsp_ws_lcd7 nvs_flash esp_timer bt mbedtls
             esp_wifi esp_netif esp_event esp_http_server
)
