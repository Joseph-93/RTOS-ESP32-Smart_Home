#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#ifdef __cplusplus

// MotionSensorComponent - manages motion sensor reading and parameters
class MotionSensorComponent : public Component {
public:
    MotionSensorComponent();
    ~MotionSensorComponent() override;
    
    void setUpDependencies(ComponentGraph* graph) override;
    void initialize() override;
    
    // Static task entry point for FreeRTOS
    static void motionSensorTaskWrapper(void* pvParameters);
    
    // Instance method that runs the task loop
    void motionSensorTask();

    static constexpr const char* TAG = "MotionSensor";
    
    // Task handle needs to be public for ISR access
    TaskHandle_t motion_sensor_task_handle = nullptr;

private:
    // Add private members here as needed
    TimerHandle_t motion_sensor_timer_handle = nullptr;
    Component* gui_component = nullptr;
};

#endif
