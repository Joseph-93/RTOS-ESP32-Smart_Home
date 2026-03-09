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

StepperMotorComponent::StepperMotorComponent() 
    : Component("StepperMotor"),
      hal(nullptr),
      halOwned(false),
      activeChoreographyIndex(-1),
      metadataModified(false),
      state(PlaybackState::IDLE),
      motionTaskHandle(nullptr),
      stepTimer(nullptr),
      taskRunning(false),
      taskExited(true),
      stopRequested(false),
      uploadExpectedSize(0),
      uploadReceivedSize(0),
      uploadInProgress(false),
      jogSpeed(500),
      backoffDistance(50)
{
    for (int i = 0; i < NUM_MOTORS; i++) {
        currentPosition[i] = 0;
        targetPosition[i] = 0;
        targetDirection[i] = false;
        jogCommand[i] = 0;
        motorIsHomed[i] = false;
        limitTriggered[i] = false;
        prevTarget[i] = 0;
        lastDirection[i] = false;
    }
    playbackStartUs = 0;
    loopsRemaining = 0;
}

StepperMotorComponent::~StepperMotorComponent() {
    // Stop playback first
    stopPlayback();
    
    // Wait for task to finish
    if (motionTaskHandle) {
        taskRunning = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        // Task should have exited, but just in case
        vTaskDelete(motionTaskHandle);
        motionTaskHandle = nullptr;
    }
    
    // Stop and delete timer
    if (stepTimer) {
        esp_timer_stop(stepTimer);
        esp_timer_delete(stepTimer);
        stepTimer = nullptr;
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
    
    jogSpeedParam = addIntParam("jogSpeed", 1, 1, 10, 5000, 500);
    backoffDistanceParam = addIntParam("backoffDistance", 1, 1, 10, 200, 50);
    
    moveToHomeParam = addBoolParam("moveToHome", 1, 1, false);
    moveToHomeParam->setOnChange([this](size_t, size_t, bool val) {
        if (val && allMotorsHomed()) {
            ESP_LOGI(TAG, "Move to home position requested");
            moveToHomeParam->setValueQuiet(0, 0, false);
            // moveToHomePosition() will be called by task
        }
    });
    
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
    
    // Delete/Query parameters
    deleteChoreographyParam = addIntParam("deleteChoreography", 1, 1, -1, 1000, -1);
    deleteChoreographyParam->setOnChange([this](size_t, size_t, int32_t val) {
        if (val >= 0) {
            deleteChoreography(val);
            deleteChoreographyParam->setValueQuiet(0, 0, -1);
        }
    });
    
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
    
    isrRateHz = addIntParam("isrRateHz", 1, 1, 1000, MAX_ISR_RATE_HZ, DEFAULT_ISR_RATE_HZ);
    
    maxRpmParam = addIntParam("maxRpm", 1, 1, 1, 3000, 1200);
    maxRpmParam->setOnChange([this](size_t, size_t, int32_t val) {
        // Recalculate ISR rate based on max RPM
        uint32_t newRate = calculateRequiredIsrRate();
        isrRateHz->setValueQuiet(0, 0, newRate);
        ESP_LOGI(TAG, "Max RPM set to %ld, ISR rate now %lu Hz", val, newRate);
    });
    
    microsteppingParam = addIntParam("microstepping", 1, 1, 1, 256, 16);
    microsteppingParam->setOnChange([this](size_t, size_t, int32_t val) {
        if (hal) {
            if (!hal->setMicrostepping(val)) {
                microsteppingParam->setValueQuiet(0, 0, hal->getMicrostepping());
            } else {
                // Recalculate ISR rate
                uint32_t newRate = calculateRequiredIsrRate();
                isrRateHz->setValueQuiet(0, 0, newRate);
            }
        }
    });
    
    microsteppingConfigurable = addBoolParam("microsteppingConfigurable", 1, 1, false, true);
    
    // Set initial microstepping
    if (hal) {
        hal->setMicrostepping(16);
        microsteppingConfigurable->setValueQuiet(0, 0, hal->isMicrosteppingSoftwareConfigurable());
    }
    
    // Create step timer (ISR dispatch)
    esp_timer_create_args_t timer_args = {
        .callback = stepTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_ISR,
        .name = "stepper_step",
        .skip_unhandled_events = true
    };
    
    err = esp_timer_create(&timer_args, &stepTimer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create step timer: %d", err);
    }
    
    // Calculate initial ISR rate
    uint32_t rate = calculateRequiredIsrRate();
    isrRateHz->setValueQuiet(0, 0, rate);
    
    ESP_LOGI(TAG, "StepperMotorComponent initialized");
    ESP_LOGI(TAG, "  Waypoint interval: %.3f sec (%d Hz)", WAYPOINT_INTERVAL_SEC, (int)(1.0f/WAYPOINT_INTERVAL_SEC));
    ESP_LOGI(TAG, "  Task rate: %lu Hz", TASK_RATE_HZ);
    ESP_LOGI(TAG, "  ISR rate: %lu Hz", rate);
}

// ============================================================================
// ISR Rate Calculation
// ============================================================================

uint32_t StepperMotorComponent::calculateRequiredIsrRate() const {
    // Calculate required step rate for max RPM
    // steps/sec = (RPM / 60) * steps_per_rev * microstepping
    
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
        errorMessage->setValueQuiet(0, 0, errMsg);
        
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
        // Handle limit-triggered back-off (manual jog mode)
        // ====================================================================
        for (int m = 0; m < NUM_MOTORS; m++) {
            if (limitTriggered[m].load()) {
                uint32_t backoff = backoffDistanceParam->getValue(0, 0);
                ESP_LOGI(TAG, "Motor %d hit limit - backing off %lu steps", m, backoff);
                
                // Back off at safe rate (not in ISR)
                hal->setDirection(m, true);  // Reverse direction (pay-out)
                for (uint32_t step = 0; step < backoff; step++) {
                    hal->step(m);
                    vTaskDelay(pdMS_TO_TICKS(2));  // 500 Hz back-off rate
                }
                
                // Zero position and mark as homed
                currentPosition[m].store(0);
                motorIsHomed[m].store(true);
                motorHomedParams[m]->setValueQuiet(0, 0, true);
                
                // Clear flag
                limitTriggered[m].store(false);
                
                ESP_LOGI(TAG, "Motor %d homed ✓", m);
            }
        }
        
        // ====================================================================
        // Check if all homed and user requested move to home
        // ====================================================================
        if (moveToHomeParam->getValue(0, 0) && allMotorsHomed()) {
            ESP_LOGI(TAG, "Moving all motors to home position");
            moveToHomePosition();
            moveToHomeParam->setValueQuiet(0, 0, false);
        }
        
        // ====================================================================
        // Read jog parameter updates
        // ====================================================================
        for (int m = 0; m < NUM_MOTORS; m++) {
            int8_t cmd = jogCommandParams[m]->getValue(0, 0);
            jogCommand[m].store(cmd);
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
            for (int m = 0; m < NUM_MOTORS; m++) {
                int32_t newTarget = trajectory.motors[m].getPosition(t);
                
                // Safety check: detect velocity that would exceed motor capability
                int32_t velocity = abs(newTarget - prevTarget[m]);
                if (velocity > MAX_VELOCITY_STEPS_PER_TICK) {
                    char errMsg[128];
                    snprintf(errMsg, sizeof(errMsg), 
                        "Motor %d velocity too high: %ld steps/tick (max %u) - E-STOP",
                        m, (long)velocity, MAX_VELOCITY_STEPS_PER_TICK);
                    errorMessage->setValueQuiet(0, 0, errMsg);
                    ESP_LOGE(TAG, "%s", errMsg);
                    emergencyStop();
                    return;
                }
                
                prevTarget[m] = newTarget;
                int32_t current = currentPosition[m].load();
                
                targetPosition[m].store(newTarget);
                targetDirection[m].store(newTarget > current);
                
                // Update read-only params (not every tick - would be too much)
                // We'll do this less frequently
            }
            
            // Update playback progress (less frequently to avoid overhead)
            static uint32_t updateCounter = 0;
            if (++updateCounter >= TASK_RATE_HZ / 10) {  // 10 Hz update
                updateCounter = 0;
                playbackProgressParam->setValueQuiet(0, 0, t / trajectory.duration_sec);
                playbackTimeParam->setValueQuiet(0, 0, t);
                
                for (int m = 0; m < NUM_MOTORS; m++) {
                    motorPositions->setValueQuiet(m, 0, currentPosition[m].load());
                    motorTargets->setValueQuiet(m, 0, targetPosition[m].load());
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
    
    // Manual jog mode - handle limit detection and jogging
    if (currentState == PlaybackState::IDLE) {
        for (uint8_t m = 0; m < NUM_MOTORS; m++) {
            // Skip if limit-triggered (task handles back-off)
            if (limitTriggered[m].load()) {
                continue;
            }
            
            int8_t jog = jogCommand[m].load();
            if (jog == 0) continue;  // No jog command
            
            // Check limit switch before stepping
            if (hal->isLimitTriggered(m)) {
                // Hit limit - stop jogging, flag for task to handle back-off
                jogCommand[m].store(0);
                limitTriggered[m].store(true);
                continue;
            }
            
            // Execute jog step
            bool direction = (jog > 0);  // 1=pay-out (positive), -1=retract (negative)
            
            // Only call setDirection if it changed
            if (direction != lastDirection[m]) {
                hal->setDirection(m, direction);
                lastDirection[m] = direction;
            }
            
            hal->step(m);
            
            int32_t pos = currentPosition[m].load();
            currentPosition[m].store(direction ? pos + 1 : pos - 1);
        }
        return;
    }
    
    if (currentState != PlaybackState::PLAYING) {
        return;
    }
    
    // Step each motor toward its target (playback mode)
    // This is the hottest path - keep it minimal
    for (uint8_t m = 0; m < NUM_MOTORS; m++) {
        int32_t current = currentPosition[m].load();
        int32_t target = targetPosition[m].load();
        
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
    errorMessage->setValueQuiet(0, 0, "");
    
    // Set initial targets and reset velocity tracking
    for (int m = 0; m < NUM_MOTORS; m++) {
        if (!trajectory.motors[m].waypoints.empty()) {
            int32_t initial = trajectory.motors[m].waypoints[0].pos_steps;
            targetPosition[m].store(initial);
            prevTarget[m] = initial;  // Initialize velocity tracking
        }
    }
    
    // Record start time
    playbackStartUs.store(esp_timer_get_time());
    loopsRemaining.store(activeChoreography.loop_count);
    
    // Enable drivers
    if (hal) {
        hal->setEnabled(0xFF, true);
    }
    
    // Start motion task if not already running
    if (!motionTaskHandle) {
        taskRunning.store(true);
        taskExited.store(false);  // Reset exit flag
        BaseType_t result = xTaskCreatePinnedToCore(
            motionTaskWrapper,
            "stepper_motion",
            4096,
            this,
            tskIDLE_PRIORITY + 3,  // Higher than webserver
            &motionTaskHandle,
            1   // Core 1 (away from WiFi)
        );
        
        if (result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create motion task");
            return ESP_FAIL;
        }
    }
    
    // Start step timer
    uint32_t rate = isrRateHz->getValue(0, 0);
    uint64_t period_us = 1000000 / rate;
    esp_timer_start_periodic(stepTimer, period_us);
    
    transitionTo(PlaybackState::PLAYING);
    playing->setValueQuiet(0, 0, true);
    enableDrivers->setValueQuiet(0, 0, true);
    
    ESP_LOGI(TAG, "Playback started (ISR period: %llu µs = %lu Hz)", period_us, rate);
    return ESP_OK;
}

void StepperMotorComponent::stopPlayback() {
    ESP_LOGI(TAG, "Stopping playback");
    
    // Stop timer first
    if (stepTimer) {
        esp_timer_stop(stepTimer);
    }
    
    // Stop task
    taskRunning.store(false);
    if (motionTaskHandle) {
        // Wait for task to signal it has exited (with timeout)
        const uint32_t timeoutMs = 200;
        const uint32_t pollIntervalMs = 10;
        uint32_t elapsed = 0;
        
        while (!taskExited.load() && elapsed < timeoutMs) {
            vTaskDelay(pdMS_TO_TICKS(pollIntervalMs));
            elapsed += pollIntervalMs;
        }
        
        if (!taskExited.load()) {
            ESP_LOGW(TAG, "Task did not exit cleanly in %lu ms", timeoutMs);
        }
        
        motionTaskHandle = nullptr;
    }
    
    transitionTo(PlaybackState::IDLE);
    playing->setValueQuiet(0, 0, false);
}

void StepperMotorComponent::emergencyStop() {
    ESP_LOGW(TAG, "EMERGENCY STOP");
    
    // Stop everything immediately
    if (stepTimer) {
        esp_timer_stop(stepTimer);
    }
    
    taskRunning.store(false);
    
    // Disable drivers
    if (hal) {
        hal->setEnabled(0xFF, false);
    }
    
    transitionTo(PlaybackState::E_STOP);
    playing->setValueQuiet(0, 0, false);
    enableDrivers->setValueQuiet(0, 0, false);
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
    
    ESP_LOGI(TAG, "Moving all motors to home position (%ld steps)", homePosition);
    
    for (int m = 0; m < NUM_MOTORS; m++) {
        int32_t current = currentPosition[m].load();
        int32_t delta = homePosition - current;
        
        if (delta == 0) continue;  // Already at home
        
        bool direction = (delta > 0);
        hal->setDirection(m, direction);
        
        for (int32_t step = 0; step < abs(delta); step++) {
            hal->step(m);
            vTaskDelay(pdMS_TO_TICKS(2));  // 500 Hz move rate
        }
        
        currentPosition[m].store(homePosition);
        motorPositions->setValueQuiet(m, 0, homePosition);
    }
    
    ESP_LOGI(TAG, "All motors at home position");
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
    
    activeChoreographyParam->setValueQuiet(0, 0, index);
    
    return ESP_OK;
}

// ============================================================================
// State Management
// ============================================================================

void StepperMotorComponent::transitionTo(PlaybackState newState) {
    state.store(newState);
    stateParam->setValueQuiet(0, 0, static_cast<int>(newState));
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

// ============================================================================
// NVS Persistence
// ============================================================================

void StepperMotorComponent::saveCustomData(nvs_handle_t handle) {
    ESP_LOGI(TAG, "Saving choreography metadata to NVS");
    
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
    
    int16_t active = -1;
    nvs_get_i16(handle, "active_choreo", &active);
    activeChoreographyIndex = active;
    
    ESP_LOGI(TAG, "Loaded %zu choreographies from NVS", choreographyMeta.size());
    
    updateStatusParams();
    updateChoreographyIdsParam();
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
