#pragma once

#include "esp_lcd_panel_ops.h"
#include "pin_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// LCD Display pins - configured in common/pin_config.h
#define LCD_PIN_DC   PIN_LCD_DC    // Data/Command
#define LCD_PIN_RST  PIN_LCD_RST   // Reset
// Note: Backlight configured in pin_config.h as PIN_LCD_BACKLIGHT

// VSPI pins - configured in common/pin_config.h
#define PIN_NUM_MISO PIN_LCD_MISO
#define PIN_NUM_MOSI PIN_LCD_MOSI
#define PIN_NUM_CLK  PIN_LCD_CLK
#define PIN_NUM_CS   PIN_LCD_CS

// Display dimensions
#define LCD_H_RES    320
#define LCD_V_RES    240

/**
 * @brief Initialize the LCD display
 * @return Panel handle on success, NULL on failure (hardware not present)
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

/**
 * @brief Check if LCD hardware is available
 * @return true if LCD initialized successfully, false otherwise
 */
bool lcd_is_available(void);

#ifdef __cplusplus
}
#endif
