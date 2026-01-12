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

void NetworkActionsComponent::initialize() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] NetworkActionsComponent::initialize");
#endif
    ESP_LOGI(TAG, "Initializing NetworkActionsComponent...");
    
    // Add WiFi connection status parameter
    addBoolParam("wifi_connected", 1, 1, false);
    ESP_LOGI(TAG, "Added wifi_connected parameter");
    
    // Create EMPTY parameters (0 rows) with 1 column - will grow as we append messages
    // Need at least 1 column for appendValue to work (it divides by cols)
    addStringParam("tcp_messages", 0, 1);
    addStringParam("http_messages", 0, 1);
    addStringParam("ws_messages", 0, 1);
    
    // Set up parse callbacks BEFORE loading (these will be called when parameter values change)
    auto* tcp_param = getStringParam("tcp_messages");
    auto* http_param = getStringParam("http_messages");
    auto* ws_param = getStringParam("ws_messages");
    
    if (tcp_param) {
        tcp_param->setOnChange([this](size_t row, size_t col, const std::string& val) { this->parseTcpMessage(row, col, val); });
    }
    if (http_param) {
        http_param->setOnChange([this](size_t row, size_t col, const std::string& val) { this->parseHttpMessage(row, col, val); });
    }
    if (ws_param) {
        ws_param->setOnChange([this](size_t row, size_t col, const std::string& val) { this->parseWsMessage(row, col, val); });
    }
    
    // NOW load all message examples into the parameters (will trigger callbacks)
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
        8192, // Stack depth
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
             tcp_messages.size(), http_messages.size(), ws_messages.size());
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
                case NetworkActionQueueItem::NetworkProtocol::TCP:
                    if (item.message_index < tcp_messages.size()) {
                        result = tcp_client.send(tcp_messages[item.message_index]);
                        protocol_name = "TCP";
                        message_name = tcp_messages[item.message_index].name;
                    }
                    break;
                case NetworkActionQueueItem::NetworkProtocol::HTTP:
                    if (item.message_index < http_messages.size()) {
                        result = http_client.send(http_messages[item.message_index]);
                        protocol_name = "HTTP";
                        message_name = http_messages[item.message_index].name;
                    }
                    break;
                case NetworkActionQueueItem::NetworkProtocol::WebSocket:
                    if (item.message_index < ws_messages.size()) {
                        result = ws_client.send(ws_messages[item.message_index]);
                        protocol_name = "WebSocket";
                        message_name = ws_messages[item.message_index].name;
                    }
                    break;
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

// JSON parsing implementations

void NetworkActionsComponent::parseTcpMessage(size_t row, size_t col, const std::string& val) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] parseTcpMessage - row: %zu, col: %zu", row, col);
#endif
    auto* param = getStringParam("tcp_messages");
    if (!param) {
        ESP_LOGE(TAG, "tcp_messages parameter not found");
        return;
    }
    
    const std::string& json_str = param->getValue(row, col);
    if (json_str.empty()) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] parseTcpMessage - empty string");
#endif
        return;
    }
    
    cJSON* item = cJSON_Parse(json_str.c_str());
    if (!item) {
        ESP_LOGE(TAG, "Failed to parse TCP message at [%zu,%zu]", row, col);
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] parseTcpMessage - parse failed");
#endif
        return;
    }
    
    cJSON* name = cJSON_GetObjectItem(item, "name");
    cJSON* host = cJSON_GetObjectItem(item, "host");
    cJSON* port = cJSON_GetObjectItem(item, "port");
    cJSON* data = cJSON_GetObjectItem(item, "data");
    cJSON* timeout = cJSON_GetObjectItem(item, "timeout_ms");
    
    if (name && host && port && data) {
        // Grow tcp_messages if needed
        if (row >= tcp_messages.size()) {
            tcp_messages.resize(row + 1);
        }
        
        tcp_messages[row] = TcpMessage(
            cJSON_GetStringValue(name),
            cJSON_GetStringValue(host),
            static_cast<uint16_t>(port->valueint),
            cJSON_GetStringValue(data),
            timeout ? static_cast<uint32_t>(timeout->valueint) : 5000
        );
        ESP_LOGI(TAG, "Parsed TCP message at index %zu: %s", row, cJSON_GetStringValue(name));
    }
    
    cJSON_Delete(item);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] parseTcpMessage");
#endif
}

void NetworkActionsComponent::parseHttpMessage(size_t row, size_t col, const std::string& val) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] parseHttpMessage - row: %zu, col: %zu", row, col);
#endif
    auto* param = getStringParam("http_messages");
    if (!param) {
        ESP_LOGE(TAG, "http_messages parameter not found");
        return;
    }
    
    const std::string& json_str = param->getValue(row, col);
    if (json_str.empty()) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] parseHttpMessage - empty string");
#endif
        return;
    }
    
    cJSON* item = cJSON_Parse(json_str.c_str());
    if (!item) {
        ESP_LOGE(TAG, "Failed to parse HTTP message at [%zu,%zu]", row, col);
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] parseHttpMessage - parse failed");
#endif
        return;
    }
    
    cJSON* name = cJSON_GetObjectItem(item, "name");
    cJSON* url = cJSON_GetObjectItem(item, "url");
    cJSON* method = cJSON_GetObjectItem(item, "method");
    cJSON* body = cJSON_GetObjectItem(item, "body");
    cJSON* headers_array = cJSON_GetObjectItem(item, "headers");
    cJSON* timeout = cJSON_GetObjectItem(item, "timeout_ms");
    
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
        
        // Grow http_messages if needed
        if (row >= http_messages.size()) {
            http_messages.resize(row + 1);
        }
        
        http_messages[row] = HttpMessage(
            cJSON_GetStringValue(name),
            cJSON_GetStringValue(url),
            cJSON_GetStringValue(method),
            body ? cJSON_GetStringValue(body) : "",
            headers,
            timeout ? static_cast<uint32_t>(timeout->valueint) : 10000
        );
        ESP_LOGI(TAG, "Parsed HTTP message at index %zu: %s", row, cJSON_GetStringValue(name));
    }
    
    cJSON_Delete(item);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] parseHttpMessage");
#endif
}

void NetworkActionsComponent::parseWsMessage(size_t row, size_t col, const std::string& val) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] parseWsMessage - row: %zu, col: %zu", row, col);
#endif
    auto* param = getStringParam("ws_messages");
    if (!param) {
        ESP_LOGE(TAG, "ws_messages parameter not found");
        return;
    }
    
    const std::string& json_str = param->getValue(row, col);
    if (json_str.empty()) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] parseWsMessage - empty string");
#endif
        return;
    }
    
    cJSON* item = cJSON_Parse(json_str.c_str());
    if (!item) {
        ESP_LOGE(TAG, "Failed to parse WebSocket message at [%zu,%zu]", row, col);
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] parseWsMessage - parse failed");
#endif
        return;
    }
    
    cJSON* name = cJSON_GetObjectItem(item, "name");
    cJSON* url = cJSON_GetObjectItem(item, "url");
    cJSON* message = cJSON_GetObjectItem(item, "message");
    cJSON* subprotocol = cJSON_GetObjectItem(item, "subprotocol");
    cJSON* timeout = cJSON_GetObjectItem(item, "timeout_ms");
    
    if (name && url && message) {
        // Grow ws_messages if needed
        if (row >= ws_messages.size()) {
            ws_messages.resize(row + 1);
        }
        
        ws_messages[row] = WsMessage(
            cJSON_GetStringValue(name),
            cJSON_GetStringValue(url),
            cJSON_GetStringValue(message),
            subprotocol ? cJSON_GetStringValue(subprotocol) : "",
            timeout ? static_cast<uint32_t>(timeout->valueint) : 10000
        );
        ESP_LOGI(TAG, "Parsed WebSocket message at index %zu: %s", row, cJSON_GetStringValue(name));
    }
    
    cJSON_Delete(item);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] parseWsMessage");
#endif
}

// Send methods - by index

bool NetworkActionsComponent::sendTcp(size_t index) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] sendTcp - index: %zu", index);
#endif
    if (index >= tcp_messages.size()) {
        ESP_LOGE(TAG, "TCP message index %zu out of range", index);
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] sendTcp - out of range");
#endif
        return false;
    }
    NetworkActionQueueItem item = {
        NetworkActionQueueItem::NetworkProtocol::TCP,
        index
    };
    BaseType_t result = xQueueSend(network_actions_queue, &item, 0); // Non-blocking
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] sendTcp - result: %d", result);
#endif
    return result == pdTRUE;
}

bool NetworkActionsComponent::sendHttp(size_t index) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] sendHttp - index: %zu", index);
#endif
    if (index >= http_messages.size()) {
        ESP_LOGE(TAG, "HTTP message index %zu out of range", index);
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] sendHttp - out of range");
#endif
        return false;
    }
    NetworkActionQueueItem item = {
        NetworkActionQueueItem::NetworkProtocol::HTTP,
        index
    };
    BaseType_t result = xQueueSend(network_actions_queue, &item, 0); // Non-blocking
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] sendHttp - result: %d", result);
#endif
    return result == pdTRUE;
}

bool NetworkActionsComponent::sendWs(size_t index) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] sendWs - index: %zu", index);
#endif
    if (index >= ws_messages.size()) {
        ESP_LOGE(TAG, "WS message index %zu out of range", index);
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] sendWs - out of range");
#endif
        return false;
    }
    NetworkActionQueueItem item = {
        NetworkActionQueueItem::NetworkProtocol::WebSocket,
        index
    };
    BaseType_t result = xQueueSend(network_actions_queue, &item, 0); // Non-blocking
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] sendWs - result: %d", result);
#endif
    return result == pdTRUE;
}

// Index lookup helpers

size_t NetworkActionsComponent::getTcpMessageIdx(const std::string& name) const {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] getTcpMessageIdx - name: %s", name.c_str());
#endif
    auto it = std::find_if(tcp_messages.begin(), tcp_messages.end(),
                          [&name](const TcpMessage& msg) { return msg.name == name; });
    
    if (it == tcp_messages.end()) {
        ESP_LOGE(TAG, "TCP message '%s' not found", name.c_str());
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] getTcpMessageIdx - not found");
#endif
        return static_cast<size_t>(-1);
    }
    
    size_t idx = std::distance(tcp_messages.begin(), it);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] getTcpMessageIdx - idx: %zu", idx);
#endif
    return idx;
}

size_t NetworkActionsComponent::getHttpMessageIdx(const std::string& name) const {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] getHttpMessageIdx - name: %s", name.c_str());
#endif
    auto it = std::find_if(http_messages.begin(), http_messages.end(),
                          [&name](const HttpMessage& msg) { return msg.name == name; });
    
    if (it == http_messages.end()) {
        ESP_LOGE(TAG, "HTTP message '%s' not found", name.c_str());
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] getHttpMessageIdx - not found");
#endif
        return static_cast<size_t>(-1);
    }
    
    size_t idx = std::distance(http_messages.begin(), it);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] getHttpMessageIdx - idx: %zu", idx);
#endif
    return idx;
}

size_t NetworkActionsComponent::getWsMessageIdx(const std::string& name) const {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] getWsMessageIdx - name: %s", name.c_str());
#endif
    auto it = std::find_if(ws_messages.begin(), ws_messages.end(),
                          [&name](const WsMessage& msg) { return msg.name == name; });
    
    if (it == ws_messages.end()) {
        ESP_LOGE(TAG, "WS message '%s' not found", name.c_str());
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] getWsMessageIdx - not found");
#endif
        return static_cast<size_t>(-1);
    }
    
    size_t idx = std::distance(ws_messages.begin(), it);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] getWsMessageIdx - idx: %zu", idx);
#endif
    return idx;
}

// Pointer getters by name

const TcpMessage* NetworkActionsComponent::getTcpMessage(const std::string& name) const {
    auto it = std::find_if(tcp_messages.begin(), tcp_messages.end(),
                          [&name](const TcpMessage& msg) { return msg.name == name; });
    
    return (it != tcp_messages.end()) ? &(*it) : nullptr;
}

const HttpMessage* NetworkActionsComponent::getHttpMessage(const std::string& name) const {
    auto it = std::find_if(http_messages.begin(), http_messages.end(),
                          [&name](const HttpMessage& msg) { return msg.name == name; });
    
    return (it != http_messages.end()) ? &(*it) : nullptr;
}

const WsMessage* NetworkActionsComponent::getWsMessage(const std::string& name) const {
    auto it = std::find_if(ws_messages.begin(), ws_messages.end(),
                          [&name](const WsMessage& msg) { return msg.name == name; });
    
    return (it != ws_messages.end()) ? &(*it) : nullptr;
}

// Register actions for GUI

void NetworkActionsComponent::registerActions() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] registerActions");
#endif
    // Register TCP actions
    for (size_t i = 0; i < tcp_messages.size(); i++) {
        const std::string& msg_name = tcp_messages[i].name;
        addAction(
            "Send TCP: " + msg_name,
            "Send TCP message to " + tcp_messages[i].host + ":" + std::to_string(tcp_messages[i].port),
            [i](Component* comp) -> bool {
                auto* net_comp = static_cast<NetworkActionsComponent*>(comp);
                return net_comp->sendTcp(i);
            }
        );
    }
    
    // Register HTTP actions
    for (size_t i = 0; i < http_messages.size(); i++) {
        const std::string& msg_name = http_messages[i].name;
        addAction(
            "Send HTTP: " + msg_name,
            "Send HTTP " + http_messages[i].method + " to " + http_messages[i].url,
            [i](Component* comp) -> bool {
                auto* net_comp = static_cast<NetworkActionsComponent*>(comp);
                return net_comp->sendHttp(i);
            }
        );
    }
    
    // Register WebSocket actions
    for (size_t i = 0; i < ws_messages.size(); i++) {
        const std::string& msg_name = ws_messages[i].name;
        addAction(
            "Send WS: " + msg_name,
            "Send WebSocket message to " + ws_messages[i].url,
            [i](Component* comp) -> bool {
                auto* net_comp = static_cast<NetworkActionsComponent*>(comp);
                return net_comp->sendWs(i);
            }
        );
    }
    
    ESP_LOGI(TAG, "Registered %zu actions", actions.size());
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] registerActions");
#endif
}
