idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES lvgl esp_lcd driver esp_timer esp_lcd_touch esp_lcd_touch_gt911 espressif__usb_host_msc
    PRIV_REQUIRES freertos heap 
)

