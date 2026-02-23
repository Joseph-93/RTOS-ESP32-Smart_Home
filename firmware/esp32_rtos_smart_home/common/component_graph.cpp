#include "component_graph.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"

// Persistence save interval in milliseconds (30 seconds)
static constexpr uint32_t PERSISTENCE_INTERVAL_MS = 30000;

// ============================================================================
// Constructor / Destructor
// ============================================================================

ComponentGraph::ComponentGraph() {
    // Create mutex for component maps
    componentsMutex = xSemaphoreCreateMutex();
    if (componentsMutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create components mutex!");
        assert(false && "Mutex creation failed");
    }
    
    // Create notification queues
    notification_queue_gui = xQueueCreate(10, sizeof(NotificationQueueItem));
    // NOTE: UART notification queue removed — no consumer exists and it
    // wastes ~2.8KB of RAM.  Re-add when a UART logging task is implemented.
    notification_queue_uart = nullptr;
    
    if (!notification_queue_gui) {
        ESP_LOGE(TAG, "Failed to create GUI notification queue");
    }
    
    // Create persistence timer (saves dirty parameters every 30 seconds)
    persistenceTimer = xTimerCreate(
        "persist_timer",
        pdMS_TO_TICKS(PERSISTENCE_INTERVAL_MS),
        pdTRUE,  // Auto-reload
        this,    // Timer ID = this pointer
        persistenceTimerCallback
    );
    if (persistenceTimer == nullptr) {
        ESP_LOGE(TAG, "Failed to create persistence timer!");
    }
}

ComponentGraph::~ComponentGraph() {
    // Stop and delete persistence timer
    if (persistenceTimer) {
        xTimerStop(persistenceTimer, portMAX_DELAY);
        xTimerDelete(persistenceTimer, portMAX_DELAY);
    }
    
    // Delete mutex
    if (componentsMutex) {
        vSemaphoreDelete(componentsMutex);
    }
    
    // Delete notification queues
    if (notification_queue_gui) {
        vQueueDelete(notification_queue_gui);
    }
    if (notification_queue_uart) {
        vQueueDelete(notification_queue_uart);
    }
    
    // Don't delete components - they're owned externally
    componentsByName.clear();
    componentsById.clear();
}

// ============================================================================
// Component Registration and Access
// ============================================================================

void ComponentGraph::registerComponent(Component* component) {
    if (!component) {
        ESP_LOGE(TAG, "Attempted to register null component");
        return;
    }
    
    if (xSemaphoreTake(componentsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for registerComponent");
        return;
    }
    
    const std::string& name = component->getName();
    uint32_t id = component->getComponentId();
    
    if (componentsByName.find(name) != componentsByName.end()) {
        ESP_LOGW(TAG, "Component '%s' already registered, replacing", name.c_str());
        // Remove old entry from componentsById
        for (auto it = componentsById.begin(); it != componentsById.end(); ++it) {
            if (it->second == componentsByName[name]) {
                componentsById.erase(it);
                break;
            }
        }
    }
    
    // Add to both maps
    componentsByName[name] = component;
    componentsById[id] = component;
    
    xSemaphoreGive(componentsMutex);
}

Component* ComponentGraph::getComponent(const std::string& name) {
    if (xSemaphoreTake(componentsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for getComponent");
        return nullptr;
    }
    
    auto it = componentsByName.find(name);
    Component* result = (it != componentsByName.end()) ? it->second : nullptr;
    
    xSemaphoreGive(componentsMutex);
    
    return result;
}

Component* ComponentGraph::getComponentById(uint32_t componentId) {
    if (xSemaphoreTake(componentsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for getComponentById");
        return nullptr;
    }
    
    auto it = componentsById.find(componentId);
    Component* result = (it != componentsById.end()) ? it->second : nullptr;
    
    xSemaphoreGive(componentsMutex);
    
    if (!result) {
        ESP_LOGE(TAG, "Component id=%u not found in graph", componentId);
    }
    return result;
}

// ============================================================================
// Parameter Access by Component Name + Param Name
// ============================================================================

BaseParameter* ComponentGraph::getParam(const std::string& component_name, const std::string& param_name) {
    Component* comp = getComponent(component_name);
    if (!comp) {
        return nullptr;
    }
    return comp->getParam(param_name);
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

BaseParameter* ComponentGraph::getParamById(uint32_t paramId) {
    if (xSemaphoreTake(componentsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for getParamById");
        return nullptr;
    }
    
    // Search all components for this parameter ID
    BaseParameter* result = nullptr;
    for (const auto& pair : componentsByName) {
        result = pair.second->getParamById(paramId);
        if (result) {
            break;
        }
    }
    
    xSemaphoreGive(componentsMutex);
    return result;
}

// ============================================================================
// Initialization
// ============================================================================

void ComponentGraph::initializeAll() {
    ESP_LOGI(TAG, "Initializing %zu components...", componentsByName.size());
    
    // Build a snapshot of components to iterate over (avoid holding mutex during init)
    std::vector<std::pair<std::string, Component*>> components_snapshot;
    
    if (xSemaphoreTake(componentsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for initializeAll");
        return;
    }
    
    components_snapshot.reserve(componentsByName.size());
    for (auto& pair : componentsByName) {
        components_snapshot.push_back(pair);
    }
    
    xSemaphoreGive(componentsMutex);  // Release mutex BEFORE calling component methods
    
    // First pass: set up dependencies (mutex NOT held - components can call getComponent safely)
    for (auto& pair : components_snapshot) {
        pair.second->setUpDependencies(this);
    }
    
    // Second pass: initialize (mutex NOT held)
    for (auto& pair : components_snapshot) {
        pair.second->initialize();
    }
    
    // Third pass: Post-initialization (for tasks that need all components ready)
    for (auto& pair : components_snapshot) {
        pair.second->postInitialize();
    }
    
    ESP_LOGI(TAG, "All components initialized");
}

std::vector<std::string> ComponentGraph::getComponentNames() const {
    if (xSemaphoreTake(componentsMutex, portMAX_DELAY) != pdTRUE) {
        return {};
    }
    
    std::vector<std::string> names;
    names.reserve(componentsByName.size());
    for (const auto& pair : componentsByName) {
        names.push_back(pair.first);
    }
    
    xSemaphoreGive(componentsMutex);
    return names;
}

bool ComponentGraph::hasComponent(const std::string& name) const {
    if (xSemaphoreTake(componentsMutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    
    bool exists = componentsByName.find(name) != componentsByName.end();
    
    xSemaphoreGive(componentsMutex);
    return exists;
}

// ============================================================================
// Notifications
// ============================================================================

void ComponentGraph::sendNotification(const char* message, bool is_error, int priority, uint32_t display_ms) {
    if (!notification_queue_gui) {
        ESP_LOGW(TAG, "Notification queue not initialized");
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
    
    // Send to UART queue if it exists (currently disabled to save RAM)
    if (notification_queue_uart) {
        xQueueSend(notification_queue_uart, &notif, 0);
    }
}

// ============================================================================
// JSON Message Processing
// ============================================================================

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
    
    // ========================================================================
    // get_components - List all components with IDs
    // ========================================================================
    if (strcmp(msg_type, "get_components") == 0) {
        const std::vector<std::string>& names = getComponentNames();
        cJSON* response = cJSON_CreateObject();
        cJSON* array = cJSON_CreateArray();
        
        for (const auto& name : names) {
            Component* comp = getComponent(name);
            if (comp) {
                cJSON* comp_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(comp_obj, "name", name.c_str());
                cJSON_AddNumberToObject(comp_obj, "id", comp->getComponentId());
                cJSON_AddItemToArray(array, comp_obj);
            }
        }
        
        cJSON_AddItemToObject(response, "components", array);
        return response;
    }
    
    // ========================================================================
    // get_component_params - Get all parameters for a component
    // ========================================================================
    else if (strcmp(msg_type, "get_component_params") == 0) {
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* comp_id_item = cJSON_GetObjectItem(request, "comp_id");
        
        Component* comp = nullptr;
        if (comp_item && cJSON_IsString(comp_item)) {
            comp = getComponent(comp_item->valuestring);
        } else if (comp_id_item && cJSON_IsNumber(comp_id_item)) {
            comp = getComponentById((uint32_t)comp_id_item->valueint);
        }
        
        if (!comp) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "component not found");
            return error;
        }
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "component", comp->getName().c_str());
        cJSON_AddNumberToObject(response, "component_id", comp->getComponentId());
        
        cJSON* params_array = cJSON_CreateArray();
        
        const auto& params = comp->getAllParams();
        for (const auto& pair : params) {
            BaseParameter* param = pair.second.get();
            cJSON* param_obj = cJSON_CreateObject();
            
            cJSON_AddStringToObject(param_obj, "name", param->getName().c_str());
            cJSON_AddNumberToObject(param_obj, "id", param->getParameterId());
            cJSON_AddStringToObject(param_obj, "type", param->getTypeString());
            cJSON_AddNumberToObject(param_obj, "rows", param->getRows());
            cJSON_AddNumberToObject(param_obj, "cols", param->getCols());
            cJSON_AddBoolToObject(param_obj, "readOnly", param->isReadOnly());
            
            // Add type-specific metadata
            ParameterType type = param->getType();
            if (type == ParameterType::INT) {
                auto* intParam = static_cast<IntParameter*>(param);
                cJSON_AddNumberToObject(param_obj, "min", intParam->getMin());
                cJSON_AddNumberToObject(param_obj, "max", intParam->getMax());
            } else if (type == ParameterType::FLOAT) {
                auto* floatParam = static_cast<FloatParameter*>(param);
                cJSON_AddNumberToObject(param_obj, "min", floatParam->getMin());
                cJSON_AddNumberToObject(param_obj, "max", floatParam->getMax());
            }
            
            cJSON_AddItemToArray(params_array, param_obj);
        }
        
        cJSON_AddItemToObject(response, "params", params_array);
        return response;
    }
    
    // ========================================================================
    // get_param - Get parameter value by name or ID
    // ========================================================================
    else if (strcmp(msg_type, "get_param") == 0) {
        cJSON* param_id_item = cJSON_GetObjectItem(request, "param_id");
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* param_name_item = cJSON_GetObjectItem(request, "param");
        cJSON* row_item = cJSON_GetObjectItem(request, "row");
        cJSON* col_item = cJSON_GetObjectItem(request, "col");
        
        int row = row_item && cJSON_IsNumber(row_item) ? row_item->valueint : 0;
        int col = col_item && cJSON_IsNumber(col_item) ? col_item->valueint : 0;
        
        BaseParameter* param = nullptr;
        
        // Lookup by param ID (preferred)
        if (param_id_item && cJSON_IsNumber(param_id_item)) {
            param = getParamById((uint32_t)param_id_item->valueint);
        }
        // Lookup by component name + param name (fallback)
        else if (comp_item && cJSON_IsString(comp_item) && 
                 param_name_item && cJSON_IsString(param_name_item)) {
            param = getParam(comp_item->valuestring, param_name_item->valuestring);
        }
        
        if (!param) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "parameter not found");
            return error;
        }
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "name", param->getName().c_str());
        cJSON_AddNumberToObject(response, "id", param->getParameterId());
        cJSON_AddStringToObject(response, "type", param->getTypeString());
        
        // Get value based on type
        cJSON* value = param->getValueAsJson(row, col);
        cJSON_AddItemToObject(response, "value", value);
        
        return response;
    }
    
    // ========================================================================
    // set_param - Set parameter value by name or ID
    // ========================================================================
    else if (strcmp(msg_type, "set_param") == 0) {
        cJSON* param_id_item = cJSON_GetObjectItem(request, "param_id");
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* param_name_item = cJSON_GetObjectItem(request, "param");
        cJSON* row_item = cJSON_GetObjectItem(request, "row");
        cJSON* col_item = cJSON_GetObjectItem(request, "col");
        cJSON* value_item = cJSON_GetObjectItem(request, "value");
        
        if (!value_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "missing value field");
            return error;
        }
        
        int row = row_item && cJSON_IsNumber(row_item) ? row_item->valueint : 0;
        int col = col_item && cJSON_IsNumber(col_item) ? col_item->valueint : 0;
        
        BaseParameter* param = nullptr;
        
        // Lookup by param ID (preferred)
        if (param_id_item && cJSON_IsNumber(param_id_item)) {
            param = getParamById((uint32_t)param_id_item->valueint);
        }
        // Lookup by component name + param name (fallback)
        else if (comp_item && cJSON_IsString(comp_item) && 
                 param_name_item && cJSON_IsString(param_name_item)) {
            param = getParam(comp_item->valuestring, param_name_item->valuestring);
        }
        
        if (!param) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "parameter not found");
            return error;
        }
        
        if (param->isReadOnly()) {
            ESP_LOGW(TAG, "Rejecting set on read-only parameter '%s'", param->getName().c_str());
            cJSON* error = cJSON_CreateObject();
            cJSON_AddBoolToObject(error, "success", false);
            cJSON_AddStringToObject(error, "error", "parameter is read-only");
            return error;
        }
        
        // Set value using polymorphic setValueFromJson
        bool success = param->setValueFromJson(row, col, value_item);
        
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "success", success);
        if (!success) {
            ESP_LOGE(TAG, "Failed to set parameter '%s'", param->getName().c_str());
            cJSON_AddStringToObject(response, "error", "failed to set value");
        }
        return response;
    }
    

    
    // ========================================================================
    // get_param_info (old API - still needed for one-at-a-time fetching)
    // Avoids overwhelming ESP32 by fetching params individually
    // ========================================================================
    else if (strcmp(msg_type, "get_param_info") == 0) {
        cJSON* comp_item = cJSON_GetObjectItem(request, "comp");
        cJSON* param_type_item = cJSON_GetObjectItem(request, "param_type");
        cJSON* idx_item = cJSON_GetObjectItem(request, "idx");
        
        if (!comp_item || !param_type_item) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "missing comp or param_type");
            return error;
        }
        
        Component* comp = getComponent(comp_item->valuestring);
        if (!comp) {
            cJSON* error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "component not found");
            return error;
        }
        
        const char* param_type = param_type_item->valuestring;
        int idx = (idx_item && cJSON_IsNumber(idx_item)) ? idx_item->valueint : -1;
        
        // Get params of this type
        const auto& params = comp->getAllParams();
        std::vector<BaseParameter*> typed_params;
        
        for (const auto& [id, param] : params) {
            const char* type_str = param->getTypeString();
            if (strcmp(type_str, param_type) == 0) {
                typed_params.push_back(param.get());
            }
        }
        
        cJSON* response = cJSON_CreateObject();
        
        if (idx == -1) {
            // Return count for this type
            cJSON_AddNumberToObject(response, "count", typed_params.size());
        } else if (idx >= 0 && idx < (int)typed_params.size()) {
            // Return info for specific param
            BaseParameter* p = typed_params[idx];
            cJSON_AddStringToObject(response, "name", p->getName().c_str());
            cJSON_AddNumberToObject(response, "param_id", p->getParameterId());
            cJSON_AddStringToObject(response, "type", p->getTypeString());
            cJSON_AddNumberToObject(response, "rows", p->getRows());
            cJSON_AddNumberToObject(response, "cols", p->getCols());
            cJSON_AddBoolToObject(response, "readOnly", p->isReadOnly());
            
            // Add min/max for numeric types
            if (strcmp(param_type, "int") == 0) {
                IntParameter* ip = static_cast<IntParameter*>(p);
                cJSON_AddNumberToObject(response, "min", ip->getMin());
                cJSON_AddNumberToObject(response, "max", ip->getMax());
            } else if (strcmp(param_type, "float") == 0) {
                FloatParameter* fp = static_cast<FloatParameter*>(p);
                cJSON_AddNumberToObject(response, "min", fp->getMin());
                cJSON_AddNumberToObject(response, "max", fp->getMax());
            }
        } else {
            cJSON_AddStringToObject(response, "error", "index out of range");
        }
        
        return response;
    }
    

    
    // Unknown message type
    cJSON* error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "unknown message type");
    return error;
}

// ============================================================================
// Parameter Persistence
// ============================================================================

void ComponentGraph::persistenceTimerCallback(TimerHandle_t timer) {
    ComponentGraph* self = static_cast<ComponentGraph*>(pvTimerGetTimerID(timer));
    if (self) {
        self->saveDirtyParameters();
    }
}

void ComponentGraph::saveDirtyParameters() {
    if (xSemaphoreTake(componentsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Could not acquire mutex for persistence save");
        return;
    }
    
    int totalSaved = 0;
    
    for (const auto& [name, component] : componentsByName) {
        // Create NVS namespace for this component: "p_<compId>"
        char nsName[16];
        snprintf(nsName, sizeof(nsName), "p_%lu", (unsigned long)component->getComponentId());
        
        nvs_handle_t handle;
        bool handleOpened = false;
        
        const auto& params = component->getAllParams();
        bool hasDirtyParams = false;
        for (const auto& [paramName, param] : params) {
            // Only save dirty, non-read-only parameters
            if (!param->isDirty() || param->isReadOnly()) {
                continue;
            }
            
            // Open handle lazily (only if we have something to save)
            if (!handleOpened) {
                esp_err_t err = nvs_open(nsName, NVS_READWRITE, &handle);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "[%s] Failed to open NVS for parameters: %s", 
                             name.c_str(), esp_err_to_name(err));
                    break;  // Skip this component
                }
                handleOpened = true;
            }
            
            // Build key: truncate param name to 15 chars for NVS key limit
            std::string keyPrefix = paramName.substr(0, 15);
            
            if (param->saveToNvs(handle, keyPrefix)) {
                param->clearDirty();
                totalSaved++;
                ESP_LOGD(TAG, "[%s] Saved param '%s' to NVS", name.c_str(), paramName.c_str());
            }
            hasDirtyParams = true;
        }
        
        // Check if component has custom data to save
        bool hasCustomData = component->hasCustomDataToSave();
        if (hasCustomData && !handleOpened) {
            esp_err_t err = nvs_open(nsName, NVS_READWRITE, &handle);
            if (err == ESP_OK) {
                handleOpened = true;
            }
        }
        
        // Call component's custom save hook if handle is open
        if (handleOpened && hasCustomData) {
            component->saveCustomData(handle);
            ESP_LOGD(TAG, "[%s] Saved custom data to NVS", name.c_str());
        }
        
        if (handleOpened) {
            nvs_commit(handle);
            nvs_close(handle);
        }
    }
    
    xSemaphoreGive(componentsMutex);
    
    if (totalSaved > 0) {
        ESP_LOGI(TAG, "Persistence: saved %d dirty parameters to NVS", totalSaved);
    }
}

void ComponentGraph::loadAllParameters() {
    if (xSemaphoreTake(componentsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Could not acquire mutex for parameter loading");
        return;
    }
    
    int totalLoaded = 0;
    
    for (const auto& [name, component] : componentsByName) {
        // NVS namespace for this component: "p_<compId>"
        char nsName[16];
        snprintf(nsName, sizeof(nsName), "p_%lu", (unsigned long)component->getComponentId());
        
        nvs_handle_t handle;
        esp_err_t err = nvs_open(nsName, NVS_READONLY, &handle);
        if (err != ESP_OK) {
            // No saved data yet - that's fine
            ESP_LOGD(TAG, "[%s] No saved parameters in NVS (namespace: %s)", name.c_str(), nsName);
            continue;
        }
        
        // CRITICAL: Load custom data FIRST (e.g., animation presets) so that
        // when parameter onChange callbacks fire, the data they reference exists
        component->loadCustomData(handle);
        
        const auto& params = component->getAllParams();
        for (const auto& [paramName, param] : params) {
            // Only load non-read-only parameters
            if (param->isReadOnly()) {
                continue;
            }
            
            std::string keyPrefix = paramName.substr(0, 15);
            
            if (param->loadFromNvs(handle, keyPrefix)) {
                totalLoaded++;
                ESP_LOGD(TAG, "[%s] Loaded param '%s' from NVS", name.c_str(), paramName.c_str());
            }
        }
        
        nvs_close(handle);
    }
    
    // After ALL data is loaded, give each component a chance to reconcile state
    // (e.g., RgbLed can now verify priority queue vs presets and start playback)
    for (const auto& [name, component] : componentsByName) {
        component->onPostLoadReconcile();
    }
    
    xSemaphoreGive(componentsMutex);
    
    if (totalLoaded > 0) {
        ESP_LOGI(TAG, "Persistence: loaded %d parameters from NVS", totalLoaded);
    }
    
    // Start the persistence timer now that loading is done
    if (persistenceTimer) {
        xTimerStart(persistenceTimer, portMAX_DELAY);
        ESP_LOGI(TAG, "Persistence timer started");
    }
}



