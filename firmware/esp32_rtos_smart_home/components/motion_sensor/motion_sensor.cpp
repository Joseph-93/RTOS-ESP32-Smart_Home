#include "motion_sensor.h"
#include "component_graph.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include "driver/gpio.h"
#include <cmath>

#define MOTION_SENSOR_PIN 13 // GPIO13

MotionSensorComponent::MotionSensorComponent() 
    : Component("MotionSensor") {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] MotionSensorComponent constructor");
#endif
    ESP_LOGI(TAG, "MotionSensorComponent created");
}

MotionSensorComponent::~MotionSensorComponent() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] ~MotionSensorComponent");
#endif
    ESP_LOGI(TAG, "MotionSensorComponent destroyed");
}

void MotionSensorComponent::setUpDependencies(ComponentGraph* graph) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] MotionSensorComponent::setUpDependencies");
#endif
    // Get reference to GUI component
    this->component_graph = graph;
    if (component_graph) {
        gui_component = component_graph->getComponent("GUI");
        if (gui_component) {
            ESP_LOGI(TAG, "GUI component reference obtained");
        } else {
            ESP_LOGW(TAG, "GUI component not found");
        }
    } else {
        ESP_LOGE(TAG, "ComponentGraph not available!");
    }
}

static void IRAM_ATTR motion_sensor_isr_handler(void* arg) {
    MotionSensorComponent* component = static_cast<MotionSensorComponent*>(arg);
    if (component && component->motion_sensor_task_handle) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(component->motion_sensor_task_handle, &xHigherPriorityTaskWoken);
    }
}

void MotionSensorComponent::onInitialize() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] MotionSensorComponent::initialize");
#endif

    addIntParam("last_motion_detected_seconds", 1, 1, 0, INT32_MAX, 0, true);

    // Configure motion sensor GPIO pin
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << MOTION_SENSOR_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;  // Pull-down so default is LOW
    io_conf.intr_type = GPIO_INTR_POSEDGE;  // Trigger on rising edge (motion detected)
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    
    ESP_LOGI(TAG, "Motion sensor GPIO %d configured", MOTION_SENSOR_PIN);

    // Install ISR service if not already installed (GUI might have already done this)
    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "GPIO ISR service installed");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "GPIO ISR service already installed (probably by GUI)");
    } else {
        ESP_ERROR_CHECK(ret);  // Fail on unexpected errors
    }
    
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)MOTION_SENSOR_PIN, motion_sensor_isr_handler, this));
    ESP_LOGI(TAG, "Motion sensor ISR handler registered for GPIO %d", MOTION_SENSOR_PIN);

    BaseType_t result = xTaskCreate(
        MotionSensorComponent::motionSensorTaskWrapper,
        "motion_sensor_task",
        4096, // Stack depth - simple timestamp update
        this, // Just send the pointer to this component
        tskIDLE_PRIORITY + 1,
        &motion_sensor_task_handle
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create motion sensor task");
    } else {
        ESP_LOGI(TAG, "Motion sensor task created successfully");
    }

    initialized = true;
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] MotionSensorComponent::initialize");
#endif
}

// Static task entry point - required for FreeRTOS task creation
void MotionSensorComponent::motionSensorTaskWrapper(void* pvParameters) {
    MotionSensorComponent* sensor = static_cast<MotionSensorComponent*>(pvParameters);
    sensor->motionSensorTask();
}

// Instance method containing the actual task loop and logic
void MotionSensorComponent::motionSensorTask() {
    ESP_LOGI(TAG, "Motion sensor task started");
    
    IntParameter* last_motion_detected_seconds_param = getIntParam("last_motion_detected_seconds");
    
    while (1) {
        // Wait for notification from ISR (blocking)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        ESP_LOGI(TAG, "*** MOTION DETECTED *** (GPIO %d triggered)", MOTION_SENSOR_PIN);
        
        // Update the last motion detected time
        if (last_motion_detected_seconds_param) {
            int current_time = esp_timer_get_time() / 1000000;
            last_motion_detected_seconds_param->setValue(0, 0, current_time); // time in seconds
            ESP_LOGI(TAG, "Motion timestamp updated: %d seconds", current_time);
        }
    }
}
