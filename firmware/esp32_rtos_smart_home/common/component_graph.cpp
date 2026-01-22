#include "component_graph.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

// Memory logging helper
static void log_component_memory(const char* component_name, const char* stage) {
    ESP_LOGI("ComponentGraph", "  [%s] %s - Free DRAM: %lu bytes", 
             component_name, stage, heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

ComponentGraph::ComponentGraph() {
    ESP_LOGI(TAG, "ComponentGraph created");
    
    // Create notification queues
    notification_queue_gui = xQueueCreate(10, sizeof(NotificationQueueItem));
    notification_queue_uart = xQueueCreate(20, sizeof(NotificationQueueItem));  // Larger queue for UART (no consumer yet)
    
    if (!notification_queue_gui || !notification_queue_uart) {
        ESP_LOGE(TAG, "Failed to create notification queues");
    } else {
        ESP_LOGI(TAG, "Notification queues created (GUI: 10 items, UART: 20 items)");
    }
}

ComponentGraph::~ComponentGraph() {
    ESP_LOGI(TAG, "ComponentGraph destroyed");
    
    // Delete notification queues
    if (notification_queue_gui) {
        vQueueDelete(notification_queue_gui);
    }
    if (notification_queue_uart) {
        vQueueDelete(notification_queue_uart);
    }
    
    // Don't delete components - they're owned externally
    components.clear();
}

void ComponentGraph::registerComponent(Component* component) {
    if (!component) {
        ESP_LOGE(TAG, "Attempted to register null component");
        return;
    }
    
    const std::string& name = component->getName();
    if (components.find(name) != components.end()) {
        ESP_LOGW(TAG, "Component '%s' already registered, replacing", name.c_str());
    }
    
    components[name] = component;
    ESP_LOGI(TAG, "Registered component: %s", name.c_str());
}

Component* ComponentGraph::getComponent(const std::string& name) {
    auto it = components.find(name);
    if (it == components.end()) {
        ESP_LOGE(TAG, "Component '%s' not found in graph", name.c_str());
        return nullptr;
    }
    return it->second;
}

IntParameter* ComponentGraph::getIntParam(const std::string& component_name, const std::string& param_name) {
    Component* comp = getComponent(component_name);
    if (!comp) {
        return nullptr;
    }
    return comp->getIntParam(param_name);
}

FloatParameter* ComponentGraph::getFloatParam(const std::string& component_name, const std::string& param_name) {
    Component* comp = getComponent(component_name);
    if (!comp) {
        return nullptr;
    }
    return comp->getFloatParam(param_name);
}

BoolParameter* ComponentGraph::getBoolParam(const std::string& component_name, const std::string& param_name) {
    Component* comp = getComponent(component_name);
    if (!comp) {
        return nullptr;
    }
    return comp->getBoolParam(param_name);
}

StringParameter* ComponentGraph::getStringParam(const std::string& component_name, const std::string& param_name) {
    Component* comp = getComponent(component_name);
    if (!comp) {
        return nullptr;
    }
    return comp->getStringParam(param_name);
}

void ComponentGraph::initializeAll() {
    ESP_LOGI(TAG, "=== STARTING COMPONENT INITIALIZATION ===");
    log_component_memory("GRAPH", "START of initializeAll");
    
    ESP_LOGI(TAG, "Setting up dependencies for all components (%zu total)...", components.size());
    log_component_memory("GRAPH", "BEFORE setUpDependencies phase");
    
    // First pass: set up dependencies
    for (auto& pair : components) {
        log_component_memory(pair.first.c_str(), "BEFORE setUpDependencies");
        ESP_LOGI(TAG, "Setting up dependencies for: %s", pair.first.c_str());
        pair.second->setUpDependencies(this);
        log_component_memory(pair.first.c_str(), "AFTER setUpDependencies");
    }
    
    log_component_memory("GRAPH", "AFTER setUpDependencies phase");
    ESP_LOGI(TAG, "Initializing all components...");
    log_component_memory("GRAPH", "BEFORE initialize phase");
    
    // Second pass: initialize
    for (auto& pair : components) {
        log_component_memory(pair.first.c_str(), "BEFORE init");
        ESP_LOGI(TAG, "Initializing component: %s", pair.first.c_str());
        pair.second->initialize();
        log_component_memory(pair.first.c_str(), "AFTER init");
    }
    
    log_component_memory("GRAPH", "AFTER initialize phase");
    // Third pass: Post-initialization (for tasks that need all components ready)
    ESP_LOGI(TAG, "Running post-initialization for all components...");
    log_component_memory("GRAPH", "BEFORE postInitialize phase");
    for (auto& pair : components) {
        log_component_memory(pair.first.c_str(), "BEFORE post-init");
        ESP_LOGI(TAG, "Post-initializing component: %s", pair.first.c_str());
        pair.second->postInitialize();
        log_component_memory(pair.first.c_str(), "AFTER post-init");
    }
    
    log_component_memory("GRAPH", "AFTER postInitialize phase");
    ESP_LOGI(TAG, "=== COMPONENT INITIALIZATION COMPLETE ===");
    log_component_memory("GRAPH", "END of initializeAll");
}

std::vector<std::string> ComponentGraph::getComponentNames() const {
    std::vector<std::string> names;
    names.reserve(components.size());
    for (const auto& pair : components) {
        names.push_back(pair.first);
    }
    return names;
}

bool ComponentGraph::hasComponent(const std::string& name) const {
    return components.find(name) != components.end();
}

void ComponentGraph::sendNotification(const char* message, bool is_error, int priority, uint32_t display_ms) {
    if (!notification_queue_gui || !notification_queue_uart) {
        ESP_LOGW(TAG, "Notification queues not initialized");
        return;
    }
    
    NotificationQueueItem notif;
    strncpy(notif.message, message, sizeof(notif.message) - 1);
    notif.message[sizeof(notif.message) - 1] = '\0';
    
    notif.level = is_error ? 
        NotificationQueueItem::NotificationLevel::ERROR :
        NotificationQueueItem::NotificationLevel::INFO;
    
    notif.ticks_to_display = pdMS_TO_TICKS(display_ms);
    notif.priority = priority;
    
    // Send to GUI queue (blocking if full)
    xQueueSend(notification_queue_gui, &notif, 0);
    
    // Send to UART queue (non-blocking, drop if full to prevent overflow since no consumer yet)
    if (xQueueSend(notification_queue_uart, &notif, 0) != pdTRUE) {
        // Queue full - silently drop (UART consumer doesn't exist yet)
    }
}

cJSON* ComponentGraph::executeMessage(const char* json_str) {
    if (!json_str) return nullptr;
    
    cJSON* request = cJSON_Parse(json_str);
    if (!request) {
        ESP_LOGE(TAG, "Failed to parse JSON message");
        return nullptr;
    }
    
    cJSON* response = executeMessage(request);
    cJSON_Delete(request);
    return response;
}

cJSON* ComponentGraph::executeMessage(cJSON* request) {
    if (!request) return nullptr;
    
    cJSON* type_item = cJSON_GetObjectItem(request, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        ESP_LOGE(TAG, "Message missing 'type' field");
        return nullptr;
    }
    
    const char* msg_type = type_item->valuestring;
    ESP_LOGI(TAG, "Executing message type: %s", msg_type);
    
    if (strcmp(msg_type, "get_components") == 0) {
        const std::vector<std::string>& names = getComponentNames();
        cJSON* response = cJSON_CreateObject();
        cJSON* array = cJSON_CreateArray();
        
        for (const auto& name : names) {
            cJSON_AddItemToArray(array, cJSON_CreateString(name.c_str()));
        }
        
        cJSON_AddItemToObject(response, "components", array);
        return response;
        
    } else if (strcmp(msg_type, "get_param_info") == 0) {
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
        
        Component* comp = getComponent(comp_name);
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
                cJSON_AddNumberToObject(response, "idx", idx);
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
                cJSON_AddNumberToObject(response, "idx", idx);
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
                cJSON_AddNumberToObject(response, "idx", idx);
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
                cJSON_AddNumberToObject(response, "idx", idx);
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
                cJSON_AddNumberToObject(response, "idx", idx);
            }
        }
        
        return response;
        
    } else if (strcmp(msg_type, "get_param") == 0) {
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
        
        Component* comp = getComponent(comp_name);
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
        } else if (strcmp(param_type, "actions") == 0) {
            const auto& actions = comp->getActions();
            if (idx < actions.size()) {
                // For actions, return the stored argument (row/col ignored)
                cJSON_AddStringToObject(response, "value", actions[idx].argument.c_str());
            }
        }
        
        return response;
        
    } else if (strcmp(msg_type, "set_param") == 0) {
        ESP_LOGI(TAG, "=== SET PARAMETER ===");
        
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
        
        Component* comp = getComponent(comp_name);
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
        } else if (strcmp(param_type, "actions") == 0) {
            const auto& actions = comp->getActions();
            if (idx < actions.size()) {
                if (cJSON_IsString(value_item)) {
                    comp->setActionArgument(idx, value_item->valuestring);
                    ESP_LOGI(TAG, "Set action argument [%d] = %s", idx, value_item->valuestring);
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
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* action_item = cJSON_GetObjectItem(request, "action");
        cJSON* arg_item = cJSON_GetObjectItem(request, "arg");
        
        if (!comp_item || !action_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "missing comp or action");
            return error;
        }
        
        const char* comp_name = comp_item->valuestring;
        const char* action_name = action_item->valuestring;
        
        Component* comp = getComponent(comp_name);
        if (!comp) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "component not found");
            return error;
        }
        
        const std::vector<ComponentAction>& actions = comp->getActions();
        for (size_t i = 0; i < actions.size(); i++) {
            if (actions[i].name == action_name) {
                // If arg provided in message, use it as temporary argument
                // Otherwise use the action's stored argument
                if (arg_item && cJSON_IsString(arg_item)) {
                    comp->invokeAction(i, arg_item->valuestring);
                } else {
                    comp->invokeAction(i);
                }
                cJSON* response = cJSON_CreateObject();
                cJSON_AddBoolToObject(response, "success", true);
                return response;
            }
        }
        
        cJSON* error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "error", "action not found");
        return error;
        
    } else if (strcmp(msg_type, "set_action_arg") == 0) {
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* action_item = cJSON_GetObjectItem(request, "action");
        cJSON* arg_item = cJSON_GetObjectItem(request, "arg");
        
        if (!comp_item || !action_item || !arg_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "missing comp, action, or arg");
            return error;
        }
        
        const char* comp_name = comp_item->valuestring;
        const char* action_name = action_item->valuestring;
        const char* arg = arg_item->valuestring;
        
        Component* comp = getComponent(comp_name);
        if (!comp) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "component not found");
            return error;
        }
        
        comp->setActionArgument(action_name, arg);
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", true);
        return response;
    }
    
    // Unknown message type
    cJSON* error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "unknown message type");
    return error;
}

