#pragma once

#include "stepper_motor.h"
#include "driver/gpio.h"
#include "pin_config.h"

/**
 * A4988 Stepper Motor Driver HAL
 * 
 * Hardware: A4988 driver board with 12V NEMA 17 stepper motors
 * Microstepping: Configurable via MS1/MS2/MS3 pins (directly controlled by ESP32)
 * 
 * Pin connections per motor:
 * - STEP: Rising edge triggers one microstep
 * - DIR:  HIGH = CW (retract/positive), LOW = CCW (extend/negative)
 * 
 * Shared pins:
 * - ENABLE: Active LOW (LOW = drivers enabled, HIGH = disabled/freewheel)
 * - MS1/MS2/MS3: Microstepping select (shared across all 4 drivers)
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
 * Microstepping table (A4988):
 *   MS1  MS2  MS3  | Resolution
 *   ---------------|-----------
 *    L    L    L   | Full step (1)
 *    H    L    L   | Half step (2)
 *    L    H    L   | Quarter step (4)
 *    H    H    L   | Eighth step (8)
 *    H    H    H   | Sixteenth step (16)
 * 
 * ESP-WROOM-32 30-pin module pin assignment:
 * Chosen to avoid boot strapping pins and flash pins.
 * This build uses ONLY heartbeat + webserver + stepper_motor components.
 */

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
    bool isLimitTriggered(uint8_t motor_index) override;       // Min (retract) limit
    bool isMaxLimitTriggered(uint8_t motor_index) override;    // Max (pay-out) limit
    
    uint8_t getMotorCount() const override { return NUM_MOTORS; }
    uint32_t getMinPulseWidthUs() const override { return MIN_PULSE_WIDTH_US; }
    
    // Microstepping configuration
    bool setMicrostepping(uint16_t divisor) override;
    uint16_t getMicrostepping() const override { return currentMicrostepping; }
    bool isMicrosteppingSoftwareConfigurable() const override { return true; }
    
private:
    static constexpr const char* TAG = "A4988_HAL";
    static constexpr uint8_t NUM_MOTORS = 4;
    static constexpr uint32_t MIN_PULSE_WIDTH_US = 2;  // 2 µs (safe margin over 1 µs spec)
    
    // Pin arrays for easy indexing
    static constexpr gpio_num_t STEP_PINS[NUM_MOTORS] = {
        PIN_STEPPER_MOTOR0_STEP,
        PIN_STEPPER_MOTOR1_STEP,
        PIN_STEPPER_MOTOR2_STEP,
        PIN_STEPPER_MOTOR3_STEP
    };
    
    static constexpr gpio_num_t DIR_PINS[NUM_MOTORS] = {
        PIN_STEPPER_MOTOR0_DIR,
        PIN_STEPPER_MOTOR1_DIR,
        PIN_STEPPER_MOTOR2_DIR,
        PIN_STEPPER_MOTOR3_DIR
    };
    
    static constexpr gpio_num_t LIMIT_MIN_PINS[NUM_MOTORS] = {
        PIN_STEPPER_LIMIT_MIN0,
        PIN_STEPPER_LIMIT_MIN1,
        PIN_STEPPER_LIMIT_MIN2,
        PIN_STEPPER_LIMIT_MIN3
    };
    
    static constexpr gpio_num_t LIMIT_MAX_PINS[NUM_MOTORS] = {
        PIN_STEPPER_LIMIT_MAX0,
        PIN_STEPPER_LIMIT_MAX1,
        PIN_STEPPER_LIMIT_MAX2,
        PIN_STEPPER_LIMIT_MAX3
    };
    
    bool initialized;
    bool currentDirection[NUM_MOTORS];  // Track direction to avoid redundant writes
    uint16_t currentMicrostepping;       // Current microstepping divisor (1,2,4,8,16)
    
    // Inline delay for step pulse width (busy-wait, ISR safe)
    inline void IRAM_ATTR delayMicroseconds(uint32_t us) const {
        uint32_t start = esp_cpu_get_cycle_count();
        uint32_t cycles = us * (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
        while ((esp_cpu_get_cycle_count() - start) < cycles) {
            // Busy wait
        }
    }
    
    // Apply MS pin states for a given divisor
    void applyMicrosteppingPins(uint16_t divisor);
};
