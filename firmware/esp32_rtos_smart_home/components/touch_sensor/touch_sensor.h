#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>

#ifdef __cplusplus

// Number of touch sensors to support (compile-time constant)
// The TTP223B is a simple digital touch sensor - one GPIO per sensor
#define TOUCH_SENSOR_COUNT 2

// Health check configuration
#define TOUCH_HEALTH_CHECK_INTERVAL_MS 30000   // Check health every 30 seconds
#define TOUCH_STUCK_THRESHOLD_MS       60000   // Consider sensor stuck if no activity for 60 seconds
#define TOUCH_MAX_REINIT_ATTEMPTS      3       // Max consecutive reinit attempts before backing off

/**
 * TouchSensorComponent - TTP223B Capacitive Touch Sensor Array
 * 
 * Simple capacitive touch detection using TTP223B modules.
 * Each TTP223B outputs a digital HIGH when touched, LOW when not touched.
 * 
 * Exposes:
 * - pins (int[N], writable): GPIO pin numbers for each touch sensor
 * - touched (bool[N], read-only): Current touch state for each sensor
 * 
 * The component supports N sensors (TOUCH_SENSOR_COUNT) which is fixed at
 * compile time. Pins can be configured at runtime via the pins parameter.
 */
class TouchSensorComponent : public Component {
public:
    TouchSensorComponent();
    ~TouchSensorComponent() override;
    
    void onInitialize() override;
    
    static constexpr const char* TAG = "TouchSensor";
    
    // Task handle for ISR access
    TaskHandle_t touch_sensor_task_handle = nullptr;

private:
    // Parameter pointers
    IntParameter* pins = nullptr;      // GPIO pins for each sensor (writable)
    BoolParameter* touched = nullptr;  // Touch state for each sensor (read-only)
    
    // Track configured pins for ISR management
    int32_t configured_pins[TOUCH_SENSOR_COUNT] = {-1};
    
    // Health monitoring state
    int64_t last_activity_time[TOUCH_SENSOR_COUNT] = {0};     // Last ISR/state change timestamp (us)
    int64_t last_health_check_time = 0;                        // Last health check timestamp (us)
    uint8_t reinit_attempts[TOUCH_SENSOR_COUNT] = {0};         // Consecutive reinit attempts per sensor
    bool sensor_stuck_state[TOUCH_SENSOR_COUNT] = {false};     // Track if sensor was in stuck state
    
    // Task functions
    static void touchSensorTaskWrapper(void* pvParameters);
    void touchSensorTask();
    
    // Pin configuration callback
    void onPinChanged(size_t row, size_t col, int32_t newPin);
    
    // Configure a single sensor's GPIO
    void configureGpio(size_t index, int32_t pin);
    
    // Unconfigure a sensor's GPIO (remove ISR, reset config)
    void unconfigureGpio(size_t index);
    
    // Health monitoring functions
    void runHealthCheck();
    void reinitializeSensor(size_t index);
    void recordSensorActivity(size_t index);
};

#endif
