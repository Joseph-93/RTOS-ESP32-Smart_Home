#include "http_client.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include <string.h>

static const char *TAG = "HttpClient";

HttpClient::HttpClient() : initialized(false) {}

HttpClient::~HttpClient() {}

bool HttpClient::initialize() {
    ESP_LOGI(TAG, "Initializing HTTP/HTTPS client");
    initialized = true;
    return true;
}

bool HttpClient::send(const HttpMessage& msg) {
    if (!initialized) {
        ESP_LOGE(TAG, "HTTP client not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Sending HTTP %s to %s (message: %s)", 
             msg.method.c_str(), msg.url.c_str(), msg.name.c_str());
    
    // Configure HTTP client
    esp_http_client_config_t config = {};
    config.url = msg.url.c_str();
    config.timeout_ms = msg.timeout_ms;
    config.method = HTTP_METHOD_GET;  // Default, will override
    
    // Create client
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }
    
    ESP_LOGI(TAG, "HTTP client created");
    bool success = false;
    
    // Set method
    if (msg.method == "GET") {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    } else if (msg.method == "POST") {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else if (msg.method == "PUT") {
        esp_http_client_set_method(client, HTTP_METHOD_PUT);
    } else if (msg.method == "DELETE") {
        esp_http_client_set_method(client, HTTP_METHOD_DELETE);
    } else if (msg.method == "PATCH") {
        esp_http_client_set_method(client, HTTP_METHOD_PATCH);
    } else if (msg.method == "HEAD") {
        esp_http_client_set_method(client, HTTP_METHOD_HEAD);
    }
    
    // Set custom headers
    for (const auto& header : msg.headers) {
        size_t colon_pos = header.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = header.substr(0, colon_pos);
            std::string value = header.substr(colon_pos + 1);
            // Trim leading space from value
            if (!value.empty() && value[0] == ' ') {
                value = value.substr(1);
            }
            esp_http_client_set_header(client, key.c_str(), value.c_str());
        }
    }
    
    // Set body if present
    if (!msg.body.empty()) {
        esp_http_client_set_post_field(client, msg.body.c_str(), msg.body.length());
    }
    
    // Perform request
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "HTTP %s Status = %d, content_length = %d",
                 msg.method.c_str(), status, content_length);
        
        // Read and log response body
        char buffer[512];
        int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
        if (read_len > 0) {
            buffer[read_len] = '\0';
            ESP_LOGI(TAG, "HTTP Response Body: %s", buffer);
        } else if (read_len == 0) {
            ESP_LOGI(TAG, "HTTP Response: Empty body");
        }
        success = true;
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }
    
    // MANDATORY CLEANUP
    ESP_LOGI(TAG, "CLEANING UP HTTP client");
    err = esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "!!!! CRITICAL: HTTP CLIENT CLEANUP FAILED !!!!");
        ESP_LOGE(TAG, "!!!! POSSIBLE FD LEAK - ERROR: %s !!!!", esp_err_to_name(err));
        assert(false && "HTTP CLIENT CLEANUP FAILED - POTENTIAL FD LEAK");
    }
    ESP_LOGI(TAG, "HTTP client cleaned up successfully");
    
    return success;
}
