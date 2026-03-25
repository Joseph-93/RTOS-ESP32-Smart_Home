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
#define PIN_STEPPER_MOTOR0_STEP     GPIO_NUM_16
#define PIN_STEPPER_MOTOR0_DIR      GPIO_NUM_17
#define PIN_STEPPER_MOTOR1_STEP     GPIO_NUM_18
#define PIN_STEPPER_MOTOR1_DIR      GPIO_NUM_19
#define PIN_STEPPER_MOTOR2_STEP     GPIO_NUM_21
#define PIN_STEPPER_MOTOR2_DIR      GPIO_NUM_22
#define PIN_STEPPER_MOTOR3_STEP     GPIO_NUM_23
#define PIN_STEPPER_MOTOR3_DIR      GPIO_NUM_25
#define PIN_STEPPER_ENABLE          GPIO_NUM_26  // Shared, active LOW
#define PIN_STEPPER_MS1             GPIO_NUM_27  // Microstepping select (shared)
#define PIN_STEPPER_MS2             GPIO_NUM_14  // Microstepping select (shared)
#define PIN_STEPPER_MS3             GPIO_NUM_12  // Microstepping select (shared)
#define PIN_STEPPER_LIMIT_MIN0      GPIO_NUM_34  // Min (retract) limit, input only
#define PIN_STEPPER_LIMIT_MIN1      GPIO_NUM_35  // Min (retract) limit, input only
#define PIN_STEPPER_LIMIT_MIN2      GPIO_NUM_36  // Min (retract) limit, input only
#define PIN_STEPPER_LIMIT_MIN3      GPIO_NUM_39  // Min (retract) limit, input only
#define PIN_STEPPER_LIMIT_MAX0      GPIO_NUM_4   // Max (pay-out) limit, supports internal pullup
#define PIN_STEPPER_LIMIT_MAX1      GPIO_NUM_5   // Max (pay-out) limit, supports internal pullup
#define PIN_STEPPER_LIMIT_MAX2      GPIO_NUM_13  // Max (pay-out) limit, supports internal pullup
#define PIN_STEPPER_LIMIT_MAX3      GPIO_NUM_33  // Max (pay-out) limit, supports internal pullup
//
// NOTE: Min limit GPIOs (34-39) have NO internal pullup.
// External 10K pullup resistors to 3.3V required on min limit pins.
// Max limit GPIOs (4, 5, 13, 33) use internal pullup (no external resistor needed).
// All limit switches: active LOW (switch closes to GND when triggered).
// NOTE: GPIO 12 (MS3) is a boot strapping pin - must be LOW at boot.
// This is fine since 1/16 microstepping (default) sets MS3=HIGH after boot.

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
 * 13    | RGB LED          | ENABLE_RGB_LED
 * 16    | Motor 0 STEP     | ENABLE_STEPPER_MOTOR
 * 17    | Motor 0 DIR      | ENABLE_STEPPER_MOTOR
 * 18    | LCD CLK / M1 STEP| ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 19    | LCD MISO / M1 DIR| ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 21    | Touch CS / M2 STEP| ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 22    | Touch IRQ / M2 DIR| ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 23    | LCD MOSI / M3 STEP| ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 25    | Touch Sensor 0 / M3 DIR| ENABLE_TOUCH_SENSOR / ENABLE_STEPPER_MOTOR
 * 26    | Touch Sensor 1 / Motor EN| ENABLE_TOUCH_SENSOR / ENABLE_STEPPER_MOTOR
 * 27    | Motion Sensor    | ENABLE_MOTION_SENSOR
 * 32    | Door Sensor      | ENABLE_DOOR_SENSOR
 * 33    | LCD Backlight    | ENABLE_GUI
 *  4    | Motor 0 LIMIT MAX| ENABLE_STEPPER_MOTOR
 *  5    | Motor 1 LIMIT MAX| ENABLE_STEPPER_MOTOR
 * 13    | RGB LED / M2 MAX | ENABLE_RGB_LED / ENABLE_STEPPER_MOTOR
 * 33    | LCD BL / M3 MAX  | ENABLE_GUI / ENABLE_STEPPER_MOTOR
 * 34    | Motor 0 LIMIT MIN| ENABLE_STEPPER_MOTOR (input only)
 * 35    | Motor 1 LIMIT MIN| ENABLE_STEPPER_MOTOR (input only)
 * 36    | Light Sensor / M2 MIN | ENABLE_LIGHT_SENSOR / ENABLE_STEPPER_MOTOR
 * 39    | Motor 3 LIMIT MIN| ENABLE_STEPPER_MOTOR (input only)
 *
 * Floating Candle build: ONLY heartbeat + webserver + stepper_motor enabled.
 * No conflicts with GUI, touch sensor, or other components.
 */
