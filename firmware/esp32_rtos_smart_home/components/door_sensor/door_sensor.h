#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus

/**
 * DoorSensorComponent - Simple door state sensor
 * 
 * Exposes:
 * - door_open (bool, read-only): Current door state (true = open, false = closed)
 * - door_open_seconds (int, read-only): How long door has been open (0 if closed)
 * - last_door_event_seconds (int, read-only): Timestamp of last state change
 * 
 * External systems (Raspberry Pi hub) can subscribe to these and implement
 * any complex logic (entry/departure detection, alerts, etc.)
 */
class DoorSensorComponent : public Component {
public:
    DoorSensorComponent();
    ~DoorSensorComponent() override;
    
    void onInitialize() override;
    
    static constexpr const char* TAG = "DoorSensor";
    
    // Task handle needs to be public for ISR access
    TaskHandle_t door_sensor_task_handle = nullptr;

private:
    // Read-only sensor state exposed to subscribers
    BoolParameter* doorOpen = nullptr;           // Current state: true=open, false=closed
    IntParameter* doorOpenSeconds = nullptr;     // How long it's been open (0 if closed)
    IntParameter* lastDoorEventSeconds = nullptr; // Timestamp of last state change
    
    // Task functions
    static void doorSensorTaskWrapper(void* pvParameters);
    void doorSensorTask();
    
    // Track when door opened for duration calculation
    int64_t doorOpenedTimestamp = 0;
};

#endif
