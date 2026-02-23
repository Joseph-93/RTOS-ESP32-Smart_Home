#include "component.h"
#include "nvs_flash.h"
#include "nvs.h"

// Static member initialization
uint32_t Component::nextComponentId = 1;
uint32_t Component::nextParameterId = 1;
bool Component::nvsLoaded = false;

// NVS namespace and keys
static const char* NVS_NAMESPACE = "component_ids";
static const char* NVS_KEY_COMP_ID = "next_comp_id";
static const char* NVS_KEY_PARAM_ID = "next_param_id";

// ============================================================================
// NVS Persistence
// ============================================================================

static bool nvsInitialized = false;

static void ensureNvsInitialized() {
    if (nvsInitialized) return;
    
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_LOGW("Component", "NVS needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    
    if (err == ESP_OK) {
        ESP_LOGI("Component", "NVS flash initialized successfully");
        nvsInitialized = true;
    } else {
        ESP_LOGE("Component", "Failed to initialize NVS flash: %s", esp_err_to_name(err));
    }
}

void Component::loadNextIds() {
    if (nvsLoaded) return;
    
    // Ensure NVS is initialized before first use
    ensureNvsInitialized();
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint32_t comp_id = 1, param_id = 1;
        nvs_get_u32(handle, NVS_KEY_COMP_ID, &comp_id);
        nvs_get_u32(handle, NVS_KEY_PARAM_ID, &param_id);
        nvs_close(handle);
        
        nextComponentId = comp_id;
        nextParameterId = param_id;
        ESP_LOGI("Component", "Loaded UUIDs from NVS: nextComponentId=%u, nextParameterId=%u", 
                 nextComponentId, nextParameterId);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI("Component", "No saved UUIDs in NVS, starting from 1");
    } else {
        ESP_LOGW("Component", "Failed to open NVS for reading: %s", esp_err_to_name(err));
    }
    
    nvsLoaded = true;
}

void Component::saveNextIds() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW("Component", "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return;
    }
    
    nvs_set_u32(handle, NVS_KEY_COMP_ID, nextComponentId);
    nvs_set_u32(handle, NVS_KEY_PARAM_ID, nextParameterId);
    nvs_commit(handle);
    nvs_close(handle);
}

// ============================================================================
// Component Implementation
// ============================================================================

Component::Component(const std::string &name) : name(name), initialized(false) {
    // Load UUIDs from NVS on first component creation
    loadNextIds();
    
    // Assign component ID and increment
    componentId = nextComponentId++;
    saveNextIds();
    
    // Create params mutex
    paramsMutex = xSemaphoreCreateMutex();
    if (paramsMutex == nullptr) {
        ESP_LOGE(TAG, "Component '%s': Failed to create params mutex!", name.c_str());
        assert(false && "Mutex creation failed");
    }
    
    ESP_LOGD(TAG, "Component '%s' created with id=%u", name.c_str(), componentId);
}

Component::~Component() {
    ESP_LOGD(TAG, "Component '%s' (id=%u) destroyed", name.c_str(), componentId);
    
    if (paramsMutex != nullptr) {
        vSemaphoreDelete(paramsMutex);
    }
}

void Component::initialize() {
    // Setup hub store BEFORE component-specific init
    setupHubStore();
    
    onInitialize();
    initialized = true;
}

const std::string& Component::getName() const {
    return name;
}

bool Component::isInitialized() const {
    return initialized;
}

// ============================================================================
// Parameter Access
// ============================================================================

BaseParameter* Component::getParam(const std::string& paramName) {
    if (xSemaphoreTake(paramsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for getParam");
        return nullptr;
    }
    
    auto it = paramsByName.find(paramName);
    BaseParameter* result = (it != paramsByName.end()) ? it->second.get() : nullptr;
    
    xSemaphoreGive(paramsMutex);
    return result;
}

BaseParameter* Component::getParamById(uint32_t paramId) {
    if (xSemaphoreTake(paramsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for getParamById");
        return nullptr;
    }
    
    auto it = paramsById.find(paramId);
    BaseParameter* result = (it != paramsById.end()) ? it->second : nullptr;
    
    xSemaphoreGive(paramsMutex);
    return result;
}

IntParameter* Component::getIntParam(const std::string& paramName) {
    BaseParameter* param = getParam(paramName);
    if (param && param->getType() == ParameterType::INT) {
        return static_cast<IntParameter*>(param);
    }
    return nullptr;
}

FloatParameter* Component::getFloatParam(const std::string& paramName) {
    BaseParameter* param = getParam(paramName);
    if (param && param->getType() == ParameterType::FLOAT) {
        return static_cast<FloatParameter*>(param);
    }
    return nullptr;
}

BoolParameter* Component::getBoolParam(const std::string& paramName) {
    BaseParameter* param = getParam(paramName);
    if (param && param->getType() == ParameterType::BOOL) {
        return static_cast<BoolParameter*>(param);
    }
    return nullptr;
}

StringParameter* Component::getStringParam(const std::string& paramName) {
    BaseParameter* param = getParam(paramName);
    if (param && param->getType() == ParameterType::STRING) {
        return static_cast<StringParameter*>(param);
    }
    return nullptr;
}

const std::unordered_map<std::string, std::unique_ptr<BaseParameter>>& Component::getAllParams() const {
    return paramsByName;
}

// ============================================================================
// Add Parameter Methods
// ============================================================================

IntParameter* Component::addIntParam(const std::string &paramName, size_t rows, size_t cols, 
                                     int min_val, int max_val, int default_val, bool readOnly) {
    if (xSemaphoreTake(paramsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for addIntParam");
        return nullptr;
    }
    
    // Check if parameter already exists
    if (paramsByName.find(paramName) != paramsByName.end()) {
        ESP_LOGE(TAG, "Parameter '%s' already exists in component '%s'", paramName.c_str(), name.c_str());
        xSemaphoreGive(paramsMutex);
        return nullptr;
    }
    
    // Assign ID and create parameter
    uint32_t paramId = nextParameterId++;
    auto param = std::make_unique<IntParameter>(paramName, paramId, rows, cols, min_val, max_val, default_val, readOnly);
    IntParameter* ptr = param.get();
    
    // Add to both maps
    paramsById[paramId] = ptr;
    paramsByName[paramName] = std::move(param);
    
    xSemaphoreGive(paramsMutex);
    
    // Save updated IDs to NVS
    saveNextIds();
    
    return ptr;
}

FloatParameter* Component::addFloatParam(const std::string &paramName, size_t rows, size_t cols, 
                                         float min_val, float max_val, float default_val, bool readOnly) {
    if (xSemaphoreTake(paramsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for addFloatParam");
        return nullptr;
    }
    
    if (paramsByName.find(paramName) != paramsByName.end()) {
        ESP_LOGE(TAG, "Parameter '%s' already exists in component '%s'", paramName.c_str(), name.c_str());
        xSemaphoreGive(paramsMutex);
        return nullptr;
    }
    
    uint32_t paramId = nextParameterId++;
    auto param = std::make_unique<FloatParameter>(paramName, paramId, rows, cols, min_val, max_val, default_val, readOnly);
    FloatParameter* ptr = param.get();
    
    paramsById[paramId] = ptr;
    paramsByName[paramName] = std::move(param);
    
    xSemaphoreGive(paramsMutex);
    saveNextIds();
    
    return ptr;
}

BoolParameter* Component::addBoolParam(const std::string &paramName, size_t rows, size_t cols, 
                                       bool default_val, bool readOnly) {
    if (xSemaphoreTake(paramsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for addBoolParam");
        return nullptr;
    }
    
    if (paramsByName.find(paramName) != paramsByName.end()) {
        ESP_LOGE(TAG, "Parameter '%s' already exists in component '%s'", paramName.c_str(), name.c_str());
        xSemaphoreGive(paramsMutex);
        return nullptr;
    }
    
    uint32_t paramId = nextParameterId++;
    // BoolParameter is Parameter<uint8_t>, so we need to pass min, max, and default as uint8_t
    auto param = std::make_unique<BoolParameter>(paramName, paramId, rows, cols, 0, 1, default_val ? 1 : 0, readOnly);
    BoolParameter* ptr = param.get();
    
    paramsById[paramId] = ptr;
    paramsByName[paramName] = std::move(param);
    
    xSemaphoreGive(paramsMutex);
    saveNextIds();
    
    return ptr;
}

StringParameter* Component::addStringParam(const std::string &paramName, size_t rows, size_t cols, 
                                           const std::string &default_val, bool readOnly) {
    if (xSemaphoreTake(paramsMutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take mutex for addStringParam");
        return nullptr;
    }
    
    if (paramsByName.find(paramName) != paramsByName.end()) {
        ESP_LOGE(TAG, "Parameter '%s' already exists in component '%s'", paramName.c_str(), name.c_str());
        xSemaphoreGive(paramsMutex);
        return nullptr;
    }
    
    uint32_t paramId = nextParameterId++;
    auto param = std::make_unique<StringParameter>(paramName, paramId, rows, cols, default_val, default_val, default_val, readOnly);
    StringParameter* ptr = param.get();
    
    paramsById[paramId] = ptr;
    paramsByName[paramName] = std::move(param);
    
    xSemaphoreGive(paramsMutex);
    saveNextIds();
    
    return ptr;
}

// ============================================================================
// Memory Diagnostics
// ============================================================================

size_t Component::getApproximateMemoryUsage() const {
    size_t total = 0;
    
    // Component overhead
    total += sizeof(Component);
    total += name.capacity();
    
    // Parameters
    if (xSemaphoreTake(paramsMutex, portMAX_DELAY) != pdTRUE) {
        return total;
    }
    
    for (const auto& pair : paramsByName) {
        const auto& param = pair.second;
        total += sizeof(BaseParameter);
        total += param->getName().capacity();
        total += param->getRows() * param->getCols() * 32;  // Rough estimate per element
    }
    
    // Map overhead
    total += paramsByName.size() * (sizeof(std::string) + sizeof(std::unique_ptr<BaseParameter>) + 32);
    total += paramsById.size() * (sizeof(uint32_t) + sizeof(BaseParameter*) + 16);
    
    xSemaphoreGive(paramsMutex);
    
    return total;
}

// ============================================================================
// Hub Store - Key-Value Storage for GUI/Central Hub Metadata
// ============================================================================

void Component::setupHubStore() {
    // Load any existing data from NVS
    hubStoreLoadFromNvs();
    
    // Create params for key-value access
    hubStoreKeyParam = addStringParam("hub_store_key", 1, 1, "");
    hubStoreValueParam = addStringParam("hub_store_value", 1, 1, "");
    hubStoreDeleteParam = addBoolParam("hub_store_delete", 1, 1, false);
    hubStoreDumpParam = addStringParam("hub_store_dump", 1, 1, "", true);  // Read-only
    
    // When key changes, load the value for that key
    hubStoreKeyParam->setOnChange([this](size_t, size_t, const std::string& key) {
        hubStoreCurrentKey = key;
        // Load current value for this key (empty if not found)
        auto it = hubStore.find(key);
        std::string val = (it != hubStore.end()) ? it->second : "";
        hubStoreValueParam->setValue(0, 0, val);
        ESP_LOGD(TAG, "[%s] Hub store key set to '%s', value='%s'", name.c_str(), key.c_str(), val.c_str());
    });
    
    // When value is set, store it for the current key
    hubStoreValueParam->setOnChange([this](size_t, size_t, const std::string& value) {
        if (hubStoreCurrentKey.empty()) {
            ESP_LOGW(TAG, "[%s] Hub store: cannot set value without key", name.c_str());
            return;
        }
        if (value.empty()) {
            // Empty value = delete key
            hubStore.erase(hubStoreCurrentKey);
        } else {
            hubStore[hubStoreCurrentKey] = value;
        }
        hubStoreSaveToNvs();
        
        // Update dump param
        cJSON* dump = cJSON_CreateObject();
        for (const auto& kv : hubStore) {
            cJSON_AddStringToObject(dump, kv.first.c_str(), kv.second.c_str());
        }
        char* json = cJSON_PrintUnformatted(dump);
        hubStoreDumpParam->setValue(0, 0, json ? json : "{}");
        cJSON_free(json);
        cJSON_Delete(dump);
    });
    
    // Delete param - when set to true, delete current key
    hubStoreDeleteParam->setOnChange([this](size_t, size_t, bool val) {
        if (val && !hubStoreCurrentKey.empty()) {
            hubStore.erase(hubStoreCurrentKey);
            hubStoreValueParam->setValue(0, 0, "");
            hubStoreSaveToNvs();
            
            // Update dump param
            cJSON* dump = cJSON_CreateObject();
            for (const auto& kv : hubStore) {
                cJSON_AddStringToObject(dump, kv.first.c_str(), kv.second.c_str());
            }
            char* json = cJSON_PrintUnformatted(dump);
            hubStoreDumpParam->setValue(0, 0, json ? json : "{}");
            cJSON_free(json);
            cJSON_Delete(dump);
        }
        // Reset to false
        hubStoreDeleteParam->setValue(0, 0, false);
    });
    
    // Initialize dump param with current contents
    cJSON* dump = cJSON_CreateObject();
    for (const auto& kv : hubStore) {
        cJSON_AddStringToObject(dump, kv.first.c_str(), kv.second.c_str());
    }
    char* json = cJSON_PrintUnformatted(dump);
    hubStoreDumpParam->setValue(0, 0, json ? json : "{}");
    cJSON_free(json);
    cJSON_Delete(dump);
}

std::string Component::hubStoreGet(const std::string& key) const {
    auto it = hubStore.find(key);
    return (it != hubStore.end()) ? it->second : "";
}

void Component::hubStoreSet(const std::string& key, const std::string& value) {
    hubStore[key] = value;
    hubStoreSaveToNvs();
}

bool Component::hubStoreDelete(const std::string& key) {
    auto it = hubStore.find(key);
    if (it != hubStore.end()) {
        hubStore.erase(it);
        hubStoreSaveToNvs();
        return true;
    }
    return false;
}

std::map<std::string, std::string> Component::hubStoreGetAll() const {
    return hubStore;
}

void Component::hubStoreLoadFromNvs() {
    // Use component-specific namespace: "hs_<componentId>"
    char nsName[16];
    snprintf(nsName, sizeof(nsName), "hs_%lu", (unsigned long)componentId);
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(nsName, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // No saved data yet
        ESP_LOGD(TAG, "[%s] No hub store data in NVS (namespace: %s)", name.c_str(), nsName);
        return;
    }
    
    // Read index (list of keys as JSON array)
    size_t indexLen = 0;
    err = nvs_get_str(handle, "_index", nullptr, &indexLen);
    if (err == ESP_OK && indexLen > 0) {
        char* indexStr = (char*)malloc(indexLen);
        if (indexStr && nvs_get_str(handle, "_index", indexStr, &indexLen) == ESP_OK) {
            cJSON* keys = cJSON_Parse(indexStr);
            if (keys && cJSON_IsArray(keys)) {
                cJSON* keyItem;
                cJSON_ArrayForEach(keyItem, keys) {
                    if (cJSON_IsString(keyItem)) {
                        const char* key = keyItem->valuestring;
                        // Read value for this key
                        size_t valLen = 0;
                        if (nvs_get_str(handle, key, nullptr, &valLen) == ESP_OK && valLen > 0) {
                            char* val = (char*)malloc(valLen);
                            if (val && nvs_get_str(handle, key, val, &valLen) == ESP_OK) {
                                hubStore[key] = val;
                            }
                            free(val);
                        }
                    }
                }
            }
            cJSON_Delete(keys);
        }
        free(indexStr);
    }
    
    nvs_close(handle);
}

void Component::hubStoreSaveToNvs() {
    // Use component-specific namespace: "hs_<componentId>"
    char nsName[16];
    snprintf(nsName, sizeof(nsName), "hs_%lu", (unsigned long)componentId);
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open(nsName, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[%s] Failed to open NVS for hub store: %s", name.c_str(), esp_err_to_name(err));
        return;
    }
    
    // Build key index
    cJSON* keys = cJSON_CreateArray();
    for (const auto& kv : hubStore) {
        cJSON_AddItemToArray(keys, cJSON_CreateString(kv.first.c_str()));
        // Save value (truncate key to 15 chars for NVS key limit)
        char nvsKey[16];
        snprintf(nvsKey, sizeof(nvsKey), "%.15s", kv.first.c_str());
        nvs_set_str(handle, nvsKey, kv.second.c_str());
    }
    
    // Save index
    char* indexStr = cJSON_PrintUnformatted(keys);
    if (indexStr) {
        nvs_set_str(handle, "_index", indexStr);
        cJSON_free(indexStr);
    }
    cJSON_Delete(keys);
    
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGD(TAG, "[%s] Saved %zu hub store entries to NVS", name.c_str(), hubStore.size());
}

// ============================================================================
// Parameter NVS Persistence Specializations
// ============================================================================

// For scalar (1x1) parameters, save directly. For arrays, save as blob.

template<>
bool Parameter<int32_t>::saveToNvs(nvs_handle_t handle, const std::string& keyPrefix) {
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    
    bool success = false;
    if (rows == 1 && cols == 1) {
        // Scalar - save as i32
        success = (nvs_set_i32(handle, keyPrefix.c_str(), data[0]) == ESP_OK);
    } else {
        // Array - save as blob
        success = (nvs_set_blob(handle, keyPrefix.c_str(), data.data(), data.size() * sizeof(int32_t)) == ESP_OK);
    }
    
    xSemaphoreGive(mutex);
    return success;
}

template<>
bool Parameter<int32_t>::loadFromNvs(nvs_handle_t handle, const std::string& keyPrefix) {
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    
    bool success = false;
    if (rows == 1 && cols == 1) {
        // Scalar - load as i32
        int32_t val;
        if (nvs_get_i32(handle, keyPrefix.c_str(), &val) == ESP_OK) {
            data[0] = val;
            success = true;
        }
    } else {
        // Array - load as blob
        size_t required_size = data.size() * sizeof(int32_t);
        size_t stored_size = 0;
        if (nvs_get_blob(handle, keyPrefix.c_str(), nullptr, &stored_size) == ESP_OK &&
            stored_size == required_size) {
            success = (nvs_get_blob(handle, keyPrefix.c_str(), data.data(), &stored_size) == ESP_OK);
        }
    }
    
    xSemaphoreGive(mutex);
    return success;
}

template<>
bool Parameter<float>::saveToNvs(nvs_handle_t handle, const std::string& keyPrefix) {
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    
    // Always save as blob (NVS doesn't have native float support)
    bool success = (nvs_set_blob(handle, keyPrefix.c_str(), data.data(), data.size() * sizeof(float)) == ESP_OK);
    
    xSemaphoreGive(mutex);
    return success;
}

template<>
bool Parameter<float>::loadFromNvs(nvs_handle_t handle, const std::string& keyPrefix) {
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    
    bool success = false;
    size_t required_size = data.size() * sizeof(float);
    size_t stored_size = 0;
    if (nvs_get_blob(handle, keyPrefix.c_str(), nullptr, &stored_size) == ESP_OK &&
        stored_size == required_size) {
        success = (nvs_get_blob(handle, keyPrefix.c_str(), data.data(), &stored_size) == ESP_OK);
    }
    
    xSemaphoreGive(mutex);
    return success;
}

template<>
bool Parameter<uint8_t>::saveToNvs(nvs_handle_t handle, const std::string& keyPrefix) {
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    
    bool success = false;
    if (rows == 1 && cols == 1) {
        // Scalar bool - save as u8
        success = (nvs_set_u8(handle, keyPrefix.c_str(), data[0]) == ESP_OK);
    } else {
        // Array - save as blob
        success = (nvs_set_blob(handle, keyPrefix.c_str(), data.data(), data.size()) == ESP_OK);
    }
    
    xSemaphoreGive(mutex);
    return success;
}

template<>
bool Parameter<uint8_t>::loadFromNvs(nvs_handle_t handle, const std::string& keyPrefix) {
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    
    bool success = false;
    if (rows == 1 && cols == 1) {
        // Scalar bool - load as u8
        uint8_t val;
        if (nvs_get_u8(handle, keyPrefix.c_str(), &val) == ESP_OK) {
            data[0] = val;
            success = true;
        }
    } else {
        // Array - load as blob
        size_t required_size = data.size();
        size_t stored_size = 0;
        if (nvs_get_blob(handle, keyPrefix.c_str(), nullptr, &stored_size) == ESP_OK &&
            stored_size == required_size) {
            success = (nvs_get_blob(handle, keyPrefix.c_str(), data.data(), &stored_size) == ESP_OK);
        }
    }
    
    xSemaphoreGive(mutex);
    return success;
}

template<>
bool Parameter<std::string>::saveToNvs(nvs_handle_t handle, const std::string& keyPrefix) {
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    
    bool success = false;
    if (rows == 1 && cols == 1) {
        // Single string - save directly
        success = (nvs_set_str(handle, keyPrefix.c_str(), data[0].c_str()) == ESP_OK);
    } else {
        // Multiple strings - save as JSON array blob
        cJSON* arr = cJSON_CreateArray();
        for (const auto& s : data) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(s.c_str()));
        }
        char* json = cJSON_PrintUnformatted(arr);
        if (json) {
            success = (nvs_set_str(handle, keyPrefix.c_str(), json) == ESP_OK);
            cJSON_free(json);
        }
        cJSON_Delete(arr);
    }
    
    xSemaphoreGive(mutex);
    return success;
}

template<>
bool Parameter<std::string>::loadFromNvs(nvs_handle_t handle, const std::string& keyPrefix) {
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
    
    bool success = false;
    size_t str_len = 0;
    
    if (nvs_get_str(handle, keyPrefix.c_str(), nullptr, &str_len) == ESP_OK && str_len > 0) {
        char* buf = (char*)malloc(str_len);
        if (buf && nvs_get_str(handle, keyPrefix.c_str(), buf, &str_len) == ESP_OK) {
            if (rows == 1 && cols == 1) {
                // Single string
                data[0] = buf;
                success = true;
            } else {
                // Multiple strings - parse JSON array
                cJSON* arr = cJSON_Parse(buf);
                if (arr && cJSON_IsArray(arr)) {
                    size_t idx = 0;
                    cJSON* item;
                    cJSON_ArrayForEach(item, arr) {
                        if (idx < data.size() && cJSON_IsString(item)) {
                            data[idx] = item->valuestring;
                            idx++;
                        }
                    }
                    success = true;
                }
                cJSON_Delete(arr);
            }
        }
        free(buf);
    }
    
    xSemaphoreGive(mutex);
    return success;
}
