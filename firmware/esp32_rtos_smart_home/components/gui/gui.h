#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize GUI subsystem (lcd, touch, and LVGL)
 */
void gui_init(void);

/**
 * @brief Create the main UI
 */
void gui_create_ui(void);

#ifdef __cplusplus
}
#endif
