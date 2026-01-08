#pragma once

#include "../component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#ifdef __cplusplus

// LightSensorComponent - manages light sensor reading and parameters
class LightSensorComponent : public Component {
public:
    LightSensorComponent();
    ~LightSensorComponent() override;
    
    void initialize() override;
    void setGuiComponent(Component* gui) { gui_component = gui; }
    
    // Static task entry point for FreeRTOS
    static void lightSensorTaskFunc(void* pvParameters);
    
    // Instance method that runs the task loop
    void runLightSensorTask();

private:
    // Add private members here as needed
    TaskHandle_t light_sensor_task_handle = nullptr;
    TimerHandle_t light_sensor_timer_handle = nullptr;
    Component* gui_component = nullptr;
};

#endif
