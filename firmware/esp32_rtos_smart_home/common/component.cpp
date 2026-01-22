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

// Non-const getters
std::vector<std::unique_ptr<IntParameter>>& Component::getIntParams() { return intParams; }
std::vector<std::unique_ptr<FloatParameter>>& Component::getFloatParams() { return floatParams; }
std::vector<std::unique_ptr<BoolParameter>>& Component::getBoolParams() { return boolParams; }
std::vector<std::unique_ptr<StringParameter>>& Component::getStringParams() { return stringParams; }

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

// Action management

const std::vector<ComponentAction>& Component::getActions() const {
    return actions;
}

const std::vector<std::string> Component::getActionNames() const {
    return actionNames;  // Return the separate names vector, not touching ComponentAction objects
}

void Component::addAction(const std::string& name, const std::string& description,
                         std::function<bool(Component*, const std::string&)> callback) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addAction() - name: %s, desc: %s", name.c_str(), description.c_str());
#endif
    actions.emplace_back(name, description, callback);
    actionNames.push_back(name);  // Store name separately to avoid std::function corruption
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addAction() - name: %s, desc: %s", name.c_str(), description.c_str());
#endif
}

const ComponentAction& Component::getAction(int actionId) const {
    if (actionId < 0 || actionId >= static_cast<int>(actions.size())) {
        ESP_LOGE("Component", "Action ID %d out of range (max: %zu) in component '%s'", 
                 actionId, actions.size(), this->name.c_str());
        static ComponentAction dummy("", "", nullptr);
        return dummy;
    }
    return actions[actionId];
}

const ComponentAction& Component::getAction(const std::string &actionName) const {
    for (const auto& action : actions) {
        if (action.name == actionName) {
            return action;
        }
    }
    ESP_LOGE("Component", "Action '%s' not found in component '%s'", 
             actionName.c_str(), this->name.c_str());
    static ComponentAction dummy("", "", nullptr);
    return dummy;
}

const std::string Component::getActionName(int actionId) const {
    if (actionId < 0 || actionId >= static_cast<int>(actionNames.size())) {
        ESP_LOGE("Component", "Action ID %d out of range (max: %zu) in component '%s'", 
                 actionId, actionNames.size(), this->name.c_str());
        return "";
    }
    return actionNames[actionId];
}

const std::string Component::getActionName(const std::string &actionName) const {
    // This returns the name if it exists, empty string otherwise
    for (const auto& name : actionNames) {
        if (name == actionName) {
            return name;
        }
    }
    return "";
}

const std::vector<std::string> Component::getActionDescriptions() const {
    std::vector<std::string> descriptions;
    descriptions.reserve(actions.size());
    for (const auto& action : actions) {
        descriptions.push_back(action.description);
    }
    return descriptions;
}

const std::string Component::getActionDescription(int actionId) const {
    if (actionId < 0 || actionId >= static_cast<int>(actions.size())) {
        ESP_LOGE("Component", "Action ID %d out of range (max: %zu) in component '%s'", 
                 actionId, actions.size(), this->name.c_str());
        return "";
    }
    return actions[actionId].description;
}

const std::string Component::getActionDescription(const std::string &actionName) const {
    for (const auto& action : actions) {
        if (action.name == actionName) {
            return action.description;
        }
    }
    ESP_LOGE("Component", "Action '%s' not found in component '%s'", 
             actionName.c_str(), this->name.c_str());
    return "";
}

void Component::invokeAction(size_t actionIndex) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] invokeAction() - actionIndex: %zu", actionIndex);
#endif
    if (actionIndex >= actions.size()) {
        ESP_LOGE("Component", "Action index %zu out of range (max: %zu) in component '%s'", 
                 actionIndex, actions.size(), this->name.c_str());
#ifdef DEBUG
        ESP_LOGI("Component", "[EXIT] invokeAction() - index out of range");
#endif
        return;
    }
    
    auto& action = actions[actionIndex];
    ESP_LOGI("Component", "Invoking action[%zu] '%s' on component '%s' with arg: '%s'", 
             actionIndex, action.name.c_str(), this->name.c_str(), action.argument.c_str());
    bool success = action.callback(this, action.argument);
    ESP_LOGI("Component", "Action[%zu] '%s' %s", actionIndex, action.name.c_str(), 
             success ? "succeeded" : "failed");
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] invokeAction() - success");
#endif
}

void Component::invokeAction(const std::string &actionName) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] invokeAction() - actionName: %s", actionName.c_str());
#endif
    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].name == actionName) {
            invokeAction(i);
#ifdef DEBUG
            ESP_LOGI("Component", "[EXIT] invokeAction() - found and invoked");
#endif
            return;
        }
    }
    ESP_LOGE("Component", "Action '%s' not found in component '%s'", 
             actionName.c_str(), this->name.c_str());
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] invokeAction() - not found");
#endif
}

void Component::invokeAction(size_t actionIndex, const std::string& arg) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] invokeAction() - actionIndex: %zu, arg: %s", actionIndex, arg.c_str());
#endif
    if (actionIndex >= actions.size()) {
        ESP_LOGE("Component", "Action index %zu out of range (max: %zu) in component '%s'", 
                 actionIndex, actions.size(), this->name.c_str());
#ifdef DEBUG
        ESP_LOGI("Component", "[EXIT] invokeAction() - index out of range");
#endif
        return;
    }
    
    auto& action = actions[actionIndex];
    ESP_LOGI("Component", "Invoking action[%zu] '%s' on component '%s' with temporary arg: '%s'", 
             actionIndex, action.name.c_str(), this->name.c_str(), arg.c_str());
    bool success = action.callback(this, arg);
    ESP_LOGI("Component", "Action[%zu] '%s' %s", actionIndex, action.name.c_str(), 
             success ? "succeeded" : "failed");
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] invokeAction() - success");
#endif
}

void Component::invokeAction(const std::string &actionName, const std::string& arg) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] invokeAction() - actionName: %s, arg: %s", actionName.c_str(), arg.c_str());
#endif
    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].name == actionName) {
            invokeAction(i, arg);
#ifdef DEBUG
            ESP_LOGI("Component", "[EXIT] invokeAction() - found and invoked");
#endif
            return;
        }
    }
    ESP_LOGE("Component", "Action '%s' not found in component '%s'", 
             actionName.c_str(), this->name.c_str());
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] invokeAction() - not found");
#endif
}

void Component::setActionArgument(const std::string &actionName, const std::string& arg) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] setActionArgument() - actionName: %s, arg: %s", actionName.c_str(), arg.c_str());
#endif
    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].name == actionName) {
            actions[i].argument = arg;
            ESP_LOGI("Component", "Set argument for action '%s' to: '%s'", actionName.c_str(), arg.c_str());
#ifdef DEBUG
            ESP_LOGI("Component", "[EXIT] setActionArgument() - success");
#endif
            return;
        }
    }
    ESP_LOGE("Component", "Action '%s' not found in component '%s'", 
             actionName.c_str(), this->name.c_str());
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] setActionArgument() - not found");
#endif
}

void Component::setActionArgument(size_t actionIndex, const std::string& arg) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] setActionArgument() - actionIndex: %zu, arg: %s", actionIndex, arg.c_str());
#endif
    if (actionIndex >= actions.size()) {
        ESP_LOGE("Component", "Action index %zu out of range (max: %zu) in component '%s'", 
                 actionIndex, actions.size(), this->name.c_str());
#ifdef DEBUG
        ESP_LOGI("Component", "[EXIT] setActionArgument() - index out of range");
#endif
        return;
    }
    
    actions[actionIndex].argument = arg;
    ESP_LOGI("Component", "Set argument for action[%zu] '%s' to: '%s'", 
             actionIndex, actions[actionIndex].name.c_str(), arg.c_str());
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] setActionArgument() - success");
#endif
}

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
    
    // Actions
    total += actions.capacity() * sizeof(ComponentAction);
    for (const auto& action : actions) {
        total += action.name.capacity();
        total += action.description.capacity();
    }
    
    return total;
}