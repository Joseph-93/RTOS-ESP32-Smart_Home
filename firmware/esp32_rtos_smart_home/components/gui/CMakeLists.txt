idf_component_register(SRCS "gui.cpp" "lcd/lcd.cpp" "touch/touch.cpp"
                       INCLUDE_DIRS "." "lcd" "touch"
                       REQUIRES common esp_lcd freertos lvgl__lvgl espressif__esp_lcd_touch esp_driver_gpio esp_driver_spi espressif__esp_lcd_ili9341 atanisoft__esp_lcd_touch_xpt2046)
