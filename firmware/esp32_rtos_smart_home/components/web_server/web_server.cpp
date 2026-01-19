#include "web_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

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

void WebServerComponent::initialize() {
    ESP_LOGI(TAG, "Starting HTTP REST server on port 80");
    ESP_LOGI(TAG, "Free heap BEFORE server: %lu bytes", esp_get_free_heap_size());
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 10;
    config.stack_size = 4096;  // Reduce stack to save RAM
    config.max_req_hdr_len = 2048;
    config.lru_purge_enable = true;
    config.max_open_sockets = 3;  // Limit concurrent connections
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
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL
    };
    httpd_register_uri_handler(http_server, &ws_uri);
    
    ESP_LOGI(TAG, "WebSocket-ONLY server started successfully");
    ESP_LOGI(TAG, "WebSocket endpoint: ws://esp32/ws");
    ESP_LOGI(TAG, "All communication through WebSocket - no HTTP REST endpoints");
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
    
    ESP_LOGI("WebServer", "WebSocket frame received");
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // Get frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE("WebServer", "Failed to get WS frame length: %d", ret);
        return ret;
    }
    
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
        cJSON* response = self->handle_ws_message(json, msg_type);
        
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
cJSON* WebServerComponent::handle_ws_message(cJSON* request, const char* msg_type) {
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
            }
        } else if (strcmp(param_type, "bool") == 0) {
            const auto& list = comp->getBoolParams();
            if (idx == -1) {
                cJSON_AddNumberToObject(response, "count", list.size());
            } else if (idx >= 0 && idx < list.size()) {
                cJSON_AddStringToObject(response, "name", list[idx]->getName().c_str());
                cJSON_AddNumberToObject(response, "rows", list[idx]->getRows());
                cJSON_AddNumberToObject(response, "cols", list[idx]->getCols());
            }
        } else if (strcmp(param_type, "str") == 0) {
            const auto& list = comp->getStringParams();
            if (idx == -1) {
                cJSON_AddNumberToObject(response, "count", list.size());
            } else if (idx >= 0 && idx < list.size()) {
                cJSON_AddStringToObject(response, "name", list[idx]->getName().c_str());
                cJSON_AddNumberToObject(response, "rows", list[idx]->getRows());
                cJSON_AddNumberToObject(response, "cols", list[idx]->getCols());
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
        
        if (strcmp(param_type, "int") == 0) {
            const auto& list = comp->getIntParams();
            if (idx < list.size()) {
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
        } else if (strcmp(param_type, "float") == 0) {
            const auto& list = comp->getFloatParams();
            if (idx < list.size()) {
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
        } else if (strcmp(param_type, "bool") == 0) {
            const auto& list = comp->getBoolParams();
            if (idx < list.size()) {
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
        } else if (strcmp(param_type, "str") == 0) {
            const auto& list = comp->getStringParams();
            if (idx < list.size()) {
                if (cJSON_IsString(value_item)) {
                    list[idx]->setValue(row, col, value_item->valuestring);
                    ESP_LOGI(TAG, "Set string parameter [%d][%d,%d] = %s", idx, row, col, value_item->valuestring);
                    success = true;
                }
            }
        }
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", success);
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
    }
    
    // Unknown message type
    cJSON* error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "unknown message type");
    return error;
}
