idf_component_register(
    SRCS "rgb_led.cpp"
    INCLUDE_DIRS "."
    REQUIRES common driver led_strip
)
