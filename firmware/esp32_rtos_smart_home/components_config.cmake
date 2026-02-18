# Component Selection Configuration for RGB LED Lamp
# This file enables only the components needed for the LED lamp

set(ENABLE_GUI OFF CACHE BOOL "Enable LCD/Touch GUI component")
set(ENABLE_HEARTBEAT ON CACHE BOOL "Enable heartbeat LED indicator")
set(ENABLE_LIGHT_SENSOR OFF CACHE BOOL "Enable ambient light sensor")
set(ENABLE_MOTION_SENSOR OFF CACHE BOOL "Enable PIR motion sensor")
set(ENABLE_DOOR_SENSOR OFF CACHE BOOL "Enable magnetic door sensor")
set(ENABLE_RGB_LED ON CACHE BOOL "Enable WS2812 RGB LED strip")
