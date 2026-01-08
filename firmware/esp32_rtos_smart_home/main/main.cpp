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
#include "light_sensor.h"
#include "wifi_init.h"
#include <vector>

static const char *TAG = "SmartHome";

// TODO: Change these to your WiFi credentials
#define WIFI_SSID      "its getting hotspot in here"
#define WIFI_PASSWORD  "SoTakeOffAllYourClothing"

// Global component instances
static GUIComponent gui_component;
static NetworkActionsComponent network_component;
static LightSensorComponent light_sensor_component;

extern "C" void app_main(void)
{
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] app_main");
#endif
    ESP_LOGI(TAG, "Starting ESP32 Smart Home System...");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    
    // Initialize WiFi and connect
    ESP_LOGI(TAG, "Initializing WiFi...");
    if (!wifi_init_sta(WIFI_SSID, WIFI_PASSWORD)) {
        ESP_LOGE(TAG, "WiFi connection failed!");
    } else {
        ESP_LOGI(TAG, "WiFi connected successfully!");
    }
    
    // Initialize components
    ESP_LOGI(TAG, "Initializing components...");
    network_component.initialize();
    gui_component.initialize();
    light_sensor_component.initialize();
    
    // Connect light sensor to GUI so it can adjust brightness
    light_sensor_component.setGuiComponent(&gui_component);
    
    // Register components with GUI
    ESP_LOGI(TAG, "Registering components with GUI...");
    gui_component.registerComponent(&gui_component);
    gui_component.registerComponent(&network_component);
    gui_component.registerComponent(&light_sensor_component);
    
    // Initialize GUI hardware system (includes lcd and touch)
    gui_init();
    
    // Request UI creation (will be done by LVGL task)
    gui_create_ui(&gui_component);
    
    ESP_LOGI(TAG, "System initialized - ready!");
    
    // Main loop - GUI runs in background task
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] app_main");
#endif
}
