#include "door_sensor.h"
#include "pin_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

// Pin configured in common/pin_config.h
#define DOOR_SENSOR_PIN PIN_DOOR_SENSOR

DoorSensorComponent::DoorSensorComponent() 
    : Component("DoorSensor")
    , doorOpenedTimestamp(0) {
}

DoorSensorComponent::~DoorSensorComponent() {
}

static void IRAM_ATTR door_sensor_isr_handler(void* arg) {
    DoorSensorComponent* component = static_cast<DoorSensorComponent*>(arg);
    if (component && component->door_sensor_task_handle) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(component->door_sensor_task_handle, &xHigherPriorityTaskWoken);
    }
}

void DoorSensorComponent::onInitialize() {
    // Create read-only parameters for sensor state
    doorOpen = addBoolParam("door_open", 1, 1, false, true);  // read-only
    doorOpenSeconds = addIntParam("door_open_seconds", 1, 1, 0, INT32_MAX, 0, true);  // read-only
    lastDoorEventSeconds = addIntParam("last_door_event_seconds", 1, 1, 0, INT32_MAX, 0, true);  // read-only

    // Configure door sensor GPIO pin
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << DOOR_SENSOR_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Install ISR service if not already installed
    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)DOOR_SENSOR_PIN, door_sensor_isr_handler, this));

    // Create task for handling sensor events
    BaseType_t result = xTaskCreate(
        DoorSensorComponent::doorSensorTaskWrapper,
        "door_sensor_task",
        4096,
        this,
        tskIDLE_PRIORITY + 1,
        &door_sensor_task_handle
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create door sensor task");
    }

    initialized = true;
}

void DoorSensorComponent::doorSensorTaskWrapper(void* pvParameters) {
    DoorSensorComponent* sensor = static_cast<DoorSensorComponent*>(pvParameters);
    sensor->doorSensorTask();
}

void DoorSensorComponent::doorSensorTask() {
    bool previousOpen = false;
    
    while (1) {
        // Wait for notification from ISR or timeout (1 second for duration updates)
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        
        // Read current state
        bool currentOpen = gpio_get_level((gpio_num_t)DOOR_SENSOR_PIN) == 1;
        int64_t nowUs = esp_timer_get_time();
        int nowSeconds = nowUs / 1000000;
        
        // Update door open state
        if (doorOpen) {
            doorOpen->setValue(0, 0, currentOpen);
        }
        
        // Detect state transitions
        if (currentOpen && !previousOpen) {
            // Door just opened
            doorOpenedTimestamp = nowUs;
            if (lastDoorEventSeconds) {
                lastDoorEventSeconds->setValue(0, 0, nowSeconds);
            }
        } else if (!currentOpen && previousOpen) {
            // Door just closed
            doorOpenedTimestamp = 0;
            if (lastDoorEventSeconds) {
                lastDoorEventSeconds->setValue(0, 0, nowSeconds);
            }
            if (doorOpenSeconds) {
                doorOpenSeconds->setValue(0, 0, 0);
            }
        }
        
        // Update door open duration if open
        if (currentOpen && doorOpenedTimestamp > 0 && doorOpenSeconds) {
            int openDuration = (nowUs - doorOpenedTimestamp) / 1000000;
            doorOpenSeconds->setValue(0, 0, openDuration);
        }
        
        previousOpen = currentOpen;
    }
}