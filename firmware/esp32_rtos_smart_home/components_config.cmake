# Component Selection Configuration — LCD Lamp (GUI + RGB + Sensors)
#
# Hardware: ESP-WROOM-32 30-pin, ILI9341 LCD, XPT2046 touch, WS2812 LEDs,
#           PIR motion sensor, door sensor, photoresistor (ADC)
# GPIOs:    LCD SPI: 18/23/19/5/2/4/33 (CLK/MOSI/MISO/CS/DC/RST/BL)
#           Touch:   22 (IRQ), 21 (CS)
#           RGB LED: 13
#           Motion:  27  |  Door: 32  |  Light ADC: GPIO36 (ADC1_CH0)
#
# Switch to this config:  .\use-config.ps1 lcd_lamp
# Then:                   idf.py fullclean && idf.py build
#
# NOTE: GPIO 12 (MS3) is a boot-strapping pin — safe to use after boot
#       but must be LOW at power-on. Not used in this build.

set(ENABLE_GUI              ON  CACHE BOOL "Enable LCD/Touch GUI component" FORCE)
set(ENABLE_HEARTBEAT        ON  CACHE BOOL "Enable heartbeat LED indicator" FORCE)
set(ENABLE_LIGHT_SENSOR     ON  CACHE BOOL "Enable ambient light sensor" FORCE)
set(ENABLE_MOTION_SENSOR    ON  CACHE BOOL "Enable PIR motion sensor" FORCE)
set(ENABLE_DOOR_SENSOR      ON  CACHE BOOL "Enable magnetic door sensor" FORCE)
set(ENABLE_TOUCH_SENSOR     OFF CACHE BOOL "Enable TTP223B capacitive touch sensor" FORCE)
set(ENABLE_RGB_LED          ON  CACHE BOOL "Enable WS2812 RGB LED strip" FORCE)
set(ENABLE_STEPPER_MOTOR    OFF CACHE BOOL "Enable A4988 stepper motor driver" FORCE)
