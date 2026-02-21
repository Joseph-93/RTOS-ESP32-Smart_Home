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
 * RGB LED Component - Animation Playback Engine
 * 
 * This is a DUMB playback device. All creativity lives on the central hub.
 * The ESP32 receives animation frames via chunked upload and plays them back.
 * 
 * States:
 * - OFF (playing=false): All LEDs off
 * - PLAY (playing=true): Playing uploaded animation frames
 * 
 * Animation upload protocol (chunked for bandwidth):
 * 1. Set anim_total_frames to start upload (allocates memory)
 * 2. Set anim_chunk_index, then anim_chunk_data (base64) for each chunk
 * 3. Set loop=true/false for looping behavior
 * 4. Set playing=true to start playback
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
    
    // Get animation status
    uint16_t getAnimationFrameCount() const { return animation_frame_count; }
    uint16_t getAnimationCurrentFrame() const { return animation_current_frame; }
    size_t getAnimationMemoryUsed() const { return animation_data.size(); }
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
    // Animation Storage
    // ========================================================================
    std::vector<uint8_t> animation_data;
    uint16_t animation_frame_count{0};       // Total frames in animation
    uint16_t animation_current_frame{0};     // Current playback position
    uint32_t animation_frame_start_ms{0};    // When current frame started
    bool animation_uploading{false};         // True during chunked upload
    size_t animation_upload_offset{0};       // Bytes received so far
    uint16_t animation_led_count{0};         // LED count the animation was built for
    
    // Animation status parameters (read-only)
    IntParameter* animFrameCount;            // Read-only: frame count
    IntParameter* animMemoryUsed;            // Read-only: bytes used
    IntParameter* animMemoryMax;             // Read-only: max bytes available
    StringParameter* animUploadData;         // Write: base64 chunk data
    IntParameter* animUploadChunkIndex;      // Write: chunk index
    IntParameter* animTotalFrames;           // Write: start upload with frame count
    BoolParameter* animCommit;               // Write: true to finalize upload
    
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
    
    // Frame size: 3 bytes per LED (RGB) + 2 bytes duration
    size_t getFrameSize() const { return (RGB_LED_COUNT * 3) + 2; }
    
    // Frame size for playback (uses snapshot from upload time)
    size_t getAnimationFrameSize() const { return (animation_led_count * 3) + 2; }
    
    // Task functions
    static void ledTaskWrapper(void* pvParameters);
    void ledTask();
};
