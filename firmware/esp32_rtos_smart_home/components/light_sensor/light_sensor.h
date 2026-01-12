#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#ifdef __cplusplus

// LightSensorComponent - manages light sensor reading and parameters
class LightSensorComponent : public Component {
public:
    LightSensorComponent();
    ~LightSensorComponent() override;
    
    void setUpDependencies(ComponentGraph* graph) override;
    void initialize() override;
    
    // Static task entry point for FreeRTOS
    static void lightSensorTaskWrapper(void* pvParameters);
    
    // Instance method that runs the task loop
    void lightSensorTask();

    static constexpr const char* TAG = "LightSensor";

private:
    // Add private members here as needed
    TaskHandle_t light_sensor_task_handle = nullptr;
    TimerHandle_t light_sensor_timer_handle = nullptr;
    Component* gui_component = nullptr;
};

#endif
