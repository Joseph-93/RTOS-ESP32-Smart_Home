#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <unordered_map>
#include <map>
#include <string>
#include <vector>
#include "cJSON.h"

#ifdef __cplusplus

class ComponentGraph {
public:
    // Notification structure for cross-component notifications
    struct NotificationQueueItem {
        char message[128];  // Fixed-size buffer to avoid std::string issues in queues
        enum class NotificationLevel {
            INFO,
            WARNING,
            ERROR
        } level;
        TickType_t ticks_to_display;
        int priority;  // Higher = more important
    };

private:
    // Owning map: name -> Component* (components owned externally, but registered here)
    std::map<std::string, Component*> componentsByName;
    
    // Non-owning map: componentId -> Component* (same pointers as componentsByName)
    std::unordered_map<uint32_t, Component*> componentsById;
    
    // Mutex for thread-safe access to component maps
    mutable SemaphoreHandle_t componentsMutex;
    
    // Notification queues (shared across all components)
    QueueHandle_t notification_queue_gui;   // For GUI display
    QueueHandle_t notification_queue_uart;  // For UART logging
    
public:
    ComponentGraph();
    ~ComponentGraph();
    
    // Register a component with the graph
    void registerComponent(Component* component);
    
    // Get a component by name
    Component* getComponent(const std::string& name);
    
    // Get a component by UUID
    Component* getComponentById(uint32_t componentId);
    
    // Get a parameter from any component by component name + parameter name
    BaseParameter* getParam(const std::string& component_name, const std::string& param_name);
    IntParameter* getIntParam(const std::string& component_name, const std::string& param_name);
    FloatParameter* getFloatParam(const std::string& component_name, const std::string& param_name);
    BoolParameter* getBoolParam(const std::string& component_name, const std::string& param_name);
    StringParameter* getStringParam(const std::string& component_name, const std::string& param_name);
    
    // Get a parameter by UUID (searches all components)
    BaseParameter* getParamById(uint32_t paramId);
    
    // Initialize all registered components in dependency order
    void initializeAll();
    
    // Get all component names (useful for debugging)
    std::vector<std::string> getComponentNames() const;
    
    // Check if a component exists
    bool hasComponent(const std::string& name) const;
    
    // Send notification to all notification consumers (GUI, UART, etc.)
    void sendNotification(const char* message, bool is_error, int priority = 2, uint32_t display_ms = 3000);
    
    // Execute a JSON message (core message processor)
    cJSON* executeMessage(const char* json_str);
    cJSON* executeMessage(cJSON* request);
    
    // Public queue handles (GUI/UART tasks read from these)
    QueueHandle_t getGuiNotificationQueue() const { return notification_queue_gui; }
    QueueHandle_t getUartNotificationQueue() const { return notification_queue_uart; }

    static constexpr const char* TAG = "ComponentGraph";
};

#endif
