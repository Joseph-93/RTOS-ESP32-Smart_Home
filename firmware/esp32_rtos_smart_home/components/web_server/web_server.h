#pragma once

#include "component.h"
#include "component_graph.h"
#include <esp_http_server.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <map>
#include <set>
#include <tuple>

// Broadcast queue item - parameter update to send to WebSocket clients
struct BroadcastQueueItem {
    char component[32];
    char param_type[8];
    int idx;
    int row;
    int col;
    char value_json[128];  // Pre-serialized JSON value to avoid cJSON in queue
};

// Subscription key: (component_name, param_type, idx, row, col)
struct SubscriptionKey {
    std::string component;
    std::string param_type;
    int idx;
    int row;
    int col;
    
    bool operator<(const SubscriptionKey& other) const {
        if (component != other.component) return component < other.component;
        if (param_type != other.param_type) return param_type < other.param_type;
        if (idx != other.idx) return idx < other.idx;
        if (row != other.row) return row < other.row;
        return col < other.col;
    }
};

class WebServerComponent : public Component {
public:
    WebServerComponent();
    ~WebServerComponent() override;
    
    void setUpDependencies(ComponentGraph* graph) override;
    void onInitialize() override;
    void postInitialize() override;  // Set up parameter broadcasting after all components initialized
    
    // Called by parameter onChange callbacks to broadcast updates
    void broadcastParameterUpdate(const char* comp, const char* param_type, int idx, int row, int col, cJSON* value);

    static constexpr const char* TAG = "WebServer";

private:
    httpd_handle_t http_server = nullptr;
    ComponentGraph* component_graph = nullptr;
    
    // Subscription tracking: socket_fd -> set of subscribed parameters
    std::map<int, std::set<SubscriptionKey>> subscriptions;
    SemaphoreHandle_t subscriptions_mutex = nullptr;  // Protect subscriptions map from concurrent access
    
    // Broadcast queue and task for thread-safe WebSocket updates
    QueueHandle_t broadcast_queue = nullptr;
    TaskHandle_t broadcast_task_handle = nullptr;
    
    // WebSocket handler
    static esp_err_t ws_handler(httpd_req_t *req);
    
    // WebSocket message processor
    cJSON* handle_ws_message(cJSON* request, const char* msg_type, int socket_fd);
    
    // Subscription management
    void subscribe_param(int socket_fd, const SubscriptionKey& key);
    void unsubscribe_param(int socket_fd, const SubscriptionKey& key);
    void clear_subscriptions(int socket_fd);
    void setupParameterBroadcasting();  // Hook onChange callbacks for all parameters
    
    // Broadcast task
    static void broadcastTaskWrapper(void* pvParameters);
    void broadcastTask();
    
    // Helper methods
    Component* get_component(const char* name);
    static esp_err_t send_json_response(httpd_req_t *req, cJSON* json);
    static esp_err_t send_json_string(httpd_req_t *req, const char* json_str);
};
