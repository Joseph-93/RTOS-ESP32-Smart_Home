#pragma once

#include "stepper_motor.h"
#include "driver/gpio.h"

/**
 * A4988 Stepper Motor Driver HAL
 * 
 * Hardware: A4988 driver board with 12V NEMA 17 stepper motors
 * Microstepping: Configurable via MS1/MS2/MS3 pins (set externally)
 * 
 * Pin connections per motor:
 * - STEP: Rising edge triggers one microstep
 * - DIR:  HIGH = CW (retract/positive), LOW = CCW (extend/negative)
 * 
 * Shared pins:
 * - ENABLE: Active LOW (LOW = drivers enabled, HIGH = disabled/freewheel)
 * 
 * Limit switches:
 * - Input only GPIOs (34, 35, 36, 39)
 * - Active LOW with external pullup (triggered when switch closes to GND)
 * 
 * Timing requirements (A4988):
 * - Minimum STEP pulse width: 1 µs
 * - Minimum STEP low time: 1 µs  
 * - Direction setup time before STEP: 200 ns
 * - Enable setup time: 200 ns
 * 
 * ESP-WROOM-32 30-pin module pin assignment:
 * Chosen to avoid boot strapping pins and flash pins.
 * This build uses ONLY heartbeat + webserver + stepper_motor components.
 */

// ============================================================================
// Pin Definitions - ESP-WROOM-32 30-pin
// ============================================================================

// Motor step pins (directly toggle GPIO on rising edge)
#define A4988_MOTOR0_STEP_PIN   GPIO_NUM_16
#define A4988_MOTOR1_STEP_PIN   GPIO_NUM_18
#define A4988_MOTOR2_STEP_PIN   GPIO_NUM_21
#define A4988_MOTOR3_STEP_PIN   GPIO_NUM_23

// Motor direction pins
#define A4988_MOTOR0_DIR_PIN    GPIO_NUM_17
#define A4988_MOTOR1_DIR_PIN    GPIO_NUM_19
#define A4988_MOTOR2_DIR_PIN    GPIO_NUM_22
#define A4988_MOTOR3_DIR_PIN    GPIO_NUM_25

// Shared enable pin (active LOW - all 4 drivers wired together)
#define A4988_ENABLE_PIN        GPIO_NUM_26

// Limit switch pins (input only GPIOs, active LOW)
#define A4988_LIMIT0_PIN        GPIO_NUM_34
#define A4988_LIMIT1_PIN        GPIO_NUM_35
#define A4988_LIMIT2_PIN        GPIO_NUM_36
#define A4988_LIMIT3_PIN        GPIO_NUM_39

// ============================================================================
// A4988 HAL Implementation
// ============================================================================

class A4988StepperMotorHAL : public StepperMotorHAL {
public:
    A4988StepperMotorHAL();
    ~A4988StepperMotorHAL() override;
    
    // ========================================================================
    // StepperMotorHAL Interface Implementation
    // ========================================================================
    
    esp_err_t init() override;
    void deinit() override;
    
    void setDirection(uint8_t motor_index, bool direction) override;
    void step(uint8_t motor_index) override;
    void stepMultiple(uint8_t motor_mask) override;
    void setEnabled(uint8_t motor_index, bool enabled) override;
    bool isLimitTriggered(uint8_t motor_index) override;
    
    uint8_t getMotorCount() const override { return NUM_MOTORS; }
    uint32_t getMinPulseWidthUs() const override { return MIN_PULSE_WIDTH_US; }
    
private:
    static constexpr const char* TAG = "A4988_HAL";
    static constexpr uint8_t NUM_MOTORS = 4;
    static constexpr uint32_t MIN_PULSE_WIDTH_US = 2;  // 2 µs (safe margin over 1 µs spec)
    
    // Pin arrays for easy indexing
    static constexpr gpio_num_t STEP_PINS[NUM_MOTORS] = {
        A4988_MOTOR0_STEP_PIN,
        A4988_MOTOR1_STEP_PIN,
        A4988_MOTOR2_STEP_PIN,
        A4988_MOTOR3_STEP_PIN
    };
    
    static constexpr gpio_num_t DIR_PINS[NUM_MOTORS] = {
        A4988_MOTOR0_DIR_PIN,
        A4988_MOTOR1_DIR_PIN,
        A4988_MOTOR2_DIR_PIN,
        A4988_MOTOR3_DIR_PIN
    };
    
    static constexpr gpio_num_t LIMIT_PINS[NUM_MOTORS] = {
        A4988_LIMIT0_PIN,
        A4988_LIMIT1_PIN,
        A4988_LIMIT2_PIN,
        A4988_LIMIT3_PIN
    };
    
    bool initialized;
    bool currentDirection[NUM_MOTORS];  // Track direction to avoid redundant writes
    
    // Inline delay for step pulse width (busy-wait, ISR safe)
    inline void IRAM_ATTR delayMicroseconds(uint32_t us) const {
        uint32_t start = esp_cpu_get_cycle_count();
        uint32_t cycles = us * (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
        while ((esp_cpu_get_cycle_count() - start) < cycles) {
            // Busy wait
        }
    }
};
