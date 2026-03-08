#include "a4988_hal.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "rom/ets_sys.h"

// Static constexpr member definitions (required for ODR-use in C++14)
constexpr gpio_num_t A4988StepperMotorHAL::STEP_PINS[];
constexpr gpio_num_t A4988StepperMotorHAL::DIR_PINS[];
constexpr gpio_num_t A4988StepperMotorHAL::LIMIT_PINS[];

// ============================================================================
// Constructor / Destructor
// ============================================================================

A4988StepperMotorHAL::A4988StepperMotorHAL() 
    : initialized(false) 
{
    for (int i = 0; i < NUM_MOTORS; i++) {
        currentDirection[i] = false;
    }
}

A4988StepperMotorHAL::~A4988StepperMotorHAL() {
    deinit();
}

// ============================================================================
// Initialization
// ============================================================================

esp_err_t A4988StepperMotorHAL::init() {
    ESP_LOGI(TAG, "Initializing A4988 HAL for %d motors", NUM_MOTORS);
    
    // Configure STEP pins as outputs
    for (int i = 0; i < NUM_MOTORS; i++) {
        gpio_config_t step_conf = {
            .pin_bit_mask = (1ULL << STEP_PINS[i]),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        esp_err_t err = gpio_config(&step_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure STEP pin %d: %d", STEP_PINS[i], err);
            return err;
        }
        gpio_set_level(STEP_PINS[i], 0);  // Start LOW
        
        ESP_LOGD(TAG, "Motor %d STEP pin: GPIO%d", i, STEP_PINS[i]);
    }
    
    // Configure DIR pins as outputs
    for (int i = 0; i < NUM_MOTORS; i++) {
        gpio_config_t dir_conf = {
            .pin_bit_mask = (1ULL << DIR_PINS[i]),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        esp_err_t err = gpio_config(&dir_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure DIR pin %d: %d", DIR_PINS[i], err);
            return err;
        }
        gpio_set_level(DIR_PINS[i], 0);  // Start with direction = extend (negative)
        currentDirection[i] = false;
        
        ESP_LOGD(TAG, "Motor %d DIR pin: GPIO%d", i, DIR_PINS[i]);
    }
    
    // Configure ENABLE pin as output (shared for all motors)
    gpio_config_t enable_conf = {
        .pin_bit_mask = (1ULL << A4988_ENABLE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    esp_err_t err = gpio_config(&enable_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ENABLE pin: %d", err);
        return err;
    }
    gpio_set_level(A4988_ENABLE_PIN, 1);  // Start DISABLED (HIGH = disabled for A4988)
    ESP_LOGI(TAG, "ENABLE pin: GPIO%d (starting disabled)", A4988_ENABLE_PIN);
    
    // Configure LIMIT switch pins as inputs
    // GPIO 34-39 are input-only and don't have internal pullups
    // External pullup resistors required (10K to 3.3V typical)
    for (int i = 0; i < NUM_MOTORS; i++) {
        gpio_config_t limit_conf = {
            .pin_bit_mask = (1ULL << LIMIT_PINS[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,  // No internal pullup on 34-39
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        err = gpio_config(&limit_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure LIMIT pin %d: %d", LIMIT_PINS[i], err);
            return err;
        }
        
        ESP_LOGD(TAG, "Motor %d LIMIT pin: GPIO%d", i, LIMIT_PINS[i]);
    }
    
    initialized = true;
    
    ESP_LOGI(TAG, "A4988 HAL initialized successfully");
    ESP_LOGI(TAG, "Pin assignment:");
    ESP_LOGI(TAG, "  Motor 0: STEP=GPIO%d, DIR=GPIO%d, LIMIT=GPIO%d", 
             STEP_PINS[0], DIR_PINS[0], LIMIT_PINS[0]);
    ESP_LOGI(TAG, "  Motor 1: STEP=GPIO%d, DIR=GPIO%d, LIMIT=GPIO%d", 
             STEP_PINS[1], DIR_PINS[1], LIMIT_PINS[1]);
    ESP_LOGI(TAG, "  Motor 2: STEP=GPIO%d, DIR=GPIO%d, LIMIT=GPIO%d", 
             STEP_PINS[2], DIR_PINS[2], LIMIT_PINS[2]);
    ESP_LOGI(TAG, "  Motor 3: STEP=GPIO%d, DIR=GPIO%d, LIMIT=GPIO%d", 
             STEP_PINS[3], DIR_PINS[3], LIMIT_PINS[3]);
    ESP_LOGI(TAG, "  ENABLE (shared): GPIO%d", A4988_ENABLE_PIN);
    
    return ESP_OK;
}

void A4988StepperMotorHAL::deinit() {
    if (!initialized) return;
    
    // Disable motors
    gpio_set_level(A4988_ENABLE_PIN, 1);  // HIGH = disabled
    
    // Reset all pins to default state
    for (int i = 0; i < NUM_MOTORS; i++) {
        gpio_reset_pin(STEP_PINS[i]);
        gpio_reset_pin(DIR_PINS[i]);
        gpio_reset_pin(LIMIT_PINS[i]);
    }
    gpio_reset_pin(A4988_ENABLE_PIN);
    
    initialized = false;
    ESP_LOGI(TAG, "A4988 HAL deinitialized");
}

// ============================================================================
// Motor Control - ISR Safe
// ============================================================================

void IRAM_ATTR A4988StepperMotorHAL::setDirection(uint8_t motor_index, bool direction) {
    if (motor_index >= NUM_MOTORS) return;
    
    // Only write if direction changed (reduces GPIO operations)
    if (currentDirection[motor_index] != direction) {
        gpio_set_level(DIR_PINS[motor_index], direction ? 1 : 0);
        currentDirection[motor_index] = direction;
        
        // A4988 requires 200ns setup time after direction change
        // One CPU cycle at 240MHz is ~4ns, so a few cycles is enough
        // But we add a small delay to be safe
        delayMicroseconds(1);
    }
}

void IRAM_ATTR A4988StepperMotorHAL::step(uint8_t motor_index) {
    if (motor_index >= NUM_MOTORS) return;
    
    gpio_num_t pin = STEP_PINS[motor_index];
    
    // Generate step pulse: HIGH -> delay -> LOW
    gpio_set_level(pin, 1);
    delayMicroseconds(MIN_PULSE_WIDTH_US);
    gpio_set_level(pin, 0);
    
    // Minimum LOW time is also 1µs, but we'll be back in the ISR
    // well after that, so no delay needed here
}

void IRAM_ATTR A4988StepperMotorHAL::stepMultiple(uint8_t motor_mask) {
    // Set all relevant STEP pins HIGH simultaneously
    for (uint8_t i = 0; i < NUM_MOTORS; i++) {
        if (motor_mask & (1 << i)) {
            gpio_set_level(STEP_PINS[i], 1);
        }
    }
    
    // Hold pulse width
    delayMicroseconds(MIN_PULSE_WIDTH_US);
    
    // Set all STEP pins LOW
    for (uint8_t i = 0; i < NUM_MOTORS; i++) {
        if (motor_mask & (1 << i)) {
            gpio_set_level(STEP_PINS[i], 0);
        }
    }
}

void A4988StepperMotorHAL::setEnabled(uint8_t motor_index, bool enabled) {
    // A4988 ENABLE is active LOW: LOW = enabled, HIGH = disabled
    // We use a shared enable pin for all motors
    // motor_index == 0xFF means all motors
    
    if (motor_index == 0xFF || motor_index < NUM_MOTORS) {
        gpio_set_level(A4988_ENABLE_PIN, enabled ? 0 : 1);
        ESP_LOGD(TAG, "Motors %s", enabled ? "ENABLED" : "DISABLED");
    }
}

bool A4988StepperMotorHAL::isLimitTriggered(uint8_t motor_index) {
    if (motor_index >= NUM_MOTORS) return false;
    
    // Limit switches are active LOW (normally HIGH with pullup, LOW when triggered)
    int level = gpio_get_level(LIMIT_PINS[motor_index]);
    return (level == 0);
}
