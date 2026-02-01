#pragma once

#include <string>
#include <vector>
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

// Forward declaration to avoid circular dependency
class ComponentGraph;

// Inline to avoid multiple definition errors when included in multiple .cpp files

// Template Parameter class - must be fully defined in header
template<typename T>
class Parameter {
public:
    Parameter(const std::string &name, size_t rows, size_t cols, T min_val = T(), T max_val = T(), T default_val = T(), bool read_only = false)
        : name(name), rows(rows), cols(cols), min_value(min_val), max_value(max_val), 
          read_only(read_only), onChange(nullptr) {
        
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
        
        ESP_LOGI(TAG, "Parameter '%s' allocated: %zux%zu (%zu bytes)", 
                 name.c_str(), rows, cols, total_bytes);
    }
    
    ~Parameter() {
        if (mutex != nullptr) {
            vSemaphoreDelete(mutex);
        }
    }
    
    // Delete copy and move - parameters should not be copied/moved
    Parameter(const Parameter&) = delete;
    Parameter& operator=(const Parameter&) = delete;
    Parameter(Parameter&&) = delete;
    Parameter& operator=(Parameter&&) = delete;
    
    std::string getName() const { return name; }
    
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
    
    size_t getRows() const { 
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return 0;
        size_t r = rows;
        xSemaphoreGive(mutex);
        return r;
    }
    
    size_t getCols() const { 
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return 0;
        size_t c = cols;
        xSemaphoreGive(mutex);
        return c;
    }
    
    T getMin() const { return min_value; }
    T getMax() const { return max_value; }
    bool isReadOnly() const { return read_only; }
    
    // Append a new value (grows the parameter by one row, assumes single column)
    void appendValue(const T& value) {
        size_t new_row = 0;
        
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Parameter '%s': Failed to take mutex for appendValue", name.c_str());
            assert(false && "Mutex take failed");
        }
        
#ifdef DEBUG
        ESP_LOGI(TAG, "Parameter '%s': Appending value, growing from %zu rows to %zu rows", 
                 name.c_str(), rows, rows + 1);
#endif
        data.push_back(value);
        new_row = rows;
        rows = data.size() / cols;
        xSemaphoreGive(mutex);
        
        // Invoke callback after releasing lock with the value that was appended
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
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            return nullptr;
        }
        auto callback = onChange;
        xSemaphoreGive(mutex);
        return callback;
    }
    
    std::vector<T> getRegion(size_t startRow, size_t startCol, size_t numRows, size_t numCols) const {
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Parameter '%s': Failed to take mutex for getRegion", name.c_str());
            return std::vector<T>();
        }
        
        if (startRow + numRows > rows || startCol + numCols > cols) {
            xSemaphoreGive(mutex);
            ESP_LOGE(TAG, "Parameter '%s': Region out of bounds [%zu,%zu]+[%zu,%zu] (size: %zux%zu)", 
                     name.c_str(), startRow, startCol, numRows, numCols, rows, cols);
            assert(false && "Parameter getRegion out of bounds");
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
            assert(false && "Mutex take failed");
        }
        
        if (startRow + numRows > rows || startCol + numCols > cols) {
            xSemaphoreGive(mutex);
            ESP_LOGE(TAG, "Parameter '%s': Region out of bounds [%zu,%zu]+[%zu,%zu] (size: %zux%zu)", 
                     name.c_str(), startRow, startCol, numRows, numCols, rows, cols);
            assert(false && "Parameter setRegion out of bounds");
        }
        
        if (values.size() != numRows * numCols) {
            xSemaphoreGive(mutex);
            ESP_LOGE(TAG, "Parameter '%s': Value count mismatch. Expected %zu, got %zu", 
                     name.c_str(), numRows * numCols, values.size());
            assert(false && "Parameter setRegion value count mismatch");
        }
        
        if (numCols == cols) {
            std::copy(values.begin(), 
                     values.begin() + numRows * numCols,
                     data.begin() + startRow * cols + startCol);
        } else {
            for (size_t r = 0; r < numRows; r++) {
                std::copy(values.begin() + r * numCols, 
                         values.begin() + (r + 1) * numCols,
                         data.begin() + (startRow + r) * cols + startCol);
            }
        }
        
        xSemaphoreGive(mutex);
        
        // Invoke callback for each changed cell after releasing lock with the value
        if (has_callback) {
            for (size_t r = 0; r < numRows; r++) {
                for (size_t c = 0; c < numCols; c++) {
                    onChange(startRow + r, startCol + c, values[r * numCols + c]);
                }
            }
        }
    }

private:
    static constexpr const char *TAG = "Parameter";
    std::string name;
    size_t rows, cols;
    std::vector<T> data;
    T min_value, max_value;
    bool read_only = false;
    std::function<void(size_t, size_t, T)> onChange;  // Callback receives value
    bool has_callback = false;
    mutable SemaphoreHandle_t mutex = nullptr;  // Mutable so const methods can lock
};

// Type aliases
using IntParameter = Parameter<int32_t>;
using FloatParameter = Parameter<float>;
using BoolParameter = Parameter<uint8_t>;  // Use uint8_t instead of bool because std::vector<bool> is specialized and doesn't play nice with our usage
using StringParameter = Parameter<std::string>;

// Forward declaration
class Component;

// TriggerParameter - a string parameter that ALWAYS invokes its callback on setValue
// Used for actions/triggers where we want to execute code regardless of value change
class TriggerParameter : public Parameter<std::string> {
public:
    TriggerParameter(const std::string &name, size_t rows, size_t cols,
                    const std::string &default_val,
                    std::function<void(Component*, size_t, size_t, const std::string&)> trigger_callback,
                    bool read_only = false)
        : Parameter<std::string>(name, rows, cols, default_val, default_val, default_val, read_only),
          triggerCallback(trigger_callback),
          ownerComponent(nullptr) {
        if (!trigger_callback) {
            ESP_LOGE("TriggerParameter", "TriggerParameter '%s' created without callback!", name.c_str());
            assert(false && "TriggerParameter requires a callback");
        }
    }
    
    // Set the owning component (called by Component::addTriggerParam)
    void setOwner(Component* owner) { ownerComponent = owner; }
    
    // Override setValue to ALWAYS call the callback
    void setValue(size_t row, size_t col, const std::string& val) {
        // First call base class to actually set the value
        Parameter<std::string>::setValue(row, col, val);
        
        // Then ALWAYS invoke our trigger callback
        if (triggerCallback && ownerComponent) {
            triggerCallback(ownerComponent, row, col, val);
        }
    }
    
    // Set value WITHOUT triggering callback (for initialization/configuration)
    void setValueQuiet(size_t row, size_t col, const std::string& val) {
        Parameter<std::string>::setValue(row, col, val);
    }
    
private:
    std::function<void(Component*, size_t, size_t, const std::string&)> triggerCallback;
    Component* ownerComponent;
};

// Component base class
class Component {
public:
    Component(const std::string &name);
    virtual ~Component();
    
    // Delete copy and move - components should not be copied/moved
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;
    Component(Component&&) = delete;
    Component& operator=(Component&&) = delete;
    
    // Base class initialization wrapper - DO NOT OVERRIDE
    // This calls onInitialize() and then runs post-initialization tasks
    void initialize();
    
    // Override this in subclasses to implement component-specific initialization
    virtual void onInitialize() = 0;
    
    // Called after ALL components have initialized (for tasks that need other components ready)
    // Override this for setup that must happen after all component initializations complete
    virtual void postInitialize() {}
    
    // Set up inter-component dependencies (called before initialize, after all components registered)
    // Override this to get references to other components or their parameters
    virtual void setUpDependencies(ComponentGraph* graph) {}
    
    std::string getName() const;
    bool isInitialized() const;
    
    const std::vector<std::unique_ptr<IntParameter>>& getIntParams() const;
    const std::vector<std::unique_ptr<FloatParameter>>& getFloatParams() const;
    const std::vector<std::unique_ptr<BoolParameter>>& getBoolParams() const;
    const std::vector<std::unique_ptr<StringParameter>>& getStringParams() const;
    const std::vector<std::unique_ptr<TriggerParameter>>& getTriggerParams() const;
    
    std::vector<std::unique_ptr<IntParameter>>& getIntParams();
    std::vector<std::unique_ptr<FloatParameter>>& getFloatParams();
    std::vector<std::unique_ptr<BoolParameter>>& getBoolParams();
    std::vector<std::unique_ptr<StringParameter>>& getStringParams();
    std::vector<std::unique_ptr<TriggerParameter>>& getTriggerParams();
    
    IntParameter* getIntParam(const std::string &paramName);
    FloatParameter* getFloatParam(const std::string &paramName);
    BoolParameter* getBoolParam(const std::string &paramName);
    StringParameter* getStringParam(const std::string &paramName);
    TriggerParameter* getTriggerParam(const std::string &paramName);
    IntParameter* getIntParam(int paramId);
    FloatParameter* getFloatParam(int paramId);
    BoolParameter* getBoolParam(int paramId);
    StringParameter* getStringParam(int paramId);
    TriggerParameter* getTriggerParam(int paramId);
    
    // Memory diagnostics - calculate approximate memory usage
    size_t getApproximateMemoryUsage() const;

protected:
    static constexpr const char *TAG = "Component";
    ComponentGraph* component_graph = nullptr; // Pointer to the component graph
    std::string name;
    bool initialized;
    
    // Parameter storage - using unique_ptr for stable heap addresses
    std::vector<std::unique_ptr<IntParameter>> intParams;
    std::vector<std::unique_ptr<FloatParameter>> floatParams;
    std::vector<std::unique_ptr<BoolParameter>> boolParams;
    std::vector<std::unique_ptr<StringParameter>> stringParams;
    std::vector<std::unique_ptr<TriggerParameter>> triggerParams;
    
    void addIntParam(const std::string &paramName, size_t rows, size_t cols, int min_val = INT_MIN, int max_val = INT_MAX, int default_val = 0, bool readOnly = false);
    void addFloatParam(const std::string &paramName, size_t rows, size_t cols, float min_val = -FLT_MAX, float max_val = FLT_MAX, float default_val = 0.0f, bool readOnly = false);
    void addBoolParam(const std::string &paramName, size_t rows, size_t cols, bool default_val = false, bool readOnly = false);
    void addStringParam(const std::string &paramName, size_t rows, size_t cols, const std::string &default_val = "", bool readOnly = false);
    void addTriggerParam(const std::string &paramName, size_t rows, size_t cols, 
                        const std::string &default_val,
                        std::function<void(Component*, size_t, size_t, const std::string&)> callback,
                        bool readOnly = false);
};