#pragma once

#include <string>
#include <cstdint>

// Compact struct for a WebSocket message configuration
struct WsMessage {
    std::string name;
    std::string url;
    std::string message;
    std::string subprotocol;  // Optional websocket subprotocol
    uint32_t timeout_ms;
    
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
    
private:
    bool initialized;
};
