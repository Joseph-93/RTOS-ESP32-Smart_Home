#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <map>
#include <string>
#include <vector>

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
    std::map<std::string, Component*> components;
    
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
    
    // Get a parameter from any component by component name + parameter name
    IntParameter* getIntParam(const std::string& component_name, const std::string& param_name);
    FloatParameter* getFloatParam(const std::string& component_name, const std::string& param_name);
    BoolParameter* getBoolParam(const std::string& component_name, const std::string& param_name);
    StringParameter* getStringParam(const std::string& component_name, const std::string& param_name);
    
    // Initialize all registered components in dependency order
    void initializeAll();
    
    // Get all component names (useful for debugging)
    std::vector<std::string> getComponentNames() const;
    
    // Check if a component exists
    bool hasComponent(const std::string& name) const;
    
    // Send notification to all notification consumers (GUI, UART, etc.)
    void sendNotification(const char* message, bool is_error, int priority = 2, uint32_t display_ms = 3000);
    
    // Public queue handles (GUI/UART tasks read from these)
    QueueHandle_t getGuiNotificationQueue() const { return notification_queue_gui; }
    QueueHandle_t getUartNotificationQueue() const { return notification_queue_uart; }

    static constexpr const char* TAG = "ComponentGraph";
};

#endif
