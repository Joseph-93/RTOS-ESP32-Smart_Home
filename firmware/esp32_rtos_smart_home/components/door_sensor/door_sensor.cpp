#include "door_sensor.h"
#include "component_graph.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include "driver/gpio.h"
#include <cmath>

#define DOOR_SENSOR_PIN 34 // GPIO34

DoorSensorComponent::DoorSensorComponent() 
    : Component("DoorSensor") {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] DoorSensorComponent constructor");
#endif
    ESP_LOGI(TAG, "DoorSensorComponent created");
}

DoorSensorComponent::~DoorSensorComponent() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] ~DoorSensorComponent");
#endif
    ESP_LOGI(TAG, "DoorSensorComponent destroyed");
}

void DoorSensorComponent::setUpDependencies(ComponentGraph* graph) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] DoorSensorComponent::setUpDependencies");
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

static void IRAM_ATTR door_sensor_isr_handler(void* arg) {
    DoorSensorComponent* component = static_cast<DoorSensorComponent*>(arg);
    if (component && component->door_sensor_task_handle) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(component->door_sensor_task_handle, &xHigherPriorityTaskWoken);
    }
}

void DoorSensorComponent::initialize() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] DoorSensorComponent::initialize");
#endif

    addIntParam("last_door_event_seconds", 1, 1, 0, INT32_MAX, 0);
    addIntParam("door_state", 1, 1, 0, 1, 0); // 0 = closed, 1 = open

    // Configure door sensor GPIO pin
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << DOOR_SENSOR_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;  // Pull-down so default is LOW
    io_conf.intr_type = GPIO_INTR_ANYEDGE;  // Trigger on both edges (door opened/closed)
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    
    ESP_LOGI(TAG, "Door sensor GPIO %d configured", DOOR_SENSOR_PIN);

    // Install ISR service if not already installed (GUI might have already done this)
    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "GPIO ISR service installed");
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "GPIO ISR service already installed (probably by GUI)");
    } else {
        ESP_ERROR_CHECK(ret);  // Fail on unexpected errors
    }
    
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)DOOR_SENSOR_PIN, door_sensor_isr_handler, this));
    ESP_LOGI(TAG, "Door sensor ISR handler registered for GPIO %d", DOOR_SENSOR_PIN);


    BaseType_t result = xTaskCreate(
        DoorSensorComponent::doorSensorTaskWrapper,
        "door_sensor_task",
        8192, // Stack depth - increased due to callback chain and logging
        this, // Just send the pointer to this component
        tskIDLE_PRIORITY + 1,
        &door_sensor_task_handle
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create door sensor task");
    } else {
        ESP_LOGI(TAG, "Door sensor task created successfully");
    }

    initialized = true;
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] DoorSensorComponent::initialize");
#endif
}

// Static task entry point - required for FreeRTOS task creation
void DoorSensorComponent::doorSensorTaskWrapper(void* pvParameters) {
    DoorSensorComponent* sensor = static_cast<DoorSensorComponent*>(pvParameters);
    sensor->doorSensorTask();
}

// Instance method containing the actual task loop and logic
void DoorSensorComponent::doorSensorTask() {
    ESP_LOGI(TAG, "Door sensor task started");
    
    IntParameter* last_door_event_seconds_param = getIntParam("last_door_event_seconds");
    IntParameter* door_state_param = getIntParam("door_state");
    
    while (1) {
        // Wait for notification from ISR (blocking)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Read the current state of the door sensor
        int current_state = gpio_get_level((gpio_num_t)DOOR_SENSOR_PIN);
                
        // Update door state
        if (door_state_param) {
            door_state_param->setValue(0, 0, current_state);
        }
        
        // Send notification via ComponentGraph
        if (component_graph) {
            if (current_state == 1) {
                component_graph->sendNotification("Door Opened!", false, 4, 3000);
                this->executeDoorOpenedActions();
            } else {
                component_graph->sendNotification("Door Closed!", false, 4, 3000);
            }
        }
    }
}

void DoorSensorComponent::executeDoorOpenedActions() {
    // Set the time of last-door-opened event
    static IntParameter* last_door_event_seconds_param = getIntParam("last_door_event_seconds");
    if (last_door_event_seconds_param) {
        last_door_event_seconds_param->setValue(0, 0, esp_timer_get_time() / 1000000); // time in seconds
    }

    // Check if this is an entry or departure event based on the motion sensor's last motion time
    if (component_graph) {
        static IntParameter* last_motion_seconds_param = component_graph->getIntParam("MotionSensor", "last_motion_detected_seconds");
        if (last_motion_seconds_param && last_door_event_seconds_param) {
            int last_motion = last_motion_seconds_param->getValue(0, 0);
            int last_door_open = last_door_event_seconds_param->getValue(0, 0);
            int time_diff = last_door_open - last_motion;
            if (time_diff >= 0 && time_diff <= 30) {
                ESP_LOGI(TAG, "Door classified as DEPARTURE (motion %d s before door)", time_diff);
                return;
            }
            else {
                ESP_LOGI(TAG, "Door classified as ENTRY (no recent motion before door)");
            }
        }
    }
}