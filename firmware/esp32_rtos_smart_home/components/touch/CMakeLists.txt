idf_component_register(SRCS "touch.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_spi esp_lcd espressif__esp_lcd_touch atanisoft__esp_lcd_touch_xpt2046)
