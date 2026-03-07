#include "light_sensor.h"
#include "component_graph.h"
#include "pin_config.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include <cmath>

// Pin configured in common/pin_config.h (ADC_CHANNEL_0 = GPIO36)
#define LIGHT_SENSOR_CHANNEL  ((adc_channel_t)PIN_LIGHT_SENSOR_ADC)
#define LIGHT_SENSOR_PERIOD_MS 500      // Sampling period in milliseconds

LightSensorComponent::LightSensorComponent() 
    : Component("LightSensor") {
}

LightSensorComponent::~LightSensorComponent() {
    if (adc_handle) {
        adc_oneshot_del_unit(adc_handle);
    }
}

void LightSensorComponent::setUpDependencies(ComponentGraph* graph) {
    // Get reference to GUI component
    this->component_graph = graph;
    if (component_graph) {
        gui_component = component_graph->getComponent("GUI");
    }
}

void LightSensorComponent::onInitialize() {
    // Initialize ADC unit 1
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    // Configure channel: 12-bit, 12dB attenuation (0-3.3V range)
    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.bitwidth = ADC_BITWIDTH_12;
    chan_config.atten    = ADC_ATTEN_DB_12;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, LIGHT_SENSOR_CHANNEL, &chan_config));

    // Create parameter and store typed pointer
    currentLightLevel = addIntParam("current_light_level", 1, 1, 0, 4095, 4095, true);

    BaseType_t result = xTaskCreate(
        LightSensorComponent::lightSensorTaskWrapper,
        "light_sensor_task",
        4096,
        this,
        tskIDLE_PRIORITY + 1,
        &light_sensor_task_handle
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create light sensor task");
    }

    light_sensor_timer_handle = xTimerCreate(
        "light_sensor_timer",
        pdMS_TO_TICKS(LIGHT_SENSOR_PERIOD_MS),
        pdTRUE,
        this,
        [](TimerHandle_t timer) {
            LightSensorComponent* sensor = static_cast<LightSensorComponent*>(pvTimerGetTimerID(timer));
            xTaskNotifyGive(sensor->light_sensor_task_handle);
        });
    
    if (light_sensor_timer_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create light sensor timer");
    } else {
        result = xTimerStart(light_sensor_timer_handle, 0);
        if (result != pdPASS) {
            ESP_LOGE(TAG, "Failed to start light sensor timer");
        }
    }
    
    initialized = true;
}

// Static task entry point - required for FreeRTOS task creation
void LightSensorComponent::lightSensorTaskWrapper(void* pvParameters) {
    LightSensorComponent* sensor = static_cast<LightSensorComponent*>(pvParameters);
    sensor->lightSensorTask();
}

// Instance method containing the actual task loop and logic
void LightSensorComponent::lightSensorTask() {
    static int sample_count = 0;

    while (1) {
        // Wait for notification from timer (blocking)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Read the light sensor on GPIO36 (ADC1_CHANNEL_0)
        int raw_value = 0;
        adc_oneshot_read(adc_handle, LIGHT_SENSOR_CHANNEL, &raw_value);
        
        // Invert so that higher light = higher value
        int inverted_value = 4095 - raw_value;
        
        sample_count++;
        
        if (currentLightLevel) {
            currentLightLevel->setValue(0, 0, inverted_value);
        }
    }
}