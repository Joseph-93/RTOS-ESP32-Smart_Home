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
 * Drives 4 stepper motors via a Hardware Abstraction Layer (HAL).
 * Stores MULTIPLE choreographies (deployment JSONs) and plays whichever one
 * is selected via the active_choreography parameter.
 * 
 * This component is designed for the Floating Candle project:
 * - 4 cable-driven motors controlling a suspended candle's XYZ position
 * - Each motor follows a cubic Hermite spline defined by knot points
 * - Coordinated playback at 2kHz evaluation rate via ISR
 * 
 * Storage: Choreographies are stored on SPIFFS as JSON files.
 * The component stores only metadata (name, filename, duration) in NVS.
 * 
 * States:
 * - IDLE (playing=false): Motors hold current position (or disabled)
 * - HOMING: Running homing sequence
 * - ARMED: Ready for playback, waiting for sync start
 * - PLAYING: Evaluating splines and stepping motors
 * - E_STOP: Emergency stop, drivers disabled
 * 
 * Choreography upload protocol (chunked):
 * 1. Set uploadName to start upload (names the new choreography)
 * 2. Set uploadTotalBytes to expected file size
 * 3. Set uploadChunkIndex, then uploadChunkData (base64) for each chunk
 * 4. Set uploadCommit=true to finalize — parses JSON, stores to SPIFFS
 * 5. Set activeChoreography to the desired index to load it
 * 6. Set playing=true to start playback
 */

// ============================================================================
// Hardware Abstraction Layer (HAL) Interface
// ============================================================================

/**
 * Motor Driver HAL - Abstract interface for stepper motor hardware.
 * 
 * Implement this interface for your specific hardware:
 * - A4988/DRV8825 step/dir drivers
 * - TMC2209 with UART
 * - Custom servo/stepper combinations
 * - etc.
 * 
 * The HAL handles:
 * - GPIO initialization for step/dir/enable pins
 * - Pulse generation (step timing, direction setting)
 * - Enable/disable driver outputs
 * - Limit switch reading (for homing)
 * 
 * Thread safety: All HAL methods may be called from the ISR context.
 * Implementations MUST be safe for ISR use (no blocking, no FreeRTOS API).
 */
class StepperMotorHAL {
public:
    virtual ~StepperMotorHAL() = default;
    
    // ========================================================================
    // Initialization
    // ========================================================================
    
    /**
     * Initialize all motor driver hardware.
     * Called once during component initialization.
     * 
     * @return ESP_OK on success, error code on failure
     */
    virtual esp_err_t init() = 0;
    
    /**
     * Deinitialize hardware, release resources.
     */
    virtual void deinit() = 0;
    
    // ========================================================================
    // Motor Control - ISR Safe
    // ========================================================================
    
    /**
     * Set the direction for a motor.
     * MUST be called before step() if direction changed.
     * 
     * @param motor_index Motor index (0-3)
     * @param direction true=retract (positive steps), false=extend (negative steps)
     */
    virtual void setDirection(uint8_t motor_index, bool direction) = 0;
    
    /**
     * Generate a step pulse on the specified motor.
     * Single pulse, minimum 1µs width typical.
     * 
     * @param motor_index Motor index (0-3)
     */
    virtual void step(uint8_t motor_index) = 0;
    
    /**
     * Step multiple motors simultaneously (for synchronized motion).
     * Generates step pulses on all motors in the bitmask.
     * 
     * @param motor_mask Bitmask of motors to step (bit 0 = motor 0, etc.)
     */
    virtual void stepMultiple(uint8_t motor_mask) = 0;
    
    /**
     * Enable or disable motor driver outputs.
     * When disabled, motors can freewheel (no holding torque).
     * 
     * @param motor_index Motor index (0-3), or 0xFF for all motors
     * @param enabled true=enabled (holding torque), false=disabled
     */
    virtual void setEnabled(uint8_t motor_index, bool enabled) = 0;
    
    // ========================================================================
    // Limit Switches (for homing)
    // ========================================================================
    
    /**
     * Check if a motor's limit switch is triggered.
     * 
     * @param motor_index Motor index (0-3)
     * @return true if limit switch is active (motor at home position)
     */
    virtual bool isLimitTriggered(uint8_t motor_index) = 0;
    
    // ========================================================================
    // Configuration
    // ========================================================================
    
    /**
     * Get the number of motors supported by this HAL.
     * @return Number of motors (typically 4)
     */
    virtual uint8_t getMotorCount() const = 0;
    
    /**
     * Get the minimum step pulse width in microseconds.
     * The ISR will ensure at least this delay between direction change and step.
     * @return Minimum pulse width in µs
     */
    virtual uint32_t getMinPulseWidthUs() const = 0;
    
    // ========================================================================
    // Microstepping Configuration
    // ========================================================================
    
    /**
     * Set microstepping divisor.
     * Common values: 1 (full), 2 (half), 4 (quarter), 8, 16, 32, 64, 128, 256
     * 
     * For drivers with hardware MS pins (A4988, DRV8825):
     *   Implementation should set MS1/MS2/MS3 pins accordingly.
     *   If MS pins are hardwired, this may be a no-op (returns false).
     * 
     * For drivers with software config (TMC2209 UART):
     *   Implementation sends the config command.
     * 
     * @param divisor Microstepping divisor (1, 2, 4, 8, 16, 32, etc.)
     * @return true if successfully set, false if not supported or invalid
     */
    virtual bool setMicrostepping(uint16_t divisor) = 0;
    
    /**
     * Get current microstepping divisor.
     * @return Current divisor (1, 2, 4, 8, 16, etc.)
     */
    virtual uint16_t getMicrostepping() const = 0;
    
    /**
     * Check if microstepping is software-configurable.
     * @return true if setMicrostepping() can change the setting
     */
    virtual bool isMicrosteppingSoftwareConfigurable() const = 0;
};

// ============================================================================
// Spline Data Structures
// ============================================================================

/**
 * A single knot point in the cubic Hermite spline.
 */
struct SplineKnot {
    float t;           // Time in seconds from choreography start
    int32_t pos_steps; // Position in microsteps (absolute from home)
    float vel_sps;     // Velocity in steps per second at this knot
};

/**
 * Parsed choreography data, ready for playback.
 */
struct Choreography {
    std::string name;                      // User-assigned name
    std::string filename;                  // SPIFFS filename
    
    // Motor parameters (from JSON)
    float r_spool;                         // Spool radius in meters
    uint16_t steps_per_rev;                // Full steps per revolution
    uint16_t microstep_divisor;            // Microstepping (e.g., 16 for 1/16)
    
    // Porch dimensions (informational)
    float porch_lx, porch_ly, porch_lz;    // Porch dimensions in meters
    
    // Spline knots for each motor
    std::vector<SplineKnot> knots[4];      // 4 motors
    
    // Duration
    float duration_sec;                    // Total choreography duration
    
    // Playback settings
    int16_t loop_count;                    // -1=infinite, 1=once, N=N times
    
    // Validation
    bool valid;                            // true if successfully parsed
    
    Choreography() : r_spool(0.015f), steps_per_rev(200), microstep_divisor(16),
                     porch_lx(0), porch_ly(0), porch_lz(0), duration_sec(0),
                     loop_count(1), valid(false) {}
};

/**
 * Choreography metadata (stored in NVS, lightweight).
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
    IDLE = 0,       // Not playing, motors may be enabled or disabled
    HOMING,         // Running homing sequence
    ARMED,          // Ready for playback, waiting for start command
    PLAYING,        // Active playback
    E_STOP          // Emergency stop, drivers disabled
};

// ============================================================================
// Stepper Motor Component
// ============================================================================

class StepperMotorComponent : public Component {
public:
    StepperMotorComponent();
    ~StepperMotorComponent() override;
    
    void onInitialize() override;
    
    // HAL injection (call before initialize, or set a default stub HAL)
    void setHAL(StepperMotorHAL* hal);
    
    // NVS persistence for choreography metadata
    void saveCustomData(nvs_handle_t handle) override;
    void loadCustomData(nvs_handle_t handle) override;
    bool hasCustomDataToSave() const override { return metadataModified; }
    
    // Post-load reconciliation
    void onPostLoadReconcile() override;
    
    // ========================================================================
    // Choreography Management API
    // ========================================================================
    
    /**
     * Begin uploading a new choreography.
     * @param name User-friendly name for the choreography
     * @param total_bytes Expected total size of the JSON file
     * @return ESP_OK, ESP_ERR_NO_MEM if insufficient space
     */
    esp_err_t uploadBegin(const std::string& name, size_t total_bytes);
    
    /**
     * Upload a chunk of choreography data.
     * @param chunk_index Chunk index (0, 1, 2, ...)
     * @param data Raw bytes (base64-decoded JSON fragment)
     * @param len Length of data
     * @return ESP_OK, ESP_ERR_INVALID_STATE if upload not started
     */
    esp_err_t uploadChunk(uint16_t chunk_index, const uint8_t* data, size_t len);
    
    /**
     * Finalize upload, parse JSON, store to SPIFFS.
     * @return ESP_OK, ESP_ERR_INVALID_ARG if JSON parse fails
     */
    esp_err_t uploadCommit();
    
    /**
     * Delete a choreography by index.
     * @param index Choreography index to delete
     */
    void deleteChoreography(int16_t index);
    
    /**
     * Load a choreography into memory for playback.
     * @param index Choreography index to load
     * @return ESP_OK if loaded successfully
     */
    esp_err_t loadChoreography(int16_t index);
    
    // ========================================================================
    // Playback Control API
    // ========================================================================
    
    /**
     * Start playback from the beginning.
     * Requires a choreography to be loaded.
     */
    esp_err_t startPlayback();
    
    /**
     * Stop playback, hold current position.
     */
    void stopPlayback();
    
    /**
     * Emergency stop - disable all drivers immediately.
     */
    void emergencyStop();
    
    /**
     * Clear emergency stop, return to IDLE.
     */
    void clearEmergencyStop();
    
    /**
     * Start homing sequence.
     */
    esp_err_t startHoming();
    
    // ========================================================================
    // Status
    // ========================================================================
    
    PlaybackState getState() const { return state; }
    size_t getChoreographyCount() const { return choreographyMeta.size(); }
    int16_t getActiveChoreographyIndex() const { return activeChoreographyIndex; }
    float getPlaybackProgress() const;  // 0.0 - 1.0
    
    static constexpr const char* TAG = "StepperMotor";
    
private:
    // ========================================================================
    // Hardware Abstraction
    // ========================================================================
    StepperMotorHAL* hal;
    bool halOwned;  // true if we should delete HAL on destruction
    
    // ========================================================================
    // Parameters (exposed to component system)
    // ========================================================================
    
    // Playback control
    BoolParameter* playing;                  // true=playing, false=stopped
    IntParameter* activeChoreographyParam;   // Selected choreography index (-1=none)
    BoolParameter* enableDrivers;            // Enable/disable motor drivers
    IntParameter* stateParam;                // Current PlaybackState (read-only)
    
    // Playback status (read-only)
    FloatParameter* playbackProgressParam;   // 0.0 - 1.0
    FloatParameter* playbackTimeParam;       // Current time in seconds
    IntParameter* choreographyCountParam;    // Number of stored choreographies
    
    // Motor positions (read-only, updated during playback)
    IntParameter* motorPositions;            // Current step positions [4]
    IntParameter* motorTargets;              // Target step positions [4]
    
    // Homing
    BoolParameter* homeCommand;              // Set true to start homing
    BoolParameter* isHomed;                  // true after successful homing
    
    // Emergency stop
    BoolParameter* eStopCommand;             // Set true to trigger E-STOP
    BoolParameter* eStopClear;               // Set true to clear E-STOP
    
    // Upload parameters (writable triggers)
    StringParameter* uploadName;             // Name for new choreography
    IntParameter* uploadTotalBytes;          // Expected file size
    IntParameter* uploadChunkIndex;          // Current chunk index
    StringParameter* uploadChunkData;        // Base64 chunk data
    BoolParameter* uploadCommitParam;        // Set true to finalize
    
    // Delete parameter
    IntParameter* deleteChoreographyParam;   // Set to index to delete
    
    // Query parameters (for GUI)
    IntParameter* queryChoreographyIndex;    // Set to query a choreography
    StringParameter* queryChoreographyName;  // Read-only: name
    FloatParameter* queryChoreographyDuration; // Read-only: duration
    IntParameter* queryChoreographyLoop;     // Read-only: loop count
    StringParameter* choreographyIdsParam;   // Comma-separated list of IDs
    
    // Configuration
    IntParameter* chunkSizeParam;            // Read-only: chunk size for uploads
    FloatParameter* evalRateHz;              // ISR evaluation rate (default 2000 Hz)
    IntParameter* microsteppingParam;        // Microstepping divisor (1,2,4,8,16,32,etc). Default 16.
    BoolParameter* microsteppingConfigurable; // Read-only: true if HAL supports software microstepping
    
    // ========================================================================
    // Choreography Storage
    // ========================================================================
    std::map<int16_t, ChoreographyMeta> choreographyMeta;  // Stable ID -> metadata
    Choreography activeChoreography;                       // Currently loaded choreography
    int16_t activeChoreographyIndex;                       // Which choreography is loaded (-1=none)
    bool metadataModified;                                 // Needs NVS save
    
    // ========================================================================
    // Playback State
    // ========================================================================
    PlaybackState state;
    int32_t currentPosition[4];              // Current step position for each motor
    int32_t targetPosition[4];               // Target position from spline eval
    uint16_t currentKnotIndex[4];            // Current knot index per motor
    float playbackStartTime;                 // micros() at playback start (converted to float seconds)
    int64_t playbackStartUs;                 // micros() at playback start (raw)
    int16_t loopsRemaining;                  // Loops left (-1=infinite)
    
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
    int32_t homingDirection[4];              // -1 or +1
    uint32_t homingTimeoutMs;
    uint32_t homingStartMs;
    
    // ========================================================================
    // ISR / Timer
    // ========================================================================
    esp_timer_handle_t evalTimer;
    SemaphoreHandle_t stateMutex;
    std::atomic_bool stopRequested;
    
    // ========================================================================
    // Internal Methods
    // ========================================================================
    
    // Spline evaluation
    float hermiteEval(float t, const SplineKnot& k0, const SplineKnot& k1);
    int32_t evaluateMotorPosition(uint8_t motor_index, float t);
    
    // Choreography management
    int16_t findNextChoreographyId() const;
    esp_err_t parseChoreographyJson(const char* json, size_t len, Choreography& out);
    esp_err_t saveChoreographyToSpiffs(const std::string& filename, const uint8_t* data, size_t len);
    esp_err_t loadChoreographyFromSpiffs(const std::string& filename, Choreography& out);
    void updateStatusParams();
    void updateChoreographyIdsParam();
    
    // ISR callback
    static void IRAM_ATTR evalTimerCallback(void* arg);
    void IRAM_ATTR evalTimerISR();
    
    // Homing
    void homingStep();
    
    // State transitions
    void transitionTo(PlaybackState newState);
    
    // SPIFFS helpers
    static constexpr const char* SPIFFS_BASE_PATH = "/spiffs";
    static constexpr const char* CHOREO_FILE_PREFIX = "choreo_";
    static constexpr size_t UPLOAD_CHUNK_SIZE = 4096;
    static constexpr size_t MAX_CHOREOGRAPHY_SIZE = 256 * 1024;  // 256KB max
    static constexpr uint8_t NUM_MOTORS = 4;
    static constexpr float DEFAULT_EVAL_RATE_HZ = 2000.0f;
};
