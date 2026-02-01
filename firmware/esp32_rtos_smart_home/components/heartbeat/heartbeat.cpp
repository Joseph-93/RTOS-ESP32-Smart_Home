#include "heartbeat.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "Heartbeat";

HeartbeatComponent::HeartbeatComponent() 
    : Component("Heartbeat")
    , taskHandle(nullptr)
{
}

void HeartbeatComponent::onInitialize() {
    ESP_LOGI(TAG, "Setting up Heartbeat component");
    
    // The heartbeat pulse - read-only, other devices subscribe to this
    heartbeat = addBoolParam("pulse", 1, 1, false, true);  // read-only
    
    // Configurable rate in Hz (beats per second)
    // Default 1.0 Hz = 1 beat per second
    // Min 0.1 Hz = 1 beat every 10 seconds
    // Max 10.0 Hz = 10 beats per second
    rateHz = addFloatParam("rate_hz", 1, 1, 0.1f, 10.0f, 1.0f);
    
    // Create the heartbeat task
    BaseType_t result = xTaskCreate(
        heartbeatTaskWrapper,
        "heartbeat_task",
        2048,
        this,
        tskIDLE_PRIORITY + 1,
        &taskHandle
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create heartbeat task");
    } else {
        ESP_LOGI(TAG, "Heartbeat task created, rate: %.2f Hz", rateHz->getValue(0, 0));
    }
}

void HeartbeatComponent::heartbeatTaskWrapper(void* pvParameters) {
    HeartbeatComponent* self = static_cast<HeartbeatComponent*>(pvParameters);
    self->heartbeatTask();
}

void HeartbeatComponent::heartbeatTask() {
    bool beatState = false;
    
    while (true) {
        float rate = rateHz->getValue(0, 0);
        
        // Period in milliseconds for half a beat (0→1 or 1→0 transition)
        // Full beat is 1/rate seconds, half beat is 1/(2*rate) seconds
        uint32_t halfPeriodMs = (uint32_t)(1000.0f / (2.0f * rate));
        
        // Toggle the beat state
        beatState = !beatState;
        heartbeat->setValue(0, 0, beatState);  // Triggers onChange which broadcasts to subscribers
        
        // Only log on rising edge to avoid spam
        if (beatState) {
            ESP_LOGD(TAG, "♥ beat (rate: %.2f Hz)", rate);
        }
        
        vTaskDelay(pdMS_TO_TICKS(halfPeriodMs));
    }
}
