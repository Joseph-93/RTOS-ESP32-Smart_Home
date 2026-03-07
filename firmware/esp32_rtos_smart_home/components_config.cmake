# Component Selection Configuration for RGB LED Lamp
# This file enables only the components needed for the LED lamp
#
# IMPORTANT: After changing this file, also verify pin_config.h in common/
# to ensure no GPIO conflicts between enabled components!

# Use FORCE to override cached values
set(ENABLE_GUI              OFF CACHE BOOL "Enable LCD/Touch GUI component" FORCE)
set(ENABLE_HEARTBEAT        ON CACHE BOOL "Enable heartbeat LED indicator" FORCE)
set(ENABLE_LIGHT_SENSOR     OFF CACHE BOOL "Enable ambient light sensor" FORCE)
set(ENABLE_MOTION_SENSOR    OFF CACHE BOOL "Enable PIR motion sensor" FORCE)
set(ENABLE_DOOR_SENSOR      OFF CACHE BOOL "Enable magnetic door sensor" FORCE)
set(ENABLE_TOUCH_SENSOR     ON CACHE BOOL "Enable TTP223B capacitive touch sensor" FORCE)
set(ENABLE_RGB_LED          ON CACHE BOOL "Enable WS2812 RGB LED strip" FORCE)
