#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <vector>
#include <map>
#include <atomic>
#include <cstdint>

/**
 * Stepper Motor Component - Multi-Choreography Cubic Hermite Spline Playback
 * 
 * ============================================================================
 * ARCHITECTURE (Three-Tier Motion Control)
 * ============================================================================
 * 
 * This component uses a three-tier architecture to achieve high step rates
 * (up to 64 kHz per motor for 1200 RPM at 1/16 microstepping) while keeping
 * the real-time motion smooth:
 * 
 * TIER 1: Trajectory Precomputation (on choreography load)
 * ---------------------------------------------------------
 * When a choreography is loaded, the full spline is evaluated at fixed time
 * intervals (e.g., every 10ms = 100 waypoints/sec). This produces a dense
 * array of (time, position) waypoints per motor. The Hermite spline math
 * happens ONCE at load time, not during playback.
 * 
 * Memory: ~8 bytes per waypoint × 4 motors × (duration_sec × waypoints/sec)
 * Example: 60s choreography @ 100 wp/s = 6000 waypoints × 4 motors × 8 bytes = 192 KB
 * 
 * TIER 2: Motion Planning Task (FreeRTOS, 100-500 Hz)
 * ----------------------------------------------------
 * A high-priority FreeRTOS task runs at 100-500 Hz. Each tick:
 * - Looks up current playback time
 * - Finds the surrounding waypoints in the precomputed array
 * - Linearly interpolates to get smooth target positions
 * - Writes to shared atomic target variables
 * 
 * This task does NOT touch GPIO. It just updates target positions.
 * Priority: Higher than webserver (tskIDLE_PRIORITY + 3)
 * Core: Pinned to core 1 (away from WiFi on core 0)
 * 
 * TIER 3: Step Generation ISR (esp_timer, up to 80 kHz)
 * ------------------------------------------------------
 * A minimal ISR fires at the required step rate. Each tick:
 * - Reads current position and target position (atomics)
 * - If delta != 0: set direction, pulse step pin, update position
 * - No math, no memory allocation, no FreeRTOS API
 * 
 * The ISR is IRAM-resident and runs at whatever frequency is needed to
 * achieve the target RPM. At 1200 RPM with 1/16 microstepping:
 * - Steps/rev = 200 × 16 = 3200
 * - Steps/sec = 1200/60 × 3200 = 64,000
 * - ISR period = 15.6 µs
 * 
 * ============================================================================
 * STORAGE
 * ============================================================================
 * 
 * Choreographies are stored on SPIFFS as JSON files. Only metadata (name,
 * filename, duration) is stored in NVS. On load, the JSON is parsed and
 * the trajectory is precomputed into RAM.
 * 
 * ============================================================================
 * STATES
 * ============================================================================
 * 
 * - IDLE: Motors hold position (or disabled)
 * - PRECOMPUTING: Loading choreography, evaluating splines (may take 1-2s)
 * - HOMING: Running homing sequence
 * - ARMED: Ready for playback, waiting for start
 * - PLAYING: Active playback (Task + ISR running)
 * - E_STOP: Emergency stop, drivers disabled
 */

// ============================================================================
// Hardware Abstraction Layer (HAL) Interface
// ============================================================================

/**
 * Motor Driver HAL - Abstract interface for stepper motor hardware.
 * 
 * Thread safety: All HAL methods may be called from the ISR context.
 * Implementations MUST be safe for ISR use (no blocking, no FreeRTOS API).
 */
class StepperMotorHAL {
public:
    virtual ~StepperMotorHAL() = default;
    
    /**
     * Initialize all motor driver hardware.
     * @return ESP_OK on success
     */
    virtual esp_err_t init() = 0;
    
    /**
     * Deinitialize hardware, release resources.
     */
    virtual void deinit() = 0;
    
    /**
     * Set the direction for a motor. MUST be called before step().
     * @param motor_index Motor index (0-3)
     * @param direction true=positive, false=negative
     */
    virtual void setDirection(uint8_t motor_index, bool direction) = 0;
    
    /**
     * Generate a step pulse. Single pulse, minimum 1-2µs width.
     * @param motor_index Motor index (0-3)
     */
    virtual void step(uint8_t motor_index) = 0;
    
    /**
     * Step multiple motors simultaneously.
     * @param motor_mask Bitmask of motors to step
     */
    virtual void stepMultiple(uint8_t motor_mask) = 0;
    
    /**
     * Enable or disable motor driver outputs.
     * @param motor_index Motor index (0-3), or 0xFF for all
     * @param enabled true=holding torque, false=freewheel
     */
    virtual void setEnabled(uint8_t motor_index, bool enabled) = 0;
    
    /**
     * Check if limit switch is triggered.
     * @param motor_index Motor index (0-3)
     * @return true if at home/limit
     */
    virtual bool isLimitTriggered(uint8_t motor_index) = 0;
    
    /** Get number of motors supported */
    virtual uint8_t getMotorCount() const = 0;
    
    /** Get minimum step pulse width in microseconds */
    virtual uint32_t getMinPulseWidthUs() const = 0;
    
    /** Set microstepping divisor (1, 2, 4, 8, 16, 32, etc.) */
    virtual bool setMicrostepping(uint16_t divisor) = 0;
    
    /** Get current microstepping divisor */
    virtual uint16_t getMicrostepping() const = 0;
    
    /** Check if microstepping is software-configurable */
    virtual bool isMicrosteppingSoftwareConfigurable() const = 0;
};

// ============================================================================
// Data Structures
// ============================================================================

/**
 * A single knot point in the cubic Hermite spline (from JSON).
 */
struct SplineKnot {
    float t;           // Time in seconds
    int32_t pos_steps; // Position in microsteps
    float vel_sps;     // Velocity in steps per second
};

/**
 * A precomputed waypoint (dense trajectory sample).
 */
struct Waypoint {
    float t;           // Time in seconds
    int32_t pos_steps; // Position in microsteps
};

/**
 * Parsed choreography data.
 */
struct Choreography {
    std::string name;
    std::string filename;
    
    // Motor parameters
    float r_spool;
    uint16_t steps_per_rev;
    uint16_t microstep_divisor;
    
    // Porch dimensions (informational)
    float porch_lx, porch_ly, porch_lz;
    
    // Raw spline knots (from JSON)
    std::vector<SplineKnot> knots[4];
    
    // Duration and loop
    float duration_sec;
    int16_t loop_count;
    
    bool valid;
    
    Choreography() : r_spool(0.015f), steps_per_rev(200), microstep_divisor(16),
                     porch_lx(0), porch_ly(0), porch_lz(0), duration_sec(0),
                     loop_count(1), valid(false) {}
};

/**
 * Precomputed trajectory for a single motor.
 */
struct MotorTrajectory {
    std::vector<Waypoint> waypoints;
    
    // For fast lookup during playback
    size_t currentIndex;
    
    MotorTrajectory() : currentIndex(0) {}
    
    void reset() { currentIndex = 0; }
    
    /**
     * Get interpolated position at time t.
     * Uses linear interpolation between waypoints.
     */
    int32_t getPosition(float t);
};

/**
 * Full precomputed trajectory for all motors.
 */
struct PrecomputedTrajectory {
    MotorTrajectory motors[4];
    float duration_sec;
    float waypointInterval_sec;  // Time between waypoints (e.g., 0.01 = 100 Hz)
    bool valid;
    
    PrecomputedTrajectory() : duration_sec(0), waypointInterval_sec(0.01f), valid(false) {}
    
    void reset() {
        for (int i = 0; i < 4; i++) {
            motors[i].reset();
        }
    }
};

/**
 * Choreography metadata (stored in NVS).
 */
struct ChoreographyMeta {
    std::string name;
    std::string filename;
    float duration_sec;
    int16_t loop_count;
};

// ============================================================================
// Playback State Machine
// ============================================================================

enum class PlaybackState : uint8_t {
    IDLE = 0,
    PRECOMPUTING,  // Loading/precomputing trajectory
    HOMING,
    ARMED,
    PLAYING,
    E_STOP
};

// ============================================================================
// Stepper Motor Component
// ============================================================================

class StepperMotorComponent : public Component {
public:
    StepperMotorComponent();
    ~StepperMotorComponent() override;
    
    void onInitialize() override;
    
    // HAL injection
    void setHAL(StepperMotorHAL* hal);
    
    // NVS persistence
    void saveCustomData(nvs_handle_t handle) override;
    void loadCustomData(nvs_handle_t handle) override;
    bool hasCustomDataToSave() const override { return metadataModified; }
    void onPostLoadReconcile() override;
    
    // Choreography Management
    esp_err_t uploadBegin(const std::string& name, size_t total_bytes);
    esp_err_t uploadChunk(uint16_t chunk_index, const uint8_t* data, size_t len);
    esp_err_t uploadCommit();
    void deleteChoreography(int16_t index);
    esp_err_t loadChoreography(int16_t index);
    
    // Playback Control
    esp_err_t startPlayback();
    void stopPlayback();
    void emergencyStop();
    void clearEmergencyStop();
    esp_err_t startHoming();
    
    // Status
    PlaybackState getState() const { return state.load(); }
    size_t getChoreographyCount() const { return choreographyMeta.size(); }
    int16_t getActiveChoreographyIndex() const { return activeChoreographyIndex; }
    float getPlaybackProgress() const;
    
    static constexpr const char* TAG = "StepperMotor";
    
private:
    // ========================================================================
    // Hardware Abstraction
    // ========================================================================
    StepperMotorHAL* hal;
    bool halOwned;
    
    // ========================================================================
    // Configuration Constants
    // ========================================================================
    static constexpr uint8_t NUM_MOTORS = 4;
    static constexpr const char* SPIFFS_BASE_PATH = "/spiffs";
    static constexpr const char* CHOREO_FILE_PREFIX = "choreo_";
    static constexpr size_t UPLOAD_CHUNK_SIZE = 4096;
    static constexpr size_t MAX_CHOREOGRAPHY_SIZE = 256 * 1024;  // 256KB
    
    // Trajectory precomputation settings
    static constexpr float WAYPOINT_INTERVAL_SEC = 0.005f;  // 200 Hz waypoint rate
    static constexpr uint32_t TASK_RATE_HZ = 200;           // Motion planning task rate
    static constexpr uint32_t DEFAULT_ISR_RATE_HZ = 40000;  // Default step ISR rate (40 kHz)
    static constexpr uint32_t MAX_ISR_RATE_HZ = 80000;      // Max step ISR rate (80 kHz)
    
    // ========================================================================
    // Parameters
    // ========================================================================
    
    // Playback control
    BoolParameter* playing;
    IntParameter* activeChoreographyParam;
    BoolParameter* enableDrivers;
    IntParameter* stateParam;
    
    // Playback status (read-only)
    FloatParameter* playbackProgressParam;
    FloatParameter* playbackTimeParam;
    IntParameter* choreographyCountParam;
    
    // Motor positions (read-only)
    IntParameter* motorPositions;
    IntParameter* motorTargets;
    
    // Homing
    BoolParameter* homeCommand;
    BoolParameter* isHomed;
    
    // Emergency stop
    BoolParameter* eStopCommand;
    BoolParameter* eStopClear;
    
    // Upload parameters
    StringParameter* uploadName;
    IntParameter* uploadTotalBytes;
    IntParameter* uploadChunkIndex;
    StringParameter* uploadChunkData;
    BoolParameter* uploadCommitParam;
    
    // Delete/Query
    IntParameter* deleteChoreographyParam;
    IntParameter* queryChoreographyIndex;
    StringParameter* queryChoreographyName;
    FloatParameter* queryChoreographyDuration;
    IntParameter* queryChoreographyLoop;
    StringParameter* choreographyIdsParam;
    
    // Configuration
    IntParameter* chunkSizeParam;
    IntParameter* isrRateHz;             // Step ISR rate (default 40 kHz)
    IntParameter* maxRpmParam;           // Max motor RPM for ISR rate calc
    IntParameter* microsteppingParam;
    BoolParameter* microsteppingConfigurable;
    
    // ========================================================================
    // Choreography Storage
    // ========================================================================
    std::map<int16_t, ChoreographyMeta> choreographyMeta;
    Choreography activeChoreography;
    PrecomputedTrajectory trajectory;
    int16_t activeChoreographyIndex;
    bool metadataModified;
    
    // ========================================================================
    // Playback State (shared between task and ISR)
    // ========================================================================
    std::atomic<PlaybackState> state;
    
    // These are written by the task, read by the ISR
    // Using atomics for lock-free access
    std::atomic<int32_t> targetPosition[4];
    std::atomic<bool> targetDirection[4];
    
    // These are written by the ISR, read by the task
    std::atomic<int32_t> currentPosition[4];
    
    // Playback timing
    std::atomic<int64_t> playbackStartUs;
    std::atomic<int16_t> loopsRemaining;
    
    // ========================================================================
    // Task and ISR handles
    // ========================================================================
    TaskHandle_t motionTaskHandle;
    esp_timer_handle_t stepTimer;
    std::atomic<bool> taskRunning;
    std::atomic<bool> stopRequested;
    
    // ========================================================================
    // Upload State
    // ========================================================================
    std::vector<uint8_t> uploadBuffer;
    size_t uploadExpectedSize;
    size_t uploadReceivedSize;
    std::string uploadPendingName;
    bool uploadInProgress;
    
    // ========================================================================
    // Homing State
    // ========================================================================
    bool homingInProgress[4];
    int32_t homingDirection[4];
    uint32_t homingTimeoutMs;
    uint32_t homingStartMs;
    
    // ========================================================================
    // Internal Methods
    // ========================================================================
    
    // Trajectory precomputation
    esp_err_t precomputeTrajectory();
    float hermiteEval(float t, const SplineKnot& k0, const SplineKnot& k1);
    int32_t evaluateSplineAt(const std::vector<SplineKnot>& knots, float t);
    
    // Motion planning task
    static void motionTaskWrapper(void* arg);
    void motionTask();
    
    // Step generation ISR
    static void IRAM_ATTR stepTimerCallback(void* arg);
    void IRAM_ATTR stepTimerISR();
    
    // Homing ISR
    void IRAM_ATTR homingStepISR();
    
    // Choreography management
    int16_t findNextChoreographyId() const;
    esp_err_t parseChoreographyJson(const char* json, size_t len, Choreography& out);
    esp_err_t saveChoreographyToSpiffs(const std::string& filename, const uint8_t* data, size_t len);
    esp_err_t loadChoreographyFromSpiffs(const std::string& filename, Choreography& out);
    
    // State management
    void transitionTo(PlaybackState newState);
    void updateStatusParams();
    void updateChoreographyIdsParam();
    
    // ISR rate calculation
    uint32_t calculateRequiredIsrRate() const;
};
