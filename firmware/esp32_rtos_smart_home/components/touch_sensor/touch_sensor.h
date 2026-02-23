#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus

// Number of touch sensors to support (compile-time constant)
// The TTP223B is a simple digital touch sensor - one GPIO per sensor
#define TOUCH_SENSOR_COUNT 2

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
    
    // Task functions
    static void touchSensorTaskWrapper(void* pvParameters);
    void touchSensorTask();
    
    // Pin configuration callback
    void onPinChanged(size_t row, size_t col, int32_t newPin);
    
    // Configure a single sensor's GPIO
    void configureGpio(size_t index, int32_t pin);
    
    // Unconfigure a sensor's GPIO (remove ISR, reset config)
    void unconfigureGpio(size_t index);
};

#endif
