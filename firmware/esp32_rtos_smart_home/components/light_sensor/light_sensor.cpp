#include "light_sensor.h"
#include "component_graph.h"
#include "esp_log.h"
#include "driver/adc.h"
#include <cmath>

#define LIGHT_SENSOR_PIN ADC1_CHANNEL_0 // GPIO36

LightSensorComponent::LightSensorComponent() 
    : Component("LightSensor") {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] LightSensorComponent constructor");
#endif
    ESP_LOGI(TAG, "LightSensorComponent created");
}

LightSensorComponent::~LightSensorComponent() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] ~LightSensorComponent");
#endif
    ESP_LOGI(TAG, "LightSensorComponent destroyed");
}

void LightSensorComponent::setUpDependencies() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] LightSensorComponent::setUpDependencies");
#endif
    // Get reference to GUI component
    if (g_component_graph) {
        gui_component = g_component_graph->getComponent("GUI");
        if (gui_component) {
            ESP_LOGI(TAG, "GUI component reference obtained");
        } else {
            ESP_LOGW(TAG, "GUI component not found");
        }
    } else {
        ESP_LOGE(TAG, "ComponentGraph not available!");
    }
}

void LightSensorComponent::initialize() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] LightSensorComponent::initialize");
#endif
    ESP_LOGI(TAG, "Initializing LightSensorComponent...");
    
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(LIGHT_SENSOR_PIN, ADC_ATTEN_DB_12);

    // Example: addIntParam("light_level", 1, 1, 0, 1023);
    addIntParam("current_light_level", 1, 1, 0, 4095, 4095);

    BaseType_t result = xTaskCreate(
        LightSensorComponent::lightSensorTaskWrapper,
        "light_sensor_task",
        8192, // Stack depth - increased due to callback chain and logging
        this, // Just send the pointer to this component
        tskIDLE_PRIORITY + 1,
        &light_sensor_task_handle
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create light sensor task");
    } else {
        ESP_LOGI(TAG, "Light sensor task created successfully");
    }

    light_sensor_timer_handle = xTimerCreate(
        "light_sensor_timer",
        pdMS_TO_TICKS(50), // 50 millisecond interval
        pdTRUE,              // Auto-reload
        this,                // Pass 'this' as timer ID so we can access it in callback
        [](TimerHandle_t timer) {
            // Get the LightSensorComponent pointer from timer ID
            LightSensorComponent* sensor = static_cast<LightSensorComponent*>(pvTimerGetTimerID(timer));
            xTaskNotifyGive(sensor->light_sensor_task_handle);
        });
    
    if (light_sensor_timer_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create light sensor timer");
    } else {
        result = xTimerStart(light_sensor_timer_handle, 0);
        if (result != pdPASS) {
            ESP_LOGE(TAG, "Failed to start light sensor timer");
        } else {
            ESP_LOGI(TAG, "Light sensor timer started successfully");
        }
    }
    
    initialized = true;
    ESP_LOGI(TAG, "LightSensorComponent initialized");
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] LightSensorComponent::initialize");
#endif
}

// Static task entry point - required for FreeRTOS task creation
void LightSensorComponent::lightSensorTaskWrapper(void* pvParameters) {
    LightSensorComponent* sensor = static_cast<LightSensorComponent*>(pvParameters);
    sensor->lightSensorTask();
}

// Instance method containing the actual task loop and logic
void LightSensorComponent::lightSensorTask() {
    ESP_LOGI(TAG, "Light sensor task started");
    
    auto* param = getIntParam("current_light_level");

    while (1) {
        // Wait for notification from timer (blocking)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Read the light sensor on GPIO36 (ADC1_CHANNEL_0)
        int raw_value = adc1_get_raw(LIGHT_SENSOR_PIN);
        
        // Change it so that higher light results in higher value
        int inverted_value = 4095 - raw_value;
        
        // Update the parameter
        if (param) {
            param->setValue(0, 0, inverted_value);
        }
    }
}