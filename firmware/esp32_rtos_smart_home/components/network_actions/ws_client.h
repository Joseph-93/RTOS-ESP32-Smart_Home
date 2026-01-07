#pragma once

#include <string>
#include <cstdint>
#include <map>
#include "esp_websocket_client.h"

// Compact struct for a WebSocket message configuration
struct WsMessage {
    std::string name;
    std::string url;
    std::string message;
    std::string subprotocol;  // Optional websocket subprotocol
    uint32_t timeout_ms;
        // Default constructor
    WsMessage() : name(""), url(""), message(""), subprotocol(""), timeout_ms(10000) {}
        WsMessage(const std::string& name, const std::string& url, const std::string& message,
              const std::string& subprotocol = "", uint32_t timeout_ms = 10000)
        : name(name), url(url), message(message), subprotocol(subprotocol), timeout_ms(timeout_ms) {}
};

// WebSocket/WSS client using ESP-IDF esp_websocket_client
class WsClient {
public:
    WsClient();
    ~WsClient();
    
    bool initialize();
    bool send(const WsMessage& msg);
    void cleanup();
    
private:
    bool initialized;
    // WebSocket client pool: key is URL, value is client handle
    std::map<std::string, esp_websocket_client_handle_t> client_pool;
    
    esp_websocket_client_handle_t getOrCreateClient(const std::string& url, const std::string& subprotocol);
    void closeClient(const std::string& url);
};
