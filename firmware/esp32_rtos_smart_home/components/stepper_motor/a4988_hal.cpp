#include "a4988_hal.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "rom/ets_sys.h"

// Static constexpr member definitions (required for ODR-use in C++14)
constexpr gpio_num_t A4988StepperMotorHAL::STEP_PINS[];
constexpr gpio_num_t A4988StepperMotorHAL::DIR_PINS[];
constexpr gpio_num_t A4988StepperMotorHAL::LIMIT_MIN_PINS[];
constexpr gpio_num_t A4988StepperMotorHAL::LIMIT_MAX_PINS[];

// ============================================================================
// Constructor / Destructor
// ============================================================================

A4988StepperMotorHAL::A4988StepperMotorHAL() 
    : initialized(false),
      currentMicrostepping(16)  // Default to 1/16 microstepping
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
        .pin_bit_mask = (1ULL << PIN_STEPPER_ENABLE),
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
    gpio_set_level(PIN_STEPPER_ENABLE, 1);  // Start DISABLED (HIGH = disabled for A4988)
    ESP_LOGI(TAG, "ENABLE pin: GPIO%d (starting disabled)", PIN_STEPPER_ENABLE);
    
    // Configure MS1/MS2/MS3 pins as outputs (shared microstepping control)
    gpio_config_t ms_conf = {
        .pin_bit_mask = (1ULL << PIN_STEPPER_MS1) | (1ULL << PIN_STEPPER_MS2) | (1ULL << PIN_STEPPER_MS3),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    err = gpio_config(&ms_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure MS pins: %d", err);
        return err;
    }
    
    // Set default microstepping (1/16)
    applyMicrosteppingPins(currentMicrostepping);
    ESP_LOGI(TAG, "MS pins: MS1=GPIO%d, MS2=GPIO%d, MS3=GPIO%d (default 1/%d)", 
             PIN_STEPPER_MS1, PIN_STEPPER_MS2, PIN_STEPPER_MS3, currentMicrostepping);
    
    // Configure MIN LIMIT switch pins as inputs
    // GPIO 34-39 are input-only and don't have internal pullups
    // External pullup resistors required (10K to 3.3V)
    for (int i = 0; i < NUM_MOTORS; i++) {
        gpio_config_t limit_conf = {
            .pin_bit_mask = (1ULL << LIMIT_MIN_PINS[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,  // No internal pullup on 34-39
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        err = gpio_config(&limit_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure LIMIT_MIN pin %d: %d", LIMIT_MIN_PINS[i], err);
            return err;
        }
        ESP_LOGD(TAG, "Motor %d LIMIT_MIN pin: GPIO%d", i, LIMIT_MIN_PINS[i]);
    }
    
    // Configure MAX LIMIT switch pins as inputs with internal pullup
    // GPIOs 4, 5, 13, 33 support internal pullup — no external resistor needed
    for (int i = 0; i < NUM_MOTORS; i++) {
        gpio_config_t limit_max_conf = {
            .pin_bit_mask = (1ULL << LIMIT_MAX_PINS[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,   // Internal pullup — active LOW
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        err = gpio_config(&limit_max_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure LIMIT_MAX pin %d: %d", LIMIT_MAX_PINS[i], err);
            return err;
        }
        ESP_LOGD(TAG, "Motor %d LIMIT_MAX pin: GPIO%d", i, LIMIT_MAX_PINS[i]);
    }
    
    initialized = true;
    
    ESP_LOGI(TAG, "A4988 HAL initialized successfully");
    ESP_LOGI(TAG, "Pin assignment:");
    ESP_LOGI(TAG, "  Motor 0: STEP=GPIO%d, DIR=GPIO%d, LIMIT_MIN=GPIO%d, LIMIT_MAX=GPIO%d", 
             STEP_PINS[0], DIR_PINS[0], LIMIT_MIN_PINS[0], LIMIT_MAX_PINS[0]);
    ESP_LOGI(TAG, "  Motor 1: STEP=GPIO%d, DIR=GPIO%d, LIMIT_MIN=GPIO%d, LIMIT_MAX=GPIO%d", 
             STEP_PINS[1], DIR_PINS[1], LIMIT_MIN_PINS[1], LIMIT_MAX_PINS[1]);
    ESP_LOGI(TAG, "  Motor 2: STEP=GPIO%d, DIR=GPIO%d, LIMIT_MIN=GPIO%d, LIMIT_MAX=GPIO%d", 
             STEP_PINS[2], DIR_PINS[2], LIMIT_MIN_PINS[2], LIMIT_MAX_PINS[2]);
    ESP_LOGI(TAG, "  Motor 3: STEP=GPIO%d, DIR=GPIO%d, LIMIT_MIN=GPIO%d, LIMIT_MAX=GPIO%d", 
             STEP_PINS[3], DIR_PINS[3], LIMIT_MIN_PINS[3], LIMIT_MAX_PINS[3]);
    ESP_LOGI(TAG, "  ENABLE (shared): GPIO%d", PIN_STEPPER_ENABLE);
    ESP_LOGI(TAG, "  Microstepping: 1/%d", currentMicrostepping);
    
    return ESP_OK;
}

void A4988StepperMotorHAL::deinit() {
    if (!initialized) return;
    
    // Disable motors
    gpio_set_level(PIN_STEPPER_ENABLE, 1);  // HIGH = disabled
    
    // Reset all pins to default state
    for (int i = 0; i < NUM_MOTORS; i++) {
        gpio_reset_pin(STEP_PINS[i]);
        gpio_reset_pin(DIR_PINS[i]);
        gpio_reset_pin(LIMIT_MIN_PINS[i]);
        gpio_reset_pin(LIMIT_MAX_PINS[i]);
    }
    gpio_reset_pin(PIN_STEPPER_ENABLE);
    gpio_reset_pin(PIN_STEPPER_MS1);
    gpio_reset_pin(PIN_STEPPER_MS2);
    gpio_reset_pin(PIN_STEPPER_MS3);
    
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
        gpio_set_level(PIN_STEPPER_ENABLE, enabled ? 0 : 1);
        ESP_LOGD(TAG, "Motors %s", enabled ? "ENABLED" : "DISABLED");
    }
}

bool IRAM_ATTR A4988StepperMotorHAL::isLimitTriggered(uint8_t motor_index) {
    if (motor_index >= NUM_MOTORS) return false;
    // Active LOW: LOW = triggered
    return (gpio_get_level(LIMIT_MIN_PINS[motor_index]) == 0);
}

bool IRAM_ATTR A4988StepperMotorHAL::isMaxLimitTriggered(uint8_t motor_index) {
    if (motor_index >= NUM_MOTORS) return false;
    // Active LOW: LOW = triggered
    return (gpio_get_level(LIMIT_MAX_PINS[motor_index]) == 0);
}

// ============================================================================
// Microstepping Configuration
// ============================================================================

bool A4988StepperMotorHAL::setMicrostepping(uint16_t divisor) {
    // A4988 supports: 1 (full), 2 (half), 4 (quarter), 8 (eighth), 16 (sixteenth)
    switch (divisor) {
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
            break;
        default:
            ESP_LOGW(TAG, "Invalid microstepping divisor %d (A4988 supports 1,2,4,8,16)", divisor);
            return false;
    }
    
    applyMicrosteppingPins(divisor);
    currentMicrostepping = divisor;
    ESP_LOGI(TAG, "Microstepping set to 1/%d", divisor);
    return true;
}

void A4988StepperMotorHAL::applyMicrosteppingPins(uint16_t divisor) {
    /*
     * A4988 Microstepping Truth Table:
     *   MS1  MS2  MS3  | Resolution
     *   ---------------|-----------
     *    L    L    L   | Full step (1)
     *    H    L    L   | Half step (2)
     *    L    H    L   | Quarter step (4)
     *    H    H    L   | Eighth step (8)
     *    H    H    H   | Sixteenth step (16)
     */
    
    bool ms1 = false, ms2 = false, ms3 = false;
    
    switch (divisor) {
        case 1:   // Full step
            ms1 = false; ms2 = false; ms3 = false;
            break;
        case 2:   // Half step
            ms1 = true;  ms2 = false; ms3 = false;
            break;
        case 4:   // Quarter step
            ms1 = false; ms2 = true;  ms3 = false;
            break;
        case 8:   // Eighth step
            ms1 = true;  ms2 = true;  ms3 = false;
            break;
        case 16:  // Sixteenth step (default)
            ms1 = true;  ms2 = true;  ms3 = true;
            break;
        default:
            // Default to 1/16 for safety (smoothest)
            ms1 = true;  ms2 = true;  ms3 = true;
            break;
    }
    
    gpio_set_level(PIN_STEPPER_MS1, ms1 ? 1 : 0);
    gpio_set_level(PIN_STEPPER_MS2, ms2 ? 1 : 0);
    gpio_set_level(PIN_STEPPER_MS3, ms3 ? 1 : 0);
    
    ESP_LOGD(TAG, "MS pins set: MS1=%d, MS2=%d, MS3=%d (1/%d stepping)", 
             ms1, ms2, ms3, divisor);
}
