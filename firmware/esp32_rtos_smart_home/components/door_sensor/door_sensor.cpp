#include "door_sensor.h"
#include "component_graph.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include "driver/gpio.h"
#include <cmath>

#define DOOR_SENSOR_PIN 32 // GPIO32

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

void DoorSensorComponent::onInitialize() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] DoorSensorComponent::initialize");
#endif

    addIntParam("last_door_event_seconds", 1, 1, 0, INT32_MAX, 0, true);
    addIntParam("door_state", 1, 1, 0, 1, 0, true); // 0 = closed, 1 = open
    addIntParam("door_open_too_long_threshold_seconds", 1, 1, 10, 3600, 10);
    addIntParam("entry_departure_motion_threshold_seconds", 1, 1, 0, 300, 30); // Time window to classify as departure

    // Configure door sensor GPIO pin
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << DOOR_SENSOR_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // Pull-up so default is HIGH (1=open if sensor pulls to GND when closed)
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
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

    // Create doorSensorState task (low priority, 1Hz)
    result = xTaskCreate(
        DoorSensorComponent::doorSensorStateTaskWrapper,
        "door_state_task",
        3072,  // Minimal stack - no logging, just conditionals and queue sends
        this,
        tskIDLE_PRIORITY,  // Low priority
        &door_sensor_state_task_handle
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create door sensor state task");
    } else {
        ESP_LOGI(TAG, "Door sensor state task created successfully");
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
    int previous_state = -1;  // Track previous state to detect transitions
    
    while (1) {
        // Wait for notification from ISR (blocking)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        ESP_LOGI(TAG, "*** DOOR SENSOR TRIGGERED *** (GPIO %d ISR fired)", DOOR_SENSOR_PIN);
        
        // Read the current state of the door sensor
        int current_state = gpio_get_level((gpio_num_t)DOOR_SENSOR_PIN);
        
        ESP_LOGI(TAG, "Door state: %s (GPIO level = %d)", current_state ? "OPEN" : "CLOSED", current_state);

        // Update door state
        if (door_state_param) {
            door_state_param->setValue(0, 0, current_state);
        }
        
        // Update the last door event time
        if (last_door_event_seconds_param) {
            int current_time = esp_timer_get_time() / 1000000;
            last_door_event_seconds_param->setValue(0, 0, current_time);
            ESP_LOGI(TAG, "Door event timestamp updated: %d seconds", current_time);
        }
        
        // Detect door opened transition (0->1) and execute entry/departure actions
        if (previous_state == 0 && current_state == 1) {
            ESP_LOGI(TAG, "Door OPENED (transition 0->1) - executing door opened actions");
            executeDoorOpenedActions();
        } else if (previous_state == 1 && current_state == 0) {
            ESP_LOGI(TAG, "Door CLOSED (transition 1->0)");
        }
        
        previous_state = current_state;
    }
}

void DoorSensorComponent::executeDoorOpenedActions() {
    // Set the time of last-door-opened event
    IntParameter* last_door_event_seconds_param = getIntParam("last_door_event_seconds");
    if (last_door_event_seconds_param) {
        last_door_event_seconds_param->setValue(0, 0, esp_timer_get_time() / 1000000); // time in seconds
    }

    // Check if this is an entry or departure event based on the motion sensor's last motion time
    if (component_graph) {
        Component* net_component = component_graph->getComponent("NetworkActions");
        if (!net_component) {
            ESP_LOGW(TAG, "NetworkActions component not found");
            return;
        }
        
        IntParameter* last_motion_seconds_param = component_graph->getIntParam("MotionSensor", "last_motion_detected_seconds");
        if (last_motion_seconds_param && last_door_event_seconds_param) {
            int last_motion = last_motion_seconds_param->getValue(0, 0);
            int last_door_open = last_door_event_seconds_param->getValue(0, 0);
            int time_diff = last_door_open - last_motion;
            
            static IntParameter* threshold_param = getIntParam("entry_departure_motion_threshold_seconds");
            int threshold = threshold_param ? threshold_param->getValue(0, 0) : 30;
            
            const auto& triggers = net_component->getTriggerParams();
            
            if (time_diff >= 0 && time_diff <= threshold) {
                // DEPARTURE - turn lights off
                ESP_LOGI(TAG, "Door classified as DEPARTURE (motion %d s before door)", time_diff);
                for (size_t i = 0; i < triggers.size(); i++) {
                    if (triggers[i]->getName() == "Send HTTP: Living Room Off") {
                        triggers[i]->setValue(0, 0, "");
                        break;
                    }
                }
            }
            else {
                // ENTRY - turn lights to cool bright
                ESP_LOGI(TAG, "Door classified as ENTRY (no recent motion before door)");
                for (size_t i = 0; i < triggers.size(); i++) {
                    if (triggers[i]->getName() == "Send HTTP: Living Room Cool Bright") {
                        triggers[i]->setValue(0, 0, "");
                        break;
                    }
                }
            }
        }
    }
}

// Static task entry point for doorSensorState task
void DoorSensorComponent::doorSensorStateTaskWrapper(void* pvParameters) {
    DoorSensorComponent* sensor = static_cast<DoorSensorComponent*>(pvParameters);
    sensor->doorSensorStateTask();
}

// Instance method for the doorSensorState task loop - runs at 1Hz
void DoorSensorComponent::doorSensorStateTask() {
    ESP_LOGI(TAG, "Door sensor state task started (1Hz)");
    
    IntParameter* door_state_param = nullptr;
    IntParameter* door_open_too_long_thresh_param = nullptr;
    Component* network_actions_component = nullptr;
    TriggerParameter* door_opened_too_long_trigger = nullptr;
    TriggerParameter* door_finally_closed_trigger = nullptr;
    uint64_t door_opened_timestamp = 0;
    
    while (1) {
        // Run at 1Hz (1000ms delay)
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!isInitialized()) {
            continue;
        }

        if (!network_actions_component && component_graph) {
            network_actions_component = component_graph->getComponent("NetworkActions");
            continue;
        }

        if (door_opened_too_long_trigger == nullptr && door_finally_closed_trigger == nullptr) {
            const auto& triggers = network_actions_component->getTriggerParams();
            for (size_t i = 0; i < triggers.size(); i++) {
                if (triggers[i]->getName() == "Send TCP: Shut The Front Door Mark Ruffalo") {
                    door_opened_too_long_trigger = triggers[i].get();
                }
                else if (triggers[i]->getName() == "Send TCP: Door Closed Comedian") {
                    door_finally_closed_trigger = triggers[i].get();
                }
            }
        }

        if (door_state_param == nullptr) {
            door_state_param = getIntParam("door_state");
            continue;
        }

        if (door_open_too_long_thresh_param == nullptr) {
            door_open_too_long_thresh_param = getIntParam("door_open_too_long_threshold_seconds");
            continue;
        }

        // ACTUAL POST-SETUP LOGIC:

        // Read current door state
        int current_state = door_state_param->getValue(0, 0);

        if (door_tracking_state == DoorTrackingState::CLOSED) {
            if (current_state == 0) {
                // Still closed
                continue;
            } else {
                // Just opened
                door_tracking_state = DoorTrackingState::OPENED;
                door_opened_timestamp = esp_timer_get_time();
            }
        }
        if (door_tracking_state == DoorTrackingState::OPENED) {
            if (current_state == 1) {
                // Still opened - check duration
                uint64_t now = esp_timer_get_time();
                uint64_t elapsed_sec = (now - door_opened_timestamp) / 1000000;
                if (elapsed_sec >= door_open_too_long_thresh_param->getValue(0, 0)) {
                    door_tracking_state = DoorTrackingState::OPENED_TOO_LONG;
                    if (door_opened_too_long_trigger) {
                        door_opened_too_long_trigger->setValue(0, 0, "");
                    }
                }
            } else {
                // Door closed again
                door_tracking_state = DoorTrackingState::CLOSED;
            }
        }
        if (door_tracking_state == DoorTrackingState::OPENED_TOO_LONG) {
            if (current_state == 0) {
                // Door finally closed
                door_tracking_state = DoorTrackingState::CLOSED;
                if (door_finally_closed_trigger) {
                    door_finally_closed_trigger->setValue(0, 0, "");
                }
            }
        }
    }
}