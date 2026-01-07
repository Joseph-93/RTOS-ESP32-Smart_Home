#include "network_actions.h"
#include "message_examples.h"
#include "esp_log.h"
#include "cJSON.h"
#include <algorithm>

static const char *TAG = "NetworkActions";

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

void NetworkActionsComponent::initialize() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] NetworkActionsComponent::initialize");
#endif
    ESP_LOGI(TAG, "Initializing NetworkActionsComponent...");
    
    // Add string parameters for message configurations
    addStringParam("tcp_messages", 1, 1);
    addStringParam("http_messages", 1, 1);
    addStringParam("ws_messages", 1, 1);
    
    // Load all message examples from MessageExamples namespace into parameters
    loadAllMessageExamples();
    
    // Parse parameters to populate message vectors
    parseTcpMessages();
    parseHttpMessages();
    parseWsMessages();
    
    // Initialize clients
    tcp_client.initialize();
    http_client.initialize();
    ws_client.initialize();
    
    // Register actions for each message type
    registerActions();
    
    initialized = true;
    ESP_LOGI(TAG, "NetworkActionsComponent initialized: %zu TCP, %zu HTTP, %zu WS messages",
             tcp_messages.size(), http_messages.size(), ws_messages.size());
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] NetworkActionsComponent::initialize");
#endif
}

// Helper function to load all message examples from MessageExamples namespace into parameters
void NetworkActionsComponent::loadAllMessageExamples() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] loadAllMessageExamples");
#endif
    
    // Build JSON arrays for each message type by categorizing examples
    cJSON* tcp_array = cJSON_CreateArray();
    cJSON* http_array = cJSON_CreateArray();
    cJSON* ws_array = cJSON_CreateArray();
    
    // Use the centralized array from message_examples.h
    for (size_t i = 0; i < MessageExamples::ALL_EXAMPLES_COUNT; i++) {
        const char* example_json = MessageExamples::ALL_EXAMPLES[i];
        cJSON* root = cJSON_Parse(example_json);
        if (!root) {
            ESP_LOGE(TAG, "Failed to parse message example JSON");
            continue;
        }
        
        // Check if it's an array with at least one item
        if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) == 0) {
            cJSON_Delete(root);
            continue;
        }
        
        cJSON* item = cJSON_GetArrayItem(root, 0);
        
        // Determine message type based on presence of fields
        cJSON* host = cJSON_GetObjectItem(item, "host");
        cJSON* url = cJSON_GetObjectItem(item, "url");
        cJSON* message = cJSON_GetObjectItem(item, "message");
        
        if (host) {
            // TCP message - add to tcp_array
            cJSON_AddItemToArray(tcp_array, cJSON_Duplicate(item, 1));
        } else if (message) {
            // WebSocket message - add to ws_array
            cJSON_AddItemToArray(ws_array, cJSON_Duplicate(item, 1));
        } else if (url) {
            // HTTP message - add to http_array
            cJSON_AddItemToArray(http_array, cJSON_Duplicate(item, 1));
        }
        
        cJSON_Delete(root);
    }
    
    // Convert arrays to JSON strings and store in parameters
    char* tcp_json = cJSON_PrintUnformatted(tcp_array);
    char* http_json = cJSON_PrintUnformatted(http_array);
    char* ws_json = cJSON_PrintUnformatted(ws_array);
    
    auto* tcp_param = getStringParam("tcp_messages");
    auto* http_param = getStringParam("http_messages");
    auto* ws_param = getStringParam("ws_messages");
    
    if (tcp_param) tcp_param->setValue(0, 0, tcp_json);
    if (http_param) http_param->setValue(0, 0, http_json);
    if (ws_param) ws_param->setValue(0, 0, ws_json);
    
    // Clean up
    cJSON_free(tcp_json);
    cJSON_free(http_json);
    cJSON_free(ws_json);
    cJSON_Delete(tcp_array);
    cJSON_Delete(http_array);
    cJSON_Delete(ws_array);
    
    ESP_LOGI(TAG, "Loaded message examples into parameters");
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] loadAllMessageExamples");
#endif
}

// JSON parsing implementations

void NetworkActionsComponent::parseTcpMessages() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] parseTcpMessages");
#endif
    auto* param = getStringParam("tcp_messages");
    if (!param) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] parseTcpMessages - no param");
#endif
        return;
    }
    
    const std::string& json_str = param->getValue(0, 0);
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse TCP messages JSON");
        return;
    }
    
    int array_size = cJSON_GetArraySize(root);
    for (int i = 0; i < array_size; i++) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* host = cJSON_GetObjectItem(item, "host");
        cJSON* port = cJSON_GetObjectItem(item, "port");
        cJSON* data = cJSON_GetObjectItem(item, "data");
        cJSON* timeout = cJSON_GetObjectItem(item, "timeout_ms");
        
        if (name && host && port && data) {
            tcp_messages.emplace_back(
                cJSON_GetStringValue(name),
                cJSON_GetStringValue(host),
                static_cast<uint16_t>(port->valueint),
                cJSON_GetStringValue(data),
                timeout ? static_cast<uint32_t>(timeout->valueint) : 5000
            );
        }
    }
    
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Parsed %zu TCP messages", tcp_messages.size());
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] parseTcpMessages");
#endif
}

void NetworkActionsComponent::parseHttpMessages() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] parseHttpMessages");
#endif
    auto* param = getStringParam("http_messages");
    if (!param) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] parseHttpMessages - no param");
#endif
        return;
    }
    
    const std::string& json_str = param->getValue(0, 0);
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse HTTP messages JSON");
        return;
    }
    
    int array_size = cJSON_GetArraySize(root);
    for (int i = 0; i < array_size; i++) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        
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
            
            http_messages.emplace_back(
                cJSON_GetStringValue(name),
                cJSON_GetStringValue(url),
                cJSON_GetStringValue(method),
                body ? cJSON_GetStringValue(body) : "",
                headers,
                timeout ? static_cast<uint32_t>(timeout->valueint) : 10000
            );
        }
    }
    
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Parsed %zu HTTP messages", http_messages.size());
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] parseHttpMessages");
#endif
}

void NetworkActionsComponent::parseWsMessages() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] parseWsMessages");
#endif
    auto* param = getStringParam("ws_messages");
    if (!param) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] parseWsMessages - no param");
#endif
        return;
    }
    
    const std::string& json_str = param->getValue(0, 0);
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse WebSocket messages JSON");
        return;
    }
    
    int array_size = cJSON_GetArraySize(root);
    for (int i = 0; i < array_size; i++) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* url = cJSON_GetObjectItem(item, "url");
        cJSON* message = cJSON_GetObjectItem(item, "message");
        cJSON* subprotocol = cJSON_GetObjectItem(item, "subprotocol");
        cJSON* timeout = cJSON_GetObjectItem(item, "timeout_ms");
        
        if (name && url && message) {
            ws_messages.emplace_back(
                cJSON_GetStringValue(name),
                cJSON_GetStringValue(url),
                cJSON_GetStringValue(message),
                subprotocol ? cJSON_GetStringValue(subprotocol) : "",
                timeout ? static_cast<uint32_t>(timeout->valueint) : 10000
            );
        }
    }
    
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Parsed %zu WebSocket messages", ws_messages.size());
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] parseWsMessages");
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
    bool result = tcp_client.send(tcp_messages[index]);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] sendTcp - result: %d", result);
#endif
    return result;
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
    bool result = http_client.send(http_messages[index]);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] sendHttp - result: %d", result);
#endif
    return result;
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
    bool result = ws_client.send(ws_messages[index]);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] sendWs - result: %d", result);
#endif
    return result;
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
