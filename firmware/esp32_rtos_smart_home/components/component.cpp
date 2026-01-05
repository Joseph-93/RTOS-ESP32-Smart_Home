#include "component.h"

// Component class implementations

Component::Component(const std::string &name) : name(name), initialized(false) {}

Component::~Component() {}

std::string Component::getName() const { return name; }

bool Component::isInitialized() const { return initialized; }

// Const getters
const std::vector<IntParameter>& Component::getIntParams() const { return intParams; }
const std::vector<FloatParameter>& Component::getFloatParams() const { return floatParams; }
const std::vector<BoolParameter>& Component::getBoolParams() const { return boolParams; }
const std::vector<StringParameter>& Component::getStringParams() const { return stringParams; }

// Non-const getters
std::vector<IntParameter>& Component::getIntParams() { return intParams; }
std::vector<FloatParameter>& Component::getFloatParams() { return floatParams; }
std::vector<BoolParameter>& Component::getBoolParams() { return boolParams; }
std::vector<StringParameter>& Component::getStringParams() { return stringParams; }

// Get methods
IntParameter* Component::getIntParam(const std::string &paramName) {
    for (auto &p : intParams) {
        if (p.getName() == paramName) return &p;
    }
    return nullptr;
}

FloatParameter* Component::getFloatParam(const std::string &paramName) {
    for (auto &p : floatParams) {
        if (p.getName() == paramName) return &p;
    }
    return nullptr;
}

BoolParameter* Component::getBoolParam(const std::string &paramName) {
    for (auto &p : boolParams) {
        if (p.getName() == paramName) return &p;
    }
    return nullptr;
}

StringParameter* Component::getStringParam(const std::string &paramName) {
    for (auto &p : stringParams) {
        if (p.getName() == paramName) return &p;
    }
    return nullptr;
}

// Protected add methods
void Component::addIntParam(const std::string &paramName, size_t rows, size_t cols) {
    intParams.emplace_back(paramName, rows, cols);
}

void Component::addFloatParam(const std::string &paramName, size_t rows, size_t cols) {
    floatParams.emplace_back(paramName, rows, cols);
}

void Component::addBoolParam(const std::string &paramName, size_t rows, size_t cols) {
    boolParams.emplace_back(paramName, rows, cols);
}

void Component::addStringParam(const std::string &paramName, size_t rows, size_t cols) {
    stringParams.emplace_back(paramName, rows, cols);
}