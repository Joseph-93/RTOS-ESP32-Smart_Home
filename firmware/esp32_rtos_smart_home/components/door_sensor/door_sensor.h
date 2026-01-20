#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#ifdef __cplusplus

// DoorSensorComponent - manages door sensor reading and parameters
class DoorSensorComponent : public Component {
public:
    DoorSensorComponent();
    ~DoorSensorComponent() override;
    
    void setUpDependencies(ComponentGraph* graph) override;
    void onInitialize() override;
    
    // Static task entry point for FreeRTOS
    static void doorSensorTaskWrapper(void* pvParameters);
    static void doorSensorStateTaskWrapper(void* pvParameters);
    
    // Instance method that runs the task loop
    void doorSensorTask();
    void doorSensorStateTask();

    static constexpr const char* TAG = "DoorSensor";
    
    // Task handle needs to be public for ISR access
    TaskHandle_t door_sensor_task_handle = nullptr;
    TaskHandle_t door_sensor_state_task_handle = nullptr;

private:
    // Add private members here as needed
    TimerHandle_t door_sensor_timer_handle = nullptr;
    Component* gui_component = nullptr;
    
    // Actions to execute when door is opened
    void executeDoorOpenedActions();

    enum class DoorTrackingState {
        CLOSED,
        OPENED,
        OPENED_TOO_LONG,
    };
    DoorTrackingState door_tracking_state = DoorTrackingState::CLOSED;
};

#endif
