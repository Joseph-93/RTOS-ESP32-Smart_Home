#pragma once

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize LVGL with display and touch
 * @param panel_handle LCD panel handle
 * @param touch_handle Touch controller handle
 */
void gui_init(esp_lcd_panel_handle_t panel_handle, esp_lcd_touch_handle_t touch_handle);

/**
 * @brief Create the main UI
 */
void gui_create_ui(void);

#ifdef __cplusplus
}
#endif
