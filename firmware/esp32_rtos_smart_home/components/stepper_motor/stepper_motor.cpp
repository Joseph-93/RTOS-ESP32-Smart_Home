#include "stepper_motor.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include <cstring>
#include <cmath>
#include <algorithm>

// Base64 decoding table
static const uint8_t base64_decode_table[256] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
    52,53,54,55,56,57,58,59,60,61,64,64,64,65,64,64,
    64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
};

static size_t base64_decode(const char* input, size_t input_len, uint8_t* output) {
    size_t out_len = 0;
    uint32_t buf = 0;
    int bits_collected = 0;
    
    for (size_t i = 0; i < input_len; i++) {
        uint8_t c = base64_decode_table[(uint8_t)input[i]];
        if (c == 64) continue;  // Skip invalid chars
        if (c == 65) break;     // Padding '='
        
        buf = (buf << 6) | c;
        bits_collected += 6;
        
        if (bits_collected >= 8) {
            bits_collected -= 8;
            output[out_len++] = (buf >> bits_collected) & 0xFF;
        }
    }
    return out_len;
}

// ============================================================================
// Stub HAL (does nothing - placeholder until real HAL is implemented)
// ============================================================================

class StubStepperMotorHAL : public StepperMotorHAL {
public:
    StubStepperMotorHAL() : microstepping(16) {}
    
    esp_err_t init() override { 
        ESP_LOGW("StubHAL", "Using stub HAL - no motors will move!");
        return ESP_OK; 
    }
    void deinit() override {}
    void setDirection(uint8_t motor_index, bool direction) override {}
    void step(uint8_t motor_index) override {}
    void stepMultiple(uint8_t motor_mask) override {}
    void setEnabled(uint8_t motor_index, bool enabled) override {}
    bool isLimitTriggered(uint8_t motor_index) override { return false; }
    uint8_t getMotorCount() const override { return 4; }
    uint32_t getMinPulseWidthUs() const override { return 2; }
    
    // Microstepping (stub just stores the value)
    bool setMicrostepping(uint16_t divisor) override { 
        microstepping = divisor;
        ESP_LOGI("StubHAL", "Microstepping set to 1/%d (stub)", divisor);
        return true; 
    }
    uint16_t getMicrostepping() const override { return microstepping; }
    bool isMicrosteppingSoftwareConfigurable() const override { return true; }
    
private:
    uint16_t microstepping;
};

// ============================================================================
// Constructor / Destructor
// ============================================================================

StepperMotorComponent::StepperMotorComponent() 
    : Component("StepperMotor"),
      hal(nullptr),
      halOwned(false),
      activeChoreographyIndex(-1),
      metadataModified(false),
      state(PlaybackState::IDLE),
      playbackStartUs(0),
      loopsRemaining(0),
      uploadExpectedSize(0),
      uploadReceivedSize(0),
      uploadInProgress(false),
      homingTimeoutMs(10000),
      homingStartMs(0),
      evalTimer(nullptr),
      stateMutex(nullptr),
      stopRequested(false)
{
    // Initialize arrays
    for (int i = 0; i < NUM_MOTORS; i++) {
        currentPosition[i] = 0;
        targetPosition[i] = 0;
        currentKnotIndex[i] = 0;
        homingInProgress[i] = false;
        homingDirection[i] = -1;
    }
}

StepperMotorComponent::~StepperMotorComponent() {
    // Stop timer
    if (evalTimer) {
        esp_timer_stop(evalTimer);
        esp_timer_delete(evalTimer);
    }
    
    // Cleanup mutex
    if (stateMutex) {
        vSemaphoreDelete(stateMutex);
    }
    
    // Cleanup HAL
    if (hal) {
        hal->deinit();
        if (halOwned) {
            delete hal;
        }
    }
}

void StepperMotorComponent::setHAL(StepperMotorHAL* newHal) {
    if (hal && halOwned) {
        delete hal;
    }
    hal = newHal;
    halOwned = false;
}

// ============================================================================
// Initialization
// ============================================================================

void StepperMotorComponent::onInitialize() {
    ESP_LOGI(TAG, "Initializing StepperMotorComponent");
    
    // Create state mutex
    stateMutex = xSemaphoreCreateMutex();
    if (!stateMutex) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return;
    }
    
    // If no HAL provided, use stub
    if (!hal) {
        hal = new StubStepperMotorHAL();
        halOwned = true;
    }
    
    // Initialize HAL
    esp_err_t err = hal->init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HAL init failed: %d", err);
        return;
    }
    
    // Initialize SPIFFS (may already be initialized by another component)
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
        // Already mounted, that's fine
        ESP_LOGI(TAG, "SPIFFS already mounted");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %d", err);
    }
    
    // ========================================================================
    // Create Parameters
    // ========================================================================
    
    // Playback control
    playing = addBoolParam("playing", 1, 1, false);
    playing->setOnChange([this](size_t, size_t, bool val) {
        if (val) {
            startPlayback();
        } else {
            stopPlayback();
        }
    });
    
    activeChoreographyParam = addIntParam("activeChoreography", 1, 1, -1, 100, -1);
    activeChoreographyParam->setOnChange([this](size_t, size_t, int32_t val) {
        if (val >= 0) {
            loadChoreography(val);
        }
    });
    
    enableDrivers = addBoolParam("enableDrivers", 1, 1, false);
    enableDrivers->setOnChange([this](size_t, size_t, bool val) {
        if (hal) {
            hal->setEnabled(0xFF, val);
        }
    });
    
    stateParam = addIntParam("state", 1, 1, 0, 4, 0, true);  // Read-only
    
    // Playback status (read-only)
    playbackProgressParam = addFloatParam("playbackProgress", 1, 1, 0.0f, 1.0f, 0.0f, true);
    playbackTimeParam = addFloatParam("playbackTime", 1, 1, 0.0f, 86400.0f, 0.0f, true);
    choreographyCountParam = addIntParam("choreographyCount", 1, 1, 0, 1000, 0, true);
    
    // Motor positions (read-only)
    motorPositions = addIntParam("motorPositions", NUM_MOTORS, 1, INT32_MIN, INT32_MAX, 0, true);
    motorTargets = addIntParam("motorTargets", NUM_MOTORS, 1, INT32_MIN, INT32_MAX, 0, true);
    
    // Homing
    homeCommand = addBoolParam("homeCommand", 1, 1, false);
    homeCommand->setOnChange([this](size_t, size_t, bool val) {
        if (val) {
            startHoming();
            homeCommand->setValueQuiet(0, 0, false);  // Reset trigger
        }
    });
    isHomed = addBoolParam("isHomed", 1, 1, false, true);
    
    // Emergency stop
    eStopCommand = addBoolParam("eStopCommand", 1, 1, false);
    eStopCommand->setOnChange([this](size_t, size_t, bool val) {
        if (val) {
            emergencyStop();
            eStopCommand->setValueQuiet(0, 0, false);
        }
    });
    
    eStopClear = addBoolParam("eStopClear", 1, 1, false);
    eStopClear->setOnChange([this](size_t, size_t, bool val) {
        if (val) {
            clearEmergencyStop();
            eStopClear->setValueQuiet(0, 0, false);
        }
    });
    
    // Upload parameters
    uploadName = addStringParam("uploadName", 1, 1, "");
    uploadTotalBytes = addIntParam("uploadTotalBytes", 1, 1, 0, MAX_CHOREOGRAPHY_SIZE, 0);
    uploadTotalBytes->setOnChange([this](size_t, size_t, int32_t val) {
        if (val > 0) {
            std::string name = uploadName->getValue(0, 0);
            uploadBegin(name, val);
        }
    });
    
    uploadChunkIndex = addIntParam("uploadChunkIndex", 1, 1, 0, 10000, 0);
    uploadChunkData = addStringParam("uploadChunkData", 1, 1, "");
    uploadChunkData->setOnChange([this](size_t, size_t, const std::string& val) {
        if (!val.empty() && uploadInProgress) {
            // Decode base64 and upload chunk
            std::vector<uint8_t> decoded(val.size());
            size_t decoded_len = base64_decode(val.c_str(), val.size(), decoded.data());
            uint16_t chunk_idx = uploadChunkIndex->getValue(0, 0);
            uploadChunk(chunk_idx, decoded.data(), decoded_len);
        }
    });
    
    uploadCommitParam = addBoolParam("uploadCommit", 1, 1, false);
    uploadCommitParam->setOnChange([this](size_t, size_t, bool val) {
        if (val) {
            uploadCommit();
            uploadCommitParam->setValueQuiet(0, 0, false);
        }
    });
    
    // Delete parameter
    deleteChoreographyParam = addIntParam("deleteChoreography", 1, 1, -1, 1000, -1);
    deleteChoreographyParam->setOnChange([this](size_t, size_t, int32_t val) {
        if (val >= 0) {
            deleteChoreography(val);
            deleteChoreographyParam->setValueQuiet(0, 0, -1);
        }
    });
    
    // Query parameters
    queryChoreographyIndex = addIntParam("queryChoreographyIndex", 1, 1, -1, 1000, -1);
    queryChoreographyIndex->setOnChange([this](size_t, size_t, int32_t val) {
        if (val >= 0 && choreographyMeta.count(val)) {
            const auto& meta = choreographyMeta.at(val);
            queryChoreographyName->setValueQuiet(0, 0, meta.name);
            queryChoreographyDuration->setValueQuiet(0, 0, meta.duration_sec);
            queryChoreographyLoop->setValueQuiet(0, 0, meta.loop_count);
        }
    });
    
    queryChoreographyName = addStringParam("queryChoreographyName", 1, 1, "", true);
    queryChoreographyDuration = addFloatParam("queryChoreographyDuration", 1, 1, 0, 86400, 0, true);
    queryChoreographyLoop = addIntParam("queryChoreographyLoop", 1, 1, -1, 10000, 1, true);
    choreographyIdsParam = addStringParam("choreographyIds", 1, 1, "", true);
    
    // Configuration
    chunkSizeParam = addIntParam("chunkSize", 1, 1, 0, 65536, UPLOAD_CHUNK_SIZE, true);
    evalRateHz = addFloatParam("evalRateHz", 1, 1, 100, 10000, DEFAULT_EVAL_RATE_HZ);
    
    // Microstepping configuration (default 1/16)
    microsteppingParam = addIntParam("microstepping", 1, 1, 1, 256, 16);
    microsteppingParam->setOnChange([this](size_t, size_t, int32_t val) {
        if (hal) {
            if (!hal->setMicrostepping(val)) {
                ESP_LOGW(TAG, "HAL rejected microstepping value %ld", val);
                // Revert to current HAL value
                microsteppingParam->setValueQuiet(0, 0, hal->getMicrostepping());
            }
        }
    });
    
    microsteppingConfigurable = addBoolParam("microsteppingConfigurable", 1, 1, false, true);
    
    // Set initial microstepping and update configurable flag
    if (hal) {
        hal->setMicrostepping(16);  // Default to 1/16
        microsteppingConfigurable->setValueQuiet(0, 0, hal->isMicrosteppingSoftwareConfigurable());
    }
    
    // Create high-precision timer for ISR
    esp_timer_create_args_t timer_args = {
        .callback = evalTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_ISR,
        .name = "stepper_eval",
        .skip_unhandled_events = true
    };
    
    err = esp_timer_create(&timer_args, &evalTimer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create eval timer: %d", err);
    }
    
    ESP_LOGI(TAG, "StepperMotorComponent initialized");
}

// ============================================================================
// NVS Persistence
// ============================================================================

void StepperMotorComponent::saveCustomData(nvs_handle_t handle) {
    ESP_LOGI(TAG, "Saving choreography metadata to NVS");
    
    // Save count
    uint16_t count = choreographyMeta.size();
    nvs_set_u16(handle, "choreo_count", count);
    
    // Save each entry
    int idx = 0;
    for (const auto& [id, meta] : choreographyMeta) {
        char key[32];
        
        snprintf(key, sizeof(key), "choreo_%d_id", idx);
        nvs_set_i16(handle, key, id);
        
        snprintf(key, sizeof(key), "choreo_%d_name", idx);
        nvs_set_str(handle, key, meta.name.c_str());
        
        snprintf(key, sizeof(key), "choreo_%d_file", idx);
        nvs_set_str(handle, key, meta.filename.c_str());
        
        snprintf(key, sizeof(key), "choreo_%d_dur", idx);
        nvs_set_blob(handle, key, &meta.duration_sec, sizeof(float));
        
        snprintf(key, sizeof(key), "choreo_%d_loop", idx);
        nvs_set_i16(handle, key, meta.loop_count);
        
        idx++;
    }
    
    // Save active choreography index
    nvs_set_i16(handle, "active_choreo", activeChoreographyIndex);
    
    metadataModified = false;
}

void StepperMotorComponent::loadCustomData(nvs_handle_t handle) {
    ESP_LOGI(TAG, "Loading choreography metadata from NVS");
    
    choreographyMeta.clear();
    
    uint16_t count = 0;
    if (nvs_get_u16(handle, "choreo_count", &count) != ESP_OK) {
        ESP_LOGI(TAG, "No choreographies in NVS");
        return;
    }
    
    for (int idx = 0; idx < count; idx++) {
        char key[32];
        ChoreographyMeta meta;
        int16_t id;
        
        snprintf(key, sizeof(key), "choreo_%d_id", idx);
        if (nvs_get_i16(handle, key, &id) != ESP_OK) continue;
        
        snprintf(key, sizeof(key), "choreo_%d_name", idx);
        size_t len = 64;
        char name_buf[64];
        if (nvs_get_str(handle, key, name_buf, &len) == ESP_OK) {
            meta.name = name_buf;
        }
        
        snprintf(key, sizeof(key), "choreo_%d_file", idx);
        len = 64;
        char file_buf[64];
        if (nvs_get_str(handle, key, file_buf, &len) == ESP_OK) {
            meta.filename = file_buf;
        }
        
        snprintf(key, sizeof(key), "choreo_%d_dur", idx);
        len = sizeof(float);
        nvs_get_blob(handle, key, &meta.duration_sec, &len);
        
        snprintf(key, sizeof(key), "choreo_%d_loop", idx);
        nvs_get_i16(handle, key, &meta.loop_count);
        
        choreographyMeta[id] = meta;
    }
    
    // Load active choreography
    int16_t active = -1;
    nvs_get_i16(handle, "active_choreo", &active);
    activeChoreographyIndex = active;
    
    ESP_LOGI(TAG, "Loaded %zu choreographies from NVS", choreographyMeta.size());
    
    updateStatusParams();
    updateChoreographyIdsParam();
}

void StepperMotorComponent::onPostLoadReconcile() {
    // If we have an active choreography, try to load it
    if (activeChoreographyIndex >= 0 && choreographyMeta.count(activeChoreographyIndex)) {
        loadChoreography(activeChoreographyIndex);
    }
}

// ============================================================================
// Choreography Upload
// ============================================================================

esp_err_t StepperMotorComponent::uploadBegin(const std::string& name, size_t total_bytes) {
    if (total_bytes > MAX_CHOREOGRAPHY_SIZE) {
        ESP_LOGE(TAG, "Choreography too large: %zu > %zu", total_bytes, MAX_CHOREOGRAPHY_SIZE);
        return ESP_ERR_NO_MEM;
    }
    
    // Allocate buffer
    uploadBuffer.clear();
    uploadBuffer.reserve(total_bytes);
    uploadExpectedSize = total_bytes;
    uploadReceivedSize = 0;
    uploadPendingName = name.empty() ? "Unnamed" : name;
    uploadInProgress = true;
    
    ESP_LOGI(TAG, "Upload started: '%s', expecting %zu bytes", uploadPendingName.c_str(), total_bytes);
    return ESP_OK;
}

esp_err_t StepperMotorComponent::uploadChunk(uint16_t chunk_index, const uint8_t* data, size_t len) {
    if (!uploadInProgress) {
        ESP_LOGE(TAG, "No upload in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    size_t expected_offset = chunk_index * UPLOAD_CHUNK_SIZE;
    if (expected_offset != uploadReceivedSize) {
        ESP_LOGW(TAG, "Chunk %d: expected offset %zu, got %zu", chunk_index, uploadReceivedSize, expected_offset);
        // Allow out-of-order for now, just append
    }
    
    uploadBuffer.insert(uploadBuffer.end(), data, data + len);
    uploadReceivedSize += len;
    
    ESP_LOGD(TAG, "Chunk %d: received %zu bytes (total: %zu/%zu)", 
             chunk_index, len, uploadReceivedSize, uploadExpectedSize);
    
    return ESP_OK;
}

esp_err_t StepperMotorComponent::uploadCommit() {
    if (!uploadInProgress) {
        ESP_LOGE(TAG, "No upload in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    uploadInProgress = false;
    
    ESP_LOGI(TAG, "Upload complete: %zu bytes received", uploadReceivedSize);
    
    // Parse JSON to validate and extract metadata
    Choreography choreo;
    esp_err_t err = parseChoreographyJson((const char*)uploadBuffer.data(), uploadBuffer.size(), choreo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse choreography JSON");
        uploadBuffer.clear();
        return err;
    }
    
    // Assign name
    choreo.name = uploadPendingName;
    
    // Find next available ID
    int16_t newId = findNextChoreographyId();
    
    // Generate filename
    char filename[32];
    snprintf(filename, sizeof(filename), "%s%03d.json", CHOREO_FILE_PREFIX, newId);
    choreo.filename = filename;
    
    // Save to SPIFFS
    err = saveChoreographyToSpiffs(choreo.filename, uploadBuffer.data(), uploadBuffer.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save choreography to SPIFFS");
        uploadBuffer.clear();
        return err;
    }
    
    // Add to metadata
    ChoreographyMeta meta;
    meta.name = choreo.name;
    meta.filename = choreo.filename;
    meta.duration_sec = choreo.duration_sec;
    meta.loop_count = choreo.loop_count;
    choreographyMeta[newId] = meta;
    metadataModified = true;
    
    // Cleanup
    uploadBuffer.clear();
    
    ESP_LOGI(TAG, "Choreography '%s' saved with ID %d (duration: %.2fs)", 
             choreo.name.c_str(), newId, choreo.duration_sec);
    
    updateStatusParams();
    updateChoreographyIdsParam();
    
    return ESP_OK;
}

void StepperMotorComponent::deleteChoreography(int16_t index) {
    auto it = choreographyMeta.find(index);
    if (it == choreographyMeta.end()) {
        ESP_LOGW(TAG, "Choreography %d not found", index);
        return;
    }
    
    // Delete file from SPIFFS
    std::string filepath = std::string(SPIFFS_BASE_PATH) + "/" + it->second.filename;
    remove(filepath.c_str());
    
    // Remove from metadata
    choreographyMeta.erase(it);
    metadataModified = true;
    
    // If this was the active choreography, clear it
    if (activeChoreographyIndex == index) {
        activeChoreographyIndex = -1;
        activeChoreography = Choreography();
    }
    
    ESP_LOGI(TAG, "Choreography %d deleted", index);
    
    updateStatusParams();
    updateChoreographyIdsParam();
}

// ============================================================================
// Choreography Loading
// ============================================================================

esp_err_t StepperMotorComponent::loadChoreography(int16_t index) {
    auto it = choreographyMeta.find(index);
    if (it == choreographyMeta.end()) {
        ESP_LOGE(TAG, "Choreography %d not found", index);
        return ESP_ERR_NOT_FOUND;
    }
    
    esp_err_t err = loadChoreographyFromSpiffs(it->second.filename, activeChoreography);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load choreography from SPIFFS");
        return err;
    }
    
    activeChoreography.name = it->second.name;
    activeChoreographyIndex = index;
    
    // Reset playback state
    for (int i = 0; i < NUM_MOTORS; i++) {
        currentKnotIndex[i] = 0;
    }
    
    ESP_LOGI(TAG, "Loaded choreography '%s' (ID %d): %.2fs, %zu/%zu/%zu/%zu knots",
             activeChoreography.name.c_str(), index, activeChoreography.duration_sec,
             activeChoreography.knots[0].size(), activeChoreography.knots[1].size(),
             activeChoreography.knots[2].size(), activeChoreography.knots[3].size());
    
    activeChoreographyParam->setValueQuiet(0, 0, index);
    
    return ESP_OK;
}

// ============================================================================
// JSON Parsing
// ============================================================================

esp_err_t StepperMotorComponent::parseChoreographyJson(const char* json, size_t len, Choreography& out) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error: %s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "unknown");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check format version
    cJSON* format_version = cJSON_GetObjectItem(root, "format_version");
    if (!format_version || format_version->valueint != 1) {
        ESP_LOGE(TAG, "Invalid format_version (expected 1)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Parse motor parameters
    cJSON* motor = cJSON_GetObjectItem(root, "motor");
    if (motor) {
        cJSON* r_spool = cJSON_GetObjectItem(motor, "r_spool");
        cJSON* steps_per_rev = cJSON_GetObjectItem(motor, "steps_per_rev");
        cJSON* microstep_divisor = cJSON_GetObjectItem(motor, "microstep_divisor");
        
        if (r_spool) out.r_spool = (float)r_spool->valuedouble;
        if (steps_per_rev) out.steps_per_rev = steps_per_rev->valueint;
        if (microstep_divisor) out.microstep_divisor = microstep_divisor->valueint;
    }
    
    // Parse porch dimensions (optional, informational)
    cJSON* porch = cJSON_GetObjectItem(root, "porch");
    if (porch) {
        cJSON* lx = cJSON_GetObjectItem(porch, "Lx");
        cJSON* ly = cJSON_GetObjectItem(porch, "Ly");
        cJSON* lz = cJSON_GetObjectItem(porch, "Lz");
        
        if (lx) out.porch_lx = (float)lx->valuedouble;
        if (ly) out.porch_ly = (float)ly->valuedouble;
        if (lz) out.porch_lz = (float)lz->valuedouble;
    }
    
    // Parse spline knots per motor
    cJSON* spline_knots = cJSON_GetObjectItem(root, "spline_knots_per_motor");
    if (!spline_knots || !cJSON_IsArray(spline_knots)) {
        ESP_LOGE(TAG, "Missing or invalid spline_knots_per_motor");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    float max_time = 0;
    
    cJSON* motor_knots;
    cJSON_ArrayForEach(motor_knots, spline_knots) {
        cJSON* motor_idx = cJSON_GetObjectItem(motor_knots, "motor");
        cJSON* knots_array = cJSON_GetObjectItem(motor_knots, "knots");
        
        if (!motor_idx || !knots_array || !cJSON_IsArray(knots_array)) {
            continue;
        }
        
        int idx = motor_idx->valueint;
        if (idx < 0 || idx >= NUM_MOTORS) {
            ESP_LOGW(TAG, "Skipping motor index %d (out of range)", idx);
            continue;
        }
        
        out.knots[idx].clear();
        
        cJSON* knot;
        cJSON_ArrayForEach(knot, knots_array) {
            SplineKnot k;
            cJSON* t = cJSON_GetObjectItem(knot, "t");
            cJSON* pos = cJSON_GetObjectItem(knot, "pos_steps");
            cJSON* vel = cJSON_GetObjectItem(knot, "vel_sps");
            
            if (t && pos && vel) {
                k.t = (float)t->valuedouble;
                k.pos_steps = pos->valueint;
                k.vel_sps = (float)vel->valuedouble;
                out.knots[idx].push_back(k);
                
                if (k.t > max_time) max_time = k.t;
            }
        }
        
        ESP_LOGD(TAG, "Motor %d: %zu knots", idx, out.knots[idx].size());
    }
    
    out.duration_sec = max_time;
    out.loop_count = 1;  // Default: play once
    out.valid = true;
    
    cJSON_Delete(root);
    
    // Validate: each motor needs at least 2 knots
    for (int i = 0; i < NUM_MOTORS; i++) {
        if (out.knots[i].size() < 2) {
            ESP_LOGE(TAG, "Motor %d has < 2 knots", i);
            out.valid = false;
            return ESP_ERR_INVALID_ARG;
        }
    }
    
    return ESP_OK;
}

// ============================================================================
// SPIFFS Operations
// ============================================================================

esp_err_t StepperMotorComponent::saveChoreographyToSpiffs(const std::string& filename, 
                                                          const uint8_t* data, size_t len) {
    std::string filepath = std::string(SPIFFS_BASE_PATH) + "/" + filename;
    
    FILE* f = fopen(filepath.c_str(), "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", filepath.c_str());
        return ESP_FAIL;
    }
    
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    
    if (written != len) {
        ESP_LOGE(TAG, "Write failed: %zu/%zu bytes", written, len);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Saved %zu bytes to %s", len, filepath.c_str());
    return ESP_OK;
}

esp_err_t StepperMotorComponent::loadChoreographyFromSpiffs(const std::string& filename, 
                                                            Choreography& out) {
    std::string filepath = std::string(SPIFFS_BASE_PATH) + "/" + filename;
    
    FILE* f = fopen(filepath.c_str(), "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for reading", filepath.c_str());
        return ESP_FAIL;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > (long)MAX_CHOREOGRAPHY_SIZE) {
        ESP_LOGE(TAG, "Invalid file size: %ld", size);
        fclose(f);
        return ESP_FAIL;
    }
    
    // Read file
    std::vector<char> buffer(size + 1);
    size_t read = fread(buffer.data(), 1, size, f);
    fclose(f);
    
    if (read != (size_t)size) {
        ESP_LOGE(TAG, "Read failed: %zu/%ld bytes", read, size);
        return ESP_FAIL;
    }
    
    buffer[size] = '\0';
    
    // Parse JSON
    return parseChoreographyJson(buffer.data(), size, out);
}

// ============================================================================
// Playback Control
// ============================================================================

esp_err_t StepperMotorComponent::startPlayback() {
    if (!activeChoreography.valid) {
        ESP_LOGE(TAG, "No valid choreography loaded");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (state == PlaybackState::E_STOP) {
        ESP_LOGE(TAG, "Cannot start playback in E-STOP state");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Reset playback state
    playbackStartUs = esp_timer_get_time();
    loopsRemaining = activeChoreography.loop_count;
    
    for (int i = 0; i < NUM_MOTORS; i++) {
        currentKnotIndex[i] = 0;
        targetPosition[i] = activeChoreography.knots[i][0].pos_steps;
    }
    
    // Enable drivers
    if (hal) {
        hal->setEnabled(0xFF, true);
    }
    
    // Start timer
    uint64_t period_us = (uint64_t)(1000000.0f / evalRateHz->getValue(0, 0));
    esp_timer_start_periodic(evalTimer, period_us);
    
    transitionTo(PlaybackState::PLAYING);
    playing->setValueQuiet(0, 0, true);
    
    ESP_LOGI(TAG, "Playback started (period: %llu µs)", period_us);
    return ESP_OK;
}

void StepperMotorComponent::stopPlayback() {
    esp_timer_stop(evalTimer);
    
    transitionTo(PlaybackState::IDLE);
    playing->setValueQuiet(0, 0, false);
    
    ESP_LOGI(TAG, "Playback stopped");
}

void StepperMotorComponent::emergencyStop() {
    esp_timer_stop(evalTimer);
    
    // Disable all drivers immediately
    if (hal) {
        hal->setEnabled(0xFF, false);
    }
    
    transitionTo(PlaybackState::E_STOP);
    playing->setValueQuiet(0, 0, false);
    enableDrivers->setValueQuiet(0, 0, false);
    
    ESP_LOGW(TAG, "EMERGENCY STOP");
}

void StepperMotorComponent::clearEmergencyStop() {
    if (state != PlaybackState::E_STOP) {
        return;
    }
    
    transitionTo(PlaybackState::IDLE);
    ESP_LOGI(TAG, "E-STOP cleared");
}

esp_err_t StepperMotorComponent::startHoming() {
    if (state == PlaybackState::PLAYING || state == PlaybackState::E_STOP) {
        ESP_LOGE(TAG, "Cannot home in current state");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Initialize homing state
    for (int i = 0; i < NUM_MOTORS; i++) {
        homingInProgress[i] = true;
        homingDirection[i] = -1;  // Pay out (toward limit switch)
    }
    homingStartMs = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Enable drivers
    if (hal) {
        hal->setEnabled(0xFF, true);
    }
    
    transitionTo(PlaybackState::HOMING);
    isHomed->setValueQuiet(0, 0, false);
    
    // Start timer for homing steps
    esp_timer_start_periodic(evalTimer, 1000);  // 1 kHz for homing
    
    ESP_LOGI(TAG, "Homing started");
    return ESP_OK;
}

// ============================================================================
// Spline Evaluation
// ============================================================================

float StepperMotorComponent::hermiteEval(float t, const SplineKnot& k0, const SplineKnot& k1) {
    float dt = k1.t - k0.t;
    if (dt < 1e-9f) return (float)k0.pos_steps;
    
    float s = (t - k0.t) / dt;
    float s2 = s * s;
    float s3 = s2 * s;
    
    float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    float h10 = s3 - 2.0f * s2 + s;
    float h01 = -2.0f * s3 + 3.0f * s2;
    float h11 = s3 - s2;
    
    return h00 * k0.pos_steps + h10 * (k0.vel_sps * dt) + 
           h01 * k1.pos_steps + h11 * (k1.vel_sps * dt);
}

int32_t StepperMotorComponent::evaluateMotorPosition(uint8_t motor_index, float t) {
    const auto& knots = activeChoreography.knots[motor_index];
    uint16_t& knot_idx = currentKnotIndex[motor_index];
    
    // Advance knot window if needed
    while (knot_idx < knots.size() - 2 && knots[knot_idx + 1].t <= t) {
        knot_idx++;
    }
    
    // Clamp to valid range
    if (knot_idx >= knots.size() - 1) {
        knot_idx = knots.size() - 2;
    }
    
    const SplineKnot& k0 = knots[knot_idx];
    const SplineKnot& k1 = knots[knot_idx + 1];
    
    return (int32_t)roundf(hermiteEval(t, k0, k1));
}

// ============================================================================
// ISR Callback
// ============================================================================

void IRAM_ATTR StepperMotorComponent::evalTimerCallback(void* arg) {
    StepperMotorComponent* self = static_cast<StepperMotorComponent*>(arg);
    self->evalTimerISR();
}

void IRAM_ATTR StepperMotorComponent::evalTimerISR() {
    if (state == PlaybackState::HOMING) {
        homingStep();
        return;
    }
    
    if (state != PlaybackState::PLAYING || !activeChoreography.valid) {
        return;
    }
    
    // Calculate current time
    int64_t now_us = esp_timer_get_time();
    float t = (float)(now_us - playbackStartUs) * 1e-6f;
    
    // Check for end of choreography
    if (t >= activeChoreography.duration_sec) {
        if (loopsRemaining > 0) {
            loopsRemaining--;
        }
        
        if (loopsRemaining == 0) {
            // Stop playback (can't call stopPlayback from ISR, set flag)
            stopRequested = true;
            return;
        }
        
        // Loop: reset time
        playbackStartUs = now_us;
        t = 0;
        for (int i = 0; i < NUM_MOTORS; i++) {
            currentKnotIndex[i] = 0;
        }
    }
    
    // Evaluate splines and step motors
    for (uint8_t i = 0; i < NUM_MOTORS; i++) {
        targetPosition[i] = evaluateMotorPosition(i, t);
        int32_t delta = targetPosition[i] - currentPosition[i];
        
        if (delta != 0) {
            hal->setDirection(i, delta > 0);
            
            // Step toward target
            int32_t steps_to_take = (delta > 0) ? delta : -delta;
            // In a real implementation, we'd limit steps per ISR tick
            // For now, assume we can catch up
            for (int32_t s = 0; s < steps_to_take && s < 10; s++) {
                hal->step(i);
            }
            
            currentPosition[i] += (delta > 0) ? std::min(steps_to_take, 10L) 
                                              : -std::min(steps_to_take, 10L);
        }
    }
}

void StepperMotorComponent::homingStep() {
    bool all_done = true;
    
    for (uint8_t i = 0; i < NUM_MOTORS; i++) {
        if (!homingInProgress[i]) continue;
        
        all_done = false;
        
        if (hal->isLimitTriggered(i)) {
            // Hit limit switch - back off a bit
            hal->setDirection(i, true);  // Retract
            for (int s = 0; s < 50; s++) {
                hal->step(i);
            }
            currentPosition[i] = 0;
            homingInProgress[i] = false;
            ESP_LOGI(TAG, "Motor %d homed", i);
        } else {
            // Keep moving toward limit
            hal->setDirection(i, false);  // Extend/pay out
            hal->step(i);
        }
    }
    
    // Check timeout
    uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - homingStartMs;
    if (elapsed > homingTimeoutMs) {
        ESP_LOGE(TAG, "Homing timeout");
        esp_timer_stop(evalTimer);
        transitionTo(PlaybackState::IDLE);
        return;
    }
    
    if (all_done) {
        esp_timer_stop(evalTimer);
        isHomed->setValue(0, 0, true);
        transitionTo(PlaybackState::IDLE);
        ESP_LOGI(TAG, "Homing complete");
    }
}

// ============================================================================
// State Management
// ============================================================================

void StepperMotorComponent::transitionTo(PlaybackState newState) {
    state = newState;
    stateParam->setValueQuiet(0, 0, static_cast<int>(newState));
}

float StepperMotorComponent::getPlaybackProgress() const {
    if (state != PlaybackState::PLAYING || activeChoreography.duration_sec <= 0) {
        return 0.0f;
    }
    
    int64_t now_us = esp_timer_get_time();
    float t = (float)(now_us - playbackStartUs) * 1e-6f;
    return std::min(1.0f, t / activeChoreography.duration_sec);
}

// ============================================================================
// Helper Methods
// ============================================================================

int16_t StepperMotorComponent::findNextChoreographyId() const {
    int16_t id = 0;
    while (choreographyMeta.count(id)) {
        id++;
    }
    return id;
}

void StepperMotorComponent::updateStatusParams() {
    choreographyCountParam->setValueQuiet(0, 0, choreographyMeta.size());
}

void StepperMotorComponent::updateChoreographyIdsParam() {
    std::string ids;
    for (const auto& [id, meta] : choreographyMeta) {
        if (!ids.empty()) ids += ",";
        ids += std::to_string(id);
    }
    choreographyIdsParam->setValueQuiet(0, 0, ids);
}
