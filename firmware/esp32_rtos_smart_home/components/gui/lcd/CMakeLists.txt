idf_component_register(SRCS "lcd.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES common esp_driver_gpio esp_driver_spi esp_lcd espressif__esp_lcd_ili9341)
