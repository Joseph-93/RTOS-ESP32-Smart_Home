#include "heartbeat.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char* TAG = "Heartbeat";

HeartbeatComponent::HeartbeatComponent() 
    : Component("Heartbeat")
    , taskHandle(nullptr)
    , beatCount(0)
{
}

void HeartbeatComponent::onInitialize() {
    // The heartbeat pulse - read-only, other devices subscribe to this
    heartbeat = addBoolParam("pulse", 1, 1, false, true);  // read-only
    
    // Configurable rate in Hz (beats per second)
    // Default 1.0 Hz = 1 beat per second
    // Min 0.1 Hz = 1 beat every 10 seconds
    // Max 10.0 Hz = 10 beats per second
    rateHz = addFloatParam("rate_hz", 1, 1, 0.1f, 10.0f, 1.0f);
    
    // Create the heartbeat task
    // Stack needs to be large enough for: ESP_LOGI → log hook → logParam->setValue
    // → onChange → cJSON broadcast chain. 2048 was too small after log hook was added.
    BaseType_t result = xTaskCreate(
        heartbeatTaskWrapper,
        "heartbeat_task",
        4096,
        this,
        tskIDLE_PRIORITY + 1,
        &taskHandle
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create heartbeat task");
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
        
        // On rising edge: increment counter and periodically log status
        if (beatState) {
            beatCount++;
            
            // Every 10 beats (~10s at 1Hz), write a detailed log via ESP_LOGI()
            // This feeds the WebSocket log pipeline to the central hub
            if (beatCount % 10 == 0) {
                uint32_t uptimeSec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
                uint32_t freeHeap = esp_get_free_heap_size();
                ESP_LOGI(TAG, "uptime %lus, heap %luB, beats %lu, rate %.1fHz",
                         uptimeSec, freeHeap, beatCount, rate);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(halfPeriodMs));
    }
}
