#include "network_actions.h"
#include "esp_log.h"
#include "cJSON.h"
#include <algorithm>

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
    
    // Create StringParameters for JSON message configurations
    // Each parameter holds a JSON array of message definitions
    addStringParam("tcp_messages", 1, 1);   // JSON array of TCP messages
    addStringParam("http_messages", 1, 1);  // JSON array of HTTP messages
    addStringParam("ws_messages", 1, 1);    // JSON array of WebSocket messages
    
    // Message configurations will be loaded from NVS or configured via parameters
    // See message_examples.h for working message templates
    
    // Temporary: Empty message arrays for now
    const char* tcp_json = "[]";
    const char* http_json = "[]";
    const char* ws_json = "[]";
    
    getStringParam("tcp_messages")->setValue(0, 0, tcp_json);
    getStringParam("http_messages")->setValue(0, 0, http_json);
    getStringParam("ws_messages")->setValue(0, 0, ws_json);
    
    // Parse JSON into compact structs
    parseTcpMessages();
    parseHttpMessages();
    parseWsMessages();
    
    // Initialize clients
    tcp_client.initialize();
    http_client.initialize();
    ws_client.initialize();
    
    initialized = true;
    ESP_LOGI(TAG, "NetworkActionsComponent initialized: %zu TCP, %zu HTTP, %zu WS messages",
             tcp_messages.size(), http_messages.size(), ws_messages.size());
}

// JSON parsing implementations

void NetworkActionsComponent::parseTcpMessages() {
    auto* param = getStringParam("tcp_messages");
    if (!param) return;
    
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
}

void NetworkActionsComponent::parseHttpMessages() {
    auto* param = getStringParam("http_messages");
    if (!param) return;
    
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
}

void NetworkActionsComponent::parseWsMessages() {
    auto* param = getStringParam("ws_messages");
    if (!param) return;
    
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
}

// Send methods - by index

bool NetworkActionsComponent::sendTcp(size_t index) {
    if (index >= tcp_messages.size()) {
        ESP_LOGE(TAG, "TCP message index %zu out of range", index);
        return false;
    }
    return tcp_client.send(tcp_messages[index]);
}

bool NetworkActionsComponent::sendHttp(size_t index) {
    if (index >= http_messages.size()) {
        ESP_LOGE(TAG, "HTTP message index %zu out of range", index);
        return false;
    }
    return http_client.send(http_messages[index]);
}

bool NetworkActionsComponent::sendWs(size_t index) {
    if (index >= ws_messages.size()) {
        ESP_LOGE(TAG, "WS message index %zu out of range", index);
        return false;
    }
    return ws_client.send(ws_messages[index]);
}

// Index lookup helpers

size_t NetworkActionsComponent::getTcpMessageIdx(const std::string& name) const {
    auto it = std::find_if(tcp_messages.begin(), tcp_messages.end(),
                          [&name](const TcpMessage& msg) { return msg.name == name; });
    
    if (it == tcp_messages.end()) {
        ESP_LOGE(TAG, "TCP message '%s' not found", name.c_str());
        return static_cast<size_t>(-1);
    }
    
    return std::distance(tcp_messages.begin(), it);
}

size_t NetworkActionsComponent::getHttpMessageIdx(const std::string& name) const {
    auto it = std::find_if(http_messages.begin(), http_messages.end(),
                          [&name](const HttpMessage& msg) { return msg.name == name; });
    
    if (it == http_messages.end()) {
        ESP_LOGE(TAG, "HTTP message '%s' not found", name.c_str());
        return static_cast<size_t>(-1);
    }
    
    return std::distance(http_messages.begin(), it);
}

size_t NetworkActionsComponent::getWsMessageIdx(const std::string& name) const {
    auto it = std::find_if(ws_messages.begin(), ws_messages.end(),
                          [&name](const WsMessage& msg) { return msg.name == name; });
    
    if (it == ws_messages.end()) {
        ESP_LOGE(TAG, "WS message '%s' not found", name.c_str());
        return static_cast<size_t>(-1);
    }
    
    return std::distance(ws_messages.begin(), it);
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
