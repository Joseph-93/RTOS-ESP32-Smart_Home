/**
 * ESP32 Smart Home RTOS Project
 * 
 * Modular architecture with separate components:
 * - GUI: LVGL graphics library integration and UI (owns LCD and Touch)
 * - Network Actions: Network-related functionality
 */

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gui.h"
#include "network_actions.h"

static const char *TAG = "SmartHome";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 Smart Home System...");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    
    // Test NetworkActionsComponent
    NetworkActionsComponent networkActions;
    networkActions.initialize();
    
    // Read back the parameters to verify
    ESP_LOGI(TAG, "Retry count: %d", networkActions.getIntParam("retry_count")->getValue(0, 0));
    ESP_LOGI(TAG, "Timeout: %.1f sec", networkActions.getFloatParam("timeout_sec")->getValue(0, 0));
    ESP_LOGI(TAG, "WiFi enabled: %s", networkActions.getBoolParam("wifi_enabled")->getValue(0, 0) ? "true" : "false");
    
    // Initialize GUI system (includes lcd and touch)
    gui_init();
    gui_create_ui();
    
    ESP_LOGI(TAG, "System initialized - ready!");
    
    // Main loop - GUI runs in background task
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
