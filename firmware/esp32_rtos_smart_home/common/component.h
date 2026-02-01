#pragma once

#include <string>
#include <unordered_map>
#include <functional>
#include <memory>
#include <climits>
#include <cfloat>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include <cassert>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

// Forward declarations
class ComponentGraph;
class Component;

// ============================================================================
// ParameterType Enum
// ============================================================================

enum class ParameterType : uint8_t {
    INT = 0,
    FLOAT = 1,
    BOOL = 2,
    STRING = 3
};

// Helper to get type string for JSON serialization
inline const char* parameterTypeToString(ParameterType type) {
    switch (type) {
        case ParameterType::INT: return "int";
        case ParameterType::FLOAT: return "float";
        case ParameterType::BOOL: return "bool";
        case ParameterType::STRING: return "str";
        default: return "unknown";
    }
}

// ============================================================================
// BaseParameter - Abstract base class for all parameters
// ============================================================================

class BaseParameter {
public:
    BaseParameter(const std::string& name, uint32_t id, bool readOnly = false)
        : name(name), parameterId(id), read_only(readOnly) {}
    
    virtual ~BaseParameter() = default;
    
    // Delete copy and move
    BaseParameter(const BaseParameter&) = delete;
    BaseParameter& operator=(const BaseParameter&) = delete;
    BaseParameter(BaseParameter&&) = delete;
    BaseParameter& operator=(BaseParameter&&) = delete;
    
    // Identity
    uint32_t getParameterId() const { return parameterId; }
    const std::string& getName() const { return name; }
    
    // Type info
    virtual ParameterType getType() const = 0;
    const char* getTypeString() const { return parameterTypeToString(getType()); }
    
    // Dimensions
    virtual size_t getRows() const = 0;
    virtual size_t getCols() const = 0;
    
    // Access control
    bool isReadOnly() const { return read_only; }
    
    // Generic JSON access for WebSocket API
    virtual cJSON* getValueAsJson(size_t row, size_t col) const = 0;
    virtual bool setValueFromJson(size_t row, size_t col, cJSON* value) = 0;
    
    // Info as JSON (for discovery)
    virtual cJSON* getInfoAsJson() const {
        cJSON* info = cJSON_CreateObject();
        cJSON_AddStringToObject(info, "name", name.c_str());
        cJSON_AddNumberToObject(info, "id", parameterId);
        cJSON_AddStringToObject(info, "type", getTypeString());
        cJSON_AddNumberToObject(info, "rows", getRows());
        cJSON_AddNumberToObject(info, "cols", getCols());
        cJSON_AddBoolToObject(info, "readOnly", read_only);
        return info;
    }

protected:
    std::string name;
    uint32_t parameterId;
    bool read_only;
    
    static constexpr const char* TAG = "BaseParameter";
};

// ============================================================================
// Parameter<T> - Template class for typed parameters
// ============================================================================

template<typename T>
class Parameter : public BaseParameter {
public:
    Parameter(const std::string &name, uint32_t id, size_t rows, size_t cols, 
              T min_val = T(), T max_val = T(), T default_val = T(), bool read_only = false)
        : BaseParameter(name, id, read_only), 
          rows(rows), cols(cols), min_value(min_val), max_value(max_val) {
        
        size_t total_elements = rows * cols;
        size_t total_bytes = total_elements * sizeof(T);
        size_t free_heap = esp_get_free_heap_size();
        
        if (total_bytes > free_heap / 2) {
            ESP_LOGE(TAG, "Parameter '%s': Allocation too large! Requested %zu bytes, only %zu free", 
                     name.c_str(), total_bytes, free_heap);
            assert(false && "Parameter allocation would exhaust heap");
        }
        
        data.resize(total_elements);
        
        // Create mutex for thread-safe access
        mutex = xSemaphoreCreateMutex();
        if (mutex == nullptr) {
            ESP_LOGE(TAG, "Parameter '%s': Failed to create mutex!", name.c_str());
            assert(false && "Mutex creation failed");
        }

        // Initialize data with default value
        for (size_t i = 0; i < total_elements; ++i) {
            data[i] = default_val;
        }
        
        ESP_LOGI(TAG, "Parameter '%s' (id=%u) allocated: %zux%zu (%zu bytes)", 
                 name.c_str(), id, rows, cols, total_bytes);
    }
    
    ~Parameter() override {
        if (mutex != nullptr) {
            vSemaphoreDelete(mutex);
        }
    }
    
    // Type info
    ParameterType getType() const override;  // Specialized below
    
    // Dimensions (can only grow, never shrink)
    size_t getRows() const override { 
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return 0;
        size_t r = rows;
        xSemaphoreGive(mutex);
        return r;
    }
    
    size_t getCols() const override { 
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return 0;
        size_t c = cols;
        xSemaphoreGive(mutex);
        return c;
    }
    
    // Value access
    T getValue(size_t row, size_t col) const { 
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Parameter '%s': Failed to take mutex for getValue", name.c_str());
            assert(false && "Mutex take failed");
        }
        
        if (row >= rows || col >= cols) {
            xSemaphoreGive(mutex);
            ESP_LOGE(TAG, "Parameter '%s': Out of bounds access [%zu,%zu] (size: %zux%zu)", 
                     name.c_str(), row, col, rows, cols);
            assert(false && "Parameter getValue out of bounds");
        }
        
        T value = data[row * cols + col];
        xSemaphoreGive(mutex);
        return value;
    }
    
    void setValue(size_t row, size_t col, T val) {
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Parameter '%s': Failed to take mutex for setValue", name.c_str());
            assert(false && "Mutex take failed");
        }
        
        if (row >= rows || col >= cols) {
            xSemaphoreGive(mutex);
            ESP_LOGE(TAG, "Parameter '%s': Out of bounds write [%zu,%zu] (size: %zux%zu)", 
                     name.c_str(), row, col, rows, cols);
            assert(false && "Parameter setValue out of bounds");
        }
        
        // Check if value actually changed
        T& current = data[row * cols + col];
        bool changed = (current != val);
        if (changed) {
            current = val;
        }
        xSemaphoreGive(mutex);
        
        // Only invoke callback if value actually changed
        if (changed && has_callback) {
            onChange(row, col, val);
        }
    }
    
    // Set value without triggering onChange callback (for internal updates)
    void setValueQuiet(size_t row, size_t col, T val) {
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Parameter '%s': Failed to take mutex for setValueQuiet", name.c_str());
            return;
        }
        
        if (row >= rows || col >= cols) {
            xSemaphoreGive(mutex);
            ESP_LOGE(TAG, "Parameter '%s': Out of bounds write [%zu,%zu] (size: %zux%zu)", 
                     name.c_str(), row, col, rows, cols);
            return;
        }
        
        data[row * cols + col] = val;
        xSemaphoreGive(mutex);
        // Note: Does NOT invoke onChange callback
    }
    
    T getMin() const { return min_value; }
    T getMax() const { return max_value; }
    
    // Append a new value (grows the parameter by one row, assumes single column)
    void appendValue(const T& value) {
        size_t new_row = 0;
        
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Parameter '%s': Failed to take mutex for appendValue", name.c_str());
            assert(false && "Mutex take failed");
        }
        
        data.push_back(value);
        new_row = rows;
        rows = data.size() / cols;
        xSemaphoreGive(mutex);
        
        // Invoke callback after releasing lock
        if (has_callback) {
            onChange(new_row, 0, value);
        }
    }
    
    // Callback management
    void setOnChange(std::function<void(size_t, size_t, T)> callback) {
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Parameter '%s': Failed to take mutex for setOnChange", name.c_str());
            return;
        }
        onChange = callback;
        has_callback = true;
        xSemaphoreGive(mutex);
    }
    
    bool hasCallback() const {
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
        bool result = has_callback;
        xSemaphoreGive(mutex);
        return result;
    }
    
    std::function<void(size_t, size_t, T)> getOnChange() const {
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return nullptr;
        auto callback = onChange;
        xSemaphoreGive(mutex);
        return callback;
    }
    
    // Region access
    std::vector<T> getRegion(size_t startRow, size_t startCol, size_t numRows, size_t numCols) const {
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Parameter '%s': Failed to take mutex for getRegion", name.c_str());
            return std::vector<T>();
        }
        
        if (startRow + numRows > rows || startCol + numCols > cols) {
            xSemaphoreGive(mutex);
            ESP_LOGE(TAG, "Parameter '%s': Region out of bounds", name.c_str());
            return std::vector<T>();
        }
        
        std::vector<T> result;
        result.reserve(numRows * numCols);
        for (size_t r = 0; r < numRows; r++) {
            for (size_t c = 0; c < numCols; c++) {
                result.push_back(data[(startRow + r) * cols + startCol + c]);
            }
        }
        
        xSemaphoreGive(mutex);
        return result;
    }
    
    void setRegion(size_t startRow, size_t startCol, size_t numRows, size_t numCols, const std::vector<T>& values) {
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Parameter '%s': Failed to take mutex for setRegion", name.c_str());
            return;
        }
        
        if (startRow + numRows > rows || startCol + numCols > cols) {
            xSemaphoreGive(mutex);
            ESP_LOGE(TAG, "Parameter '%s': Region out of bounds for setRegion", name.c_str());
            return;
        }
        
        if (values.size() != numRows * numCols) {
            xSemaphoreGive(mutex);
            ESP_LOGE(TAG, "Parameter '%s': Value count mismatch in setRegion", name.c_str());
            return;
        }
        
        for (size_t r = 0; r < numRows; r++) {
            for (size_t c = 0; c < numCols; c++) {
                data[(startRow + r) * cols + startCol + c] = values[r * numCols + c];
            }
        }
        
        xSemaphoreGive(mutex);
        
        // Invoke callbacks
        if (has_callback) {
            for (size_t r = 0; r < numRows; r++) {
                for (size_t c = 0; c < numCols; c++) {
                    onChange(startRow + r, startCol + c, values[r * numCols + c]);
                }
            }
        }
    }
    
    // JSON access - implementations below for each type
    cJSON* getValueAsJson(size_t row, size_t col) const override;
    bool setValueFromJson(size_t row, size_t col, cJSON* value) override;
    
    // Extended info
    cJSON* getInfoAsJson() const override {
        cJSON* info = BaseParameter::getInfoAsJson();
        // Add min/max for numeric types (specialized below)
        addMinMaxToJson(info);
        return info;
    }

protected:
    void addMinMaxToJson(cJSON* info) const;  // Specialized below
    
    static constexpr const char *TAG = "Parameter";
    size_t rows, cols;
    std::vector<T> data;
    T min_value, max_value;
    std::function<void(size_t, size_t, T)> onChange;
    bool has_callback = false;
    mutable SemaphoreHandle_t mutex = nullptr;
};

// ============================================================================
// Type specializations for getType()
// ============================================================================

template<> inline ParameterType Parameter<int32_t>::getType() const { return ParameterType::INT; }
template<> inline ParameterType Parameter<float>::getType() const { return ParameterType::FLOAT; }
template<> inline ParameterType Parameter<uint8_t>::getType() const { return ParameterType::BOOL; }
template<> inline ParameterType Parameter<std::string>::getType() const { return ParameterType::STRING; }

// ============================================================================
// JSON specializations for getValueAsJson()
// ============================================================================

template<> inline cJSON* Parameter<int32_t>::getValueAsJson(size_t row, size_t col) const {
    return cJSON_CreateNumber(getValue(row, col));
}

template<> inline cJSON* Parameter<float>::getValueAsJson(size_t row, size_t col) const {
    return cJSON_CreateNumber(getValue(row, col));
}

template<> inline cJSON* Parameter<uint8_t>::getValueAsJson(size_t row, size_t col) const {
    return cJSON_CreateBool(getValue(row, col));
}

template<> inline cJSON* Parameter<std::string>::getValueAsJson(size_t row, size_t col) const {
    return cJSON_CreateString(getValue(row, col).c_str());
}

// ============================================================================
// JSON specializations for setValueFromJson()
// ============================================================================

template<> inline bool Parameter<int32_t>::setValueFromJson(size_t row, size_t col, cJSON* value) {
    if (read_only) return false;
    int32_t val = 0;
    if (cJSON_IsNumber(value)) {
        val = value->valueint;
    } else if (cJSON_IsString(value)) {
        val = atoi(value->valuestring);
    } else {
        return false;
    }
    setValue(row, col, val);
    return true;
}

template<> inline bool Parameter<float>::setValueFromJson(size_t row, size_t col, cJSON* value) {
    if (read_only) return false;
    float val = 0.0f;
    if (cJSON_IsNumber(value)) {
        val = (float)value->valuedouble;
    } else if (cJSON_IsString(value)) {
        val = atof(value->valuestring);
    } else {
        return false;
    }
    setValue(row, col, val);
    return true;
}

template<> inline bool Parameter<uint8_t>::setValueFromJson(size_t row, size_t col, cJSON* value) {
    if (read_only) return false;
    uint8_t val = 0;
    if (cJSON_IsBool(value)) {
        val = cJSON_IsTrue(value) ? 1 : 0;
    } else if (cJSON_IsNumber(value)) {
        val = value->valueint ? 1 : 0;
    } else if (cJSON_IsString(value)) {
        const char* str = value->valuestring;
        val = (strcmp(str, "true") == 0 || strcmp(str, "1") == 0) ? 1 : 0;
    } else {
        return false;
    }
    setValue(row, col, val);
    return true;
}

template<> inline bool Parameter<std::string>::setValueFromJson(size_t row, size_t col, cJSON* value) {
    if (read_only) return false;
    if (!cJSON_IsString(value)) return false;
    setValue(row, col, value->valuestring);
    return true;
}

// ============================================================================
// Min/Max JSON specializations
// ============================================================================

template<> inline void Parameter<int32_t>::addMinMaxToJson(cJSON* info) const {
    cJSON_AddNumberToObject(info, "min", min_value);
    cJSON_AddNumberToObject(info, "max", max_value);
}

template<> inline void Parameter<float>::addMinMaxToJson(cJSON* info) const {
    cJSON_AddNumberToObject(info, "min", min_value);
    cJSON_AddNumberToObject(info, "max", max_value);
}

template<> inline void Parameter<uint8_t>::addMinMaxToJson(cJSON* info) const {
    // Bool doesn't need min/max
}

template<> inline void Parameter<std::string>::addMinMaxToJson(cJSON* info) const {
    // String doesn't have min/max
}

// ============================================================================
// Type aliases
// ============================================================================

using IntParameter = Parameter<int32_t>;
using FloatParameter = Parameter<float>;
using BoolParameter = Parameter<uint8_t>;
using StringParameter = Parameter<std::string>;

// ============================================================================
// ============================================================================
// Component base class
// ============================================================================

class Component {
public:
    Component(const std::string &name);
    virtual ~Component();
    
    // Delete copy and move
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&) = delete;
    Component& operator=(Component&&) = delete;
    
    // Initialization
    void initialize();
    virtual void onInitialize() = 0;
    virtual void postInitialize() {}
    virtual void setUpDependencies(ComponentGraph* graph) {}
    
    // Identity
    uint32_t getComponentId() const { return componentId; }
    const std::string& getName() const;
    bool isInitialized() const;
    
    // Parameter access by name
    BaseParameter* getParam(const std::string& paramName);
    
    // Parameter access by UUID (fast)
    BaseParameter* getParamById(uint32_t paramId);
    
    // Typed parameter access (returns nullptr if type mismatch)
    IntParameter* getIntParam(const std::string& paramName);
    FloatParameter* getFloatParam(const std::string& paramName);
    BoolParameter* getBoolParam(const std::string& paramName);
    StringParameter* getStringParam(const std::string& paramName);
    
    // Get all parameters (for iteration)
    const std::unordered_map<std::string, std::unique_ptr<BaseParameter>>& getAllParams() const;
    
    // Memory diagnostics
    size_t getApproximateMemoryUsage() const;

protected:
    static constexpr const char *TAG = "Component";
    
    // Identity
    uint32_t componentId;
    static uint32_t nextComponentId;
    std::string name;
    bool initialized;
    
    // Component graph reference
    ComponentGraph* component_graph = nullptr;
    
    // Parameter storage - mutex protected
    std::unordered_map<std::string, std::unique_ptr<BaseParameter>> paramsByName;
    std::unordered_map<uint32_t, BaseParameter*> paramsById;  // Non-owning for fast lookup
    mutable SemaphoreHandle_t paramsMutex = nullptr;
    
    // Parameter ID counter
    static uint32_t nextParameterId;
    
    // Protected add methods - return pointer for member assignment
    IntParameter* addIntParam(const std::string &paramName, size_t rows, size_t cols, 
                              int min_val = INT_MIN, int max_val = INT_MAX, int default_val = 0, bool readOnly = false);
    
    FloatParameter* addFloatParam(const std::string &paramName, size_t rows, size_t cols, 
                                  float min_val = -FLT_MAX, float max_val = FLT_MAX, float default_val = 0.0f, bool readOnly = false);
    
    BoolParameter* addBoolParam(const std::string &paramName, size_t rows, size_t cols, 
                                bool default_val = false, bool readOnly = false);
    
    StringParameter* addStringParam(const std::string &paramName, size_t rows, size_t cols, 
                                    const std::string &default_val = "", bool readOnly = false);

private:
    // NVS persistence helpers
    static void loadNextIds();
    static void saveNextIds();
    static bool nvsLoaded;
};
