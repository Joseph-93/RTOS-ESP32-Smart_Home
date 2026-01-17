#pragma once

#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

// LCD Display pins
#define LCD_PIN_DC   2   // Data/Command
#define LCD_PIN_RST  4   // Reset
// Note: Backlight is on GPIO 33 (PWM), configured in lcd.cpp

// VSPI pins
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

// Display dimensions
#define LCD_H_RES    320
#define LCD_V_RES    240

/**
 * @brief Initialize the LCD display
 * @return Panel handle on success, NULL on failure
 */
esp_lcd_panel_handle_t lcd_init(void);

/**
 * @brief Set LCD backlight brightness using DAC
 * @param brightness Brightness level (0-100)
 */
void lcd_set_brightness(uint8_t brightness);

/**
 * @brief Get current LCD backlight brightness
 * @return Current brightness level (0-100)
 */
uint8_t lcd_get_brightness(void);

#ifdef __cplusplus
}
#endif
