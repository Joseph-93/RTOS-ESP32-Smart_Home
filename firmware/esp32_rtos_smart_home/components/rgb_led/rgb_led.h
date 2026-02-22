#pragma once

#include "component.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include <vector>
#include <atomic>

/**
 * RGB LED Component - Multi-Preset Animation Playback Engine
 * 
 * This is a DUMB playback device. All creativity lives on the central hub.
 * The ESP32 stores MULTIPLE animation presets in a shared memory pool and
 * plays whichever one is selected via the active_preset parameter.
 * 
 * States:
 * - OFF (playing=false): All LEDs off
 * - PLAY (playing=true): Playing the active preset's frames
 * 
 * Preset upload protocol (chunked, adds a NEW preset each time):
 * 1. Set anim_total_frames to start upload (allocates staging buffer)
 *    - Rejected with ESP_ERR_NO_MEM if it won't fit in the pool
 * 2. Set anim_chunk_index, then anim_chunk_data (base64) for each chunk
 * 3. Set anim_commit to finalize — moves staging into a new preset slot
 * 4. Set active_preset to the desired index, playing=true to start
 * 
 * Preset management:
 * - active_preset: selects which preset plays (0-indexed, -1 = none)
 * - delete_preset: set to an index to remove that preset and free memory
 * - preset_count: read-only, how many presets are stored
 * - anim_memory_used / anim_memory_max: pool usage across ALL presets
 * 
 * Frame format: [R0,G0,B0, R1,G1,B1, ..., Rn,Gn,Bn, duration_ms_lo, duration_ms_hi]
 * Frame size = (led_count * 3) + 2 bytes
 */

// Hardware configuration
#define RGB_LED_DEFAULT_PIN     GPIO_NUM_13  // Data pin for LED strip
#define RGB_LED_COUNT           30           // Number of LEDs on the strip

// Animation memory limits
// This is a dedicated lamp — animation playback is the primary workload.
// The swap-before-allocate pattern in animationBegin() prevents double-allocation
// OOM, so we can safely use most of the available heap for animation data.
#define RGB_LED_MAX_ANIMATION_MEMORY    (150 * 1024)  // 150KB — ~1,630 frames at 30 LEDs
#define RGB_LED_CHUNK_SIZE              1024          // Max bytes per upload chunk

// A single animation preset: its raw frame data + metadata
struct AnimationPreset {
    std::string name;                 // User-assigned name (e.g. "Rainbow", "Breathing")
    std::vector<uint8_t> data;        // Raw frame bytes (all frames contiguous)
    uint16_t frame_count{0};          // Number of frames in this preset
    uint16_t led_count{0};            // LED count it was built for
    bool loop{true};                  // Whether this preset should loop

    size_t getFrameSize() const { return (led_count * 3) + 2; }
};

class RgbLedComponent : public Component {
public:
    RgbLedComponent();
    ~RgbLedComponent();
    
    void onInitialize() override;
    
    // ========================================================================
    // Animation API
    // ========================================================================
    
    // Begin a new animation upload (clears existing, allocates memory)
    // Returns: ESP_OK, ESP_ERR_NO_MEM if too many frames requested
    esp_err_t animationBegin(uint16_t total_frames);
    
    // Upload a chunk of animation data (base64 decoded bytes)
    // chunk_index: which chunk (0, 1, 2, ...)
    // data: raw frame data bytes
    // len: number of bytes in this chunk
    // Returns: ESP_OK, ESP_ERR_INVALID_STATE if animationBegin not called
    esp_err_t animationChunk(uint16_t chunk_index, const uint8_t* data, size_t len);
    
    // Finalize animation upload (does not start playback - set playing=true for that)
    esp_err_t animationCommit();
    
    // Clear animation data and free memory
    void animationClear();
    
    // Get animation/preset status
    size_t getPresetCount() const { return presets.size(); }
    int16_t getActivePresetIndex() const { return active_preset_index; }
    uint16_t getAnimationCurrentFrame() const { return animation_current_frame; }
    size_t getAnimationMemoryMax() const { return RGB_LED_MAX_ANIMATION_MEMORY; }

private:
    // Parameters (exposed to component system)
    IntParameter* brightness;         // Global brightness (0-100)
    BoolParameter* playing;           // true=play animation, false=off
    BoolParameter* loop;              // true=loop forever, false=play once
    
    // ESP-IDF led_strip handle (RMT backend)
    led_strip_handle_t led_strip;
    
    // Thread safety: mutex protects led_strip and color_buffer
    SemaphoreHandle_t strip_mutex;
    
    // Internal color buffer (RGB format, 3 bytes per LED)
    std::vector<uint8_t> color_buffer;
    
    // Task handles and synchronization
    TaskHandle_t led_task_handle;
    std::atomic_bool stop_requested{false};    // Atomic: signals task to exit cleanly
    std::atomic_bool task_running{false};      // Atomic: task sets false before exiting
    
    // ========================================================================
    // Preset Storage (shared memory pool)
    // ========================================================================
    std::vector<AnimationPreset> presets;             // All stored presets
    int16_t active_preset_index{-1};                  // Which preset is playing (-1 = none)
    
    // Playback state (for active preset)
    uint16_t animation_current_frame{0};              // Current frame in active preset
    uint32_t animation_frame_start_ms{0};             // When current frame started
    
    // Upload staging area (builds a new preset before committing to pool)
    std::vector<uint8_t> upload_staging_data;
    uint16_t upload_frame_count{0};                   // Frames in staging preset
    uint16_t upload_led_count{0};                     // LED count for staging preset
    size_t upload_offset{0};                          // Bytes received so far
    bool uploading{false};                            // True during chunked upload
    
    // Read-only status parameters
    IntParameter* animFrameCount;                     // Active preset frame count
    IntParameter* animMemoryUsed;                     // Total bytes across all presets
    IntParameter* animMemoryMax;                      // Max pool size (150KB)
    IntParameter* presetCountParam;                   // Number of stored presets
    
    // Preset management parameters (writable)
    IntParameter* activePresetParam;                  // Select which preset to play
    IntParameter* deletePresetParam;                  // Set to index to delete that preset
    
    // Animation upload parameters (writable triggers)
    StringParameter* animUploadData;                  // Write: base64 chunk data
    IntParameter* animUploadChunkIndex;               // Write: chunk index
    IntParameter* animTotalFrames;                    // Write: start upload with frame count
    StringParameter* animPresetName;                  // Write: name for the preset being uploaded
    BoolParameter* animUploadLoop;                    // Write: loop setting for the preset being uploaded
    BoolParameter* animCommit;                        // Write: true to finalize upload
    
    // Preset query/download parameters
    IntParameter* queryPresetIndex;                   // Write: set to select preset to query
    StringParameter* queryPresetName;                 // Read-only: name of queried preset
    IntParameter* queryPresetFrameCount;              // Read-only: frame count of queried preset
    IntParameter* queryPresetDataSize;                // Read-only: data size in bytes
    BoolParameter* queryPresetLoop;                   // Read-only: loop setting of queried preset
    IntParameter* chunkSizeParam;                     // Read-only: chunk size for upload/download
    IntParameter* queryDownloadChunkIndex;            // Write: set to request a download chunk
    StringParameter* queryDownloadChunkData;          // Read-only: base64 chunk of preset data
    
    // Initialize LED strip with RMT backend
    esp_err_t initLedStrip(gpio_num_t gpio, uint16_t led_count);
    
    // Mutex helpers
    bool takeMutex();           // Timeout-based (for external callers)
    bool takeMutexBlocking();   // Blocking (for LED task - never times out)
    void giveMutex();
    
    // Internal transmit (assumes mutex held)
    esp_err_t transmitBuffer();
    
    // Blocking show variant for LED task
    esp_err_t showBlocking();
    
    // Apply brightness to a color value
    uint8_t applyBrightness(uint8_t color);
    
    // Play current animation frame, advance when duration expires
    void playFrame();
    
    // Turn all LEDs off
    void ledsOff();
    
    // Frame size for new uploads (uses hardware LED count)
    size_t getFrameSize() const { return (RGB_LED_COUNT * 3) + 2; }
    
    // Total memory used across all presets
    size_t calcTotalMemoryUsed() const;
    
    // Delete a specific preset by index, adjusting active_preset_index
    void deletePresetByIndex(int index);
    
    // Push all read-only status parameters (memory, counts) to the param system
    void updateStatusParams();
    
    // Task functions
    static void ledTaskWrapper(void* pvParameters);
    void ledTask();
};
