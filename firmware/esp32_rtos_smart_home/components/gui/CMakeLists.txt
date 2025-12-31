idf_component_register(SRCS "gui.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd freertos lvgl__lvgl espressif__esp_lcd_touch)
