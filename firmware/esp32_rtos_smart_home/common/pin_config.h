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
// Stepper Motor Component (4 motors, A4988 drivers)
// ============================================================================
// A4988 driver board with 12V NEMA 17 stepper motors
// Used for Floating Candle project - 4 cable-driven motors
//
// Pin assignment for ESP-WROOM-32 30-pin module:
#define PIN_STEPPER_MOTOR0_STEP     GPIO_NUM_15  // Boot strapping - safe with ENABLE pull-up
#define PIN_STEPPER_MOTOR0_DIR      GPIO_NUM_2   // Boot strapping - harmless for DIR
#define PIN_STEPPER_MOTOR1_STEP     GPIO_NUM_4
#define PIN_STEPPER_MOTOR1_DIR      GPIO_NUM_16
#define PIN_STEPPER_MOTOR2_STEP     GPIO_NUM_17
#define PIN_STEPPER_MOTOR2_DIR      GPIO_NUM_5   // Internal pull-up at boot - harmless for DIR
#define PIN_STEPPER_MOTOR3_STEP     GPIO_NUM_18
#define PIN_STEPPER_MOTOR3_DIR      GPIO_NUM_19
#define PIN_STEPPER_ENABLE          GPIO_NUM_27  // Shared, active LOW. External 10K pull-up to 3.3V
#define PIN_STEPPER_MS1             GPIO_NUM_14  // Microstepping select (shared)
#define PIN_STEPPER_MS2             GPIO_NUM_12  // Microstepping select (shared). Burn eFuse!
#define PIN_STEPPER_MS3             GPIO_NUM_13  // Microstepping select (shared)
#define PIN_STEPPER_LIMIT_MIN0      GPIO_NUM_26  // Min (retract) limit, internal pullup
#define PIN_STEPPER_LIMIT_MIN1      GPIO_NUM_33  // Min (retract) limit, internal pullup
#define PIN_STEPPER_LIMIT_MIN2      GPIO_NUM_35  // Min (retract) limit, input only, external 10K pullup
#define PIN_STEPPER_LIMIT_MIN3      GPIO_NUM_39  // Min (retract) limit, input only, external 10K pullup
#define PIN_STEPPER_LIMIT_MAX0      GPIO_NUM_25  // Max (pay-out) limit, internal pullup
#define PIN_STEPPER_LIMIT_MAX1      GPIO_NUM_32  // Max (pay-out) limit, internal pullup
#define PIN_STEPPER_LIMIT_MAX2      GPIO_NUM_34  // Max (pay-out) limit, input only, external 10K pullup
#define PIN_STEPPER_LIMIT_MAX3      GPIO_NUM_36  // Max (pay-out) limit, input only, external 10K pullup
//
// NOTE: GPIOs 35, 39, 34, 36 are input-only with NO internal pullup.
// External 10K pullup resistors to 3.3V required on those limit pins.
// GPIOs 26, 33, 25, 32 use internal pullup (no external resistor needed).
// All limit switches: active LOW (switch closes to GND when triggered).
// NOTE: GPIO 12 (MS2) is a boot strapping pin — burn VDD_SDIO eFuse:
//   espefuse.py --port COM<X> set_flash_voltage 3.3V
// NOTE: GPIO 27 (ENABLE) needs external 10K pull-up to 3.3V to keep
//   drivers disabled during ESP32 boot (before firmware takes over).
// NOTE: SLEEP and RESET on all A4988 boards are tied together and to 3.3V.
//   No ESP32 GPIOs needed for SLEEP/RESET.

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
 *  2    | LCD DC / M0 DIR  | ENABLE_GUI / ENABLE_STEPPER_MOTOR
 *  4    | LCD RST / M1 STEP| ENABLE_GUI / ENABLE_STEPPER_MOTOR
 *  5    | LCD CS / M2 DIR  | ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 12    | Stepper MS2      | ENABLE_STEPPER_MOTOR (burn eFuse!)
 * 13    | RGB LED / MS3    | ENABLE_RGB_LED / ENABLE_STEPPER_MOTOR
 * 14    | Stepper MS1      | ENABLE_STEPPER_MOTOR
 * 15    | Motor 0 STEP     | ENABLE_STEPPER_MOTOR (boot strapping)
 * 16    | Motor 1 DIR      | ENABLE_STEPPER_MOTOR
 * 17    | Motor 2 STEP     | ENABLE_STEPPER_MOTOR
 * 18    | LCD CLK / M3 STEP| ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 19    | LCD MISO / M3 DIR| ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 25    | Touch Sensor 0 / M0 MAX | ENABLE_TOUCH_SENSOR / ENABLE_STEPPER_MOTOR
 * 26    | Touch Sensor 1 / M0 MIN | ENABLE_TOUCH_SENSOR / ENABLE_STEPPER_MOTOR
 * 27    | Motion Sensor / ENABLE   | ENABLE_MOTION_SENSOR / ENABLE_STEPPER_MOTOR
 * 32    | Door Sensor / M1 MAX     | ENABLE_DOOR_SENSOR / ENABLE_STEPPER_MOTOR
 * 33    | LCD BL / M1 MIN  | ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 34    | Motor 2 MAX      | ENABLE_STEPPER_MOTOR (input only)
 * 35    | Motor 2 MIN      | ENABLE_STEPPER_MOTOR (input only)
 * 36    | Light Sensor / M3 MAX | ENABLE_LIGHT_SENSOR / ENABLE_STEPPER_MOTOR
 * 39    | Motor 3 MIN      | ENABLE_STEPPER_MOTOR (input only)
 *
 * Floating Candle build: ONLY heartbeat + webserver + stepper_motor enabled.
 * No conflicts with GUI, touch sensor, or other components.
 * Spare GPIOs: 21, 22, 23
 */
