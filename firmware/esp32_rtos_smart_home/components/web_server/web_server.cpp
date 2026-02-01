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
    ESP_LOGI(TAG, "Starting WebSocket-ONLY server on port 80");
    ESP_LOGI(TAG, "Free heap BEFORE server: %lu bytes", esp_get_free_heap_size());
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 10;
    config.stack_size = 3072;  // Reduced from 4096 to save RAM
    config.max_req_hdr_len = 1024;  // Reduced from 2048 - WebSocket doesn't need large headers
    config.lru_purge_enable = true;
    config.max_open_sockets = 2;  // Reduced from 3 - minimal concurrent connections
    config.send_wait_timeout = 3;  // Shorter timeout
    config.recv_wait_timeout = 3;
    config.task_priority = 10;  // Higher priority to avoid starvation by WiFi tasks
    
    ESP_LOGI(TAG, "Config: stack=%d hdr_len=%d max_sockets=%d", 
             config.stack_size, config.max_req_hdr_len, config.max_open_sockets);
    
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
    
    ESP_LOGI(TAG, "WebSocket-ONLY server started successfully");
    ESP_LOGI(TAG, "WebSocket endpoint: ws://esp32/ws");
    ESP_LOGI(TAG, "All communication through WebSocket - no HTTP REST endpoints");
    
    // Create mutex for subscriptions map protection
    subscriptions_mutex = xSemaphoreCreateMutex();
    if (!subscriptions_mutex) {
        ESP_LOGE(TAG, "Failed to create subscriptions mutex - FATAL");
        abort();
    }
    
    // Create broadcast queue (3 items - minimal for memory constraints)
    broadcast_queue = xQueueCreate(3, sizeof(BroadcastQueueItem));
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
    ESP_LOGI(TAG, "WebSocket broadcast task created (priority 15)");
    
    // NOTE: setupParameterBroadcasting() is now called in postInitialize()
    // to ensure all components have set their onChange callbacks first
}

void WebServerComponent::postInitialize() {
    ESP_LOGI(TAG, "Setting up parameter broadcasting (all components initialized)");
    
    // Print memory diagnostics BEFORE setting up broadcasting
    print_memory_diagnostics(TAG);
    
    // Component memory breakdown
    if (component_graph) {
        ESP_LOGI(TAG, "=== COMPONENT MEMORY USAGE ===");
        const auto& comp_names = component_graph->getComponentNames();
        ESP_LOGI(TAG, "Found %d components to analyze", comp_names.size());
        
        size_t total_component_mem = 0;
        for (size_t i = 0; i < comp_names.size(); i++) {
            const auto& comp_name = comp_names[i];
            ESP_LOGI(TAG, "Analyzing component %d/%d: %s", i+1, comp_names.size(), comp_name.c_str());
            
            Component* comp = component_graph->getComponent(comp_name);
            if (comp) {
                size_t mem = comp->getApproximateMemoryUsage();
                total_component_mem += mem;
                ESP_LOGI(TAG, "  %s: ~%zu bytes", comp_name.c_str(), mem);
            } else {
                ESP_LOGW(TAG, "  %s: NULL component pointer!", comp_name.c_str());
            }
        }
        ESP_LOGI(TAG, "  TOTAL COMPONENTS: ~%zu bytes", total_component_mem);
        ESP_LOGI(TAG, "==============================");
    }
    
    setupParameterBroadcasting();
    
    // Print memory diagnostics AFTER setting up broadcasting
    print_memory_diagnostics(TAG);
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
            // But we skip params that already have callbacks (to avoid overwriting)
            switch (param->getType()) {
                case ParameterType::INT: {
                    auto* int_param = static_cast<IntParameter*>(param);
                    if (int_param->hasCallback()) continue;
                    int_param->setOnChange([this, param_id](size_t row, size_t col, int val) {
                        cJSON* value = cJSON_CreateNumber(val);
                        broadcastParameterUpdate(param_id, row, col, value);
                        cJSON_Delete(value);
                    });
                    break;
                }
                case ParameterType::FLOAT: {
                    auto* float_param = static_cast<FloatParameter*>(param);
                    if (float_param->hasCallback()) continue;
                    float_param->setOnChange([this, param_id](size_t row, size_t col, float val) {
                        cJSON* value = cJSON_CreateNumber(val);
                        broadcastParameterUpdate(param_id, row, col, value);
                        cJSON_Delete(value);
                    });
                    break;
                }
                case ParameterType::BOOL: {
                    auto* bool_param = static_cast<BoolParameter*>(param);
                    if (bool_param->hasCallback()) continue;
                    bool_param->setOnChange([this, param_id](size_t row, size_t col, bool val) {
                        cJSON* value = cJSON_CreateBool(val);
                        broadcastParameterUpdate(param_id, row, col, value);
                        cJSON_Delete(value);
                    });
                    break;
                }
                case ParameterType::STRING: {
                    auto* str_param = static_cast<StringParameter*>(param);
                    if (str_param->hasCallback()) continue;
                    str_param->setOnChange([this, param_id](size_t row, size_t col, const std::string& val) {
                        cJSON* value = cJSON_CreateString(val.c_str());
                        broadcastParameterUpdate(param_id, row, col, value);
                        cJSON_Delete(value);
                    });
                    break;
                }
                default:
                    break;
            }
        }
    }
    
    ESP_LOGI(TAG, "Parameter broadcasting set up for %d components", comp_names.size());
}

Component* WebServerComponent::get_component(const char* name) {
    if (!component_graph) return nullptr;
    return component_graph->getComponent(name);
}

// WebSocket handler for real-time updates
esp_err_t WebServerComponent::ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI("WebServer", "WebSocket handshake initiated");
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
        ESP_LOGI("WebServer", "WebSocket close frame received from socket %d", socket_fd);
        WebServerComponent* self = (WebServerComponent*)req->user_ctx;
        self->clear_subscriptions(socket_fd);
        return ESP_OK;
    }
    
    ESP_LOGI("WebServer", "WebSocket frame received");
    
    ESP_LOGI("WebServer", "WS frame len: %d", ws_pkt.len);
    
    if (ws_pkt.len) {
        uint8_t* buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
        if (!buf) {
            ESP_LOGE("WebServer", "Failed to allocate WS buffer");
            return ESP_ERR_NO_MEM;
        }
        
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE("WebServer", "Failed to receive WS frame: %d", ret);
            free(buf);
            return ret;
        }
        
        ESP_LOGI("WebServer", "WS received: %s", ws_pkt.payload);
        
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
                ESP_LOGI("WebServer", "WS sending: %s", response_str);
                ws_pkt.payload = (uint8_t*)response_str;
                ws_pkt.len = strlen(response_str);
                ws_pkt.type = HTTPD_WS_TYPE_TEXT;
                ret = httpd_ws_send_frame(req, &ws_pkt);
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
    ESP_LOGI(TAG, "Handling WebSocket message from socket %d", socket_fd);
    
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
        
        // Add to subscriptions
        SubscriptionKey key{param_id, row, col};
        subscribe_param(socket_fd, key);
        
        // Return current value using polymorphic JSON access
        cJSON* response = cJSON_CreateObject();
        cJSON* value = param->getValueAsJson(row, col);
        if (value) {
            cJSON_AddItemToObject(response, "value", value);
        }
        
        ESP_LOGI(TAG, "Subscribed socket %d to param %u[%d][%d]", 
                 socket_fd, param_id, row, col);
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
        
        ESP_LOGI(TAG, "Unsubscribed socket %d from param %u[%d][%d]", 
                 socket_fd, param_id, row, col);
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        return response;
    }
    
    // All other message types: delegate to executeMessage
    return executeMessage(request);
}

// Subscription management methods
void WebServerComponent::subscribe_param(int socket_fd, const SubscriptionKey& key) {
    if (xSemaphoreTake(subscriptions_mutex, portMAX_DELAY) == pdTRUE) {
        subscriptions[socket_fd].insert(key);
        int count = subscriptions[socket_fd].size();
        xSemaphoreGive(subscriptions_mutex);
        ESP_LOGI(TAG, "Socket %d subscribed to param %u[%d][%d]. Total subscriptions: %d",
                 socket_fd, key.param_id, key.row, key.col, count);
    }
}

void WebServerComponent::unsubscribe_param(int socket_fd, const SubscriptionKey& key) {
    if (xSemaphoreTake(subscriptions_mutex, portMAX_DELAY) == pdTRUE) {
        auto it = subscriptions.find(socket_fd);
        if (it != subscriptions.end()) {
            it->second.erase(key);
            if (it->second.empty()) {
                subscriptions.erase(it);
            }
        }
        xSemaphoreGive(subscriptions_mutex);
    }
}

void WebServerComponent::clear_subscriptions(int socket_fd) {
    if (xSemaphoreTake(subscriptions_mutex, portMAX_DELAY) == pdTRUE) {
        auto it = subscriptions.find(socket_fd);
        if (it != subscriptions.end()) {
            int count = it->second.size();
            subscriptions.erase(it);
            ESP_LOGI(TAG, "Cleared %d subscriptions for socket %d", count, socket_fd);
        }
        xSemaphoreGive(subscriptions_mutex);
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
    ESP_LOGI(TAG, "Broadcast task started");
    BroadcastQueueItem item;
    
    while (true) {
        // Wait for parameter update
        if (xQueueReceive(broadcast_queue, &item, portMAX_DELAY) == pdTRUE) {
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
                // Find all sockets subscribed to this parameter and send (with mutex protection)
                std::vector<int> dead_sockets;  // Collect failed sockets for cleanup
                
                if (xSemaphoreTake(subscriptions_mutex, portMAX_DELAY) == pdTRUE) {
                    for (const auto& [socket_fd, subscribed_params] : subscriptions) {
                        if (subscribed_params.find(key) != subscribed_params.end()) {
                            httpd_ws_frame_t ws_pkt;
                            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
                            ws_pkt.payload = (uint8_t*)msg_str;
                            ws_pkt.len = strlen(msg_str);
                            ws_pkt.type = HTTPD_WS_TYPE_TEXT;
                            
                            esp_err_t ret = httpd_ws_send_frame_async(http_server, socket_fd, &ws_pkt);
                            if (ret != ESP_OK) {
                                ESP_LOGW(TAG, "Failed to send param update to socket %d: %d", socket_fd, ret);
                                dead_sockets.push_back(socket_fd);
                            }
                        }
                    }
                    xSemaphoreGive(subscriptions_mutex);
                }
                
                // Clean up dead sockets outside the iteration
                for (int dead_fd : dead_sockets) {
                    clear_subscriptions(dead_fd);
                }
                
                free(msg_str);
            }
            
            cJSON_Delete(push_msg);
        }
    }
}
