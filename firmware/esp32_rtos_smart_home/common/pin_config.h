/**
 * Centralized GPIO Pin Configuration
 * 
 * ALL pin assignments for ALL components are defined here.
 * Edit this file to configure pins for your specific device build.
 * 
 * CONFLICT CHECK: Before building, verify no two enabled components
 * share the same GPIO. Use components_config.cmake to enable/disable.
 * 
 * ESP32 GPIO Reference:
 * - GPIO 0, 2, 15: Boot strapping pins (avoid or use carefully)
 * - GPIO 6-11: Connected to flash (DO NOT USE)
 * - GPIO 34-39: Input only (no internal pullup/pulldown)
 * - GPIO 36, 39: Often used for ADC (light sensor)
 */

#pragma once

#include "driver/gpio.h"

// ============================================================================
// RGB LED Component
// ============================================================================
// WS2812/NeoPixel data pin - needs RMT peripheral support
#define PIN_RGB_LED_DATA        GPIO_NUM_13

// ============================================================================
// Motion Sensor Component (PIR)
// ============================================================================
// PIR sensor output - digital HIGH when motion detected
#define PIN_MOTION_SENSOR       GPIO_NUM_27  // Changed from 13 to avoid conflict

// ============================================================================
// Door Sensor Component (Magnetic Reed Switch)
// ============================================================================
// Reed switch input - digital state indicates open/closed
#define PIN_DOOR_SENSOR         GPIO_NUM_32

// ============================================================================
// Light Sensor Component (Photoresistor/LDR)
// ============================================================================
// ADC input for analog light level reading
// Note: GPIO36 = ADC1_CHANNEL_0 = enum value 0 (input-only pin, no pullup)
// Raw integer used here to avoid pulling deprecated driver/adc.h into every component.
// light_sensor.cpp includes driver/adc.h directly for the typed API.
#define PIN_LIGHT_SENSOR_ADC    0  // ADC1_CHANNEL_0 = GPIO36

// ============================================================================
// Touch Sensor Component (TTP223B Capacitive Touch)
// ============================================================================
// 2 touch sensors supported (TOUCH_SENSOR_COUNT = 2 in touch_sensor.h)
// TTP223B outputs HIGH when touched (active high)
#define PIN_TOUCH_SENSOR_0      GPIO_NUM_25
#define PIN_TOUCH_SENSOR_1      GPIO_NUM_26

// ============================================================================
// GUI Component (LCD + Touch Display)
// ============================================================================
// ILI9341 LCD - SPI bus pins
#define PIN_LCD_MISO            GPIO_NUM_19
#define PIN_LCD_MOSI            GPIO_NUM_23
#define PIN_LCD_CLK             GPIO_NUM_18
#define PIN_LCD_CS              GPIO_NUM_5
#define PIN_LCD_DC              GPIO_NUM_2   // Data/Command
#define PIN_LCD_RST             GPIO_NUM_4   // Reset
#define PIN_LCD_BACKLIGHT       GPIO_NUM_33

// XPT2046 Touch Controller (shares SPI bus with LCD)
#define PIN_TOUCH_IRQ           GPIO_NUM_22  // PENIRQ - active low
// Touch CS is typically on a separate pin, often GPIO 21
#define PIN_TOUCH_CS            GPIO_NUM_21

// ============================================================================
// Stepper Motor Component (4 motors, step/dir/enable)
// ============================================================================
// These are PLACEHOLDER pins - actual HAL implementation will define real pins
// based on the specific hardware (A4988, DRV8825, TMC2209, etc.)
// 
// Suggested GPIO layout for floating candle (4 motors):
// Motor 0: STEP=25, DIR=26
// Motor 1: STEP=27, DIR=14
// Motor 2: STEP=12, DIR=13
// Motor 3: STEP=32, DIR=33
// ENABLE (shared): 15
// LIMIT_0-3: 34, 35, 36, 39 (input-only GPIOs for limit switches)
//
// NOTE: The HAL interface is abstract - pins are defined in the concrete HAL
// implementation, not in this config file. See INTERFACE.md in floating_candle
// repo for the recommended GPIO assignments.

// ============================================================================
// RESERVED / SYSTEM PINS (DO NOT USE)
// ============================================================================
// GPIO 0:  Boot button / strapping
// GPIO 1:  TX0 (USB serial)
// GPIO 3:  RX0 (USB serial)
// GPIO 6-11: Flash memory (DO NOT USE)

// ============================================================================
// PIN CONFLICT TABLE - Update when changing pins!
// ============================================================================
/*
 * GPIO  | Component        | Enabled By
 * ------|------------------|------------------
 *  2    | LCD DC           | ENABLE_GUI
 *  4    | LCD RST          | ENABLE_GUI
 *  5    | LCD CS           | ENABLE_GUI
 * 12    | Motor 2 STEP*    | ENABLE_STEPPER_MOTOR
 * 13    | RGB LED / M2 DIR*| ENABLE_RGB_LED / ENABLE_STEPPER_MOTOR
 * 14    | Motor 1 DIR*     | ENABLE_STEPPER_MOTOR
 * 15    | Motor ENABLE*    | ENABLE_STEPPER_MOTOR
 * 18    | LCD CLK          | ENABLE_GUI
 * 19    | LCD MISO         | ENABLE_GUI
 * 21    | Touch CS (disp)  | ENABLE_GUI
 * 22    | Touch IRQ (disp) | ENABLE_GUI
 * 23    | LCD MOSI         | ENABLE_GUI
 * 25    | Touch Sensor 0 / M0 STEP* | ENABLE_TOUCH_SENSOR / ENABLE_STEPPER_MOTOR
 * 26    | Touch Sensor 1 / M0 DIR*  | ENABLE_TOUCH_SENSOR / ENABLE_STEPPER_MOTOR
 * 27    | Motion Sensor / M1 STEP*  | ENABLE_MOTION_SENSOR / ENABLE_STEPPER_MOTOR
 * 32    | Door Sensor / M3 STEP*    | ENABLE_DOOR_SENSOR / ENABLE_STEPPER_MOTOR
 * 33    | LCD Backlight / M3 DIR*   | ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 34    | Motor 0 LIMIT*   | ENABLE_STEPPER_MOTOR (input only)
 * 35    | Motor 1 LIMIT*   | ENABLE_STEPPER_MOTOR (input only)
 * 36    | Light Sensor / M2 LIMIT* | ENABLE_LIGHT_SENSOR / ENABLE_STEPPER_MOTOR
 * 39    | Motor 3 LIMIT*   | ENABLE_STEPPER_MOTOR (input only)
 *
 * * = Stepper pins are defined in HAL implementation, not pin_config.h
 *     Floating candle uses dedicated ESP32, no conflicts with other components.
 */
