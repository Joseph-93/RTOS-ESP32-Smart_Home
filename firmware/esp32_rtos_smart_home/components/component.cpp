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

std::string Component::getName() const {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER/EXIT] getName() - returning: %s", name.c_str());
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

// Protected add methods
void Component::addIntParam(const std::string &paramName, size_t rows, size_t cols, int min_val, int max_val, int default_val) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addIntParam() - paramName: %s", paramName.c_str());
#endif
    intParams.push_back(std::make_unique<IntParameter>(paramName, rows, cols, min_val, max_val, default_val));
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addIntParam() - paramName: %s", paramName.c_str());
#endif
}

void Component::addFloatParam(const std::string &paramName, size_t rows, size_t cols, float min_val, float max_val, float default_val) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addFloatParam() - paramName: %s", paramName.c_str());
#endif
    floatParams.push_back(std::make_unique<FloatParameter>(paramName, rows, cols, min_val, max_val, default_val));
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addFloatParam() - paramName: %s", paramName.c_str());
#endif
}

void Component::addBoolParam(const std::string &paramName, size_t rows, size_t cols, bool default_val) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addBoolParam() - paramName: %s", paramName.c_str());
#endif
    // BoolParameter is Parameter<uint8_t>, so we need to pass min, max, and default as uint8_t
    boolParams.push_back(std::make_unique<BoolParameter>(paramName, rows, cols, 0, 1, default_val ? 1 : 0));
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addBoolParam() - paramName: %s", paramName.c_str());
#endif
}

void Component::addStringParam(const std::string &paramName, size_t rows, size_t cols, const std::string &default_val) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addStringParam() - paramName: %s", paramName.c_str());
#endif
    stringParams.push_back(std::make_unique<StringParameter>(paramName, rows, cols, default_val));
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addStringParam() - paramName: %s", paramName.c_str());
#endif
}

// Action management

const std::vector<ComponentAction>& Component::getActions() const {
    return actions;
}

void Component::addAction(const std::string& name, const std::string& description,
                         std::function<bool(Component*)> callback) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] addAction() - name: %s, desc: %s", name.c_str(), description.c_str());
#endif
    actions.emplace_back(name, description, callback);
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] addAction() - name: %s, desc: %s", name.c_str(), description.c_str());
#endif
}

void Component::invokeAction(const std::string& actionName) {
#ifdef DEBUG
    ESP_LOGI("Component", "[ENTER] invokeAction() - actionName: %s", actionName.c_str());
#endif
    for (auto& action : actions) {
        if (action.name == actionName) {
            ESP_LOGI("Component", "Invoking action '%s' on component '%s'", 
                     actionName.c_str(), this->name.c_str());
            bool success = action.callback(this);
            ESP_LOGI("Component", "Action '%s' %s", actionName.c_str(), 
                     success ? "succeeded" : "failed");
            return;
        }
    }
    ESP_LOGE("Component", "Action '%s' not found in component '%s'", 
             actionName.c_str(), this->name.c_str());
#ifdef DEBUG
    ESP_LOGI("Component", "[EXIT] invokeAction() - action not found");
#endif
}