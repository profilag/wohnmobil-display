idf_component_register(
    SRCS "bsp_ws_lcd7.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
        esp_lcd
        esp_timer
        ch422g
        aw9523b
        espressif__esp_lcd_touch_gt911
        espressif__esp_lvgl_port
        lvgl__lvgl
)
