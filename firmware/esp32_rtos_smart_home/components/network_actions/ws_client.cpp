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

WsClient::~WsClient() {}

bool WsClient::initialize() {
    ESP_LOGI(TAG, "Initializing WebSocket client");
    initialized = true;
    return true;
}

bool WsClient::send(const WsMessage& msg) {
    if (!initialized) {
        ESP_LOGE(TAG, "WebSocket client not initialized");
        return false;
    }
    
    ESP_LOGI(TAG, "Sending WS message '%s' to %s", 
             msg.name.c_str(), msg.url.c_str());
    
    // Configure WebSocket client
    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = msg.url.c_str();
    ws_cfg.disable_auto_reconnect = true;
    
    if (!msg.subprotocol.empty()) {
        ws_cfg.subprotocol = msg.subprotocol.c_str();
    }
    
    // Create client
    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return false;
    }
    
    ESP_LOGI(TAG, "WebSocket client created");
    bool success = false;
    int sent = 0;  // Declare before any goto
    esp_err_t err = ESP_OK;
    
    // Register event handler
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    
    // Start connection
    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    
    // Wait for connection
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Send message
    sent = esp_websocket_client_send_text(client, msg.message.c_str(), msg.message.length(), pdMS_TO_TICKS(msg.timeout_ms));
    
    if (sent < 0) {
        ESP_LOGE(TAG, "WebSocket send failed");
        esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
        goto cleanup;
    }
    
    ESP_LOGI(TAG, "Sent %d bytes via WebSocket", sent);
    
    // Wait for response (10 seconds)
    ESP_LOGI(TAG, "Waiting 10 seconds for WebSocket response...");
    vTaskDelay(pdMS_TO_TICKS(10000));
    
    // Close connection
    esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    success = true;
    
cleanup:
    // MANDATORY CLEANUP - NO EXCEPTIONS
    ESP_LOGI(TAG, "DESTROYING WebSocket client");
    err = esp_websocket_client_destroy(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "!!!! CRITICAL: WEBSOCKET CLIENT DESTROY FAILED !!!!");
        ESP_LOGE(TAG, "!!!! FILE DESCRIPTOR LEAK GUARANTEED - ERROR: %s !!!!", esp_err_to_name(err));
        ESP_LOGE(TAG, "!!!! THIS IS THE BUG THAT EXHAUSTED YOUR FDs !!!!");
        assert(false && "WEBSOCKET CLIENT DESTROY FAILED - FD LEAK");
    }
    ESP_LOGI(TAG, "WebSocket client destroyed successfully");
    
    return success;
}
