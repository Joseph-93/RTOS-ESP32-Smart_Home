#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "esp_http_client.h"

// Compact struct for an HTTP/HTTPS message configuration
struct HttpMessage {
    std::string name;
    std::string url;
    std::string method;  // GET, POST, PUT, DELETE, etc.
    std::string body;
    std::vector<std::string> headers;  // Format: "Key: Value"
    uint32_t timeout_ms;
    
    HttpMessage(const std::string& name, const std::string& url, const std::string& method,
                const std::string& body = "", const std::vector<std::string>& headers = {},
                uint32_t timeout_ms = 10000)
        : name(name), url(url), method(method), body(body), headers(headers), timeout_ms(timeout_ms) {}
};

// HTTP/HTTPS client using ESP-IDF esp_http_client
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    
    bool initialize();
    bool send(const HttpMessage& msg);
    void cleanup();
    
private:
    bool initialized;
    esp_http_client_handle_t client;
};
