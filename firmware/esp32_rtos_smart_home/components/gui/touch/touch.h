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
 * @return Touch handle on success, NULL on failure
 */
esp_lcd_touch_handle_t touch_init(void);

#ifdef __cplusplus
}
#endif
