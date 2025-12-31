/**
 * ESP32 Smart Home RTOS Project
 * 
 * Modular architecture with separate components:
 * - LCD: ILI9341 display hardware initialization
 * - Touch: XPT2046 touch controller initialization
 * - GUI: LVGL graphics library integration and UI
 */

#include "esp_log.h"
#include "lcd.h"
#include "touch.h"
#include "gui.h"

static const char *TAG = "SmartHome";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 Smart Home System...");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    
    // Initialize hardware components
    esp_lcd_panel_handle_t panel = lcd_init();
    esp_lcd_touch_handle_t touch = touch_init();
    
    // Initialize GUI system
    gui_init(panel, touch);
    gui_create_ui();
    
    ESP_LOGI(TAG, "System initialized - ready!");
    
    // Main loop - GUI runs in background task
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
