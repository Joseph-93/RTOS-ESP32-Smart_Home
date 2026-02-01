#include "component.h"

// Component class implementations

Component::Component(const std::string &name) : name(name), initialized(false) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER/EXIT] Component() - name: %s", name.c_str());
#endif
}

Component::~Component() {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER/EXIT] ~Component() - name: %s", name.c_str());
#endif
}

void Component::initialize() {
    ESP_LOGI(TAG, "Initializing component: %s", name.c_str());
    onInitialize();
    initialized = true;
    ESP_LOGI(TAG, "Component %s initialized successfully", name.c_str());
}

std::string Component::getName() const {
#ifdef DEBUG
    // Log with backtrace to see caller
    void* trace[10];
    int trace_size = 0;
    ESP_LOGI("Component", "getName() called for '%s' from task '%s'", 
             name.c_str(), pcTaskGetName(NULL));
#endif
    return name;
}

bool Component::isInitialized() const {
    return initialized;
}

// Const getters
const std::vector<std::unique_ptr<IntParameter>>& Component::getIntParams() const { return intParams; }
const std::vector<std::unique_ptr<FloatParameter>>& Component::getFloatParams() const { return floatParams; }
const std::vector<std::unique_ptr<BoolParameter>>& Component::getBoolParams() const { return boolParams; }
const std::vector<std::unique_ptr<StringParameter>>& Component::getStringParams() const { return stringParams; }
const std::vector<std::unique_ptr<TriggerParameter>>& Component::getTriggerParams() const { return triggerParams; }

// Non-const getters
std::vector<std::unique_ptr<IntParameter>>& Component::getIntParams() { return intParams; }
std::vector<std::unique_ptr<FloatParameter>>& Component::getFloatParams() { return floatParams; }
std::vector<std::unique_ptr<BoolParameter>>& Component::getBoolParams() { return boolParams; }
std::vector<std::unique_ptr<StringParameter>>& Component::getStringParams() { return stringParams; }
std::vector<std::unique_ptr<TriggerParameter>>& Component::getTriggerParams() { return triggerParams; }

// Get methods
IntParameter* Component::getIntParam(const std::string &paramName) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] getIntParam() - paramName: %s", paramName.c_str());
#endif
    for (auto &p : intParams) {
        if (p->getName() == paramName) {
#ifdef DEBUG
            ESP_LOGI("Component", "[EXIT] getIntParam() - found");
#endif
            return p.get();
        }
    }
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] getIntParam() - not found");
#endif
    return nullptr;
}

IntParameter* Component::getIntParam(int paramId) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] getIntParam() - paramId: %d", paramId);
#endif
    if (paramId < 0 || paramId >= static_cast<int>(intParams.size())) {
        ESP_LOGE("Component", "Int parameter ID %d out of range (max: %zu) in component '%s'", 
                 paramId, intParams.size(), this->name.c_str());
#ifdef DEBUG
        ESP_LOGI("Component", "[EXIT] getIntParam() - out of range");
#endif
        return nullptr;
    }
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] getIntParam() - found");
#endif
    return intParams[paramId].get();
}

FloatParameter* Component::getFloatParam(const std::string &paramName) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] getFloatParam() - paramName: %s", paramName.c_str());
#endif
    for (auto &p : floatParams) {
        if (p->getName() == paramName) {
#ifdef DEBUG
            ESP_LOGI("Component", "[EXIT] getFloatParam() - found");
#endif
            return p.get();
        }
    }
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] getFloatParam() - not found");
#endif
    return nullptr;
}

FloatParameter* Component::getFloatParam(int paramId) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] getFloatParam() - paramId: %d", paramId);
#endif
    if (paramId < 0 || paramId >= static_cast<int>(floatParams.size())) {
        ESP_LOGE("Component", "Float parameter ID %d out of range (max: %zu) in component '%s'", 
                 paramId, floatParams.size(), this->name.c_str());
#ifdef DEBUG
        ESP_LOGI("Component", "[EXIT] getFloatParam() - out of range");
#endif
        return nullptr;
    }
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] getFloatParam() - found");
#endif
    return floatParams[paramId].get();
}

BoolParameter* Component::getBoolParam(const std::string &paramName) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] getBoolParam() - paramName: %s", paramName.c_str());
#endif
    for (auto &p : boolParams) {
        if (p->getName() == paramName) {
#ifdef DEBUG
            ESP_LOGI("Component", "[EXIT] getBoolParam() - found");
#endif
            return p.get();
        }
    }
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] getBoolParam() - not found");
#endif
    return nullptr;
}

BoolParameter* Component::getBoolParam(int paramId) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] getBoolParam() - paramId: %d", paramId);
#endif
    if (paramId < 0 || paramId >= static_cast<int>(boolParams.size())) {
        ESP_LOGE("Component", "Bool parameter ID %d out of range (max: %zu) in component '%s'", 
                 paramId, boolParams.size(), this->name.c_str());
#ifdef DEBUG
        ESP_LOGI("Component", "[EXIT] getBoolParam() - out of range");
#endif
        return nullptr;
    }
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] getBoolParam() - found");
#endif
    return boolParams[paramId].get();
}

StringParameter* Component::getStringParam(const std::string &paramName) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] getStringParam() - paramName: %s", paramName.c_str());
#endif
    for (auto &p : stringParams) {
        if (p->getName() == paramName) {
#ifdef DEBUG
            ESP_LOGI("Component", "[EXIT] getStringParam() - found");
#endif
            return p.get();
        }
    }
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] getStringParam() - not found");
#endif
    return nullptr;
}

StringParameter* Component::getStringParam(int paramId) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] getStringParam() - paramId: %d", paramId);
#endif
    if (paramId < 0 || paramId >= static_cast<int>(stringParams.size())) {
        ESP_LOGE("Component", "String parameter ID %d out of range (max: %zu) in component '%s'", 
                 paramId, stringParams.size(), this->name.c_str());
#ifdef DEBUG
        ESP_LOGI("Component", "[EXIT] getStringParam() - out of range");
#endif
        return nullptr;
    }
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] getStringParam() - found");
#endif
    return stringParams[paramId].get();
}

TriggerParameter* Component::getTriggerParam(const std::string &paramName) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] getTriggerParam() - paramName: %s", paramName.c_str());
#endif
    for (auto &p : triggerParams) {
        if (p->getName() == paramName) {
#ifdef DEBUG
            ESP_LOGI("Component", "[EXIT] getTriggerParam() - found");
#endif
            return p.get();
        }
    }
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] getTriggerParam() - not found");
#endif
    return nullptr;
}

TriggerParameter* Component::getTriggerParam(int paramId) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] getTriggerParam() - paramId: %d", paramId);
#endif
    if (paramId < 0 || paramId >= static_cast<int>(triggerParams.size())) {
        ESP_LOGE("Component", "Trigger parameter ID %d out of range (max: %zu) in component '%s'", 
                 paramId, triggerParams.size(), this->name.c_str());
#ifdef DEBUG
        ESP_LOGI("Component", "[EXIT] getTriggerParam() - out of range");
#endif
        return nullptr;
    }
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] getTriggerParam() - found");
#endif
    return triggerParams[paramId].get();
}

// Protected add methods
void Component::addIntParam(const std::string &paramName, size_t rows, size_t cols, int min_val, int max_val, int default_val, bool readOnly) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addIntParam() - paramName: %s", paramName.c_str());
#endif
    intParams.push_back(std::make_unique<IntParameter>(paramName, rows, cols, min_val, max_val, default_val, readOnly));
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addIntParam() - paramName: %s", paramName.c_str());
#endif
}

void Component::addFloatParam(const std::string &paramName, size_t rows, size_t cols, float min_val, float max_val, float default_val, bool readOnly) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addFloatParam() - paramName: %s", paramName.c_str());
#endif
    floatParams.push_back(std::make_unique<FloatParameter>(paramName, rows, cols, min_val, max_val, default_val, readOnly));
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addFloatParam() - paramName: %s", paramName.c_str());
#endif
}

void Component::addBoolParam(const std::string &paramName, size_t rows, size_t cols, bool default_val, bool readOnly) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addBoolParam() - paramName: %s", paramName.c_str());
#endif
    // BoolParameter is Parameter<uint8_t>, so we need to pass min, max, and default as uint8_t
    boolParams.push_back(std::make_unique<BoolParameter>(paramName, rows, cols, 0, 1, default_val ? 1 : 0, readOnly));
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addBoolParam() - paramName: %s", paramName.c_str());
#endif
}

void Component::addStringParam(const std::string &paramName, size_t rows, size_t cols, const std::string &default_val, bool readOnly) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addStringParam() - paramName: %s", paramName.c_str());
#endif
    stringParams.push_back(std::make_unique<StringParameter>(paramName, rows, cols, default_val, default_val, default_val, readOnly));
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addStringParam() - paramName: %s", paramName.c_str());
#endif
}

void Component::addTriggerParam(const std::string &paramName, size_t rows, size_t cols,
                               const std::string &default_val,
                               std::function<void(Component*, size_t, size_t, const std::string&)> callback,
                               bool readOnly) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addTriggerParam() - paramName: %s", paramName.c_str());
#endif
    auto trigger = std::make_unique<TriggerParameter>(paramName, rows, cols, default_val, callback, readOnly);
    trigger->setOwner(this);
    triggerParams.push_back(std::move(trigger));
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addTriggerParam() - paramName: %s", paramName.c_str());
#endif
}

// Action management section removed - replaced by TriggerParameter system

size_t Component::getApproximateMemoryUsage() const {
    size_t total = 0;
    
    // Component name and basic overhead
    total += sizeof(Component);
    total += name.capacity();
    
    // Int parameters
    for (const auto& param : intParams) {
        total += sizeof(IntParameter);
        total += param->getName().capacity();
        total += param->getRows() * param->getCols() * sizeof(int32_t);
    }
    
    // Float parameters
    for (const auto& param : floatParams) {
        total += sizeof(FloatParameter);
        total += param->getName().capacity();
        total += param->getRows() * param->getCols() * sizeof(float);
    }
    
    // Bool parameters
    for (const auto& param : boolParams) {
        total += sizeof(BoolParameter);
        total += param->getName().capacity();
        total += param->getRows() * param->getCols() * sizeof(uint8_t);
    }
    
    // String parameters
    for (const auto& param : stringParams) {
        total += sizeof(StringParameter);
        total += param->getName().capacity();
        // Estimate average string size (rough)
        total += param->getRows() * param->getCols() * 32;  // Assume avg 32 bytes per string
    }
    
    // Trigger parameters (similar to string parameters)
    for (const auto& param : triggerParams) {
        total += sizeof(TriggerParameter);
        total += param->getName().capacity();
        // Estimate average string size (rough)
        total += param->getRows() * param->getCols() * 32;  // Assume avg 32 bytes per string
    }
    
    return total;
}