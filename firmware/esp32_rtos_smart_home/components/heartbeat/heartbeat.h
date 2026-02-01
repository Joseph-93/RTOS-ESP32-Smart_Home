#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Heartbeat Component
 * 
 * Simple component that pulses a boolean parameter from 0→1→0 at a configurable rate.
 * Other devices can subscribe to this to monitor if the ESP32 is alive.
 */
class HeartbeatComponent : public Component {
public:
    HeartbeatComponent();
    
    void onInitialize() override;
    
private:
    BoolParameter* heartbeat;      // The pulse: toggles 0→1→0
    FloatParameter* rateHz;        // How many beats per second (Hz)
    
    TaskHandle_t taskHandle;
    
    static void heartbeatTaskWrapper(void* pvParameters);
    void heartbeatTask();
};
