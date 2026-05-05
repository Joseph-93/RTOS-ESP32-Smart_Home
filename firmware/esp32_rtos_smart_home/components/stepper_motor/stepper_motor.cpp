#include "stepper_motor.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include <cstring>
#include <cmath>
#include <algorithm>

// ============================================================================
// Base64 Decoding
// ============================================================================

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
        if (c == 64) continue;
        if (c == 65) break;
        
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
// MotorTrajectory - Linear interpolation between precomputed waypoints
// ============================================================================

int32_t MotorTrajectory::getPosition(float t) {
    if (waypoints.empty()) return 0;
    if (waypoints.size() == 1) return waypoints[0].pos_steps;
    
    // Clamp to valid range
    if (t <= waypoints.front().t) return waypoints.front().pos_steps;
    if (t >= waypoints.back().t) return waypoints.back().pos_steps;
    
    // Advance index if needed (waypoints are sorted by time)
    while (currentIndex < waypoints.size() - 2 && waypoints[currentIndex + 1].t <= t) {
        currentIndex++;
    }
    
    // Linear interpolation between currentIndex and currentIndex+1
    const Waypoint& w0 = waypoints[currentIndex];
    const Waypoint& w1 = waypoints[currentIndex + 1];
    
    float dt = w1.t - w0.t;
    if (dt < 1e-9f) return w0.pos_steps;
    
    float alpha = (t - w0.t) / dt;
    return w0.pos_steps + (int32_t)(alpha * (w1.pos_steps - w0.pos_steps));
}

// ============================================================================
// Stub HAL
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
    bool isMaxLimitTriggered(uint8_t motor_index) override { return false; }
    uint8_t getMotorCount() const override { return 4; }
    uint32_t getMinPulseWidthUs() const override { return 2; }
    
    bool setMicrostepping(uint16_t divisor) override { 
        microstepping = divisor;
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

StepperMotorComponent::StepperMotorComponent(StepperMotorHAL* hal_ptr)
    : StepperMotorComponent()
{
    setHAL(hal_ptr);
}

StepperMotorComponent::StepperMotorComponent() 
    : Component("StepperMotor"),
      hal(nullptr),
      halOwned(false),
      activeChoreographyIndex(-1),
      metadataModified(false),
      state(PlaybackState::IDLE),
      cachedMaxPosition(INT32_MAX),
      cachedLimitsEnabled(false),
      cachedDriversEnabled(false),
      cachedJogTargetSpeedFP(500 * 256),
      cachedJogAccelPerTickFP(7),
      cachedJogThresholdFP(DEFAULT_ISR_RATE_HZ * 256),
      actualIsrRateHz(DEFAULT_ISR_RATE_HZ),
      motionTaskHandle(nullptr),
      stepTimer(nullptr),
      taskRunning(false),
      taskExited(true),
      stopRequested(false),
      uploadExpectedSize(0),
      uploadReceivedSize(0),
      uploadInProgress(false),
      homingPhase(HomingPhase::IDLE),
    homingPaused(false),
    homingCheckMode(false),
    homingCheckFailures(0),
      homingMotorIndex(0),
      homingBackoffTarget(0),
      savedJogSpeedFP(0)
{
    for (int i = 0; i < NUM_MOTORS; i++) {
        currentPosition[i] = 0;
        targetPosition[i] = 0;
        targetDirection[i] = false;
        jogCommand[i] = 0;
        motorIsHomed[i] = false;
        limitMinTriggered[i] = false;
        limitMaxTriggered[i] = false;
        prevTarget[i] = 0;
        lastDirection[i] = false;
        cachedSimulateLimitMin[i] = false;
        cachedSimulateLimitMax[i] = false;
        jogCurrentSpeedFP[i] = 0;
        jogAccumFP[i] = 0;
        homingPhaseStartVirtualPos[i] = 0;
        virtualToPhysicalMotorCache[i].store(i);
        physicalToVirtualMotorCache[i].store(i);
        directionSignCache[i].store(1);
        virtualMinSensorMapCache[i].store(i);
        virtualMaxSensorMapCache[i].store(i);
    }
    playbackStartUs = 0;
    loopsRemaining = 0;
}

StepperMotorComponent::~StepperMotorComponent() {
    // Stop timer first (stops ISR calls)
    if (stepTimer) {
        esp_timer_stop(stepTimer);
        esp_timer_delete(stepTimer);
        stepTimer = nullptr;
    }
    
    // Stop motion task
    taskRunning.store(false);
    if (motionTaskHandle) {
        const uint32_t timeoutMs = 200;
        const uint32_t pollIntervalMs = 10;
        uint32_t elapsed = 0;
        while (!taskExited.load() && elapsed < timeoutMs) {
            vTaskDelay(pdMS_TO_TICKS(pollIntervalMs));
            elapsed += pollIntervalMs;
        }
        if (!taskExited.load()) {
            vTaskDelete(motionTaskHandle);
        }
        motionTaskHandle = nullptr;
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
    ESP_LOGI(TAG, "Initializing StepperMotorComponent (3-tier architecture)");
    
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
    
    // Initialize SPIFFS
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE_PATH,
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    };
    err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
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
        cachedDriversEnabled.store(val);  // Cache for ISR
        if (hal) {
            hal->setEnabled(0xFF, val);
        }
    });
    
    stateParam = addIntParam("state", 1, 1, 0, 5, 0, true);
    
    // Playback status (read-only)
    playbackProgressParam = addFloatParam("playbackProgress", 1, 1, 0.0f, 1.0f, 0.0f, true);
    playbackTimeParam = addFloatParam("playbackTime", 1, 1, 0.0f, 86400.0f, 0.0f, true);
    choreographyCountParam = addIntParam("choreographyCount", 1, 1, 0, 1000, 0, true);
    
    // Error reporting (read-only)
    errorMessage = addStringParam("errorMessage", 1, 1, "", true);
    
    // Motor positions (read-only)
    motorPositions = addIntParam("motorPositions", NUM_MOTORS, 1, INT32_MIN, INT32_MAX, 0, true);
    motorTargets = addIntParam("motorTargets", NUM_MOTORS, 1, INT32_MIN, INT32_MAX, 0, true);
    
    // Manual jog control
    for (int i = 0; i < NUM_MOTORS; i++) {
        std::string motorName = "motor" + std::to_string(i);
        
        jogCommandParams[i] = addIntParam(motorName + "JogCommand", 1, 1, -1, 1, 0);
        motorHomedParams[i] = addBoolParam(motorName + "Homed", 1, 1, false, true);
    }

    // Virtual axis mapping
    virtualToPhysicalMotorParam = addIntParam("virtualToPhysicalMotor", 1, 4, 0, 3, 0);
    virtualDirectionInvertedParam = addBoolParam("virtualDirectionInverted", 1, 4, false);
    virtualMinSensorMapParam = addIntParam("virtualMinSensorMap", 1, 4, 0, 3, 0);
    virtualMaxSensorMapParam = addIntParam("virtualMaxSensorMap", 1, 4, 0, 3, 0);

    for (int v = 0; v < NUM_MOTORS; v++) {
        virtualToPhysicalMotorParam->setValue(0, v, v);
        virtualMinSensorMapParam->setValue(0, v, v);
        virtualMaxSensorMapParam->setValue(0, v, v);
    }

    virtualToPhysicalMotorParam->setOnChange([this](size_t, size_t, int32_t) {
        if (applyVirtualMotorMappingFromParams()) {
            metadataModified = true;
        }
    });

    virtualDirectionInvertedParam->setOnChange([this](size_t, size_t, bool) {
        applyDirectionMappingFromParams();
        metadataModified = true;
    });

    virtualMinSensorMapParam->setOnChange([this](size_t, size_t, int32_t) {
        applyVirtualSensorMappingFromParams();
        metadataModified = true;
    });

    virtualMaxSensorMapParam->setOnChange([this](size_t, size_t, int32_t) {
        applyVirtualSensorMappingFromParams();
        metadataModified = true;
    });

    applyVirtualMotorMappingFromParams();
    applyDirectionMappingFromParams();
    applyVirtualSensorMappingFromParams();
    
    jogSpeedParam = addIntParam("jogSpeed", 1, 1, 10, 5000, 500);
    jogSpeedParam->setOnChange([this](size_t, size_t, int32_t) {
        updateJogParams();
    });
    jogAccelerationParam = addIntParam("jogAcceleration", 1, 1, 10, 100000, 1000);
    jogAccelerationParam->setOnChange([this](size_t, size_t, int32_t) {
        updateJogParams();
    });
    backoffDistanceParam = addIntParam("backoffDistance", 1, 1, 10, 200, 50);
    
    // Homing control
    homingSpeedParam = addIntParam("homingSpeed", 1, 1, 1, 1000, 50);
    startHomingParam = addBoolParam("startHoming", 1, 1, false);
    startHomingParam->setOnChange([this](size_t, size_t, bool val) {
        if (val) {
            startHomingParam->setValue(0, 0, false);  // Auto-reset trigger
            beginHoming();
        }
    });
    startHomingCheckParam = addBoolParam("startHomingCheck", 1, 1, false);
    startHomingCheckParam->setOnChange([this](size_t, size_t, bool val) {
        if (val) {
            startHomingCheckParam->setValue(0, 0, false);  // Auto-reset trigger
            beginHomingCheck();
        }
    });
    homingCheckToleranceParam = addIntParam("homingCheckTolerance", 1, 1, 5, 500, 50);
    abortHomingParam = addBoolParam("abortHoming", 1, 1, false);
    abortHomingParam->setOnChange([this](size_t, size_t, bool val) {
        if (val) {
            abortHomingParam->setValue(0, 0, false);  // Auto-reset trigger
            abortHoming();
        }
    });
    homingMotorParam = addIntParam("homingMotor", 1, 1, -1, 3, -1, true);  // read-only
    pauseHomingParam = addBoolParam("pauseHoming", 1, 1, false);
    pauseHomingParam->setOnChange([this](size_t, size_t, bool val) {
        if (state.load() != PlaybackState::HOMING) {
            // Pause only applies during active homing.
            if (val) {
                pauseHomingParam->setValue(0, 0, false);
            }
            homingPaused = false;
            return;
        }

        if (val) {
            homingPaused = true;
            for (int m = 0; m < NUM_MOTORS; m++) {
                jogCommand[m].store(0);
            }
            ESP_LOGI(TAG, "Homing paused (manual jog enabled)");
        } else {
            homingPaused = false;
            applyHomingJogPattern();
            ESP_LOGI(TAG, "Homing resumed");
        }
    });
    
    // Limit switch simulation (1×4: one bool per motor)
    simulateLimitMin = addBoolParam("simulateLimitMin", 1, 4, false);
    simulateLimitMin->setOnChange([this](size_t row, size_t col, bool val) {
        cachedSimulateLimitMin[col].store(val);
        ESP_LOGI(TAG, "Motor %zu simulate MIN limit: %s", col, val ? "TRIGGERED" : "clear");
    });
    simulateLimitMax = addBoolParam("simulateLimitMax", 1, 4, false);
    simulateLimitMax->setOnChange([this](size_t row, size_t col, bool val) {
        cachedSimulateLimitMax[col].store(val);
        ESP_LOGI(TAG, "Motor %zu simulate MAX limit: %s", col, val ? "TRIGGERED" : "clear");
    });
    
    limitMinState = addBoolParam("limitMinState", 1, 4, false, true);  // read-only, broadcast on change
    limitMaxState = addBoolParam("limitMaxState", 1, 4, false, true);  // read-only, broadcast on change
    
    moveToHomeParam = addBoolParam("moveToHome", 1, 1, false);
    moveToHomeParam->setOnChange([this](size_t, size_t, bool val) {
        if (val && allMotorsHomed()) {
            ESP_LOGI(TAG, "Move to home position requested");
            moveToHomeParam->setValue(0, 0, false);
            // moveToHomePosition() will be called by task
        }
    });
    
    // Emergency stop
    eStopCommand = addBoolParam("eStopCommand", 1, 1, false);
    eStopCommand->setOnChange([this](size_t, size_t, bool val) {
        if (val) {
            emergencyStop();
            eStopCommand->setValue(0, 0, false);
        }
    });
    
    eStopClear = addBoolParam("eStopClear", 1, 1, false);
    eStopClear->setOnChange([this](size_t, size_t, bool val) {
        if (val) {
            clearEmergencyStop();
            eStopClear->setValue(0, 0, false);
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
            uploadCommitParam->setValue(0, 0, false);
        }
    });
    
    // Delete/Query parameters
    deleteChoreographyParam = addIntParam("deleteChoreography", 1, 1, -1, 1000, -1);
    deleteChoreographyParam->setOnChange([this](size_t, size_t, int32_t val) {
        if (val >= 0) {
            deleteChoreography(val);
            deleteChoreographyParam->setValue(0, 0, -1);
        }
    });
    
    queryChoreographyIndex = addIntParam("queryChoreographyIndex", 1, 1, -1, 1000, -1);
    queryChoreographyIndex->setOnChange([this](size_t, size_t, int32_t val) {
        if (val >= 0 && choreographyMeta.count(val)) {
            const auto& meta = choreographyMeta.at(val);
            queryChoreographyName->setValue(0, 0, meta.name);
            queryChoreographyDuration->setValue(0, 0, meta.duration_sec);
            queryChoreographyLoop->setValue(0, 0, meta.loop_count);
        }
    });
    
    queryChoreographyName = addStringParam("queryChoreographyName", 1, 1, "", true);
    queryChoreographyDuration = addFloatParam("queryChoreographyDuration", 1, 1, 0, 86400, 0, true);
    queryChoreographyLoop = addIntParam("queryChoreographyLoop", 1, 1, -1, 10000, 1, true);
    choreographyIdsParam = addStringParam("choreographyIds", 1, 1, "", true);
    
    // Configuration
    chunkSizeParam = addIntParam("chunkSize", 1, 1, 0, 65536, UPLOAD_CHUNK_SIZE, true);
    
    isrRateHz = addIntParam("isrRateHz", 1, 1, 1000, MAX_ISR_RATE_HZ, DEFAULT_ISR_RATE_HZ);
    
    maxRpmParam = addIntParam("maxRpm", 1, 1, 1, 3000, 1200);
    maxRpmParam->setOnChange([this](size_t, size_t, int32_t val) {
        uint32_t newRate = calculateRequiredIsrRate();
        isrRateHz->setValue(0, 0, newRate);
        restartStepTimer(newRate);  // Actually restart — also calls updateJogParams
        ESP_LOGI(TAG, "Max RPM set to %ld, ISR rate now %lu Hz", val, newRate);
    });
    
    microsteppingParam = addIntParam("microstepping", 1, 1, 1, 256, 16);
    microsteppingParam->setOnChange([this](size_t, size_t, int32_t val) {
        if (hal) {
            if (!hal->setMicrostepping(val)) {
                microsteppingParam->setValue(0, 0, hal->getMicrostepping());
            } else {
                uint32_t newRate = calculateRequiredIsrRate();
                isrRateHz->setValue(0, 0, newRate);
                restartStepTimer(newRate);  // Actually restart — also calls updateJogParams
            }
        }
    });
    
    microsteppingConfigurable = addBoolParam("microsteppingConfigurable", 1, 1, false, true);
    
    // Set initial microstepping
    if (hal) {
        hal->setMicrostepping(16);
        microsteppingConfigurable->setValue(0, 0, hal->isMicrosteppingSoftwareConfigurable());
    }
    
    // Room dimensions & position limits
    roomDimensionX = addFloatParam("roomDimensionX", 1, 1, 0.5f, 20.0f, 3.0f);
    roomDimensionX->setOnChange([this](size_t, size_t, float) {
        updateMaxPositionFromRoomDimensions();
        checkPositionLimitsAfterRoomChange();
    });
    
    roomDimensionY = addFloatParam("roomDimensionY", 1, 1, 0.5f, 20.0f, 2.5f);
    roomDimensionY->setOnChange([this](size_t, size_t, float) {
        updateMaxPositionFromRoomDimensions();
        checkPositionLimitsAfterRoomChange();
    });
    
    roomDimensionZ = addFloatParam("roomDimensionZ", 1, 1, 0.5f, 20.0f, 2.5f);
    roomDimensionZ->setOnChange([this](size_t, size_t, float) {
        updateMaxPositionFromRoomDimensions();
        checkPositionLimitsAfterRoomChange();
    });
    
    maxPositionSteps = addIntParam("maxPositionSteps", 1, 1, 0, INT32_MAX, 0, true);  // Read-only
    softLimitsEnabled = addBoolParam("softLimitsEnabled", 1, 1, true);  // Enabled by default
    softLimitsEnabled->setOnChange([this](size_t, size_t, bool val) {
        cachedLimitsEnabled.store(val);  // Cache for ISR
        ESP_LOGI(TAG, "Soft position limits %s", val ? "enabled" : "disabled");
    });
    cachedLimitsEnabled.store(true);  // Initialize cache
    
    spoolRadius = addFloatParam("spoolRadius", 1, 1, 0.005f, 0.100f, 0.050f);
    
    // Calculate initial max position
    updateMaxPositionFromRoomDimensions();
    
    // Create step timer (ISR dispatch)
    esp_timer_create_args_t timer_args = {
        .callback = stepTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "stepper_step",
        .skip_unhandled_events = true
    };
    
    err = esp_timer_create(&timer_args, &stepTimer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create step timer: %d", err);
    }
    
    // Calculate initial ISR rate and jog params
    uint32_t rate = calculateRequiredIsrRate();
    isrRateHz->setValue(0, 0, rate);
    actualIsrRateHz.store(rate);  // Must be set BEFORE updateJogParams uses it
    updateJogParams();
    
    // Start motion task immediately — it runs forever and handles:
    //   - Limit switch GPIO polling (always)
    //   - Jog back-off handling (always)
    //   - Playback position updates (when PLAYING)
    taskRunning.store(true);
    taskExited.store(false);
    BaseType_t result = xTaskCreatePinnedToCore(
        motionTaskWrapper,
        "stepper_motion",
        8192,
        this,
        tskIDLE_PRIORITY + 3,
        &motionTaskHandle,
        1   // Core 1 (away from WiFi)
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create motion task!");
    }
    
    // Start step timer immediately — ISR handles both jog stepping
    // and playback stepping based on current state
    uint64_t period_us = 1000000 / rate;
    esp_timer_start_periodic(stepTimer, period_us);
    
    ESP_LOGI(TAG, "StepperMotorComponent initialized");
    ESP_LOGI(TAG, "  Waypoint interval: %.3f sec (%d Hz)", WAYPOINT_INTERVAL_SEC, (int)(1.0f/WAYPOINT_INTERVAL_SEC));
    ESP_LOGI(TAG, "  Task rate: %lu Hz", TASK_RATE_HZ);
    ESP_LOGI(TAG, "  ISR rate: %lu Hz (period: %llu µs)", rate, period_us);
}

// ============================================================================
// ISR Rate Calculation
// ============================================================================

uint32_t StepperMotorComponent::calculateRequiredIsrRate() const {
    // Calculate required pulse rate for max RPM
    // pulses/sec = (RPM / 60) * steps_per_rev * microstepping
    
    uint32_t rpm = maxRpmParam ? maxRpmParam->getValue(0, 0) : 1200;
    uint32_t microstepping = microsteppingParam ? microsteppingParam->getValue(0, 0) : 16;
    uint32_t steps_per_rev = 200;  // Standard NEMA 17
    
    uint32_t steps_per_sec = (rpm * steps_per_rev * microstepping) / 60;
    
    // Add 25% headroom
    uint32_t rate = (steps_per_sec * 125) / 100;
    
    // Clamp to valid range
    if (rate < 1000) rate = 1000;
    if (rate > MAX_ISR_RATE_HZ) rate = MAX_ISR_RATE_HZ;
    
    return rate;
}

void StepperMotorComponent::updateJogParams() {
    uint32_t rate  = actualIsrRateHz.load();  // Use ACTUAL running timer rate
    uint32_t speed = jogSpeedParam        ? (uint32_t)jogSpeedParam->getValue(0, 0)        : 500;
    uint32_t accel = jogAccelerationParam ? (uint32_t)jogAccelerationParam->getValue(0, 0) : 1000;
    if (speed < 1) speed = 1;
    if (accel < 1) accel = 1;

    // Target speed in fixed-point (pulses/sec × 256)
    cachedJogTargetSpeedFP.store(speed * 256);

    // Accumulator threshold: one full second = isrRate × 256
    cachedJogThresholdFP.store(rate * 256);

    // Speed change per ISR tick in fixed-point: ceil((accel × 256) / rate)
    uint32_t aptp = (accel * 256 + rate - 1) / rate;
    if (aptp < 1) aptp = 1;
    cachedJogAccelPerTickFP.store(aptp);

    ESP_LOGD(TAG, "Jog params: speed=%lu sps, accel=%lu sps², accelPerTick=%lu/256, threshold=%lu",
             speed, accel, aptp, rate * 256);
}

void StepperMotorComponent::restartStepTimer(uint32_t rate_hz) {
    if (!stepTimer) return;
    if (rate_hz < 1000) rate_hz = 1000;
    if (rate_hz > MAX_ISR_RATE_HZ) rate_hz = MAX_ISR_RATE_HZ;
    esp_timer_stop(stepTimer);
    uint64_t period_us = 1000000ULL / rate_hz;
    esp_timer_start_periodic(stepTimer, period_us);
    actualIsrRateHz.store(rate_hz);
    updateJogParams();
    ESP_LOGI(TAG, "Step timer restarted: %lu Hz (%llu us period)", rate_hz, period_us);
}

// ============================================================================
// Trajectory Precomputation
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

int32_t StepperMotorComponent::evaluateSplineAt(const std::vector<SplineKnot>& knots, float t) {
    if (knots.empty()) return 0;
    if (knots.size() == 1) return knots[0].pos_steps;
    
    // Find the knot interval containing t
    size_t idx = 0;
    while (idx < knots.size() - 2 && knots[idx + 1].t <= t) {
        idx++;
    }
    
    // Clamp
    if (idx >= knots.size() - 1) {
        idx = knots.size() - 2;
    }
    
    return (int32_t)roundf(hermiteEval(t, knots[idx], knots[idx + 1]));
}

esp_err_t StepperMotorComponent::precomputeTrajectory() {
    if (!activeChoreography.valid) {
        ESP_LOGE(TAG, "Cannot precompute: no valid choreography");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Precomputing trajectory (duration: %.2fs, interval: %.3fs)",
             activeChoreography.duration_sec, WAYPOINT_INTERVAL_SEC);
    
    transitionTo(PlaybackState::PRECOMPUTING);
    
    float duration = activeChoreography.duration_sec;
    size_t numWaypoints = (size_t)(duration / WAYPOINT_INTERVAL_SEC) + 2;
    
    // Estimate memory usage
    size_t memBytes = numWaypoints * NUM_MOTORS * sizeof(Waypoint);
    ESP_LOGI(TAG, "  Waypoints: %zu per motor, %zu KB total", numWaypoints, memBytes / 1024);
    
    // Check heap before allocation
    size_t freeHeap = esp_get_free_heap_size();
    size_t minHeapReserve = 32 * 1024;  // Keep 32KB for stack/other operations
    
    if (memBytes > freeHeap - minHeapReserve) {
        ESP_LOGE(TAG, "Not enough heap for trajectory: need %zu KB, have %zu KB free (reserve %zu KB)",
                 memBytes / 1024, freeHeap / 1024, minHeapReserve / 1024);
        
        char errMsg[128];
        snprintf(errMsg, sizeof(errMsg), 
            "Out of memory: trajectory needs %zu KB, only %zu KB available",
            memBytes / 1024, (freeHeap - minHeapReserve) / 1024);
        errorMessage->setValue(0, 0, errMsg);
        
        transitionTo(PlaybackState::E_STOP);
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "  Heap check OK: %zu KB free (need %zu KB + %zu KB reserve)",
             freeHeap / 1024, memBytes / 1024, minHeapReserve / 1024);
    
    // Clear and reserve
    trajectory.valid = false;
    trajectory.duration_sec = duration;
    trajectory.waypointInterval_sec = WAYPOINT_INTERVAL_SEC;
    
    for (int m = 0; m < NUM_MOTORS; m++) {
        trajectory.motors[m].waypoints.clear();
        trajectory.motors[m].waypoints.reserve(numWaypoints);
        trajectory.motors[m].currentIndex = 0;
        
        const auto& knots = activeChoreography.knots[m];
        
        // Sample the spline at regular intervals
        for (size_t i = 0; i < numWaypoints; i++) {
            float t = i * WAYPOINT_INTERVAL_SEC;
            if (t > duration) t = duration;
            
            Waypoint wp;
            wp.t = t;
            wp.pos_steps = evaluateSplineAt(knots, t);
            trajectory.motors[m].waypoints.push_back(wp);
        }
        
        ESP_LOGD(TAG, "  Motor %d: %zu waypoints", m, trajectory.motors[m].waypoints.size());
    }
    
    trajectory.valid = true;
    transitionTo(PlaybackState::ARMED);
    
    ESP_LOGI(TAG, "Trajectory precomputation complete");
    return ESP_OK;
}

// ============================================================================
// Room Dimensions & Position Limits
// ============================================================================

void StepperMotorComponent::updateMaxPositionFromRoomDimensions() {
    float Lx = roomDimensionX->getValue(0, 0);
    float Ly = roomDimensionY->getValue(0, 0);
    float Lz = roomDimensionZ->getValue(0, 0);
    
    // Max cable length = room diagonal (corner to corner)
    float diagonal = sqrt(Lx*Lx + Ly*Ly + Lz*Lz);
    
    // Convert to steps using choreography motor config
    float r_spool = activeChoreography.r_spool;
    uint16_t steps_per_rev = activeChoreography.steps_per_rev;
    uint16_t microstep = activeChoreography.microstep_divisor;
    
    float circumference = 2.0f * M_PI * r_spool;
    float revolutions = diagonal / circumference;
    int32_t maxSteps = (int32_t)(revolutions * steps_per_rev * microstep);
    
    maxPositionSteps->setValue(0, 0, maxSteps);
    cachedMaxPosition.store(maxSteps);  // Cache for ISR
    
    ESP_LOGI(TAG, "Room dimensions: %.2f × %.2f × %.2f m, diagonal: %.2f m, max position: %ld steps",
             Lx, Ly, Lz, diagonal, maxSteps);
}

void StepperMotorComponent::checkPositionLimitsAfterRoomChange() {
    if (!softLimitsEnabled->getValue(0, 0)) {
        return;  // Limits disabled, no check needed
    }
    
    int32_t maxPos = maxPositionSteps->getValue(0, 0);
    bool anyMotorOutOfRange = false;
    
    for (int m = 0; m < NUM_MOTORS; m++) {
            int32_t p = getPhysicalMotorFromVirtual(m);
            if (p < 0 || p >= NUM_MOTORS) {
                continue;
            }
            int32_t current = toVirtualPosition(m, currentPosition[p].load());
            if (current > maxPos) {
                ESP_LOGW(TAG, "Motor %d position %ld exceeds new max %ld!", m, current, maxPos);
            anyMotorOutOfRange = true;
        }
    }
    
    if (anyMotorOutOfRange) {
        char errMsg[256];
        snprintf(errMsg, sizeof(errMsg),
            "Room dimensions reduced! Motors exceed new max position (%ld steps). "
            "STOPPED: Use manual jog to bring motors within range.",
            maxPos);
        errorMessage->setValue(0, 0, errMsg);
        
        ESP_LOGE(TAG, "%s", errMsg);
        emergencyStop();  // Stop all movement
    }
}

// ============================================================================
// Motion Planning Task (Tier 2)
// ============================================================================

void StepperMotorComponent::motionTaskWrapper(void* arg) {
    StepperMotorComponent* self = static_cast<StepperMotorComponent*>(arg);
    self->motionTask();
}

void StepperMotorComponent::motionTask() {
    ESP_LOGI(TAG, "Motion task started on core %d", xPortGetCoreID());
    
    const TickType_t taskPeriod = pdMS_TO_TICKS(1000 / TASK_RATE_HZ);
    TickType_t lastWakeTime = xTaskGetTickCount();
    
    while (taskRunning.load()) {
        PlaybackState currentState = state.load();
        
        // ====================================================================
        // Homing state machine
        // ====================================================================
        if (currentState == PlaybackState::HOMING) {
            homingTick();
        }
        
        // ====================================================================
        // Check if all homed and user requested move to home
        // ====================================================================
        if (moveToHomeParam->getValue(0, 0) && allMotorsHomed()) {
            ESP_LOGI(TAG, "Moving all motors to home position");
            moveToHomePosition();
            moveToHomeParam->setValue(0, 0, false);
        }
        
        // ====================================================================
        // Read jog parameter updates (blocked during active homing, allowed when paused)
        // ====================================================================
        if (currentState != PlaybackState::HOMING || homingPaused) {
            for (int p = 0; p < NUM_MOTORS; p++) {
                jogCommand[p].store(0);
            }
            for (int v = 0; v < NUM_MOTORS; v++) {
                int8_t cmdVirtual = (int8_t)jogCommandParams[v]->getValue(0, 0);
                int8_t p = getPhysicalMotorFromVirtual(v);
                if (p < 0 || p >= NUM_MOTORS) {
                    continue;
                }
                jogCommand[p].store(virtualJogToPhysical(v, cmdVirtual));
            }
        }
        
        // ====================================================================
        // Poll limit switch states → mirror to parameters for GUI
        // Both hardware GPIO AND software simulation.
        // Runs every tick regardless of playback state.
        // ====================================================================
        for (int v = 0; v < NUM_MOTORS; v++) {
            bool hwMin = readVirtualMinLimit(v);
            bool hwMax = readVirtualMaxLimit(v);
            
            if (hwMin != limitMinState->getValue(0, v)) {
                limitMinState->setValue(0, v, hwMin);
            }
            if (hwMax != limitMaxState->getValue(0, v)) {
                limitMaxState->setValue(0, v, hwMax);
            }
        }
        
        // ====================================================================
        // Report motor positions to GUI (all states, throttled to 10 Hz)
        // ====================================================================
        static uint32_t positionReportCounter = 0;
        if (++positionReportCounter >= TASK_RATE_HZ / 10) {
            positionReportCounter = 0;
            for (int v = 0; v < NUM_MOTORS; v++) {
                int8_t p = getPhysicalMotorFromVirtual(v);
                if (p < 0 || p >= NUM_MOTORS) {
                    continue;
                }
                motorPositions->setValue(v, 0, toVirtualPosition(v, currentPosition[p].load()));
            }
        }
        
        // ====================================================================
        // Playback mode
        // ====================================================================
        if (currentState == PlaybackState::PLAYING && trajectory.valid) {
            // Calculate current playback time
            int64_t now_us = esp_timer_get_time();
            int64_t start_us = playbackStartUs.load();
            float t = (float)(now_us - start_us) * 1e-6f;
            
            // Check for end of choreography
            if (t >= trajectory.duration_sec) {
                int16_t loops = loopsRemaining.load();
                if (loops > 0) {
                    loops--;
                    loopsRemaining.store(loops);
                }
                
                if (loops == 0) {
                    stopRequested.store(true);
                    continue;
                }
                
                // Loop: reset time
                playbackStartUs.store(now_us);
                t = 0;
                trajectory.reset();
            }
            
            // Update target positions from precomputed trajectory
            for (int v = 0; v < NUM_MOTORS; v++) {
                int32_t newTargetVirtual = trajectory.motors[v].getPosition(t);

                if (cachedLimitsEnabled.load()) {
                    int32_t maxPos = cachedMaxPosition.load();
                    if (newTargetVirtual > maxPos) {
                        newTargetVirtual = maxPos;
                    }
                    if (newTargetVirtual < 0) {
                        newTargetVirtual = 0;
                    }
                }
                
                // Safety check: detect velocity that would exceed motor capability
                int32_t velocity = abs(newTargetVirtual - prevTarget[v]);
                if (velocity > MAX_VELOCITY_STEPS_PER_TICK) {
                    char errMsg[128];
                    snprintf(errMsg, sizeof(errMsg), 
                        "Motor %d velocity too high: %ld steps/tick (max %lu) - E-STOP",
                        v, (long)velocity, (unsigned long)MAX_VELOCITY_STEPS_PER_TICK);
                    errorMessage->setValue(0, 0, errMsg);
                    ESP_LOGE(TAG, "%s", errMsg);
                    emergencyStop();
                    return;
                }
                
                prevTarget[v] = newTargetVirtual;

                int8_t p = getPhysicalMotorFromVirtual(v);
                if (p < 0 || p >= NUM_MOTORS) {
                    continue;
                }

                int32_t newTargetPhysical = toPhysicalPosition(v, newTargetVirtual);
                int32_t currentPhysical = currentPosition[p].load();
                
                targetPosition[p].store(newTargetPhysical);
                targetDirection[p].store(newTargetPhysical > currentPhysical);
                
                // Update read-only params (not every tick - would be too much)
                // We'll do this less frequently
            }
            
            // Update playback progress (less frequently to avoid overhead)
            static uint32_t updateCounter = 0;
            if (++updateCounter >= TASK_RATE_HZ / 10) {  // 10 Hz update
                updateCounter = 0;
                playbackProgressParam->setValue(0, 0, t / trajectory.duration_sec);
                playbackTimeParam->setValue(0, 0, t);
                
                for (int v = 0; v < NUM_MOTORS; v++) {
                    int8_t p = getPhysicalMotorFromVirtual(v);
                    if (p < 0 || p >= NUM_MOTORS) {
                        continue;
                    }
                    motorPositions->setValue(v, 0, toVirtualPosition(v, currentPosition[p].load()));
                    motorTargets->setValue(v, 0, toVirtualPosition(v, targetPosition[p].load()));
                }
            }
        }
        
        // Check for stop request
        if (stopRequested.load()) {
            stopRequested.store(false);
            stopPlayback();
        }
        
        vTaskDelayUntil(&lastWakeTime, taskPeriod);
    }
    
    ESP_LOGI(TAG, "Motion task exiting");
    taskExited.store(true);  // Signal we're done
    vTaskDelete(NULL);
}

// ============================================================================
// Step Generation ISR (Tier 3)
// ============================================================================

void IRAM_ATTR StepperMotorComponent::stepTimerCallback(void* arg) {
    StepperMotorComponent* self = static_cast<StepperMotorComponent*>(arg);
    self->stepTimerISR();
}

void IRAM_ATTR StepperMotorComponent::stepTimerISR() {
    PlaybackState currentState = state.load();
    
    // ================================================================
    // Manual jog with smooth acceleration / deceleration ramp.
    // Works in ALL states except E_STOP and PLAYING (including HOMING).
    //
    // Algorithm (fixed-point ×256 fractional accumulator):
    //   - Each ISR tick, ramp currentSpeed toward targetSpeed (accel)
    //     or toward 0 (decel when jog released).
    //   - Add currentSpeed to per-motor accumulator each tick.
    //   - When accumulator overflows threshold (= isrRate×256), step once.
    //
    // Rules:
    //   1) Drivers must be enabled
    //   2) MIN limit blocks RETRACT; MAX limit blocks PAY OUT
    //   3) On release, continue last direction while decelerating to stop
    // ================================================================
    if (currentState != PlaybackState::E_STOP &&
        currentState != PlaybackState::PLAYING &&
        cachedDriversEnabled.load()) {

        const uint32_t targetSpeedFP = cachedJogTargetSpeedFP.load();
        const uint32_t accelPerTick  = cachedJogAccelPerTickFP.load();
        const uint32_t thresholdFP   = cachedJogThresholdFP.load();

        for (uint8_t m = 0; m < NUM_MOTORS; m++) {
            int8_t jog = jogCommand[m].load();

            if (jog != 0) {
                // CRYSTAL CLEAR LIMIT LOGIC:
                // - MIN sensor only blocks RETRACT (-1 virtual direction)
                // - MAX sensor only blocks PAY OUT (+1 virtual direction)
                // - BUT: if the motor is inverted, physical directions are swapped!
                // - This allows escape from any limit corner.
                
                int8_t virtualIdx = getVirtualMotorFromPhysical(m);
                bool isInverted = false;
                if (virtualIdx >= 0 && virtualIdx < NUM_MOTORS) {
                    isInverted = (getDirectionSignForVirtual(virtualIdx) < 0);
                }
                
                const bool isRetract = (jog < 0);
                const bool isPayOut = (jog > 0);
                const bool minHit = readPhysicalMinLimitForMotor(m);
                const bool maxHit = readPhysicalMaxLimitForMotor(m);
                
                if (!isInverted) {
                    // Normal (non-inverted): physical direction = virtual semantics
                    // Block retract ONLY if MIN is hit
                    if (isRetract && minHit) {
                        jogCurrentSpeedFP[m] = 0;
                        jogAccumFP[m] = 0;
                        continue;
                    }
                    // Block payout ONLY if MAX is hit
                    if (isPayOut && maxHit) {
                        jogCurrentSpeedFP[m] = 0;
                        jogAccumFP[m] = 0;
                        continue;
                    }
                } else {
                    // Inverted: physical direction is opposite to virtual semantics
                    // Physical retract = virtual payout, check against MAX
                    if (isRetract && maxHit) {
                        jogCurrentSpeedFP[m] = 0;
                        jogAccumFP[m] = 0;
                        continue;
                    }
                    // Physical payout = virtual retract, check against MIN
                    if (isPayOut && minHit) {
                        jogCurrentSpeedFP[m] = 0;
                        jogAccumFP[m] = 0;
                        continue;
                    }
                }

                // Ramp up toward target speed
                if (jogCurrentSpeedFP[m] + accelPerTick < targetSpeedFP) {
                    jogCurrentSpeedFP[m] += accelPerTick;
                } else {
                    jogCurrentSpeedFP[m] = targetSpeedFP;
                }
            } else {
                // Jog released — ramp down (decelerate in the same direction)
                if (jogCurrentSpeedFP[m] > accelPerTick) {
                    jogCurrentSpeedFP[m] -= accelPerTick;
                } else {
                    // Fully stopped
                    jogCurrentSpeedFP[m] = 0;
                    jogAccumFP[m] = 0;
                    continue;
                }
            }

            // Advance fractional accumulator
            jogAccumFP[m] += jogCurrentSpeedFP[m];
            if (jogAccumFP[m] < thresholdFP) {
                continue;  // Not yet time for a step
            }
            jogAccumFP[m] -= thresholdFP;

            // Direction: active jog sets direction; decel continues last direction
            bool direction = (jog != 0) ? (jog > 0) : lastDirection[m];
            if (direction != lastDirection[m]) {
                hal->setDirection(m, direction);
                lastDirection[m] = direction;
            }

            hal->step(m);
            int32_t pos = currentPosition[m].load();
            currentPosition[m].store(direction ? pos + 1 : pos - 1);
        }
    }
    
    // ================================================================
    // Playback mode — chase precomputed target positions
    // ================================================================
    if (currentState != PlaybackState::PLAYING) {
        return;
    }
    
    // Step each motor toward its target (playback mode)
    // This is the hottest path - keep it minimal
    for (uint8_t m = 0; m < NUM_MOTORS; m++) {
        int32_t current = currentPosition[m].load();
        int32_t target = targetPosition[m].load();
        
        // Apply soft position limits (if enabled)
        if (cachedLimitsEnabled.load()) {
            int32_t maxPos = cachedMaxPosition.load();
            int8_t v = getVirtualMotorFromPhysical(m);
            if (v >= 0 && v < NUM_MOTORS) {
                int32_t minPhysical = toPhysicalPosition(v, 0);
                int32_t maxPhysical = toPhysicalPosition(v, maxPos);
                if (minPhysical > maxPhysical) {
                    int32_t tmp = minPhysical;
                    minPhysical = maxPhysical;
                    maxPhysical = tmp;
                }
                if (target > maxPhysical) {
                    target = maxPhysical;
                }
                if (target < minPhysical) {
                    target = minPhysical;
                }
            }
        }
        
        if (current != target) {
            // Direction is pre-computed by motion task - just read it
            bool dir = targetDirection[m].load();
            
            // Only call setDirection if it changed (minimize GPIO writes)
            if (dir != lastDirection[m]) {
                hal->setDirection(m, dir);
                lastDirection[m] = dir;
            }
            
            hal->step(m);
            currentPosition[m].store(current + (dir ? 1 : -1));
        }
    }
}

// ============================================================================
// Playback Control
// ============================================================================

esp_err_t StepperMotorComponent::startPlayback() {
    if (!trajectory.valid) {
        ESP_LOGE(TAG, "No valid trajectory - load a choreography first");
        return ESP_ERR_INVALID_STATE;
    }
    
    PlaybackState currentState = state.load();
    if (currentState == PlaybackState::E_STOP) {
        ESP_LOGE(TAG, "Cannot start in E-STOP state");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (currentState == PlaybackState::PLAYING) {
        ESP_LOGW(TAG, "Already playing");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Starting playback");
    
    // Reset trajectory indices
    trajectory.reset();
    
    // Clear error message
    errorMessage->setValue(0, 0, "");
    
    // Set initial targets and reset velocity tracking
    for (int v = 0; v < NUM_MOTORS; v++) {
        if (!trajectory.motors[v].waypoints.empty()) {
            int8_t p = getPhysicalMotorFromVirtual(v);
            if (p < 0 || p >= NUM_MOTORS) {
                continue;
            }
            int32_t initialVirtual = trajectory.motors[v].waypoints[0].pos_steps;
            int32_t initialPhysical = toPhysicalPosition(v, initialVirtual);
            targetPosition[p].store(initialPhysical);
            prevTarget[v] = initialVirtual;  // Initialize velocity tracking
        }
    }
    
    // Record start time
    playbackStartUs.store(esp_timer_get_time());
    loopsRemaining.store(activeChoreography.loop_count);
    
    // Enable drivers
    if (hal) {
        hal->setEnabled(0xFF, true);
    }
    
    // Task and timer are already running from init — just transition state
    transitionTo(PlaybackState::PLAYING);
    playing->setValue(0, 0, true);
    enableDrivers->setValue(0, 0, true);
    
    ESP_LOGI(TAG, "Playback started");
    return ESP_OK;
}

void StepperMotorComponent::stopPlayback() {
    ESP_LOGI(TAG, "Stopping playback");
    
    // Task and timer keep running — they handle jog/limit polling in IDLE state.
    // Just transition state so ISR stops chasing playback targets.
    transitionTo(PlaybackState::IDLE);
    playing->setValue(0, 0, false);
}

void StepperMotorComponent::emergencyStop() {
    ESP_LOGW(TAG, "EMERGENCY STOP");
    
    // Abort homing if active
    if (homingPhase != HomingPhase::IDLE && homingPhase != HomingPhase::DONE) {
        ESP_LOGW(TAG, "E-STOP: aborting homing sequence");
        homingPhase = HomingPhase::IDLE;
        homingMotorParam->setValue(0, 0, -1);
        cachedJogTargetSpeedFP.store(savedJogSpeedFP);  // Restore jog speed
    }
    
    // Disable drivers immediately
    if (hal) {
        hal->setEnabled(0xFF, false);
    }
    
    // Clear all jog commands so ISR stops stepping
    for (int m = 0; m < NUM_MOTORS; m++) {
        jogCommand[m].store(0);
    }
    
    // Task and timer keep running — ISR checks state and skips stepping in E_STOP.
    // Motion task still polls limit switches so you can see them in the GUI.
    playing->setValue(0, 0, false);
    enableDrivers->setValue(0, 0, false);
    transitionTo(PlaybackState::E_STOP);
}

void StepperMotorComponent::clearEmergencyStop() {
    if (state.load() != PlaybackState::E_STOP) {
        return;
    }
    
    transitionTo(PlaybackState::IDLE);
    ESP_LOGI(TAG, "E-STOP cleared");
}

// ============================================================================
// Manual Jog Helpers
// ============================================================================

bool StepperMotorComponent::allMotorsHomed() const {
    for (int i = 0; i < NUM_MOTORS; i++) {
        if (!motorIsHomed[i].load()) return false;
    }
    return true;
}

void StepperMotorComponent::moveToHomePosition() {
    // Move all motors to center position (1000 steps retracted from limit)
    const int32_t homePosition = -1000;
    
    ESP_LOGI(TAG, "Moving all motors to home position (%ld pulses)", homePosition);
    
    for (int v = 0; v < NUM_MOTORS; v++) {
        int8_t p = getPhysicalMotorFromVirtual(v);
        if (p < 0 || p >= NUM_MOTORS) {
            continue;
        }

        int32_t currentVirtual = toVirtualPosition(v, currentPosition[p].load());
        int32_t deltaVirtual = homePosition - currentVirtual;
        
        if (deltaVirtual == 0) continue;  // Already at home
        
        int8_t jogPhysical = virtualJogToPhysical(v, deltaVirtual > 0 ? 1 : -1);
        bool directionPhysical = (jogPhysical > 0);
        hal->setDirection(p, directionPhysical);
        
        for (int32_t step = 0; step < abs(deltaVirtual); step++) {
            hal->step(p);
            vTaskDelay(pdMS_TO_TICKS(2));  // 500 Hz move rate
        }
        
        currentPosition[p].store(toPhysicalPosition(v, homePosition));
        motorPositions->setValue(v, 0, homePosition);
    }
    
    ESP_LOGI(TAG, "All motors at home position");
}

// ============================================================================
// Homing Sequence
// ============================================================================

void StepperMotorComponent::captureHomingPhaseStartPositions() {
    for (int v = 0; v < NUM_MOTORS; v++) {
        int8_t p = getPhysicalMotorFromVirtual(v);
        if (p >= 0 && p < NUM_MOTORS) {
            homingPhaseStartVirtualPos[v] = toVirtualPosition(v, currentPosition[p].load());
        } else {
            homingPhaseStartVirtualPos[v] = 0;
        }
    }
}

void StepperMotorComponent::applyHomingJogPattern() {
    if (state.load() != PlaybackState::HOMING || homingPaused) {
        return;
    }

    for (int p = 0; p < NUM_MOTORS; p++) {
        jogCommand[p].store(0);
    }

    if (homingPhase == HomingPhase::RETRACT || homingPhase == HomingPhase::NEXT) {
        // Coupled payout: active motor retracts; the other motors only pay out enough
        // to follow the active motor's retract progress and keep cable tension sane.
        uint8_t activeV = homingMotorIndex;
        int8_t activeP = getPhysicalMotorFromVirtual(activeV);
        if (activeP < 0 || activeP >= NUM_MOTORS) {
            return;
        }

        int32_t activeCurrent = toVirtualPosition(activeV, currentPosition[activeP].load());
        int32_t activeDelta = homingPhaseStartVirtualPos[activeV] - activeCurrent;
        if (activeDelta < 0) {
            activeDelta = 0;
        }

        for (int v = 0; v < NUM_MOTORS; v++) {
            int8_t p = getPhysicalMotorFromVirtual(v);
            if (p < 0 || p >= NUM_MOTORS) {
                continue;
            }

            int8_t cmdVirtual = 0;
            if (v == activeV) {
                cmdVirtual = -1;  // Active motor retracts toward MIN
            } else {
                // Distribute active retract across the other 3 motors.
                int32_t desired = homingPhaseStartVirtualPos[v] + (activeDelta / 3);
                int32_t current = toVirtualPosition(v, currentPosition[p].load());
                if (current + 1 < desired) {
                    cmdVirtual = 1;
                }
            }

            jogCommand[p].store(virtualJogToPhysical(v, cmdVirtual));
        }
    } else if (homingPhase == HomingPhase::BACKOFF) {
        int8_t p = getPhysicalMotorFromVirtual(homingMotorIndex);
        if (p >= 0 && p < NUM_MOTORS) {
            jogCommand[p].store(virtualJogToPhysical(homingMotorIndex, 1));
        }
    }
}

void StepperMotorComponent::beginHoming() {
    PlaybackState cur = state.load();
    if (cur == PlaybackState::PLAYING || cur == PlaybackState::E_STOP) {
        ESP_LOGW(TAG, "Cannot start homing in state %d", (int)cur);
        return;
    }
    
    ESP_LOGI(TAG, "=== HOMING SEQUENCE START ===");
    
    // Ensure drivers are enabled
    if (!cachedDriversEnabled.load()) {
        enableDrivers->setValue(0, 0, true);
        cachedDriversEnabled.store(true);
        if (hal) hal->setEnabled(0xFF, true);
        ESP_LOGI(TAG, "Auto-enabled drivers for homing");
    }
    
    // Clear all homed flags
    for (int m = 0; m < NUM_MOTORS; m++) {
        motorIsHomed[m].store(false);
        motorHomedParams[m]->setValue(0, 0, false);
    }
    
    // Save current jog speed and apply homing speed
    savedJogSpeedFP = cachedJogTargetSpeedFP.load();
    uint32_t homingSpeed = (uint32_t)homingSpeedParam->getValue(0, 0);
    cachedJogTargetSpeedFP.store(homingSpeed * 256);
    ESP_LOGI(TAG, "Homing speed: %lu pulses/sec (saved jog speed: %lu)",
             homingSpeed, savedJogSpeedFP / 256);
    
    // Start with motor 0
    homingMotorIndex = 0;
    homingPhase = HomingPhase::RETRACT;
    homingPaused = false;
    homingCheckMode = false;
    homingCheckFailures = 0;
    pauseHomingParam->setValue(0, 0, false);
    homingMotorParam->setValue(0, 0, (int32_t)homingMotorIndex);
    captureHomingPhaseStartPositions();

    applyHomingJogPattern();
    
    transitionTo(PlaybackState::HOMING);
    ESP_LOGI(TAG, "Homing motor %d: RETRACT (others PAY OUT)", homingMotorIndex);
}

void StepperMotorComponent::beginHomingCheck() {
    PlaybackState cur = state.load();
    if (cur == PlaybackState::PLAYING || cur == PlaybackState::E_STOP) {
        ESP_LOGW(TAG, "Cannot start homing check in state %d", (int)cur);
        return;
    }
    if (!allMotorsHomed()) {
        ESP_LOGW(TAG, "Cannot start homing check: all motors must already be homed");
        errorMessage->setValue(0, 0, "Homing check requires all motors already homed.");
        return;
    }

    ESP_LOGI(TAG, "=== HOMING CHECK START ===");

    if (!cachedDriversEnabled.load()) {
        enableDrivers->setValue(0, 0, true);
        cachedDriversEnabled.store(true);
        if (hal) hal->setEnabled(0xFF, true);
        ESP_LOGI(TAG, "Auto-enabled drivers for homing check");
    }

    savedJogSpeedFP = cachedJogTargetSpeedFP.load();
    uint32_t homingSpeed = (uint32_t)homingSpeedParam->getValue(0, 0);
    cachedJogTargetSpeedFP.store(homingSpeed * 256);

    homingMotorIndex = 0;
    homingPhase = HomingPhase::RETRACT;
    homingPaused = false;
    homingCheckMode = true;
    homingCheckFailures = 0;
    pauseHomingParam->setValue(0, 0, false);
    homingMotorParam->setValue(0, 0, (int32_t)homingMotorIndex);
    captureHomingPhaseStartPositions();

    applyHomingJogPattern();

    transitionTo(PlaybackState::HOMING);
    errorMessage->setValue(0, 0, "");
    ESP_LOGI(TAG, "Homing check motor %d: RETRACT with coupled payout", homingMotorIndex);
}

void StepperMotorComponent::abortHoming() {
    if (state.load() != PlaybackState::HOMING) return;
    
    ESP_LOGW(TAG, "=== HOMING ABORTED ===");
    
    // Stop all motors
    for (int m = 0; m < NUM_MOTORS; m++) {
        jogCommand[m].store(0);
    }
    
    // Restore jog speed
    cachedJogTargetSpeedFP.store(savedJogSpeedFP);
    
    homingPaused = false;
    homingCheckMode = false;
    homingCheckFailures = 0;
    pauseHomingParam->setValue(0, 0, false);
    homingPhase = HomingPhase::IDLE;
    homingMotorParam->setValue(0, 0, -1);
    transitionTo(PlaybackState::IDLE);
}

void StepperMotorComponent::homingTick() {
    // Called at TASK_RATE_HZ (200 Hz) while state == HOMING
    if (homingPaused) {
        return;
    }
    
    switch (homingPhase) {
    
    case HomingPhase::RETRACT: {
        // Check if homing motor hit MIN limit
        uint8_t v = homingMotorIndex;
        int8_t p = getPhysicalMotorFromVirtual(v);
        if (p < 0 || p >= NUM_MOTORS) {
            emergencyStop();
            return;
        }

        applyHomingJogPattern();

        bool minHit = readVirtualMinLimit(v);
        
        if (minHit) {
            int32_t atMin = toVirtualPosition(v, currentPosition[p].load());
            ESP_LOGI(TAG, "Motor %d hit MIN limit at pos=%ld — stopping all, backing off",
                     v, (long)atMin);
            
            // Stop all motors
            for (int i = 0; i < NUM_MOTORS; i++) {
                jogCommand[i].store(0);
            }

            int32_t backoff = (int32_t)backoffDistanceParam->getValue(0, 0);
            if (homingCheckMode) {
                int32_t travelled = homingPhaseStartVirtualPos[v] - atMin;
                int32_t tolerance = homingCheckToleranceParam
                    ? homingCheckToleranceParam->getValue(0, 0)
                    : 50;
                int32_t err = (travelled > backoff) ? (travelled - backoff) : (backoff - travelled);
                if (err > tolerance) {
                    homingCheckFailures++;
                    ESP_LOGE(TAG, "Homing check FAIL motor %d: expected MIN at %ld pulses, measured %ld (|err|=%ld > tol=%ld)",
                             v, (long)backoff, (long)travelled, (long)err, (long)tolerance);
                } else {
                    ESP_LOGI(TAG, "Homing check PASS motor %d: expected %ld, measured %ld (|err|=%ld)",
                             v, (long)backoff, (long)travelled, (long)err);
                }

                // Return this motor to where it started this check phase.
                homingBackoffTarget = homingPhaseStartVirtualPos[v];
            } else {
                // Standard homing: snap to zero on MIN hit and back off to configured home offset.
                currentPosition[p].store(0);
                motorPositions->setValue(v, 0, 0);
                homingBackoffTarget = backoff;
            }

            // Start backing off just the active motor
            jogCommand[p].store(virtualJogToPhysical(v, 1));
            homingPhase = HomingPhase::BACKOFF;
            
            ESP_LOGI(TAG, "Motor %d: backing off %ld pulses to pos=%ld",
                     v, (long)(homingBackoffTarget - atMin), (long)homingBackoffTarget);
        }
        break;
    }
    
    case HomingPhase::BACKOFF: {
        uint8_t v = homingMotorIndex;
        int8_t p = getPhysicalMotorFromVirtual(v);
        if (p < 0 || p >= NUM_MOTORS) {
            emergencyStop();
            return;
        }

        int32_t pos = toVirtualPosition(v, currentPosition[p].load());
        
        if (pos >= homingBackoffTarget) {
            // Backoff complete — stop, zero position, mark homed
            jogCommand[p].store(0);

            if (homingCheckMode) {
                // Return to starting point of this check leg.
                currentPosition[p].store(toPhysicalPosition(v, homingPhaseStartVirtualPos[v]));
                motorPositions->setValue(v, 0, homingPhaseStartVirtualPos[v]);
                ESP_LOGI(TAG, "Motor %d homing-check leg complete (returned to %ld)",
                         v, (long)homingPhaseStartVirtualPos[v]);
            } else {
                currentPosition[p].store(0);
                motorPositions->setValue(v, 0, 0);
                motorIsHomed[v].store(true);
                motorHomedParams[v]->setValue(0, 0, true);
                ESP_LOGI(TAG, "Motor %d HOMED (zeroed at backoff position)", v);
            }
            
            homingPhase = HomingPhase::NEXT;
        }
        break;
    }
    
    case HomingPhase::NEXT: {
        homingMotorIndex++;
        
        if (homingMotorIndex >= NUM_MOTORS) {
            if (homingCheckMode) {
                if (homingCheckFailures == 0) {
                    ESP_LOGI(TAG, "=== HOMING CHECK PASS — all %d motors within tolerance ===", NUM_MOTORS);
                    errorMessage->setValue(0, 0, "Homing check PASS: all motors hit MIN near expected counts.");
                } else {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Homing check FAIL: %u motor(s) outside tolerance.", homingCheckFailures);
                    ESP_LOGE(TAG, "%s", msg);
                    errorMessage->setValue(0, 0, msg);
                }
            } else {
                ESP_LOGI(TAG, "=== HOMING COMPLETE — all %d motors homed ===", NUM_MOTORS);
            }
            
            // Restore jog speed
            cachedJogTargetSpeedFP.store(savedJogSpeedFP);
            
            homingPaused = false;
            homingCheckMode = false;
            homingCheckFailures = 0;
            pauseHomingParam->setValue(0, 0, false);
            homingPhase = HomingPhase::DONE;
            homingMotorParam->setValue(0, 0, -1);
            transitionTo(PlaybackState::IDLE);
        } else {
            // Start homing next motor
            homingMotorParam->setValue(0, 0, (int32_t)homingMotorIndex);

            homingPhase = HomingPhase::RETRACT;
            captureHomingPhaseStartPositions();
            applyHomingJogPattern();
            if (homingCheckMode) {
                ESP_LOGI(TAG, "Homing check motor %d: RETRACT with coupled payout", homingMotorIndex);
            } else {
                ESP_LOGI(TAG, "Homing motor %d: RETRACT with coupled payout", homingMotorIndex);
            }
        }
        break;
    }
    
    case HomingPhase::DONE:
    case HomingPhase::IDLE:
        break;
    }
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
    
    // Stop any current playback
    if (state.load() == PlaybackState::PLAYING) {
        stopPlayback();
    }
    
    esp_err_t err = loadChoreographyFromSpiffs(it->second.filename, activeChoreography);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load from SPIFFS");
        return err;
    }
    
    activeChoreography.name = it->second.name;
    activeChoreographyIndex = index;
    
    ESP_LOGI(TAG, "Loaded choreography '%s' (ID %d)", activeChoreography.name.c_str(), index);
    
    // Precompute trajectory
    err = precomputeTrajectory();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Trajectory precomputation failed");
        return err;
    }
    
    activeChoreographyParam->setValue(0, 0, index);
    
    return ESP_OK;
}

// ============================================================================
// State Management
// ============================================================================

void StepperMotorComponent::transitionTo(PlaybackState newState) {
    state.store(newState);
    stateParam->setValue(0, 0, static_cast<int>(newState));
}

float StepperMotorComponent::getPlaybackProgress() const {
    if (state.load() != PlaybackState::PLAYING || trajectory.duration_sec <= 0) {
        return 0.0f;
    }
    
    int64_t now_us = esp_timer_get_time();
    int64_t start_us = playbackStartUs.load();
    float t = (float)(now_us - start_us) * 1e-6f;
    return std::min(1.0f, t / trajectory.duration_sec);
}

bool StepperMotorComponent::applyVirtualMotorMappingFromParams() {
    bool usedPhysical[NUM_MOTORS] = { false, false, false, false };
    int8_t v2p[NUM_MOTORS] = { 0, 1, 2, 3 };
    int8_t p2v[NUM_MOTORS] = { -1, -1, -1, -1 };

    for (int v = 0; v < NUM_MOTORS; v++) {
        int32_t p = virtualToPhysicalMotorParam ? virtualToPhysicalMotorParam->getValue(0, v) : v;
        if (p < 0 || p >= NUM_MOTORS || usedPhysical[p]) {
            ESP_LOGD(TAG, "Skipping virtual->physical apply until mapping is a full unique permutation (v=%d p=%ld)", v, p);
            return false;
        }
        usedPhysical[p] = true;
        v2p[v] = (int8_t)p;
        p2v[p] = (int8_t)v;
    }

    for (int v = 0; v < NUM_MOTORS; v++) {
        virtualToPhysicalMotorCache[v].store(v2p[v]);
    }
    for (int p = 0; p < NUM_MOTORS; p++) {
        physicalToVirtualMotorCache[p].store(p2v[p]);
    }

    return true;
}

void StepperMotorComponent::applyDirectionMappingFromParams() {
    for (int v = 0; v < NUM_MOTORS; v++) {
        bool inverted = virtualDirectionInvertedParam && virtualDirectionInvertedParam->getValue(0, v);
        directionSignCache[v].store(inverted ? -1 : 1);
    }
}

void StepperMotorComponent::applyVirtualSensorMappingFromParams() {
    for (int v = 0; v < NUM_MOTORS; v++) {
        int32_t minSensor = virtualMinSensorMapParam ? virtualMinSensorMapParam->getValue(0, v) : v;
        int32_t maxSensor = virtualMaxSensorMapParam ? virtualMaxSensorMapParam->getValue(0, v) : v;
        if (minSensor < 0) minSensor = 0;
        if (minSensor >= NUM_MOTORS) minSensor = NUM_MOTORS - 1;
        if (maxSensor < 0) maxSensor = 0;
        if (maxSensor >= NUM_MOTORS) maxSensor = NUM_MOTORS - 1;
        virtualMinSensorMapCache[v].store((int8_t)minSensor);
        virtualMaxSensorMapCache[v].store((int8_t)maxSensor);
    }
}

int8_t StepperMotorComponent::getPhysicalMotorFromVirtual(uint8_t virtualIdx) const {
    if (virtualIdx >= NUM_MOTORS) {
        return -1;
    }
    return virtualToPhysicalMotorCache[virtualIdx].load();
}

int8_t StepperMotorComponent::getVirtualMotorFromPhysical(uint8_t physicalIdx) const {
    if (physicalIdx >= NUM_MOTORS) {
        return -1;
    }
    return physicalToVirtualMotorCache[physicalIdx].load();
}

int8_t StepperMotorComponent::getDirectionSignForVirtual(uint8_t virtualIdx) const {
    if (virtualIdx >= NUM_MOTORS) {
        return 1;
    }
    return directionSignCache[virtualIdx].load();
}

int32_t StepperMotorComponent::toPhysicalPosition(uint8_t virtualIdx, int32_t virtualSteps) const {
    return virtualSteps * (int32_t)getDirectionSignForVirtual(virtualIdx);
}

int32_t StepperMotorComponent::toVirtualPosition(uint8_t virtualIdx, int32_t physicalSteps) const {
    return physicalSteps * (int32_t)getDirectionSignForVirtual(virtualIdx);
}

int8_t StepperMotorComponent::virtualJogToPhysical(uint8_t virtualIdx, int8_t virtualJog) const {
    if (virtualJog == 0) {
        return 0;
    }
    int32_t mapped = (int32_t)virtualJog * (int32_t)getDirectionSignForVirtual(virtualIdx);
    if (mapped > 0) {
        return 1;
    }
    return -1;
}

bool StepperMotorComponent::readVirtualMinLimit(uint8_t virtualIdx) const {
    if (!hal || virtualIdx >= NUM_MOTORS) {
        return false;
    }
    int8_t sensorIdx = virtualMinSensorMapCache[virtualIdx].load();
    if (sensorIdx < 0 || sensorIdx >= NUM_MOTORS) {
        return false;
    }
    return hal->isLimitTriggered(sensorIdx) || cachedSimulateLimitMin[sensorIdx].load();
}

bool StepperMotorComponent::readVirtualMaxLimit(uint8_t virtualIdx) const {
    if (!hal || virtualIdx >= NUM_MOTORS) {
        return false;
    }
    int8_t sensorIdx = virtualMaxSensorMapCache[virtualIdx].load();
    if (sensorIdx < 0 || sensorIdx >= NUM_MOTORS) {
        return false;
    }
    return hal->isMaxLimitTriggered(sensorIdx) || cachedSimulateLimitMax[sensorIdx].load();
}

bool StepperMotorComponent::readPhysicalMinLimitForMotor(uint8_t physicalMotorIdx) const {
    int8_t virtualIdx = getVirtualMotorFromPhysical(physicalMotorIdx);
    if (virtualIdx >= 0) {
        return readVirtualMinLimit((uint8_t)virtualIdx);
    }
    if (!hal || physicalMotorIdx >= NUM_MOTORS) {
        return false;
    }
    return hal->isLimitTriggered(physicalMotorIdx) || cachedSimulateLimitMin[physicalMotorIdx].load();
}

bool StepperMotorComponent::readPhysicalMaxLimitForMotor(uint8_t physicalMotorIdx) const {
    int8_t virtualIdx = getVirtualMotorFromPhysical(physicalMotorIdx);
    if (virtualIdx >= 0) {
        return readVirtualMaxLimit((uint8_t)virtualIdx);
    }
    if (!hal || physicalMotorIdx >= NUM_MOTORS) {
        return false;
    }
    return hal->isMaxLimitTriggered(physicalMotorIdx) || cachedSimulateLimitMax[physicalMotorIdx].load();
}

// ============================================================================
// NVS Persistence
// ============================================================================

void StepperMotorComponent::saveCustomData(nvs_handle_t handle) {
    ESP_LOGI(TAG, "Saving choreography metadata + mapping to NVS");
    
    uint16_t count = choreographyMeta.size();
    nvs_set_u16(handle, "choreo_count", count);
    
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
    
    nvs_set_i16(handle, "active_choreo", activeChoreographyIndex);

    for (int v = 0; v < NUM_MOTORS; v++) {
        char key[32];
        snprintf(key, sizeof(key), "map_v2p_%d", v);
        nvs_set_i8(handle, key, getPhysicalMotorFromVirtual(v));

        snprintf(key, sizeof(key), "map_dir_inv_%d", v);
        nvs_set_u8(handle, key, getDirectionSignForVirtual(v) < 0 ? 1 : 0);

        snprintf(key, sizeof(key), "map_min_%d", v);
        nvs_set_i8(handle, key, virtualMinSensorMapCache[v].load());

        snprintf(key, sizeof(key), "map_max_%d", v);
        nvs_set_i8(handle, key, virtualMaxSensorMapCache[v].load());
    }

    metadataModified = false;
}

void StepperMotorComponent::loadCustomData(nvs_handle_t handle) {
    ESP_LOGI(TAG, "Loading choreography metadata from NVS");
    
    choreographyMeta.clear();
    
    uint16_t count = 0;
    if (nvs_get_u16(handle, "choreo_count", &count) != ESP_OK) {
        ESP_LOGI(TAG, "No choreographies in NVS");
        count = 0;
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
    
    int16_t active = -1;
    nvs_get_i16(handle, "active_choreo", &active);
    activeChoreographyIndex = active;

    for (int v = 0; v < NUM_MOTORS; v++) {
        char key[32];
        int8_t mapVal = v;
        uint8_t dirInv = 0;
        int8_t minSensor = v;
        int8_t maxSensor = v;

        snprintf(key, sizeof(key), "map_v2p_%d", v);
        nvs_get_i8(handle, key, &mapVal);
        if (virtualToPhysicalMotorParam) {
            virtualToPhysicalMotorParam->setValue(0, v, mapVal);
        }

        snprintf(key, sizeof(key), "map_dir_inv_%d", v);
        nvs_get_u8(handle, key, &dirInv);
        if (virtualDirectionInvertedParam) {
            virtualDirectionInvertedParam->setValue(0, v, dirInv != 0);
        }

        snprintf(key, sizeof(key), "map_min_%d", v);
        nvs_get_i8(handle, key, &minSensor);
        if (virtualMinSensorMapParam) {
            virtualMinSensorMapParam->setValue(0, v, minSensor);
        }

        snprintf(key, sizeof(key), "map_max_%d", v);
        nvs_get_i8(handle, key, &maxSensor);
        if (virtualMaxSensorMapParam) {
            virtualMaxSensorMapParam->setValue(0, v, maxSensor);
        }
    }

    if (!applyVirtualMotorMappingFromParams()) {
        for (int v = 0; v < NUM_MOTORS; v++) {
            virtualToPhysicalMotorParam->setValue(0, v, v);
        }
        applyVirtualMotorMappingFromParams();
    }
    applyDirectionMappingFromParams();
    applyVirtualSensorMappingFromParams();
    
    ESP_LOGI(TAG, "Loaded %zu choreographies from NVS", choreographyMeta.size());
    
    updateStatusParams();
    updateChoreographyIdsParam();
    metadataModified = false;
}

void StepperMotorComponent::onPostLoadReconcile() {
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
        return ESP_ERR_INVALID_STATE;
    }
    
    uploadBuffer.insert(uploadBuffer.end(), data, data + len);
    uploadReceivedSize += len;
    
    ESP_LOGD(TAG, "Chunk %d: %zu bytes (total: %zu/%zu)", 
             chunk_index, len, uploadReceivedSize, uploadExpectedSize);
    
    return ESP_OK;
}

esp_err_t StepperMotorComponent::uploadCommit() {
    if (!uploadInProgress) {
        return ESP_ERR_INVALID_STATE;
    }
    
    uploadInProgress = false;
    
    Choreography choreo;
    esp_err_t err = parseChoreographyJson((const char*)uploadBuffer.data(), uploadBuffer.size(), choreo);
    if (err != ESP_OK) {
        uploadBuffer.clear();
        return err;
    }
    
    choreo.name = uploadPendingName;
    
    int16_t newId = findNextChoreographyId();
    
    char filename[32];
    snprintf(filename, sizeof(filename), "%s%03d.json", CHOREO_FILE_PREFIX, newId);
    choreo.filename = filename;
    
    err = saveChoreographyToSpiffs(choreo.filename, uploadBuffer.data(), uploadBuffer.size());
    if (err != ESP_OK) {
        uploadBuffer.clear();
        return err;
    }
    
    ChoreographyMeta meta;
    meta.name = choreo.name;
    meta.filename = choreo.filename;
    meta.duration_sec = choreo.duration_sec;
    meta.loop_count = choreo.loop_count;
    choreographyMeta[newId] = meta;
    metadataModified = true;
    
    uploadBuffer.clear();
    
    ESP_LOGI(TAG, "Choreography '%s' saved (ID %d, %.2fs)", 
             choreo.name.c_str(), newId, choreo.duration_sec);
    
    updateStatusParams();
    updateChoreographyIdsParam();
    
    return ESP_OK;
}

void StepperMotorComponent::deleteChoreography(int16_t index) {
    auto it = choreographyMeta.find(index);
    if (it == choreographyMeta.end()) {
        return;
    }
    
    std::string filepath = std::string(SPIFFS_BASE_PATH) + "/" + it->second.filename;
    remove(filepath.c_str());
    
    choreographyMeta.erase(it);
    metadataModified = true;
    
    if (activeChoreographyIndex == index) {
        activeChoreographyIndex = -1;
        activeChoreography = Choreography();
        trajectory.valid = false;
    }
    
    updateStatusParams();
    updateChoreographyIdsParam();
}

// ============================================================================
// JSON Parsing
// ============================================================================

esp_err_t StepperMotorComponent::parseChoreographyJson(const char* json, size_t len, Choreography& out) {
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse error");
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON* format_version = cJSON_GetObjectItem(root, "format_version");
    if (!format_version || format_version->valueint != 1) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON* motor = cJSON_GetObjectItem(root, "motor");
    if (motor) {
        cJSON* r_spool = cJSON_GetObjectItem(motor, "r_spool");
        cJSON* steps_per_rev = cJSON_GetObjectItem(motor, "steps_per_rev");
        cJSON* microstep_divisor = cJSON_GetObjectItem(motor, "microstep_divisor");
        
        if (r_spool) out.r_spool = (float)r_spool->valuedouble;
        if (steps_per_rev) out.steps_per_rev = steps_per_rev->valueint;
        if (microstep_divisor) out.microstep_divisor = microstep_divisor->valueint;
    }
    
    cJSON* porch = cJSON_GetObjectItem(root, "porch");
    if (porch) {
        cJSON* lx = cJSON_GetObjectItem(porch, "Lx");
        cJSON* ly = cJSON_GetObjectItem(porch, "Ly");
        cJSON* lz = cJSON_GetObjectItem(porch, "Lz");
        
        if (lx) out.porch_lx = (float)lx->valuedouble;
        if (ly) out.porch_ly = (float)ly->valuedouble;
        if (lz) out.porch_lz = (float)lz->valuedouble;
    }
    
    cJSON* spline_knots = cJSON_GetObjectItem(root, "spline_knots_per_motor");
    if (!spline_knots || !cJSON_IsArray(spline_knots)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    float max_time = 0;
    
    cJSON* motor_knots;
    cJSON_ArrayForEach(motor_knots, spline_knots) {
        cJSON* motor_idx = cJSON_GetObjectItem(motor_knots, "motor");
        cJSON* knots_array = cJSON_GetObjectItem(motor_knots, "knots");
        
        if (!motor_idx || !knots_array) continue;
        
        int idx = motor_idx->valueint;
        if (idx < 0 || idx >= NUM_MOTORS) continue;
        
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
    }
    
    out.duration_sec = max_time;
    out.loop_count = 1;
    out.valid = true;
    
    cJSON_Delete(root);
    
    for (int i = 0; i < NUM_MOTORS; i++) {
        if (out.knots[i].size() < 2) {
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
        return ESP_FAIL;
    }
    
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    
    return (written == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t StepperMotorComponent::loadChoreographyFromSpiffs(const std::string& filename, 
                                                            Choreography& out) {
    std::string filepath = std::string(SPIFFS_BASE_PATH) + "/" + filename;
    
    FILE* f = fopen(filepath.c_str(), "r");
    if (!f) {
        return ESP_FAIL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > (long)MAX_CHOREOGRAPHY_SIZE) {
        fclose(f);
        return ESP_FAIL;
    }
    
    std::vector<char> buffer(size + 1);
    size_t read = fread(buffer.data(), 1, size, f);
    fclose(f);
    
    if (read != (size_t)size) {
        return ESP_FAIL;
    }
    
    buffer[size] = '\0';
    
    return parseChoreographyJson(buffer.data(), size, out);
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
    choreographyCountParam->setValue(0, 0, choreographyMeta.size());
}

void StepperMotorComponent::updateChoreographyIdsParam() {
    std::string ids;
    for (const auto& [id, meta] : choreographyMeta) {
        if (!ids.empty()) ids += ",";
        ids += std::to_string(id);
    }
    choreographyIdsParam->setValue(0, 0, ids);
}
