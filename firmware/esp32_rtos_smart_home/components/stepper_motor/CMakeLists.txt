idf_component_register(
    SRCS "stepper_motor.cpp" "a4988_hal.cpp"
    INCLUDE_DIRS "."
    REQUIRES 
        freertos 
        esp_timer 
        esp_common 
        nvs_flash 
        spiffs
        json
        common
        driver
)
