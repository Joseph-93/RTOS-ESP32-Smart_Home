#pragma once

#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

// Touch controller pins
#define TOUCH_CS     21  // Touch CS

// Display dimensions (for coordinate mapping)
#define TOUCH_X_MAX  240
#define TOUCH_Y_MAX  320

/**
 * @brief Initialize the touch controller
 * @return Touch handle on success, NULL on failure (hardware not present)
 */
esp_lcd_touch_handle_t touch_init(void);

/**
 * @brief Check if touch hardware is available
 * @return true if touch initialized successfully, false otherwise
 */
bool touch_is_available(void);

#ifdef __cplusplus
}
#endif
