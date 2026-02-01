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
    
    ESP_LOGI(TAG, "Component '%s' created with id=%u", name.c_str(), componentId);
}

Component::~Component() {
    ESP_LOGI(TAG, "Component '%s' (id=%u) destroyed", name.c_str(), componentId);
    
    if (paramsMutex != nullptr) {
        vSemaphoreDelete(paramsMutex);
    }
}

void Component::initialize() {
    ESP_LOGI(TAG, "Initializing component: %s (id=%u)", name.c_str(), componentId);
    onInitialize();
    initialized = true;
    ESP_LOGI(TAG, "Component %s initialized successfully", name.c_str());
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
    
    ESP_LOGI(TAG, "Added int param '%s' (id=%u) to component '%s'", paramName.c_str(), paramId, name.c_str());
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
    
    ESP_LOGI(TAG, "Added float param '%s' (id=%u) to component '%s'", paramName.c_str(), paramId, name.c_str());
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
    
    ESP_LOGI(TAG, "Added bool param '%s' (id=%u) to component '%s'", paramName.c_str(), paramId, name.c_str());
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
    
    ESP_LOGI(TAG, "Added string param '%s' (id=%u) to component '%s'", paramName.c_str(), paramId, name.c_str());
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
