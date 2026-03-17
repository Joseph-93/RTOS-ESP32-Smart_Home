/**
 * ESP32 Smart Home RTOS Project
 * 
 * Modular architecture with separate components:
 * - GUI: LVGL graphics library integration and UI (owns LCD and Touch)
 * - Light Sensor: Ambient light detection for auto-brightness
 * - Motion Sensor: PIR motion detection
 * - Door Sensor: Magnetic door/window state
 * - Heartbeat: Periodic pulse to indicate device is alive
 * - RGB LED: WS2812 LED strip control with scripted animations
 * - Web Server: WebSocket API for external control
 * 
 * Uses ComponentGraph for centralized component management and inter-component communication.
 * All components are "dumb" - they expose read-only sensor data and writable settings.
 * Complex logic is delegated to external systems (e.g., Raspberry Pi hub).
 * 
 * Component selection is controlled via components_config.cmake
 */
    
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "component_graph.h"

// Conditionally include component headers
#ifdef ENABLE_GUI
#include "gui.h"
#endif
#ifdef ENABLE_HEARTBEAT
#include "heartbeat.h"
#endif
#ifdef ENABLE_LIGHT_SENSOR
#include "light_sensor.h"
#endif
#ifdef ENABLE_MOTION_SENSOR
#include "motion_sensor.h"
#endif
#ifdef ENABLE_DOOR_SENSOR
#include "door_sensor.h"
#endif
#ifdef ENABLE_TOUCH_SENSOR
#include "touch_sensor.h"
#endif
#ifdef ENABLE_RGB_LED
#include "rgb_led.h"
#endif
#ifdef ENABLE_STEPPER_MOTOR
#include "stepper_motor.h"
#include "a4988_hal.h"
#endif

#include "web_server.h"
#include "wifi_init.h"
#include "ota_update.h"
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

// TODO: Make the system dynamic - allow wifi credentials and component selection to be configured at runtime (e.g., possibly by making the ESP32 a WiFi AP on first boot and asking for configuration through a simple web interface, then saving to NVS for future boots). For now, hardcode WiFi credentials and component selection at compile time for simplicity and memory savings.
#define WIFI_SSID      "its getting hotspot in here"
#define WIFI_PASSWORD  "SoTakeOffAllYourClothing"

// Global component instances (conditionally compiled)
#ifdef ENABLE_GUI
static GUIComponent gui_component;
#endif
#ifdef ENABLE_HEARTBEAT
static HeartbeatComponent heartbeat_component;
#endif
#ifdef ENABLE_LIGHT_SENSOR
static LightSensorComponent light_sensor_component;
#endif
#ifdef ENABLE_MOTION_SENSOR
static MotionSensorComponent motion_sensor_component;
#endif
#ifdef ENABLE_DOOR_SENSOR
static DoorSensorComponent door_sensor_component;
#endif
#ifdef ENABLE_TOUCH_SENSOR
static TouchSensorComponent touch_sensor_component;
#endif
#ifdef ENABLE_RGB_LED
static RgbLedComponent rgb_led_component;
#endif
#ifdef ENABLE_STEPPER_MOTOR
static A4988StepperMotorHAL stepper_hal;
static StepperMotorComponent stepper_motor_component(&stepper_hal);
#endif
static WebServerComponent web_server_component;
static OtaUpdateComponent ota_component;

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32 Smart Home System (ESP-IDF %s)", esp_get_idf_version());
    
    // Initialize WiFi and connect
    if (!wifi_init_sta(WIFI_SSID, WIFI_PASSWORD)) {
        ESP_LOGE(TAG, "WiFi connection failed!");
    }
    
    // Create ComponentGraph
    ComponentGraph* component_graph = new ComponentGraph();
    
    // Set device name from mDNS hostname for identification
    component_graph->setDeviceName(wifi_get_hostname());
    
    // Register all components with graph (conditionally compiled)
    
#ifdef ENABLE_GUI
    component_graph->registerComponent(&gui_component);
#endif

#ifdef ENABLE_HEARTBEAT
    component_graph->registerComponent(&heartbeat_component);
#endif

#ifdef ENABLE_LIGHT_SENSOR
    component_graph->registerComponent(&light_sensor_component);
#endif

#ifdef ENABLE_MOTION_SENSOR
    component_graph->registerComponent(&motion_sensor_component);
#endif

#ifdef ENABLE_DOOR_SENSOR
    component_graph->registerComponent(&door_sensor_component);
#endif

#ifdef ENABLE_TOUCH_SENSOR
    component_graph->registerComponent(&touch_sensor_component);
#endif

#ifdef ENABLE_RGB_LED
    component_graph->registerComponent(&rgb_led_component);
#endif

#ifdef ENABLE_STEPPER_MOTOR
    component_graph->registerComponent(&stepper_motor_component);
#endif

    component_graph->registerComponent(&web_server_component);
    component_graph->registerComponent(&ota_component);
    
    // Initialize all components (graph handles setUpDependencies + initialize)
    component_graph->initializeAll();
    
    // Load saved parameter values from NVS and start persistence timer
    ESP_LOGI(TAG, "Loading saved parameters from NVS...");
    component_graph->loadAllParameters();
    
#ifdef ENABLE_GUI
    // Create simple button grid GUI
    gui_component.createSimpleButtonGrid();
#endif
    
    ESP_LOGI(TAG, "System initialized - ready!");
    
    // Main loop - GUI runs in background task
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
