#include "network_actions.h"
#include "esp_log.h"

static const char *TAG = "NetworkActions";

// NetworkActionsComponent implementation

NetworkActionsComponent::NetworkActionsComponent() 
    : Component("NetworkActions") {
    ESP_LOGI(TAG, "NetworkActionsComponent created");
}

NetworkActionsComponent::~NetworkActionsComponent() {
    ESP_LOGI(TAG, "NetworkActionsComponent destroyed");
}

void NetworkActionsComponent::initialize() {
    ESP_LOGI(TAG, "Initializing NetworkActionsComponent...");
    
    // Create some example parameters
    addIntParam("retry_count", 1, 1);        // Single int parameter
    addFloatParam("timeout_sec", 1, 1);      // Single float parameter
    addBoolParam("wifi_enabled", 1, 1);      // Single bool parameter
    
    // Set some default values
    getIntParam("retry_count")->setValue(0, 0, 3);
    getFloatParam("timeout_sec")->setValue(0, 0, 5.0f);
    getBoolParam("wifi_enabled")->setValue(0, 0, true);
    
    initialized = true;
    ESP_LOGI(TAG, "NetworkActionsComponent initialized with %d parameters", 
             getIntParams().size() + getFloatParams().size() + 
             getBoolParams().size() + getStringParams().size());
}

// C API for backward compatibility

void network_actions_init(void) {
    ESP_LOGI(TAG, "Network actions system initialized (C API)");
    // TODO: Initialize network stack, WiFi, etc.
}

void network_actions_send(const char *action, const char *value) {
    ESP_LOGI(TAG, "Action: %s = %s", action, value);
    // TODO: Format and send network message
}
