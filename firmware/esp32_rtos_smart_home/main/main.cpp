/**
 * ESP32 Smart Home RTOS Project
 * 
 * Modular architecture with separate components:
 * - GUI: LVGL graphics library integration and UI (owns LCD and Touch)
 * - Light Sensor: Ambient light detection for auto-brightness
 * - Motion Sensor: PIR motion detection
 * - Door Sensor: Magnetic door/window state
 * - Heartbeat: Periodic pulse to indicate device is alive
 * - Web Server: WebSocket API for external control
 * 
 * Uses ComponentGraph for centralized component management and inter-component communication.
 * All components are "dumb" - they expose read-only sensor data and writable settings.
 * Complex logic is delegated to external systems (e.g., Raspberry Pi hub).
 */
    
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "component_graph.h"
#include "gui.h"
#include "heartbeat.h"
#include "light_sensor.h"
#include "motion_sensor.h"
#include "door_sensor.h"
#include "web_server.h"
#include "wifi_init.h"
#include <vector>

static const char *TAG = "main";

// Memory checkpoint logging
static void log_memory_checkpoint(const char* checkpoint_name) {
    ESP_LOGI(TAG, "=== CHECKPOINT: %s ===", checkpoint_name);
    ESP_LOGI(TAG, "Free DRAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Min free DRAM: %lu bytes", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Largest block: %lu bytes", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "========================");
}

// TODO: Change these to your WiFi credentials
#define WIFI_SSID      "its getting hotspot in here"
#define WIFI_PASSWORD  "SoTakeOffAllYourClothing"

// Global component instances
static GUIComponent gui_component;
static HeartbeatComponent heartbeat_component;
static LightSensorComponent light_sensor_component;
static MotionSensorComponent motion_sensor_component;
static DoorSensorComponent door_sensor_component;
static WebServerComponent web_server_component;

extern "C" void app_main(void)
{
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] app_main");
#endif
    ESP_LOGI(TAG, "Starting ESP32 Smart Home System...");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    
    log_memory_checkpoint("APP START");
    
    // Initialize WiFi and connect
    ESP_LOGI(TAG, "Initializing WiFi...");
    if (!wifi_init_sta(WIFI_SSID, WIFI_PASSWORD)) {
        ESP_LOGE(TAG, "WiFi connection failed!");
    } else {
        ESP_LOGI(TAG, "WiFi connected successfully!");
    }
    
    log_memory_checkpoint("AFTER WIFI");
    
    // Create ComponentGraph
    ESP_LOGI(TAG, "Creating component graph...");
    ComponentGraph* component_graph = new ComponentGraph();
    
    log_memory_checkpoint("AFTER GRAPH CREATE");
    
    // Register all components with graph
    ESP_LOGI(TAG, "Registering components with graph...");
    component_graph->registerComponent(&gui_component);
    log_memory_checkpoint("AFTER GUI REGISTER");
    
    component_graph->registerComponent(&heartbeat_component);
    log_memory_checkpoint("AFTER HEARTBEAT REGISTER");
    
    component_graph->registerComponent(&light_sensor_component);
    component_graph->registerComponent(&motion_sensor_component);
    component_graph->registerComponent(&door_sensor_component);
    component_graph->registerComponent(&web_server_component);
    
    log_memory_checkpoint("AFTER ALL REGISTERS");
    
    // Initialize all components (graph handles setUpDependencies + initialize)
    ESP_LOGI(TAG, "Initializing all components...");
    component_graph->initializeAll();
    
    log_memory_checkpoint("AFTER INITIALIZE ALL");
    
    // Create simple button grid GUI
    ESP_LOGI(TAG, "Creating simple button grid...");
    gui_component.createSimpleButtonGrid();
    ESP_LOGI(TAG, "GUI created successfully");
    
    log_memory_checkpoint("AFTER GUI CREATION");
    
    ESP_LOGI(TAG, "System initialized - ready!");
    
    // Main loop - GUI runs in background task
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] app_main");
#endif
}
