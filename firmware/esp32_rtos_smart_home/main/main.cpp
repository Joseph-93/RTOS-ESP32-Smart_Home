/**
 * ESP32 Smart Home RTOS Project
 * 
 * Modular architecture with separate components:
 * - GUI: LVGL graphics library integration and UI (owns LCD and Touch)
 * - Network Actions: Network-related functionality
 * - Light Sensor: Ambient light detection for auto-brightness
 * 
 * Uses ComponentGraph for centralized component management and inter-component communication
 */

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "component_graph.h"
#include "gui.h"
#include "network_actions.h"
#include "light_sensor.h"
#include "motion_sensor.h"
#include "wifi_init.h"
#include <vector>

static const char *TAG = "main";

// TODO: Change these to your WiFi credentials
#define WIFI_SSID      "its getting hotspot in here"
#define WIFI_PASSWORD  "SoTakeOffAllYourClothing"

// Global component instances
static GUIComponent gui_component;
static NetworkActionsComponent network_component;
static LightSensorComponent light_sensor_component;
static MotionSensorComponent motion_sensor_component;

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
    
    // Create ComponentGraph
    ESP_LOGI(TAG, "Creating component graph...");
    g_component_graph = new ComponentGraph();
    
    // Register all components with graph
    ESP_LOGI(TAG, "Registering components with graph...");
    g_component_graph->registerComponent(&gui_component);
    g_component_graph->registerComponent(&network_component);
    g_component_graph->registerComponent(&light_sensor_component);
    g_component_graph->registerComponent(&motion_sensor_component);
    
    // Initialize all components (graph handles setUpDependencies + initialize)
    ESP_LOGI(TAG, "Initializing all components...");
    g_component_graph->initializeAll();
    
    // Now that ALL components are initialized, build the GUI menu tree
    ESP_LOGI(TAG, "Building GUI menu tree...");
    gui_component.buildMenuTree();
    ESP_LOGI(TAG, "GUI menu tree built successfully");
    
    ESP_LOGI(TAG, "System initialized - ready!");
    
    // Main loop - GUI runs in background task
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] app_main");
#endif
}
