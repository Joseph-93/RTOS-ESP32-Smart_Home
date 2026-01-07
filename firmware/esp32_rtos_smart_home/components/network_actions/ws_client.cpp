#include "ws_client.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WsClient";

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket DISCONNECTED");
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(TAG, "WebSocket DATA received - op_code: %d, data_len: %d, payload_len: %d", 
                 data->op_code, data->data_len, data->payload_len);
        if (data->op_code == 0x01) {  // Text frame
            ESP_LOGI(TAG, "TEXT frame (%d bytes): %.*s", 
                     data->data_len, data->data_len, (char *)data->data_ptr);
        } else if (data->op_code == 0x02) {  // Binary frame
            ESP_LOG_BUFFER_HEX(TAG, data->data_ptr, data->data_len);
        } else if (data->op_code == 0x08) {  // Close frame
            ESP_LOGI(TAG, "Close frame received");
        } else {
            ESP_LOGI(TAG, "Other frame type: 0x%02x", data->op_code);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket ERROR");
        break;
    default:
        break;
    }
}

WsClient::WsClient() : initialized(false) {}

WsClient::~WsClient() {
    cleanup();
}

bool WsClient::initialize() {
    ESP_LOGI(TAG, "Initializing WebSocket client");
    initialized = true;
    return true;
}

void WsClient::cleanup() {
    ESP_LOGI(TAG, "Cleaning up WebSocket client - closing all connections");
    for (auto& pair : client_pool) {
        if (pair.second) {
            esp_websocket_client_close(pair.second, pdMS_TO_TICKS(1000));
            esp_websocket_client_destroy(pair.second);
            ESP_LOGI(TAG, "Closed WebSocket client for %s", pair.first.c_str());
        }
    }
    client_pool.clear();
}

esp_websocket_client_handle_t WsClient::getOrCreateClient(const std::string& url, const std::string& subprotocol) {
    // Check if we already have a connection
    auto it = client_pool.find(url);
    if (it != client_pool.end() && it->second != nullptr) {
        ESP_LOGI(TAG, "Reusing existing WebSocket client for %s", url.c_str());
        return it->second;
    }
    
    ESP_LOGI(TAG, "Creating new WebSocket client for %s", url.c_str());
    
    // Configure WebSocket client
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = url.c_str();
    ws_cfg.disable_auto_reconnect = false;  // Enable auto-reconnect for persistent connections
    
    if (!subprotocol.empty()) {
        ws_cfg.subprotocol = subprotocol.c_str();
    }
    
    // Create client
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return nullptr;
    }
    
    // Register event handler
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    
    // Start connection
    esp_err_t err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(client);
        return nullptr;
    }
    
    // Wait for connection
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "WebSocket client connected to %s", url.c_str());
    client_pool[url] = client;
    return client;
}

void WsClient::closeClient(const std::string& url) {
    auto it = client_pool.find(url);
    if (it != client_pool.end() && it->second != nullptr) {
        esp_websocket_client_close(it->second, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(it->second);
        ESP_LOGI(TAG, "Closed WebSocket client for %s", url.c_str());
        client_pool.erase(it);
    }
}

bool WsClient::send(const WsMessage& msg) {
    if (!initialized) {
        ESP_LOGE(TAG, "WebSocket client not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Sending WS message '%s' to %s", 
             msg.name.c_str(), msg.url.c_str());
    
    esp_websocket_client_handle_t client = getOrCreateClient(msg.url, msg.subprotocol);
    if (!client) {
        ESP_LOGE(TAG, "Failed to get/create WebSocket client");
        return false;
    }
    
    // Send message
    int sent = esp_websocket_client_send_text(client, msg.message.c_str(), msg.message.length(), pdMS_TO_TICKS(msg.timeout_ms));
    
    if (sent < 0) {
        ESP_LOGE(TAG, "WebSocket send failed - closing connection");
        closeClient(msg.url);
        return false;
    }
    
    ESP_LOGI(TAG, "Sent %d bytes via WebSocket", sent);
    
    // Note: Responses are handled asynchronously via the event handler
    
    return true;
}
