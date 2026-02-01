#include "network_actions.h"
#include "component_graph.h"
#include "message_examples.h"
#include "wifi_init.h"
#include "esp_log.h"
#include "cJSON.h"
#include <algorithm>

// Forward declaration to avoid including gui.h (which includes lvgl.h)
class GUIComponent;

// NetworkActionsComponent implementation

NetworkActionsComponent::NetworkActionsComponent() 
    : Component("NetworkActions") {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] NetworkActionsComponent constructor");
#endif
    ESP_LOGI(TAG, "NetworkActionsComponent created");
}

NetworkActionsComponent::~NetworkActionsComponent() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] ~NetworkActionsComponent");
#endif
    ESP_LOGI(TAG, "NetworkActionsComponent destroyed");
}

void NetworkActionsComponent::setUpDependencies(ComponentGraph* graph) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER/EXIT] NetworkActionsComponent::setUpDependencies");
#endif
    this->component_graph = graph;
}

void NetworkActionsComponent::onInitialize() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] NetworkActionsComponent::initialize");
#endif
    ESP_LOGI(TAG, "Initializing NetworkActionsComponent...");
    
    // Add WiFi connection status parameter
    addBoolParam("wifi_connected", 1, 1, false, true);
    ESP_LOGI(TAG, "Added wifi_connected parameter");
    
    // Create EMPTY parameters (0 rows) with 1 column - will grow as we append messages
    // Need at least 1 column for appendValue to work (it divides by cols)
    addStringParam("tcp_messages", 0, 1);
    addStringParam("http_messages", 0, 1);
    addStringParam("ws_messages", 0, 1);
    
    // Load all message examples into the parameters (no parsing - done on-demand when sending)
    loadAllMessageExamples();
    
    // Initialize clients
    tcp_client.initialize();
    http_client.initialize();
    ws_client.initialize();

    network_actions_queue = xQueueCreate(10, sizeof(NetworkActionQueueItem));
    if (network_actions_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create network actions queue");
        assert(false && "Queue creation failed");
    }
    BaseType_t result = xTaskCreate(
        network_actions_task,
        "network_actions_task",
        6144, // Stack depth - queue processing and network calls
        this, // Just send the pointer to this component
        tskIDLE_PRIORITY + 1,
        &network_actions_task_handle
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create network actions task");
        assert(false && "Task creation failed");
    }
    
    // Register WiFi event callback
    wifi_set_status_callback(wifi_event_callback, this);
    
    // Check if WiFi is already connected (callback only fires on state changes)
    // If we registered after WiFi already connected, manually invoke callback
    if (wifi_is_connected()) {
        ESP_LOGI(TAG, "WiFi already connected - updating parameter and sending notification");
        wifi_event_callback(true, this);
    }
    
    // Register actions for each message type
    registerActions();
    
    initialized = true;
    ESP_LOGI(TAG, "NetworkActionsComponent initialized: %zu TCP, %zu HTTP, %zu WS messages",
             getTcpMessageCount(), getHttpMessageCount(), getWsMessageCount());
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] NetworkActionsComponent::initialize");
#endif
}

void NetworkActionsComponent::send_next_from_queue() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] send_next_from_queue");
#endif
    NetworkActionQueueItem item;
    while (true) {
        if (xQueueReceive(network_actions_queue, &item, portMAX_DELAY) == pdTRUE) {
            bool result = false;
            std::string protocol_name;
            std::string message_name;
            
            switch (item.protocol) {
                case NetworkActionQueueItem::NetworkProtocol::TCP: {
                    TcpMessage msg;
                    if (parseTcpMessageAt(item.message_index, msg)) {
                        message_name = msg.name;
                        result = tcp_client.send(msg);
                        protocol_name = "TCP";
                    }
                    break;
                }
                case NetworkActionQueueItem::NetworkProtocol::HTTP: {
                    HttpMessage msg;
                    if (parseHttpMessageAt(item.message_index, msg)) {
                        message_name = msg.name;
                        result = http_client.send(msg);
                        protocol_name = "HTTP";
                    }
                    break;
                }
                case NetworkActionQueueItem::NetworkProtocol::WebSocket: {
                    WsMessage msg;
                    if (parseWsMessageAt(item.message_index, msg)) {
                        message_name = msg.name;
                        result = ws_client.send(msg);
                        protocol_name = "WebSocket";
                    }
                    break;
                }
            }
            ESP_LOGI(TAG, "Sent network action (protocol: %d, index: %zu) - result: %d",
                     static_cast<int>(item.protocol), item.message_index, result);
            
            // Send notification via ComponentGraph
            if (component_graph) {
                std::string msg = protocol_name + ": " + message_name + (result ? " OK" : " FAIL");
                component_graph->sendNotification(msg.c_str(), !result, 2, 3000);
            }
        }
        else {
            ESP_LOGE(TAG, "Failed to receive from network actions queue");
        }
    }
}

void NetworkActionsComponent::wifi_event_callback(bool connected, void* user_data) {
    NetworkActionsComponent* self = static_cast<NetworkActionsComponent*>(user_data);
    if (!self) return;
    
    // Update wifi_connected parameter
    auto* wifi_param = self->getBoolParam("wifi_connected");
    if (wifi_param) {
        wifi_param->setValue(0, 0, connected);
    }
    
    // Send notification via ComponentGraph
    if (self->component_graph) {
        if (connected) {
            self->component_graph->sendNotification("WiFi Connected", false, 3, 3000);
            ESP_LOGI(TAG, "WiFi connected - notification sent");
        } else {
            self->component_graph->sendNotification("WiFi Disconnected", true, 5, 5000);
            ESP_LOGW(TAG, "WiFi disconnected - notification sent");
        }
    }
}

void NetworkActionsComponent::network_actions_task(void* pvParameters) {
    NetworkActionsComponent* network_actions_component = static_cast<NetworkActionsComponent*>(pvParameters);
    network_actions_component->send_next_from_queue();
}

// Helper function to load all message examples from MessageExamples namespace into parameters
void NetworkActionsComponent::loadAllMessageExamples() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] loadAllMessageExamples");
#endif
    
    // Get the parameters (they should already be created)
    auto* tcp_param = getStringParam("tcp_messages");
    auto* http_param = getStringParam("http_messages");
    auto* ws_param = getStringParam("ws_messages");
    
    if (!tcp_param || !http_param || !ws_param) {
        ESP_LOGE(TAG, "Parameters not created before loading examples!");
        return;
    }
    
    // Append each message as a new row (grows parameters dynamically)
    size_t tcp_count = 0, http_count = 0, ws_count = 0;
    
    for (size_t i = 0; i < MessageExamples::ALL_EXAMPLES_COUNT; i++) {
        const char* example_json = MessageExamples::ALL_EXAMPLES[i];
        cJSON* root = cJSON_Parse(example_json);
        if (!root || !cJSON_IsArray(root) || cJSON_GetArraySize(root) == 0) {
            if (root) cJSON_Delete(root);
            continue;
        }
        
        cJSON* item = cJSON_GetArrayItem(root, 0);
        
        // Convert single item to JSON string
        char* item_json = cJSON_PrintUnformatted(item);
        
        // Determine message type and append to appropriate parameter
        cJSON* host = cJSON_GetObjectItem(item, "host");
        cJSON* url = cJSON_GetObjectItem(item, "url");
        cJSON* message = cJSON_GetObjectItem(item, "message");
        
        if (host) {
            tcp_param->appendValue(std::string(item_json));
            tcp_count++;
        } else if (message) {
            ws_param->appendValue(std::string(item_json));
            ws_count++;
        } else if (url) {
            http_param->appendValue(std::string(item_json));
            http_count++;
        }
        
        cJSON_free(item_json);
        cJSON_Delete(root);
    }
    
    ESP_LOGI(TAG, "Loaded %zu TCP, %zu HTTP, %zu WS message examples", tcp_count, http_count, ws_count);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] loadAllMessageExamples");
#endif
}

// JSON parsing implementations - parse on demand from StringParameter

bool NetworkActionsComponent::parseTcpMessageAt(size_t index, TcpMessage& out) const {
    auto* param = const_cast<NetworkActionsComponent*>(this)->getStringParam("tcp_messages");
    if (!param || index >= param->getRows()) {
        return false;
    }
    
    const std::string& json_str = param->getValue(index, 0);
    if (json_str.empty()) return false;
    
    cJSON* item = cJSON_Parse(json_str.c_str());
    if (!item) return false;
    
    cJSON* name = cJSON_GetObjectItem(item, "name");
    cJSON* host = cJSON_GetObjectItem(item, "host");
    cJSON* port = cJSON_GetObjectItem(item, "port");
    cJSON* data = cJSON_GetObjectItem(item, "data");
    cJSON* timeout = cJSON_GetObjectItem(item, "timeout_ms");
    
    bool success = false;
    if (name && host && port && data) {
        out = TcpMessage(
            cJSON_GetStringValue(name),
            cJSON_GetStringValue(host),
            static_cast<uint16_t>(port->valueint),
            cJSON_GetStringValue(data),
            timeout ? static_cast<uint32_t>(timeout->valueint) : 5000
        );
        success = true;
    }
    
    cJSON_Delete(item);
    return success;
}

bool NetworkActionsComponent::parseHttpMessageAt(size_t index, HttpMessage& out) const {
    auto* param = const_cast<NetworkActionsComponent*>(this)->getStringParam("http_messages");
    if (!param || index >= param->getRows()) {
        return false;
    }
    
    const std::string& json_str = param->getValue(index, 0);
    if (json_str.empty()) return false;
    
    cJSON* item = cJSON_Parse(json_str.c_str());
    if (!item) return false;
    
    cJSON* name = cJSON_GetObjectItem(item, "name");
    cJSON* url = cJSON_GetObjectItem(item, "url");
    cJSON* method = cJSON_GetObjectItem(item, "method");
    cJSON* body = cJSON_GetObjectItem(item, "body");
    cJSON* headers_array = cJSON_GetObjectItem(item, "headers");
    cJSON* timeout = cJSON_GetObjectItem(item, "timeout_ms");
    
    bool success = false;
    if (name && url && method) {
        std::vector<std::string> headers;
        if (headers_array && cJSON_IsArray(headers_array)) {
            int header_count = cJSON_GetArraySize(headers_array);
            for (int j = 0; j < header_count; j++) {
                cJSON* header = cJSON_GetArrayItem(headers_array, j);
                if (cJSON_IsString(header)) {
                    headers.push_back(cJSON_GetStringValue(header));
                }
            }
        }
        
        out = HttpMessage(
            cJSON_GetStringValue(name),
            cJSON_GetStringValue(url),
            cJSON_GetStringValue(method),
            body ? cJSON_GetStringValue(body) : "",
            headers,
            timeout ? static_cast<uint32_t>(timeout->valueint) : 10000
        );
        success = true;
    }
    
    cJSON_Delete(item);
    return success;
}

bool NetworkActionsComponent::parseWsMessageAt(size_t index, WsMessage& out) const {
    auto* param = const_cast<NetworkActionsComponent*>(this)->getStringParam("ws_messages");
    if (!param || index >= param->getRows()) {
        return false;
    }
    
    const std::string& json_str = param->getValue(index, 0);
    if (json_str.empty()) return false;
    
    cJSON* item = cJSON_Parse(json_str.c_str());
    if (!item) return false;
    
    cJSON* name = cJSON_GetObjectItem(item, "name");
    cJSON* url = cJSON_GetObjectItem(item, "url");
    cJSON* message = cJSON_GetObjectItem(item, "message");
    cJSON* subprotocol = cJSON_GetObjectItem(item, "subprotocol");
    cJSON* timeout = cJSON_GetObjectItem(item, "timeout_ms");
    
    bool success = false;
    if (name && url && message) {
        out = WsMessage(
            cJSON_GetStringValue(name),
            cJSON_GetStringValue(url),
            cJSON_GetStringValue(message),
            subprotocol ? cJSON_GetStringValue(subprotocol) : "",
            timeout ? static_cast<uint32_t>(timeout->valueint) : 10000
        );
        success = true;
    }
    
    cJSON_Delete(item);
    return success;
}

// Helper to get just the name field without full parsing
std::string NetworkActionsComponent::getTcpMessageName(size_t index) const {
    auto* param = const_cast<NetworkActionsComponent*>(this)->getStringParam("tcp_messages");
    if (!param || index >= param->getRows()) return "";
    
    const std::string& json_str = param->getValue(index, 0);
    cJSON* item = cJSON_Parse(json_str.c_str());
    if (!item) return "";
    
    cJSON* name = cJSON_GetObjectItem(item, "name");
    std::string result = name ? cJSON_GetStringValue(name) : "";
    cJSON_Delete(item);
    return result;
}

std::string NetworkActionsComponent::getHttpMessageName(size_t index) const {
    auto* param = const_cast<NetworkActionsComponent*>(this)->getStringParam("http_messages");
    if (!param || index >= param->getRows()) return "";
    
    const std::string& json_str = param->getValue(index, 0);
    cJSON* item = cJSON_Parse(json_str.c_str());
    if (!item) return "";
    
    cJSON* name = cJSON_GetObjectItem(item, "name");
    std::string result = name ? cJSON_GetStringValue(name) : "";
    cJSON_Delete(item);
    return result;
}

std::string NetworkActionsComponent::getWsMessageName(size_t index) const {
    auto* param = const_cast<NetworkActionsComponent*>(this)->getStringParam("ws_messages");
    if (!param || index >= param->getRows()) return "";
    
    const std::string& json_str = param->getValue(index, 0);
    cJSON* item = cJSON_Parse(json_str.c_str());
    if (!item) return "";
    
    cJSON* name = cJSON_GetObjectItem(item, "name");
    std::string result = name ? cJSON_GetStringValue(name) : "";
    cJSON_Delete(item);
    return result;
}

// Count getters - use StringParameter row count
size_t NetworkActionsComponent::getTcpMessageCount() const {
    auto* param = const_cast<NetworkActionsComponent*>(this)->getStringParam("tcp_messages");
    return param ? param->getRows() : 0;
}

size_t NetworkActionsComponent::getHttpMessageCount() const {
    auto* param = const_cast<NetworkActionsComponent*>(this)->getStringParam("http_messages");
    return param ? param->getRows() : 0;
}

size_t NetworkActionsComponent::getWsMessageCount() const {
    auto* param = const_cast<NetworkActionsComponent*>(this)->getStringParam("ws_messages");
    return param ? param->getRows() : 0;
}

// Send methods - by index

bool NetworkActionsComponent::sendTcp(size_t index) {
    if (index >= getTcpMessageCount()) {
        return false;
    }
    NetworkActionQueueItem item = {
        NetworkActionQueueItem::NetworkProtocol::TCP,
        index
    };
    return xQueueSend(network_actions_queue, &item, 0) == pdTRUE;
}

bool NetworkActionsComponent::sendHttp(size_t index) {
    if (index >= getHttpMessageCount()) {
        return false;
    }
    NetworkActionQueueItem item = {
        NetworkActionQueueItem::NetworkProtocol::HTTP,
        index
    };
    return xQueueSend(network_actions_queue, &item, 0) == pdTRUE;
}

bool NetworkActionsComponent::sendWs(size_t index) {
    if (index >= getWsMessageCount()) {
        return false;
    }
    NetworkActionQueueItem item = {
        NetworkActionQueueItem::NetworkProtocol::WebSocket,
        index
    };
    return xQueueSend(network_actions_queue, &item, 0) == pdTRUE;
}

// Index lookup helpers

size_t NetworkActionsComponent::getTcpMessageIdx(const std::string& name) const {
    size_t count = getTcpMessageCount();
    for (size_t i = 0; i < count; i++) {
        if (getTcpMessageName(i) == name) {
            return i;
        }
    }
    ESP_LOGE(TAG, "TCP message '%s' not found", name.c_str());
    return static_cast<size_t>(-1);
}

size_t NetworkActionsComponent::getHttpMessageIdx(const std::string& name) const {
    size_t count = getHttpMessageCount();
    for (size_t i = 0; i < count; i++) {
        if (getHttpMessageName(i) == name) {
            return i;
        }
    }
    ESP_LOGE(TAG, "HTTP message '%s' not found", name.c_str());
    return static_cast<size_t>(-1);
}

size_t NetworkActionsComponent::getWsMessageIdx(const std::string& name) const {
    size_t count = getWsMessageCount();
    for (size_t i = 0; i < count; i++) {
        if (getWsMessageName(i) == name) {
            return i;
        }
    }
    ESP_LOGE(TAG, "WS message '%s' not found", name.c_str());
    return static_cast<size_t>(-1);
}

// Get message by name - parse into output struct

bool NetworkActionsComponent::getTcpMessage(const std::string& name, TcpMessage& out) const {
    size_t idx = getTcpMessageIdx(name);
    if (idx == static_cast<size_t>(-1)) return false;
    return parseTcpMessageAt(idx, out);
}

bool NetworkActionsComponent::getHttpMessage(const std::string& name, HttpMessage& out) const {
    size_t idx = getHttpMessageIdx(name);
    if (idx == static_cast<size_t>(-1)) return false;
    return parseHttpMessageAt(idx, out);
}

bool NetworkActionsComponent::getWsMessage(const std::string& name, WsMessage& out) const {
    size_t idx = getWsMessageIdx(name);
    if (idx == static_cast<size_t>(-1)) return false;
    return parseWsMessageAt(idx, out);
}

// Register triggers for GUI

void NetworkActionsComponent::registerActions() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] registerActions");
#endif
    size_t total_triggers = 0;
    
    // Register TCP triggers - parse each message to get name/host/port for description
    size_t tcp_count = getTcpMessageCount();
    for (size_t i = 0; i < tcp_count; i++) {
        TcpMessage msg;
        if (parseTcpMessageAt(i, msg)) {
            std::string trigger_name = "Send TCP: " + msg.name;
            
            // Add trigger parameter (no separate description to save memory)
            addTriggerParam(
                trigger_name,
                1, 1,
                "",  // default empty value
                [i](Component* comp, size_t row, size_t col, const std::string& arg) {
                    auto* net_comp = static_cast<NetworkActionsComponent*>(comp);
                    net_comp->sendTcp(i);
                },
                false  // not read-only
            );
            total_triggers++;
        }
    }
    
    // Register HTTP triggers
    size_t http_count = getHttpMessageCount();
    for (size_t i = 0; i < http_count; i++) {
        HttpMessage msg;
        if (parseHttpMessageAt(i, msg)) {
            std::string trigger_name = "Send HTTP: " + msg.name;
            
            // Add trigger parameter (no separate description to save memory)
            addTriggerParam(
                trigger_name,
                1, 1,
                "",  // default empty value
                [i](Component* comp, size_t row, size_t col, const std::string& arg) {
                    auto* net_comp = static_cast<NetworkActionsComponent*>(comp);
                    net_comp->sendHttp(i);
                },
                false  // not read-only
            );
            total_triggers++;
        }
    }
    
    // Register WebSocket triggers
    size_t ws_count = getWsMessageCount();
    for (size_t i = 0; i < ws_count; i++) {
        WsMessage msg;
        if (parseWsMessageAt(i, msg)) {
            std::string trigger_name = "Send WS: " + msg.name;
            
            // Add trigger parameter (no separate description to save memory)
            addTriggerParam(
                trigger_name,
                1, 1,
                "",  // default empty value
                [i](Component* comp, size_t row, size_t col, const std::string& arg) {
                    auto* net_comp = static_cast<NetworkActionsComponent*>(comp);
                    net_comp->sendWs(i);
                },
                false  // not read-only
            );
            total_triggers++;
        }
    }
    
    ESP_LOGI(TAG, "Registered %zu triggers", total_triggers);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] registerActions");
#endif
}
