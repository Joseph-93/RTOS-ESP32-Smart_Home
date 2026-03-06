#include "web_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/task.h"

// Memory diagnostics helper
static void print_memory_diagnostics(const char* tag) {
    // Heap summary
    ESP_LOGI(tag, "=== MEMORY DIAGNOSTICS ===");
    ESP_LOGI(tag, "Free heap: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(tag, "Minimum free heap (low watermark): %lu bytes", esp_get_minimum_free_heap_size());
    ESP_LOGI(tag, "Largest free block: %lu bytes", heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    
    // Heap by capability
    ESP_LOGI(tag, "Free DRAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(tag, "Free IRAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_32BIT));
    ESP_LOGI(tag, "=========================");
}

WebServerComponent::WebServerComponent() : Component("WebServer") {
}

WebServerComponent::~WebServerComponent() {
    if (http_server) {
        httpd_stop(http_server);
    }
}

void WebServerComponent::setUpDependencies(ComponentGraph* graph) {
    component_graph = graph;
}

void WebServerComponent::onInitialize() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 10;
    config.stack_size = 8192;  // Needs headroom for WebSocket handlers (base64 decode, JSON, etc.)
    config.max_req_hdr_len = 1024;  // Reduced from 2048 - WebSocket doesn't need large headers
    config.lru_purge_enable = true;
    config.max_open_sockets = 2;  // Reduced from 3 - minimal concurrent connections
    config.send_wait_timeout = 3;  // Shorter timeout
    config.recv_wait_timeout = 3;
    config.task_priority = 10;  // Higher priority to avoid starvation by WiFi tasks
    
    if (httpd_start(&http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server (heap: %lu)", esp_get_free_heap_size());
        return;
    }
    
    // Register ONLY WebSocket endpoint - all communication through WebSocket
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = this,
        .is_websocket = true,
        .handle_ws_control_frames = true,  // Enable to detect close frames
        .supported_subprotocol = NULL
    };
    httpd_register_uri_handler(http_server, &ws_uri);
    
    ESP_LOGI(TAG, "WebSocket server started on port 80");
    
    // Create mutex for subscriptions map protection
    subscriptions_mutex = xSemaphoreCreateMutex();
    if (!subscriptions_mutex) {
        ESP_LOGE(TAG, "Failed to create subscriptions mutex - FATAL");
        abort();
    }
    
    // Create mutex for WebSocket send operations (prevents concurrent frame sends)
    ws_send_mutex = xSemaphoreCreateMutex();
    if (!ws_send_mutex) {
        ESP_LOGE(TAG, "Failed to create ws_send_mutex - FATAL");
        abort();
    }
    
    // Create broadcast queue (16 items to handle burst updates during preset operations)
    broadcast_queue = xQueueCreate(16, sizeof(BroadcastQueueItem));
    if (!broadcast_queue) {
        ESP_LOGE(TAG, "Failed to create broadcast queue - FATAL (heap: %lu)", esp_get_free_heap_size());
        abort();
    }
    
    // Create high-priority broadcast task
    BaseType_t result = xTaskCreate(
        broadcastTaskWrapper,
        "ws_broadcast",
        3072,  // Stack size - needs room for cJSON operations
        this,
        15,    // Very high priority (above HTTP 10, near WiFi 18-23 but below)
        &broadcast_task_handle
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create broadcast task - FATAL (heap: %lu)", esp_get_free_heap_size());
        abort();
    }
    
    // NOTE: setupParameterBroadcasting() is now called in postInitialize()
    // to ensure all components have set their onChange callbacks first
}

void WebServerComponent::postInitialize() {
    setupParameterBroadcasting();
}

void WebServerComponent::setupParameterBroadcasting() {
    if (!component_graph) return;
    
    // Get all components
    const auto& comp_names = component_graph->getComponentNames();
    
    for (const auto& comp_name : comp_names) {
        Component* comp = component_graph->getComponent(comp_name);
        if (!comp) continue;
        
        // Iterate over all parameters using the new unified map
        const auto& params = comp->getAllParams();
        
        for (const auto& [param_name, param_ptr] : params) {
            BaseParameter* param = param_ptr.get();
            if (!param) continue;
            
            // Get the parameter ID for the callback capture
            uint32_t param_id = param->getParameterId();
            
            // Set up onChange callback based on parameter type
            // Note: We DO set up broadcasts for read-only params - external systems subscribe to them
            // Chain callbacks: if param already has a callback, wrap it to also broadcast
            // This ensures component logic AND WebSocket broadcasting both happen
            switch (param->getType()) {
                case ParameterType::INT: {
                    auto* int_param = static_cast<IntParameter*>(param);
                    if (int_param->hasCallback()) {
                        // Wrap existing callback to also broadcast
                        auto existing_cb = int_param->getOnChange();
                        int_param->setOnChange([this, param_id, existing_cb](size_t row, size_t col, int val) {
                            // Call original callback first
                            if (existing_cb) existing_cb(row, col, val);
                            // Then broadcast
                            cJSON* value = cJSON_CreateNumber(val);
                            broadcastParameterUpdate(param_id, row, col, value);
                            cJSON_Delete(value);
                        });
                    } else {
                        int_param->setOnChange([this, param_id](size_t row, size_t col, int val) {
                            cJSON* value = cJSON_CreateNumber(val);
                            broadcastParameterUpdate(param_id, row, col, value);
                            cJSON_Delete(value);
                        });
                    }
                    break;
                }
                case ParameterType::FLOAT: {
                    auto* float_param = static_cast<FloatParameter*>(param);
                    if (float_param->hasCallback()) {
                        auto existing_cb = float_param->getOnChange();
                        float_param->setOnChange([this, param_id, existing_cb](size_t row, size_t col, float val) {
                            if (existing_cb) existing_cb(row, col, val);
                            cJSON* value = cJSON_CreateNumber(val);
                            broadcastParameterUpdate(param_id, row, col, value);
                            cJSON_Delete(value);
                        });
                    } else {
                        float_param->setOnChange([this, param_id](size_t row, size_t col, float val) {
                            cJSON* value = cJSON_CreateNumber(val);
                            broadcastParameterUpdate(param_id, row, col, value);
                            cJSON_Delete(value);
                        });
                    }
                    break;
                }
                case ParameterType::BOOL: {
                    auto* bool_param = static_cast<BoolParameter*>(param);
                    if (bool_param->hasCallback()) {
                        auto existing_cb = bool_param->getOnChange();
                        bool_param->setOnChange([this, param_id, existing_cb](size_t row, size_t col, bool val) {
                            if (existing_cb) existing_cb(row, col, val);
                            cJSON* value = cJSON_CreateBool(val);
                            broadcastParameterUpdate(param_id, row, col, value);
                            cJSON_Delete(value);
                        });
                    } else {
                        bool_param->setOnChange([this, param_id](size_t row, size_t col, bool val) {
                            cJSON* value = cJSON_CreateBool(val);
                            broadcastParameterUpdate(param_id, row, col, value);
                            cJSON_Delete(value);
                        });
                    }
                    break;
                }
                case ParameterType::STRING: {
                    auto* str_param = static_cast<StringParameter*>(param);
                    if (str_param->hasCallback()) {
                        auto existing_cb = str_param->getOnChange();
                        str_param->setOnChange([this, param_id, existing_cb](size_t row, size_t col, const std::string& val) {
                            if (existing_cb) existing_cb(row, col, val);
                            cJSON* value = cJSON_CreateString(val.c_str());
                            broadcastParameterUpdate(param_id, row, col, value);
                            cJSON_Delete(value);
                        });
                    } else {
                        str_param->setOnChange([this, param_id](size_t row, size_t col, const std::string& val) {
                            cJSON* value = cJSON_CreateString(val.c_str());
                            broadcastParameterUpdate(param_id, row, col, value);
                            cJSON_Delete(value);
                        });
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
}

Component* WebServerComponent::get_component(const char* name) {
    if (!component_graph) return nullptr;
    return component_graph->getComponent(name);
}

// WebSocket handler for real-time updates
esp_err_t WebServerComponent::ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        return ESP_OK;
    }
    
    // Get socket file descriptor for subscription tracking
    int socket_fd = httpd_req_to_sockfd(req);
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // Get frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE("WebServer", "Failed to get WS frame length: %d", ret);
        WebServerComponent* self = (WebServerComponent*)req->user_ctx;
        self->clear_subscriptions(socket_fd);
        return ret;
    }
    
    // Handle control frames (close, ping, pong)
    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        WebServerComponent* self = (WebServerComponent*)req->user_ctx;
        self->clear_subscriptions(socket_fd);
        return ESP_OK;
    }
    
    // Respond to PING with PONG (must echo back any payload)
    if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        // Control frame payloads are max 125 bytes per WebSocket spec
        uint8_t ping_payload[125];
        size_t ping_len = ws_pkt.len;
        
        // Read PING payload if any
        if (ping_len > 0) {
            if (ping_len > 125) ping_len = 125;
            ws_pkt.payload = ping_payload;
            esp_err_t recv_ret = httpd_ws_recv_frame(req, &ws_pkt, ping_len);
            if (recv_ret != ESP_OK) {
                ESP_LOGE("WebServer", "Failed to read PING payload: %d", recv_ret);
                return recv_ret;
            }
        }
        
        // Send PONG with same payload
        httpd_ws_frame_t pong_pkt;
        memset(&pong_pkt, 0, sizeof(httpd_ws_frame_t));
        pong_pkt.type = HTTPD_WS_TYPE_PONG;
        pong_pkt.payload = ping_len > 0 ? ping_payload : NULL;
        pong_pkt.len = ping_len;
        
        return httpd_ws_send_frame(req, &pong_pkt);
    }
    
    // Ignore PONG frames (response to our pings, if any)
    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        return ESP_OK;
    }
    
    // Only process TEXT frames - reject anything else
    if (ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
        ESP_LOGW("WebServer", "Ignoring non-TEXT frame type: %d", ws_pkt.type);
        return ESP_OK;
    }
    
    // Guard against oversized frames exhausting heap
    // Animation chunks are ~1400 bytes base64; normal JSON commands are <512 bytes.
    // 4KB is generous for any legitimate message.
    static constexpr size_t MAX_WS_FRAME_LEN = 4096;
    
    if (ws_pkt.len > MAX_WS_FRAME_LEN) {
        ESP_LOGE("WebServer", "WebSocket frame too large: %d bytes (max %zu)", ws_pkt.len, MAX_WS_FRAME_LEN);
        return ESP_ERR_NO_MEM;
    }
    
    if (ws_pkt.len) {
        uint8_t* buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
        if (!buf) {
            ESP_LOGE("WebServer", "Failed to allocate WS buffer (heap: %lu)", esp_get_free_heap_size());
            return ESP_ERR_NO_MEM;
        }
        
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE("WebServer", "Failed to receive WS frame: %d", ret);
            free(buf);
            return ret;
        }
        
        // Parse JSON message and handle request
        WebServerComponent* self = (WebServerComponent*)req->user_ctx;
        cJSON* json = cJSON_Parse((const char*)ws_pkt.payload);
        if (!json) {
            ESP_LOGE("WebServer", "Failed to parse WebSocket JSON");
            const char* error_msg = "{\"error\":\"invalid JSON\"}";
            ws_pkt.payload = (uint8_t*)error_msg;
            ws_pkt.len = strlen(error_msg);
            ws_pkt.type = HTTPD_WS_TYPE_TEXT;
            httpd_ws_send_frame(req, &ws_pkt);
            free(buf);
            return ESP_OK;
        }
        
        // Extract request ID if present
        cJSON* id_item = cJSON_GetObjectItem(json, "id");
        int request_id = id_item ? id_item->valueint : -1;
        
        cJSON* type_item = cJSON_GetObjectItem(json, "type");
        if (!type_item || !cJSON_IsString(type_item)) {
            ESP_LOGE("WebServer", "Missing 'type' field in WebSocket message");
            cJSON* error = cJSON_CreateObject();
            if (request_id >= 0) cJSON_AddNumberToObject(error, "id", request_id);
            cJSON_AddStringToObject(error, "error", "missing type field");
            char* error_str = cJSON_PrintUnformatted(error);
            if (error_str) {
                ws_pkt.payload = (uint8_t*)error_str;
                ws_pkt.len = strlen(error_str);
                ws_pkt.type = HTTPD_WS_TYPE_TEXT;
                httpd_ws_send_frame(req, &ws_pkt);
                free(error_str);
            }
            cJSON_Delete(error);
            cJSON_Delete(json);
            free(buf);
            return ESP_OK;
        }
        
        const char* msg_type = type_item->valuestring;
        cJSON* response = self->handle_ws_message(json, msg_type, socket_fd);
        
        if (response) {
            // Add request ID to response if it was in the request
            if (request_id >= 0) {
                cJSON_AddNumberToObject(response, "id", request_id);
            }
            
            char* response_str = cJSON_PrintUnformatted(response);
            if (response_str) {
                // Use fresh frame struct for sending (avoid any corruption from received frame)
                httpd_ws_frame_t send_pkt;
                memset(&send_pkt, 0, sizeof(httpd_ws_frame_t));
                send_pkt.payload = (uint8_t*)response_str;
                send_pkt.len = strlen(response_str);
                send_pkt.type = HTTPD_WS_TYPE_TEXT;
                send_pkt.final = true;  // Explicitly mark as final frame
                
                // Take send mutex to prevent concurrent frame sends with broadcast task
                if (xSemaphoreTake(self->ws_send_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    ret = httpd_ws_send_frame(req, &send_pkt);
                    xSemaphoreGive(self->ws_send_mutex);
                } else {
                    ESP_LOGW("WebServer", "Send mutex timeout - dropping response");
                    ret = ESP_ERR_TIMEOUT;
                }
                
                if (ret != ESP_OK) {
                    ESP_LOGE("WebServer", "Failed to send WS frame: %d", ret);
                }
                free(response_str);
            }
            cJSON_Delete(response);
        }
        
        cJSON_Delete(json);
        free(buf);
        return ret;
    }
    
    return ESP_OK;
}

// WebSocket message handler - wrapper for executeMessage, handles subscriptions
cJSON* WebServerComponent::handle_ws_message(cJSON* request, const char* msg_type, int socket_fd) {
    // Subscribe/unsubscribe use param_id (UUID) instead of component/type/index
    if (strcmp(msg_type, "subscribe") == 0) {
        cJSON* param_id_item = cJSON_GetObjectItem(request, "param_id");
        cJSON* row_item = cJSON_GetObjectItem(request, "row");
        cJSON* col_item = cJSON_GetObjectItem(request, "col");
        
        if (!param_id_item || !row_item || !col_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "missing required fields (param_id, row, col)");
            return error;
        }
        
        uint32_t param_id = (uint32_t)param_id_item->valueint;
        int row = row_item->valueint;
        int col = col_item->valueint;
        
        // Validate parameter exists
        BaseParameter* param = component_graph ? component_graph->getParamById(param_id) : nullptr;
        if (!param) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "parameter not found");
            return error;
        }
        
        // Validate bounds before accessing
        if (!param->checkBounds(row, col)) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "index out of bounds");
            cJSON_AddNumberToObject(error, "param_id", param_id);
            cJSON_AddNumberToObject(error, "requested_row", row);
            cJSON_AddNumberToObject(error, "requested_col", col);
            cJSON_AddNumberToObject(error, "max_rows", param->getRows());
            cJSON_AddNumberToObject(error, "max_cols", param->getCols());
            ESP_LOGW(TAG, "Client requested out-of-bounds index [%d,%d] for param %u (size: %zux%zu)",
                     row, col, param_id, param->getRows(), param->getCols());
            return error;
        }
        
        // Add to subscriptions
        SubscriptionKey key{param_id, row, col};
        subscribe_param(socket_fd, key);
        
        // Return current value using polymorphic JSON access
        cJSON* response = cJSON_CreateObject();
        cJSON* value = param->getValueAsJson(row, col);
        if (value) {
            cJSON_AddItemToObject(response, "value", value);
        }
        
        return response;
        
    } else if (strcmp(msg_type, "unsubscribe") == 0) {
        cJSON* param_id_item = cJSON_GetObjectItem(request, "param_id");
        cJSON* row_item = cJSON_GetObjectItem(request, "row");
        cJSON* col_item = cJSON_GetObjectItem(request, "col");
        
        if (!param_id_item || !row_item || !col_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "missing required fields (param_id, row, col)");
            return error;
        }
        
        uint32_t param_id = (uint32_t)param_id_item->valueint;
        int row = row_item->valueint;
        int col = col_item->valueint;
        
        // Remove from subscriptions
        SubscriptionKey key{param_id, row, col};
        unsubscribe_param(socket_fd, key);
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        return response;
    }
    
    // All other message types: delegate to executeMessage
    return executeMessage(request);
}

// Subscription management methods
void WebServerComponent::subscribe_param(int socket_fd, const SubscriptionKey& key) {
    if (xSemaphoreTake(subscriptions_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        subscriptions[socket_fd].insert(key);
        xSemaphoreGive(subscriptions_mutex);
    } else {
        ESP_LOGW(TAG, "subscribe_param: mutex timeout for socket %d", socket_fd);
    }
}

void WebServerComponent::unsubscribe_param(int socket_fd, const SubscriptionKey& key) {
    if (xSemaphoreTake(subscriptions_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        auto it = subscriptions.find(socket_fd);
        if (it != subscriptions.end()) {
            it->second.erase(key);
            if (it->second.empty()) {
                subscriptions.erase(it);
            }
        }
        xSemaphoreGive(subscriptions_mutex);
    } else {
        ESP_LOGW(TAG, "unsubscribe_param: mutex timeout for socket %d", socket_fd);
    }
}

void WebServerComponent::clear_subscriptions(int socket_fd) {
    if (xSemaphoreTake(subscriptions_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        subscriptions.erase(socket_fd);
        xSemaphoreGive(subscriptions_mutex);
    } else {
        ESP_LOGW(TAG, "clear_subscriptions: mutex timeout for socket %d", socket_fd);
    }
}

void WebServerComponent::broadcastParameterUpdate(uint32_t param_id, 
                                                   int row, int col, cJSON* value) {
    if (!broadcast_queue || !subscriptions_mutex) return;
    
    // Check if anyone is subscribed (with mutex protection)
    bool has_subscribers = false;
    if (xSemaphoreTake(subscriptions_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        // Early exit if NO ONE has any subscriptions at all
        if (subscriptions.empty()) {
            xSemaphoreGive(subscriptions_mutex);
            return;
        }
        
        // Check if anyone is subscribed to this specific parameter
        SubscriptionKey key{param_id, row, col};
        for (const auto& [socket_fd, subscribed_params] : subscriptions) {
            if (subscribed_params.find(key) != subscribed_params.end()) {
                has_subscribers = true;
                break;
            }
        }
        xSemaphoreGive(subscriptions_mutex);
    }
    
    // Don't queue if no one is subscribed to this specific parameter
    if (!has_subscribers) return;
    
    // Serialize value to JSON string (avoid cJSON in queue)
    char* value_str = cJSON_PrintUnformatted(value);
    if (!value_str) return;
    
    // Build queue item
    BroadcastQueueItem item;
    item.param_id = param_id;
    item.row = row;
    item.col = col;
    strncpy(item.value_json, value_str, sizeof(item.value_json) - 1);
    item.value_json[sizeof(item.value_json) - 1] = '\0';
    
    free(value_str);
    
    // Queue for broadcast task (non-blocking - drop if queue full)
    if (xQueueSend(broadcast_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Broadcast queue full - dropping update for param %u[%d][%d]",
                 param_id, row, col);
    }
}

// Execute JSON message (core implementation - used by both WebSocket and internal calls)
cJSON* WebServerComponent::executeMessage(const char* json_str) {
    // Delegate to ComponentGraph
    return component_graph ? component_graph->executeMessage(json_str) : nullptr;
}

cJSON* WebServerComponent::executeMessage(cJSON* request) {
    // Delegate to ComponentGraph
    return component_graph ? component_graph->executeMessage(request) : nullptr;
}

// Broadcast task wrapper
void WebServerComponent::broadcastTaskWrapper(void* pvParameters) {
    WebServerComponent* self = static_cast<WebServerComponent*>(pvParameters);
    self->broadcastTask();
}

// Broadcast task - reads queue and sends WebSocket frames
void WebServerComponent::broadcastTask() {
    BroadcastQueueItem item;
    TickType_t last_purge = xTaskGetTickCount();
    const TickType_t purge_interval = pdMS_TO_TICKS(30000);  // Purge stale connections every 30s
    
    while (true) {
        // Use a timeout so we can periodically purge stale connections
        BaseType_t received = xQueueReceive(broadcast_queue, &item, pdMS_TO_TICKS(5000));
        
        // Periodic stale connection cleanup
        TickType_t now = xTaskGetTickCount();
        if ((now - last_purge) >= purge_interval) {
            purgeStaleConnections();
            last_purge = now;
        }
        
        if (received != pdTRUE) continue;  // Timeout — loop back to check again
        if (!http_server) continue;
        
        SubscriptionKey key{item.param_id, item.row, item.col};
        
        // Parse value back from JSON string
        cJSON* value = cJSON_Parse(item.value_json);
        if (!value) {
            ESP_LOGE(TAG, "Failed to parse queued value JSON");
            continue;
        }
        
        // Build push message - now uses param_id instead of component/type/idx
        cJSON* push_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(push_msg, "type", "param_update");
        cJSON_AddNumberToObject(push_msg, "param_id", item.param_id);
        cJSON_AddNumberToObject(push_msg, "row", item.row);
        cJSON_AddNumberToObject(push_msg, "col", item.col);
        cJSON_AddItemToObject(push_msg, "value", value);  // Transfer ownership
        
        char* msg_str = cJSON_PrintUnformatted(push_msg);
        if (msg_str) {
            // ── Collect target sockets while holding mutex (fast) ──
            std::vector<int> target_sockets;
            
            if (xSemaphoreTake(subscriptions_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                for (const auto& [socket_fd, subscribed_params] : subscriptions) {
                    if (subscribed_params.find(key) != subscribed_params.end()) {
                        target_sockets.push_back(socket_fd);
                    }
                }
                xSemaphoreGive(subscriptions_mutex);
            } else {
                ESP_LOGW(TAG, "Broadcast: mutex timeout — skipping update for param %u", item.param_id);
            }
            
            // ── Send OUTSIDE the subscriptions mutex — network I/O can block ──
            // But take ws_send_mutex to prevent concurrent frame sends
            std::vector<int> dead_sockets;
            for (int socket_fd : target_sockets) {
                httpd_ws_frame_t ws_pkt;
                memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                ws_pkt.payload = (uint8_t*)msg_str;
                ws_pkt.len = strlen(msg_str);
                ws_pkt.type = HTTPD_WS_TYPE_TEXT;
                
                if (xSemaphoreTake(ws_send_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                    esp_err_t ret = httpd_ws_send_frame_async(http_server, socket_fd, &ws_pkt);
                    xSemaphoreGive(ws_send_mutex);
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to send param update to socket %d: %d", socket_fd, ret);
                        dead_sockets.push_back(socket_fd);
                    }
                } else {
                    ESP_LOGW(TAG, "Broadcast: send mutex timeout for socket %d", socket_fd);
                }
            }
            
            // Clean up dead sockets
            for (int dead_fd : dead_sockets) {
                clear_subscriptions(dead_fd);
            }
            
            free(msg_str);
        }
        
        cJSON_Delete(push_msg);
    }
}

// Purge stale WebSocket connections — removes subscriptions for sockets
// that are no longer valid (client crashed, WiFi dropped, half-open TCP)
void WebServerComponent::purgeStaleConnections() {
    if (!http_server || !subscriptions_mutex) return;
    
    std::vector<int> stale_sockets;
    
    if (xSemaphoreTake(subscriptions_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (const auto& [socket_fd, _] : subscriptions) {
            // httpd_ws_get_fd_info returns HTTPD_WS_CLIENT_WEBSOCKET for valid WS connections
            httpd_ws_client_info_t info = httpd_ws_get_fd_info(http_server, socket_fd);
            if (info != HTTPD_WS_CLIENT_WEBSOCKET) {
                stale_sockets.push_back(socket_fd);
            }
        }
        xSemaphoreGive(subscriptions_mutex);
    }
    
    for (int fd : stale_sockets) {
        clear_subscriptions(fd);
    }
}
