#pragma once

#include "component.h"
#include <map>
#include <string>
#include <vector>

#ifdef __cplusplus

class ComponentGraph {
public:
    ComponentGraph();
    ~ComponentGraph();
    
    // Register a component with the graph
    void registerComponent(Component* component);
    
    // Get a component by name
    Component* getComponent(const std::string& name);
    
    // Get a parameter from any component by component name + parameter name
    IntParameter* getIntParam(const std::string& component_name, const std::string& param_name);
    FloatParameter* getFloatParam(const std::string& component_name, const std::string& param_name);
    BoolParameter* getBoolParam(const std::string& component_name, const std::string& param_name);
    StringParameter* getStringParam(const std::string& component_name, const std::string& param_name);
    
    // Initialize all registered components in dependency order
    void initializeAll();
    
    // Get all component names (useful for debugging)
    std::vector<std::string> getComponentNames() const;
    
    // Check if a component exists
    bool hasComponent(const std::string& name) const;

    static constexpr const char* TAG = "ComponentGraph";

private:
    std::map<std::string, Component*> components;
};

// Global component graph instance (declared here, defined in component_graph.cpp)
extern ComponentGraph* g_component_graph;

#endif
