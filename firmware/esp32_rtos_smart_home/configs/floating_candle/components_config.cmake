# Component Selection Configuration — Floating Candle (Stepper Motors)
#
# Hardware: ESP-WROOM-32 30-pin, 4× A4988 + NEMA 17, 4× cable-driven axes
# GPIOs:    15/2, 4/16, 17/5, 18/19 (STEP/DIR), 27 (EN), 14/12/13 (MS)
#           26/33/35/39 (LIMIT_MIN), 25/32/34/36 (LIMIT_MAX)
#
# Switch to this config:  .\use-config.ps1 floating_candle
# Then:                   idf.py fullclean && idf.py build

set(ENABLE_GUI              OFF CACHE BOOL "Enable LCD/Touch GUI component" FORCE)
set(ENABLE_HEARTBEAT        ON  CACHE BOOL "Enable heartbeat LED indicator" FORCE)
set(ENABLE_LIGHT_SENSOR     OFF CACHE BOOL "Enable ambient light sensor" FORCE)
set(ENABLE_MOTION_SENSOR    OFF CACHE BOOL "Enable PIR motion sensor" FORCE)
set(ENABLE_DOOR_SENSOR      OFF CACHE BOOL "Enable magnetic door sensor" FORCE)
set(ENABLE_TOUCH_SENSOR     OFF CACHE BOOL "Enable TTP223B capacitive touch sensor" FORCE)
set(ENABLE_RGB_LED          OFF CACHE BOOL "Enable WS2812 RGB LED strip" FORCE)
set(ENABLE_STEPPER_MOTOR    ON  CACHE BOOL "Enable A4988 stepper motor driver" FORCE)
