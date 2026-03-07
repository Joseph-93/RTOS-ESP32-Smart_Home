#include "touch_sensor.h"
#include "pin_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

// Default GPIO pins for touch sensors (can be changed at runtime)
// Pins configured in common/pin_config.h - set to -1 to disable a sensor slot
static const int32_t DEFAULT_TOUCH_PINS[TOUCH_SENSOR_COUNT] = {
    PIN_TOUCH_SENSOR_0,  // Touch sensor 0
    PIN_TOUCH_SENSOR_1   // Touch sensor 1
};

TouchSensorComponent::TouchSensorComponent() 
    : Component("TouchSensor") {
    // Initialize configured_pins to -1 (unconfigured)
    for (size_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        configured_pins[i] = -1;
    }
}

TouchSensorComponent::~TouchSensorComponent() {
    // Clean up GPIO configurations
    for (size_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        unconfigureGpio(i);
    }
}

static void IRAM_ATTR touch_sensor_isr_handler(void* arg) {
    TouchSensorComponent* component = static_cast<TouchSensorComponent*>(arg);
    if (component && component->touch_sensor_task_handle) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(component->touch_sensor_task_handle, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void TouchSensorComponent::onInitialize() {
    // Create parameters
    // pins: array of GPIO pin numbers (writable, so user can reconfigure)
    // Using x-size = TOUCH_SENSOR_COUNT (cols), y-size = 1 (rows)
    pins = addIntParam("pins", 1, TOUCH_SENSOR_COUNT, 0, 39, -1, false);  // -1 = disabled
    
    // touched: array of touch states (read-only)
    touched = addBoolParam("touched", 1, TOUCH_SENSOR_COUNT, false, true);  // read-only

    // Set default pin values
    for (size_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        pins->setValue(0, i, DEFAULT_TOUCH_PINS[i]);
    }

    // Register callback for pin changes
    pins->setOnChange([this](size_t row, size_t col, int32_t newPin) {
        this->onPinChanged(row, col, newPin);
    });

    // Install ISR service if not already installed
    esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    // Configure GPIOs for default pins and initialize health monitoring
    int64_t now = esp_timer_get_time();
    last_health_check_time = now;
    for (size_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        configureGpio(i, DEFAULT_TOUCH_PINS[i]);
        last_activity_time[i] = now;  // Start with current time
        reinit_attempts[i] = 0;
        sensor_stuck_state[i] = false;
    }

    // Create task for handling sensor events
    BaseType_t result = xTaskCreate(
        TouchSensorComponent::touchSensorTaskWrapper,
        "touch_sensor_task",
        4096,
        this,
        tskIDLE_PRIORITY + 1,
        &touch_sensor_task_handle
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch sensor task");
    }

    initialized = true;
}

void TouchSensorComponent::onPinChanged(size_t row, size_t col, int32_t newPin) {
    // col is the sensor index
    if (col >= TOUCH_SENSOR_COUNT) {
        ESP_LOGE(TAG, "Invalid sensor index: %zu", col);
        return;
    }
    
    // Unconfigure old pin if it was configured
    unconfigureGpio(col);
    
    // Configure new pin if valid
    if (newPin >= 0 && newPin <= 39) {
        configureGpio(col, newPin);
    }
}

void TouchSensorComponent::configureGpio(size_t index, int32_t pin) {
    if (pin < 0 || pin > 39) {
        ESP_LOGW(TAG, "Invalid pin %ld for sensor %zu, skipping", (long)pin, index);
        return;
    }
    
    // Check if pin is already in use by another sensor
    for (size_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        if (i != index && configured_pins[i] == pin) {
            ESP_LOGE(TAG, "Pin %ld already in use by sensor %zu", (long)pin, i);
            return;
        }
    }
    
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << pin);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;  // TTP223B outputs HIGH when touched
    io_conf.intr_type = GPIO_INTR_ANYEDGE;  // Trigger on both touch and release
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO %ld: %s", (long)pin, esp_err_to_name(ret));
        return;
    }
    
    ret = gpio_isr_handler_add((gpio_num_t)pin, touch_sensor_isr_handler, this);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler for GPIO %ld: %s", (long)pin, esp_err_to_name(ret));
        return;
    }
    
    configured_pins[index] = pin;
}

void TouchSensorComponent::unconfigureGpio(size_t index) {
    if (index >= TOUCH_SENSOR_COUNT) return;
    
    int32_t pin = configured_pins[index];
    if (pin < 0) return;  // Not configured
    
    // Remove ISR handler
    gpio_isr_handler_remove((gpio_num_t)pin);
    
    // Reset GPIO to default state
    gpio_reset_pin((gpio_num_t)pin);
    
    configured_pins[index] = -1;
}

void TouchSensorComponent::touchSensorTaskWrapper(void* pvParameters) {
    TouchSensorComponent* sensor = static_cast<TouchSensorComponent*>(pvParameters);
    sensor->touchSensorTask();
}

void TouchSensorComponent::touchSensorTask() {
    bool prev_states[TOUCH_SENSOR_COUNT] = {false};  // Track previous states for logging
    
    while (1) {
        // Wait for notification from ISR or timeout (100ms for periodic polling)
        // Polling ensures we catch states even if ISR is missed
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        
        // Read all configured sensors
        for (size_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
            int32_t pin = configured_pins[i];
            if (pin >= 0 && touched) {
                bool state = gpio_get_level((gpio_num_t)pin) == 1;
                
                // Log state changes and record activity
                if (state != prev_states[i]) {
                    ESP_LOGI(TAG, "Touch sensor %zu (GPIO%ld): %s -> %s", 
                             i, (long)pin, 
                             prev_states[i] ? "PRESSED" : "released",
                             state ? "PRESSED" : "released");
                    prev_states[i] = state;
                    
                    // Record activity - sensor is working!
                    recordSensorActivity(i);
                }
                
                touched->setValue(0, i, state);
            }
        }
        
        // Run periodic health check
        runHealthCheck();
    }
}

void TouchSensorComponent::recordSensorActivity(size_t index) {
    if (index >= TOUCH_SENSOR_COUNT) return;
    
    last_activity_time[index] = esp_timer_get_time();
    
    // Reset reinit attempts counter since sensor is working
    if (reinit_attempts[index] > 0) {
        ESP_LOGI(TAG, "Sensor %zu recovered after %u reinit attempts", index, reinit_attempts[index]);
        reinit_attempts[index] = 0;
    }
    sensor_stuck_state[index] = false;
}

void TouchSensorComponent::runHealthCheck() {
    int64_t now = esp_timer_get_time();
    int64_t elapsed_since_check = (now - last_health_check_time) / 1000;  // Convert to ms
    
    // Only run health check periodically
    if (elapsed_since_check < TOUCH_HEALTH_CHECK_INTERVAL_MS) {
        return;
    }
    
    last_health_check_time = now;
    ESP_LOGD(TAG, "Running health check...");
    
    for (size_t i = 0; i < TOUCH_SENSOR_COUNT; i++) {
        int32_t pin = configured_pins[i];
        if (pin < 0) continue;  // Skip unconfigured sensors
        
        int64_t elapsed_since_activity = (now - last_activity_time[i]) / 1000;  // Convert to ms
        
        // Check if sensor appears stuck (no state changes for a long time)
        if (elapsed_since_activity > TOUCH_STUCK_THRESHOLD_MS) {
            // Only log and attempt reinit if we haven't exceeded max attempts
            if (reinit_attempts[i] < TOUCH_MAX_REINIT_ATTEMPTS) {
                if (!sensor_stuck_state[i]) {
                    ESP_LOGW(TAG, "Sensor %zu (GPIO%ld) appears stuck - no activity for %lld ms", 
                             i, (long)pin, (long long)elapsed_since_activity);
                    sensor_stuck_state[i] = true;
                }
                
                ESP_LOGI(TAG, "Attempting to reinitialize sensor %zu (attempt %u/%d)", 
                         i, reinit_attempts[i] + 1, TOUCH_MAX_REINIT_ATTEMPTS);
                reinitializeSensor(i);
                reinit_attempts[i]++;
            } else if (reinit_attempts[i] == TOUCH_MAX_REINIT_ATTEMPTS) {
                // Log once that we're backing off
                ESP_LOGE(TAG, "Sensor %zu (GPIO%ld) failed after %d reinit attempts - backing off", 
                         i, (long)pin, TOUCH_MAX_REINIT_ATTEMPTS);
                reinit_attempts[i]++;  // Increment to prevent repeated logging
            }
            // After max attempts, just leave it alone until manual intervention or activity
        }
    }
}

void TouchSensorComponent::reinitializeSensor(size_t index) {
    if (index >= TOUCH_SENSOR_COUNT) return;
    
    int32_t pin = configured_pins[index];
    if (pin < 0) return;
    
    ESP_LOGI(TAG, "Reinitializing sensor %zu on GPIO%ld", index, (long)pin);
    
    // Unconfigure and reconfigure the GPIO
    unconfigureGpio(index);
    
    // Small delay to allow GPIO to fully reset
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Reconfigure with the same pin
    configureGpio(index, pin);
    
    // Reset activity timer to give it time to prove itself
    last_activity_time[index] = esp_timer_get_time();
    
    ESP_LOGI(TAG, "Sensor %zu reinitialized", index);
}
