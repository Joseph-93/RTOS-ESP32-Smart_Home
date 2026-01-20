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
        
        // Set up int params
        const auto& int_params = comp->getIntParams();
        for (size_t idx = 0; idx < int_params.size(); idx++) {
            auto* param = int_params[idx].get();
            // Wrap existing onChange callback
            auto existing_cb = param->getOnChange();  // Copy the function
            param->setOnChange([this, comp_name, idx, existing_cb](size_t row, size_t col, int val) {
                // Call existing callback if it exists
                if (existing_cb) {
                    existing_cb(row, col, val);
                }
                // Broadcast update
                cJSON* value = cJSON_CreateNumber(val);
                broadcastParameterUpdate(comp_name.c_str(), "int", idx, row, col, value);
                cJSON_Delete(value);
            });
        }
        
        // Set up float params
        const auto& float_params = comp->getFloatParams();
        for (size_t idx = 0; idx < float_params.size(); idx++) {
            auto* param = float_params[idx].get();
            auto existing_cb = param->getOnChange();  // Copy the function
            param->setOnChange([this, comp_name, idx, existing_cb](size_t row, size_t col, float val) {
                if (existing_cb) {
                    existing_cb(row, col, val);
                }
                cJSON* value = cJSON_CreateNumber(val);
                broadcastParameterUpdate(comp_name.c_str(), "float", idx, row, col, value);
                cJSON_Delete(value);
            });
        }
        
        // Set up bool params
        const auto& bool_params = comp->getBoolParams();
        for (size_t idx = 0; idx < bool_params.size(); idx++) {
            auto* param = bool_params[idx].get();
            auto existing_cb = param->getOnChange();  // Copy the function
            param->setOnChange([this, comp_name, idx, existing_cb](size_t row, size_t col, bool val) {
                if (existing_cb) {
                    existing_cb(row, col, val);
                }
                cJSON* value = cJSON_CreateBool(val);
                broadcastParameterUpdate(comp_name.c_str(), "bool", idx, row, col, value);
                cJSON_Delete(value);
            });
        }
        
        // Set up string params
        const auto& str_params = comp->getStringParams();
        for (size_t idx = 0; idx < str_params.size(); idx++) {
            auto* param = str_params[idx].get();
            auto existing_cb = param->getOnChange();  // Copy the function
            param->setOnChange([this, comp_name, idx, existing_cb](size_t row, size_t col, const std::string& val) {
                if (existing_cb) {
                    existing_cb(row, col, val);
                }
                cJSON* value = cJSON_CreateString(val.c_str());
                broadcastParameterUpdate(comp_name.c_str(), "str", idx, row, col, value);
                cJSON_Delete(value);
            });
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

// WebSocket message handler - routes messages by type
cJSON* WebServerComponent::handle_ws_message(cJSON* request, const char* msg_type, int socket_fd) {
    ESP_LOGI(TAG, "Handling WebSocket message type: %s", msg_type);
    
    if (strcmp(msg_type, "get_components") == 0) {
        // Return list of all components
        if (!component_graph) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "no graph");
            return error;
        }
        
        const std::vector<std::string>& names = component_graph->getComponentNames();
        cJSON* response = cJSON_CreateObject();
        cJSON* array = cJSON_CreateArray();
        
        for (const auto& name : names) {
            cJSON_AddItemToArray(array, cJSON_CreateString(name.c_str()));
        }
        
        cJSON_AddItemToObject(response, "components", array);
        return response;
        
    } else if (strcmp(msg_type, "get_param_info") == 0) {
        // Get parameter info for a component
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* type_item = cJSON_GetObjectItem(request, "param_type");
        cJSON* idx_item = cJSON_GetObjectItem(request, "idx");
        
        if (!comp_item || !type_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "missing comp or param_type");
            return error;
        }
        
        const char* comp_name = comp_item->valuestring;
        const char* param_type = type_item->valuestring;
        int idx = idx_item ? idx_item->valueint : -1;
        
        Component* comp = get_component(comp_name);
        if (!comp) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "component not found");
            return error;
        }
        
        cJSON* response = cJSON_CreateObject();
        
        if (strcmp(param_type, "int") == 0) {
            const auto& list = comp->getIntParams();
            if (idx == -1) {
                cJSON_AddNumberToObject(response, "count", list.size());
            } else if (idx >= 0 && idx < list.size()) {
                cJSON_AddStringToObject(response, "name", list[idx]->getName().c_str());
                cJSON_AddNumberToObject(response, "rows", list[idx]->getRows());
                cJSON_AddNumberToObject(response, "cols", list[idx]->getCols());
                cJSON_AddNumberToObject(response, "min", list[idx]->getMin());
                cJSON_AddNumberToObject(response, "max", list[idx]->getMax());
                cJSON_AddBoolToObject(response, "readOnly", list[idx]->isReadOnly());
            }
        } else if (strcmp(param_type, "float") == 0) {
            const auto& list = comp->getFloatParams();
            if (idx == -1) {
                cJSON_AddNumberToObject(response, "count", list.size());
            } else if (idx >= 0 && idx < list.size()) {
                cJSON_AddStringToObject(response, "name", list[idx]->getName().c_str());
                cJSON_AddNumberToObject(response, "rows", list[idx]->getRows());
                cJSON_AddNumberToObject(response, "cols", list[idx]->getCols());
                cJSON_AddNumberToObject(response, "min", list[idx]->getMin());
                cJSON_AddNumberToObject(response, "max", list[idx]->getMax());
                cJSON_AddBoolToObject(response, "readOnly", list[idx]->isReadOnly());
            }
        } else if (strcmp(param_type, "bool") == 0) {
            const auto& list = comp->getBoolParams();
            if (idx == -1) {
                cJSON_AddNumberToObject(response, "count", list.size());
            } else if (idx >= 0 && idx < list.size()) {
                cJSON_AddStringToObject(response, "name", list[idx]->getName().c_str());
                cJSON_AddNumberToObject(response, "rows", list[idx]->getRows());
                cJSON_AddNumberToObject(response, "cols", list[idx]->getCols());
                cJSON_AddBoolToObject(response, "readOnly", list[idx]->isReadOnly());
            }
        } else if (strcmp(param_type, "str") == 0) {
            const auto& list = comp->getStringParams();
            if (idx == -1) {
                cJSON_AddNumberToObject(response, "count", list.size());
            } else if (idx >= 0 && idx < list.size()) {
                cJSON_AddStringToObject(response, "name", list[idx]->getName().c_str());
                cJSON_AddNumberToObject(response, "rows", list[idx]->getRows());
                cJSON_AddNumberToObject(response, "cols", list[idx]->getCols());
                cJSON_AddBoolToObject(response, "readOnly", list[idx]->isReadOnly());
            }
        } else if (strcmp(param_type, "actions") == 0) {
            std::vector<std::string> actionNames = comp->getActionNames();
            if (idx == -1) {
                cJSON_AddNumberToObject(response, "count", actionNames.size());
            } else if (idx >= 0 && idx < actionNames.size()) {
                cJSON_AddStringToObject(response, "name", actionNames[idx].c_str());
            }
        }
        
        return response;
        
    } else if (strcmp(msg_type, "get_param") == 0) {
        // Get parameter value
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* type_item = cJSON_GetObjectItem(request, "param_type");
        cJSON* idx_item = cJSON_GetObjectItem(request, "idx");
        cJSON* row_item = cJSON_GetObjectItem(request, "row");
        cJSON* col_item = cJSON_GetObjectItem(request, "col");
        
        if (!comp_item || !type_item || !idx_item || !row_item || !col_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "missing required fields");
            return error;
        }
        
        const char* comp_name = comp_item->valuestring;
        const char* param_type = type_item->valuestring;
        int idx = idx_item->valueint;
        int row = row_item->valueint;
        int col = col_item->valueint;
        
        Component* comp = get_component(comp_name);
        if (!comp) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "component not found");
            return error;
        }
        
        cJSON* response = cJSON_CreateObject();
        
        if (strcmp(param_type, "int") == 0) {
            const auto& list = comp->getIntParams();
            if (idx < list.size()) {
                int value = list[idx]->getValue(row, col);
                cJSON_AddNumberToObject(response, "value", value);
            }
        } else if (strcmp(param_type, "float") == 0) {
            const auto& list = comp->getFloatParams();
            if (idx < list.size()) {
                float value = list[idx]->getValue(row, col);
                cJSON_AddNumberToObject(response, "value", value);
            }
        } else if (strcmp(param_type, "bool") == 0) {
            const auto& list = comp->getBoolParams();
            if (idx < list.size()) {
                bool value = list[idx]->getValue(row, col);
                cJSON_AddBoolToObject(response, "value", value);
            }
        } else if (strcmp(param_type, "str") == 0) {
            const auto& list = comp->getStringParams();
            if (idx < list.size()) {
                std::string value = list[idx]->getValue(row, col);
                cJSON_AddStringToObject(response, "value", value.c_str());
            }
        }
        
        return response;
        
    } else if (strcmp(msg_type, "set_param") == 0) {
        // Set parameter value
        ESP_LOGI(TAG, "=== SET PARAMETER (WebSocket) ===");
        
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* type_item = cJSON_GetObjectItem(request, "param_type");
        cJSON* idx_item = cJSON_GetObjectItem(request, "idx");
        cJSON* row_item = cJSON_GetObjectItem(request, "row");
        cJSON* col_item = cJSON_GetObjectItem(request, "col");
        cJSON* value_item = cJSON_GetObjectItem(request, "value");
        
        if (!comp_item || !type_item || !idx_item || !row_item || !col_item || !value_item) {
            ESP_LOGE(TAG, "Missing required fields");
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "missing required fields");
            return error;
        }
        
        const char* comp_name = comp_item->valuestring;
        const char* param_type = type_item->valuestring;
        int idx = idx_item->valueint;
        int row = row_item->valueint;
        int col = col_item->valueint;
        
        ESP_LOGI(TAG, "Parsed: comp=%s, type=%s, idx=%d, row=%d, col=%d", 
                 comp_name, param_type, idx, row, col);
        
        Component* comp = get_component(comp_name);
        if (!comp) {
            ESP_LOGE(TAG, "Component '%s' not found", comp_name);
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "component not found");
            return error;
        }
        
        bool success = false;
        const char* error_msg = nullptr;
        
        if (strcmp(param_type, "int") == 0) {
            const auto& list = comp->getIntParams();
            if (idx < list.size()) {
                if (list[idx]->isReadOnly()) {
                    ESP_LOGW(TAG, "Rejecting set on read-only int parameter [%d]", idx);
                    error_msg = "parameter is read-only";
                } else {
                    int val;
                    if (cJSON_IsNumber(value_item)) {
                        val = value_item->valueint;
                    } else if (cJSON_IsString(value_item)) {
                        val = atoi(value_item->valuestring);
                    } else {
                        val = 0;
                    }
                    list[idx]->setValue(row, col, val);
                    ESP_LOGI(TAG, "Set int parameter [%d][%d,%d] = %d", idx, row, col, val);
                    success = true;
                }
            }
        } else if (strcmp(param_type, "float") == 0) {
            const auto& list = comp->getFloatParams();
            if (idx < list.size()) {
                if (list[idx]->isReadOnly()) {
                    ESP_LOGW(TAG, "Rejecting set on read-only float parameter [%d]", idx);
                    error_msg = "parameter is read-only";
                } else {
                    float val;
                    if (cJSON_IsNumber(value_item)) {
                        val = (float)value_item->valuedouble;
                    } else if (cJSON_IsString(value_item)) {
                        val = atof(value_item->valuestring);
                    } else {
                        val = 0.0f;
                    }
                    list[idx]->setValue(row, col, val);
                    ESP_LOGI(TAG, "Set float parameter [%d][%d,%d] = %f", idx, row, col, val);
                    success = true;
                }
            }
        } else if (strcmp(param_type, "bool") == 0) {
            const auto& list = comp->getBoolParams();
            if (idx < list.size()) {
                if (list[idx]->isReadOnly()) {
                    ESP_LOGW(TAG, "Rejecting set on read-only bool parameter [%d]", idx);
                    error_msg = "parameter is read-only";
                } else {
                    bool val;
                    if (cJSON_IsBool(value_item)) {
                        val = cJSON_IsTrue(value_item);
                    } else if (cJSON_IsString(value_item)) {
                        const char* str = value_item->valuestring;
                        val = (strcmp(str, "true") == 0 || strcmp(str, "1") == 0);
                    } else if (cJSON_IsNumber(value_item)) {
                        val = (value_item->valueint != 0);
                    } else {
                        val = false;
                    }
                    list[idx]->setValue(row, col, val);
                    ESP_LOGI(TAG, "Set bool parameter [%d][%d,%d] = %s", idx, row, col, val ? "true" : "false");
                    success = true;
                }
            }
        } else if (strcmp(param_type, "str") == 0) {
            const auto& list = comp->getStringParams();
            if (idx < list.size()) {
                if (list[idx]->isReadOnly()) {
                    ESP_LOGW(TAG, "Rejecting set on read-only string parameter [%d]", idx);
                    error_msg = "parameter is read-only";
                } else if (cJSON_IsString(value_item)) {
                    list[idx]->setValue(row, col, value_item->valuestring);
                    ESP_LOGI(TAG, "Set string parameter [%d][%d,%d] = %s", idx, row, col, value_item->valuestring);
                    success = true;
                }
            }
        }
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", success);
        if (error_msg) {
            cJSON_AddStringToObject(response, "error", error_msg);
        }
        if (success) {
            ESP_LOGI(TAG, "Parameter set successfully");
        } else {
            ESP_LOGE(TAG, "Failed to set parameter");
        }
        return response;
        
    } else if (strcmp(msg_type, "invoke_action") == 0) {
        // Invoke action
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* action_item = cJSON_GetObjectItem(request, "action");
        
        if (!comp_item || !action_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "missing comp or action");
            return error;
        }
        
        const char* comp_name = comp_item->valuestring;
        const char* action_name = action_item->valuestring;
        
        Component* comp = get_component(comp_name);
        if (!comp) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "component not found");
            return error;
        }
        
        const std::vector<ComponentAction>& actions = comp->getActions();
        for (size_t i = 0; i < actions.size(); i++) {
            if (actions[i].name == action_name) {
                comp->invokeAction(i);
                cJSON* response = cJSON_CreateObject();
                cJSON_AddBoolToObject(response, "success", true);
                return response;
            }
        }
        
        cJSON* error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "error", "action not found");
        return error;
    
    } else if (strcmp(msg_type, "subscribe") == 0) {
        // Subscribe to parameter updates
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* type_item = cJSON_GetObjectItem(request, "param_type");
        cJSON* idx_item = cJSON_GetObjectItem(request, "idx");
        cJSON* row_item = cJSON_GetObjectItem(request, "row");
        cJSON* col_item = cJSON_GetObjectItem(request, "col");
        
        if (!comp_item || !type_item || !idx_item || !row_item || !col_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "missing required fields");
            return error;
        }
        
        const char* comp_name = comp_item->valuestring;
        const char* param_type = type_item->valuestring;
        int idx = idx_item->valueint;
        int row = row_item->valueint;
        int col = col_item->valueint;
        
        // Add to subscriptions
        SubscriptionKey key{comp_name, param_type, idx, row, col};
        subscribe_param(socket_fd, key);
        
        // Return current value (same as get_param)
        Component* comp = get_component(comp_name);
        if (!comp) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "component not found");
            return error;
        }
        
        cJSON* response = cJSON_CreateObject();
        
        if (strcmp(param_type, "int") == 0) {
            const auto& list = comp->getIntParams();
            if (idx < list.size()) {
                int value = list[idx]->getValue(row, col);
                cJSON_AddNumberToObject(response, "value", value);
            }
        } else if (strcmp(param_type, "float") == 0) {
            const auto& list = comp->getFloatParams();
            if (idx < list.size()) {
                float value = list[idx]->getValue(row, col);
                cJSON_AddNumberToObject(response, "value", value);
            }
        } else if (strcmp(param_type, "bool") == 0) {
            const auto& list = comp->getBoolParams();
            if (idx < list.size()) {
                bool value = list[idx]->getValue(row, col);
                cJSON_AddBoolToObject(response, "value", value);
            }
        } else if (strcmp(param_type, "str") == 0) {
            const auto& list = comp->getStringParams();
            if (idx < list.size()) {
                std::string value = list[idx]->getValue(row, col);
                cJSON_AddStringToObject(response, "value", value.c_str());
            }
        }
        
        ESP_LOGI(TAG, "Subscribed socket %d to %s.%s[%d][%d][%d]", 
                 socket_fd, comp_name, param_type, idx, row, col);
        return response;
        
    } else if (strcmp(msg_type, "unsubscribe") == 0) {
        // Unsubscribe from parameter updates
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* type_item = cJSON_GetObjectItem(request, "param_type");
        cJSON* idx_item = cJSON_GetObjectItem(request, "idx");
        cJSON* row_item = cJSON_GetObjectItem(request, "row");
        cJSON* col_item = cJSON_GetObjectItem(request, "col");
        
        if (!comp_item || !type_item || !idx_item || !row_item || !col_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "missing required fields");
            return error;
        }
        
        const char* comp_name = comp_item->valuestring;
        const char* param_type = type_item->valuestring;
        int idx = idx_item->valueint;
        int row = row_item->valueint;
        int col = col_item->valueint;
        
        // Remove from subscriptions
        SubscriptionKey key{comp_name, param_type, idx, row, col};
        unsubscribe_param(socket_fd, key);
        
        ESP_LOGI(TAG, "Unsubscribed socket %d from %s.%s[%d][%d][%d]", 
                 socket_fd, comp_name, param_type, idx, row, col);
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        return response;
    }
    
    // Unknown message type
    cJSON* error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "unknown message type");
    return error;
}

// Subscription management methods
void WebServerComponent::subscribe_param(int socket_fd, const SubscriptionKey& key) {
    if (xSemaphoreTake(subscriptions_mutex, portMAX_DELAY) == pdTRUE) {
        subscriptions[socket_fd].insert(key);
        int count = subscriptions[socket_fd].size();
        xSemaphoreGive(subscriptions_mutex);
        ESP_LOGI(TAG, "Socket %d subscribed to %s.%s[%d][%d][%d]. Total subscriptions: %d",
                 socket_fd, key.component.c_str(), key.param_type.c_str(), 
                 key.idx, key.row, key.col, count);
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

void WebServerComponent::broadcastParameterUpdate(const char* comp, const char* param_type, 
                                                   int idx, int row, int col, cJSON* value) {
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
        SubscriptionKey key{comp, param_type, idx, row, col};
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
    strncpy(item.component, comp, sizeof(item.component) - 1);
    item.component[sizeof(item.component) - 1] = '\0';
    strncpy(item.param_type, param_type, sizeof(item.param_type) - 1);
    item.param_type[sizeof(item.param_type) - 1] = '\0';
    item.idx = idx;
    item.row = row;
    item.col = col;
    strncpy(item.value_json, value_str, sizeof(item.value_json) - 1);
    item.value_json[sizeof(item.value_json) - 1] = '\0';
    
    free(value_str);
    
    // Queue for broadcast task (non-blocking - drop if queue full)
    if (xQueueSend(broadcast_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Broadcast queue full - dropping update for %s.%s[%d][%d][%d]",
                 comp, param_type, idx, row, col);
    }
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
            
            SubscriptionKey key{item.component, item.param_type, item.idx, item.row, item.col};
            
            // Parse value back from JSON string
            cJSON* value = cJSON_Parse(item.value_json);
            if (!value) {
                ESP_LOGE(TAG, "Failed to parse queued value JSON");
                continue;
            }
            
            // Build push message
            cJSON* push_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(push_msg, "type", "param_update");
            cJSON_AddStringToObject(push_msg, "comp", item.component);
            cJSON_AddStringToObject(push_msg, "param_type", item.param_type);
            cJSON_AddNumberToObject(push_msg, "idx", item.idx);
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
