#pragma once

#include <string>
#include <vector>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include <cassert>

// Inline to avoid multiple definition errors when included in multiple .cpp files
inline const char *PARAM_TAG = "Parameter";

// Template Parameter class - must be fully defined in header
template<typename T>
class Parameter {
public:
    Parameter(const std::string &name, size_t rows, size_t cols)
        : name(name), rows(rows), cols(cols) {
        
        size_t total_elements = rows * cols;
        size_t total_bytes = total_elements * sizeof(T);
        size_t free_heap = esp_get_free_heap_size();
        
        if (total_bytes > free_heap / 2) {
            ESP_LOGE(PARAM_TAG, "Parameter '%s': Allocation too large! Requested %zu bytes, only %zu free", 
                     name.c_str(), total_bytes, free_heap);
            assert(false && "Parameter allocation would exhaust heap");
        }
        
        data.resize(total_elements);
        ESP_LOGI(PARAM_TAG, "Parameter '%s' allocated: %zux%zu (%zu bytes)", 
                 name.c_str(), rows, cols, total_bytes);
    }
    
    std::string getName() const { return name; }
    
    T& getValue(size_t row, size_t col) { 
        if (row >= rows || col >= cols) {
            ESP_LOGE(PARAM_TAG, "Parameter '%s': Out of bounds access [%zu,%zu] (size: %zux%zu)", 
                     name.c_str(), row, col, rows, cols);
            assert(false && "Parameter getValue out of bounds");
        }
        return data[row * cols + col]; 
    }
    
    const T& getValue(size_t row, size_t col) const { 
        if (row >= rows || col >= cols) {
            ESP_LOGE(PARAM_TAG, "Parameter '%s': Out of bounds access [%zu,%zu] (size: %zux%zu)", 
                     name.c_str(), row, col, rows, cols);
            assert(false && "Parameter getValue out of bounds");
        }
        return data[row * cols + col]; 
    }
    
    void setValue(size_t row, size_t col, T val) { 
        if (row >= rows || col >= cols) {
            ESP_LOGE(PARAM_TAG, "Parameter '%s': Out of bounds write [%zu,%zu] (size: %zux%zu)", 
                     name.c_str(), row, col, rows, cols);
            assert(false && "Parameter setValue out of bounds");
        }
        data[row * cols + col] = val; 
    }
    
    size_t getRows() const { return rows; }
    size_t getCols() const { return cols; }
    
    std::vector<T> getRegion(size_t startRow, size_t startCol, size_t numRows, size_t numCols) const {
        if (startRow + numRows > rows || startCol + numCols > cols) {
            ESP_LOGE(PARAM_TAG, "Parameter '%s': Region out of bounds [%zu,%zu]+[%zu,%zu] (size: %zux%zu)", 
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
        return result;
    }
    
    void setRegion(size_t startRow, size_t startCol, size_t numRows, size_t numCols, const std::vector<T>& values) {
        if (startRow + numRows > rows || startCol + numCols > cols) {
            ESP_LOGE(PARAM_TAG, "Parameter '%s': Region out of bounds [%zu,%zu]+[%zu,%zu] (size: %zux%zu)", 
                     name.c_str(), startRow, startCol, numRows, numCols, rows, cols);
            assert(false && "Parameter setRegion out of bounds");
        }
        
        if (values.size() != numRows * numCols) {
            ESP_LOGE(PARAM_TAG, "Parameter '%s': Value count mismatch. Expected %zu, got %zu", 
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
    }

private:
    std::string name;
    size_t rows;
    size_t cols;
    std::vector<T> data;
};

// Type aliases
using IntParameter = Parameter<int>;
using FloatParameter = Parameter<float>;
using BoolParameter = Parameter<uint8_t>;  // Use uint8_t instead of bool because std::vector<bool> is specialized and doesn't play nice with our usage
using StringParameter = Parameter<std::string>;

// Component base class
class Component {
public:
    Component(const std::string &name);
    virtual ~Component();
    
    virtual void initialize() = 0;
    
    std::string getName() const;
    bool isInitialized() const;
    
    const std::vector<IntParameter>& getIntParams() const;
    const std::vector<FloatParameter>& getFloatParams() const;
    const std::vector<BoolParameter>& getBoolParams() const;
    const std::vector<StringParameter>& getStringParams() const;
    
    std::vector<IntParameter>& getIntParams();
    std::vector<FloatParameter>& getFloatParams();
    std::vector<BoolParameter>& getBoolParams();
    std::vector<StringParameter>& getStringParams();
    
    IntParameter* getIntParam(const std::string &paramName);
    FloatParameter* getFloatParam(const std::string &paramName);
    BoolParameter* getBoolParam(const std::string &paramName);
    StringParameter* getStringParam(const std::string &paramName);

protected:
    std::string name;
    bool initialized;
    std::vector<IntParameter> intParams;
    std::vector<FloatParameter> floatParams;
    std::vector<BoolParameter> boolParams;
    std::vector<StringParameter> stringParams;
    
    void addIntParam(const std::string &paramName, size_t rows, size_t cols);
    void addFloatParam(const std::string &paramName, size_t rows, size_t cols);
    void addBoolParam(const std::string &paramName, size_t rows, size_t cols);
    void addStringParam(const std::string &paramName, size_t rows, size_t cols);
};