#include "component_graph.h"
#include "esp_log.h"

// Global component graph instance
ComponentGraph* g_component_graph = nullptr;

ComponentGraph::ComponentGraph() {
    ESP_LOGI(TAG, "ComponentGraph created");
}

ComponentGraph::~ComponentGraph() {
    ESP_LOGI(TAG, "ComponentGraph destroyed");
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
    ESP_LOGI(TAG, "Setting up dependencies for all components (%zu total)...", components.size());
    
    // First pass: set up dependencies
    for (auto& pair : components) {
        ESP_LOGI(TAG, "Setting up dependencies for: %s", pair.first.c_str());
        pair.second->setUpDependencies();
    }
    
    ESP_LOGI(TAG, "Initializing all components...");
    
    // Second pass: initialize
    for (auto& pair : components) {
        ESP_LOGI(TAG, "Initializing component: %s", pair.first.c_str());
        pair.second->initialize();
    }
    
    ESP_LOGI(TAG, "All components initialized successfully");
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
