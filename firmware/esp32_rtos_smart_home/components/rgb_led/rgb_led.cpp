/**
 * WS2812B RGB LED Driver using ESP-IDF led_strip component (RMT backend)
 * 
 * DEPENDENCY: Requires ESP-IDF led_strip component.
 * Add to idf_component.yml:
 *   dependencies:
 *     espressif/led_strip: "^2.0.0"
 * 
 * Or add to CMakeLists.txt REQUIRES: led_strip
 * 
 * WHY led_strip COMPONENT:
 * - Official ESP-IDF driver with RMT backend for deterministic timing
 * - Handles WS2812 protocol timing in hardware (not bit-banging)
 * - Works reliably under FreeRTOS with WiFi/BLE interrupts
 * - Handles latch/reset timing automatically via led_strip_refresh()
 * 
 * WHY GRB PIXEL FORMAT:
 * - WS2812/WS2812B LEDs expect data in Green-Red-Blue order
 * - The led_strip component handles this via LED_PIXEL_FORMAT_GRB
 * 
 * WHY MUTEX:
 * - Multiple FreeRTOS tasks may call setLedColor/refresh concurrently
 * - The RMT peripheral and internal pixel buffer must be protected
 * - Mutex ensures atomic access to the LED strip state
 */

#include "rgb_led.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include <cstring>
#include <algorithm>
#include <cmath>

static const char* TAG = "RgbLed";

// ============================================================================
// Configuration Constants
// ============================================================================

// RMT resolution: 10 MHz = 0.1 µs per tick (required for WS2812 timing)
#define RGB_LED_RMT_RESOLUTION_HZ   10000000

// Pixel format: WS2812/WS2812B uses GRB ordering
// Change to LED_PIXEL_FORMAT_RGB if using RGB-ordered LEDs
#define RGB_LED_PIXEL_FORMAT        LED_PIXEL_FORMAT_GRB

// LED model: WS2812 (also works for WS2812B, SK6812 in RGB mode)
#define RGB_LED_MODEL               LED_MODEL_WS2812

// Mutex timeout for thread-safe operations (in ms)
#define RGB_LED_MUTEX_TIMEOUT_MS    100

// ============================================================================
// Base64 Decoding (for animation chunk uploads)
// ============================================================================

static const uint8_t base64_decode_table[256] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63, // +, /
    52,53,54,55,56,57,58,59,60,61,64,64,64,64,64,64, // 0-9
    64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, // A-O
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64, // P-Z
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, // a-o
    41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64, // p-z
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
};

// Decode base64 string to binary data
// Returns number of bytes decoded, or -1 on error
static int base64_decode(const char* input, size_t input_len, uint8_t* output, size_t output_max) {
    size_t out_len = 0;
    uint32_t buffer = 0;
    int bits_collected = 0;
    
    for (size_t i = 0; i < input_len; i++) {
        char c = input[i];
        if (c == '=') break; // Padding, we're done
        
        uint8_t d = base64_decode_table[(uint8_t)c];
        if (d == 64) continue; // Skip invalid chars (whitespace, etc.)
        
        buffer = (buffer << 6) | d;
        bits_collected += 6;
        
        if (bits_collected >= 8) {
            bits_collected -= 8;
            if (out_len >= output_max) return -1; // Buffer overflow
            output[out_len++] = (buffer >> bits_collected) & 0xFF;
        }
    }
    
    return (int)out_len;
}

// ============================================================================
// RgbLedComponent Implementation
// ============================================================================

RgbLedComponent::RgbLedComponent() 
    : Component("RgbLed")
    , brightness(nullptr)
    , playing(nullptr)
    , loop(nullptr)
    , led_strip(nullptr)
    , strip_mutex(nullptr)
    , led_task_handle(nullptr)
{
    // Atomic members are initialized via brace-init in header
    ESP_LOGI(TAG, "RgbLedComponent created");
}

RgbLedComponent::~RgbLedComponent() {
    // Signal task to stop and wait for clean exit
    if (led_task_handle && task_running.load()) {
        stop_requested.store(true);
        
        // Wake the task if it's waiting
        xTaskNotifyGive(led_task_handle);
        
        // Wait for task to exit (with timeout to avoid deadlock)
        int timeout_ms = 500;
        while (task_running.load() && timeout_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            timeout_ms -= 10;
        }
        
        // If task didn't exit cleanly, force delete (last resort)
        if (task_running.load()) {
            ESP_LOGW(TAG, "Task did not exit cleanly, forcing delete");
            vTaskDelete(led_task_handle);
        }
        led_task_handle = nullptr;
    }
    
    // Now safe to clean up led_strip (task is stopped)
    // Take mutex to ensure no other caller is mid-operation
    if (strip_mutex) {
        xSemaphoreTake(strip_mutex, portMAX_DELAY);
    }
    
    if (led_strip) {
        led_strip_clear(led_strip);
        led_strip_refresh(led_strip);
        led_strip_del(led_strip);
        led_strip = nullptr;
    }
    
    if (strip_mutex) {
        xSemaphoreGive(strip_mutex);
        vSemaphoreDelete(strip_mutex);
        strip_mutex = nullptr;
    }
}

esp_err_t RgbLedComponent::initLedStrip(gpio_num_t gpio, uint16_t led_count) {
    ESP_LOGI(TAG, "Initializing LED strip on core %d: GPIO %d, %d LEDs",
             xPortGetCoreID(), gpio, led_count);
    
    // Configure LED strip with RMT backend
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = RGB_LED_COUNT;
    strip_config.led_model = RGB_LED_MODEL;
    strip_config.led_pixel_format = RGB_LED_PIXEL_FORMAT;
    strip_config.flags.invert_out = false;
    
    // Configure RMT backend with 10 MHz resolution
    // 30 LEDs × 24 bits = 720 RMT symbols per refresh.
    // ESP32 has 512 total RMT memory entries.  No other RMT users, so take
    // all 8 blocks (512 symbols).  This means only 1 ISR refill per refresh
    // (720 − 512 = 208 remaining), minimising the window for WiFi interrupts
    // to cause an RMT underflow that shifts the data stream.
    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = RGB_LED_RMT_RESOLUTION_HZ;
    rmt_config.mem_block_symbols = 512;
    rmt_config.flags.with_dma = false;  // DMA not available on original ESP32
    
    // Create LED strip with RMT backend
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Clear the strip initially
    ret = led_strip_clear(led_strip);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Initial clear failed: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "LED strip initialized: %d LEDs, GPIO %d, RMT %d Hz, 512 symbol buffer",
             RGB_LED_COUNT, gpio, RGB_LED_RMT_RESOLUTION_HZ);
    return ESP_OK;
}

bool RgbLedComponent::takeMutex() {
    if (!strip_mutex) return false;
    return xSemaphoreTake(strip_mutex, pdMS_TO_TICKS(RGB_LED_MUTEX_TIMEOUT_MS)) == pdTRUE;
}

void RgbLedComponent::giveMutex() {
    if (strip_mutex) {
        xSemaphoreGive(strip_mutex);
    }
}

void RgbLedComponent::onInitialize() {
    ESP_LOGI(TAG, "Initializing RgbLedComponent...");
    
    // Create mutex first — needed before the task starts
    strip_mutex = xSemaphoreCreateMutex();
    if (!strip_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }
    
    // Allocate color buffer (always 30 LEDs)
    color_buffer.resize(RGB_LED_COUNT * 3, 0);
    
    // Create parameters
    brightness = addIntParam("brightness", 1, 1, 0, 100, 100);
    playing = addBoolParam("playing", 1, 1, false);  // false=off, true=play animation
    loop = addBoolParam("loop", 1, 1, true);  // true=loop forever, false=play once
    
    // Animation status parameters (read-only)
    animFrameCount = addIntParam("anim_frame_count", 1, 1, 0, 65535, 0, true);  // Read-only
    animMemoryUsed = addIntParam("anim_memory_used", 1, 1, 0, INT32_MAX, 0, true);  // Read-only
    animMemoryMax = addIntParam("anim_memory_max", 1, 1, 0, INT32_MAX, RGB_LED_MAX_ANIMATION_MEMORY, true);  // Read-only
    
    // Animation upload parameters (writable)
    animTotalFrames = addIntParam("anim_total_frames", 1, 1, 0, 65535, 0);
    animUploadChunkIndex = addIntParam("anim_chunk_index", 1, 1, 0, 65535, 0);
    animUploadData = addStringParam("anim_chunk_data", 1, 1, "");
    animCommit = addBoolParam("anim_commit", 1, 1, false);
    
    // Set up onChange callbacks
    if (playing) {
        playing->setOnChange([this](size_t row, size_t col, bool val) {
            ESP_LOGI(TAG, "Playing: %s", val ? "true" : "false");
            if (val) {
                // Reset to frame 0 when starting playback
                animation_current_frame = 0;
                animation_frame_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            }
        });
    }
    
    // Animation upload callbacks
    if (animTotalFrames) {
        animTotalFrames->setOnChange([this](size_t row, size_t col, int val) {
            if (val > 0) {
                esp_err_t ret = animationBegin((uint16_t)val);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to begin animation: %s", esp_err_to_name(ret));
                }
            }
        });
    }
    
    if (animCommit) {
        animCommit->setOnChange([this](size_t row, size_t col, bool val) {
            if (val) {
                animationCommit();
                animCommit->setValue(0, 0, false);  // Reset trigger
            }
        });
    }
    
    if (animUploadData && animUploadChunkIndex) {
        animUploadData->setOnChange([this](size_t row, size_t col, const std::string& val) {
            if (val.empty()) return;
            
            int chunk_index = animUploadChunkIndex->getValue(0, 0);
            
            // Decode base64
            size_t max_decoded = (val.size() * 3) / 4 + 4;
            std::vector<uint8_t> decoded(max_decoded);
            
            int decoded_len = base64_decode(val.c_str(), val.size(), decoded.data(), max_decoded);
            if (decoded_len < 0) {
                ESP_LOGE(TAG, "Base64 decode failed for chunk %d", chunk_index);
                return;
            }
            
            esp_err_t ret = animationChunk((uint16_t)chunk_index, decoded.data(), (size_t)decoded_len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Animation chunk %d failed: %s", chunk_index, esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "Received animation chunk %d, %d bytes", chunk_index, decoded_len);
            }
            
            animUploadData->setValue(0, 0, "");  // Clear to detect next write
        });
    }
    
    // Pin LED task to core 1 so that led_strip_new_rmt_device() (called
    // inside the task) registers the RMT refill ISR on core 1.  WiFi runs
    // on core 0, so the two can never preempt each other — eliminating the
    // RMT buffer underflow that causes LED flicker.
    BaseType_t result = xTaskCreatePinnedToCore(
        ledTaskWrapper,
        "rgb_led_task",
        4096,
        this,
        tskIDLE_PRIORITY + 1,
        &led_task_handle,
        1   // Core 1 — away from WiFi (core 0)
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RGB LED task");
    } else {
        ESP_LOGI(TAG, "RGB LED task created, %d LEDs on GPIO %d", 
                 RGB_LED_COUNT, RGB_LED_DEFAULT_PIN);
    }
}

uint8_t RgbLedComponent::applyBrightness(uint8_t color) {
    if (!brightness) return color;
    int bright = brightness->getValue(0, 0);
    return (uint8_t)((color * bright) / 100);
}

// ============================================================================
// LED Control
// ============================================================================

void RgbLedComponent::ledsOff() {
    if (!takeMutexBlocking()) return;
    
    std::fill(color_buffer.begin(), color_buffer.end(), 0);
    if (led_strip) {
        led_strip_clear(led_strip);
        led_strip_refresh(led_strip);
    }
    
    giveMutex();
}

esp_err_t RgbLedComponent::showBlocking() {
    if (!led_strip) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!takeMutexBlocking()) {
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t ret = transmitBuffer();
    
    giveMutex();
    return ret;
}

esp_err_t RgbLedComponent::transmitBuffer() {
    // Internal: assumes mutex is held
    for (uint16_t i = 0; i < RGB_LED_COUNT; i++) {
        size_t offset = i * 3;
        uint8_t r = applyBrightness(color_buffer[offset + 0]);
        uint8_t g = applyBrightness(color_buffer[offset + 1]);
        uint8_t b = applyBrightness(color_buffer[offset + 2]);

        esp_err_t ret = led_strip_set_pixel(led_strip, i, r, g, b);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return led_strip_refresh(led_strip);
}

// ============================================================================
// Animation Implementation
// ============================================================================

esp_err_t RgbLedComponent::animationBegin(uint16_t total_frames) {
    size_t frame_size = getFrameSize();
    size_t total_bytes = total_frames * frame_size;
    
    ESP_LOGI(TAG, "Animation begin: %u frames, %u bytes/frame, %zu total bytes",
             total_frames, frame_size, total_bytes);
    
    // Check memory limits
    if (total_bytes > RGB_LED_MAX_ANIMATION_MEMORY) {
        ESP_LOGE(TAG, "Animation too large: %zu bytes requested, max %d bytes",
                 total_bytes, RGB_LED_MAX_ANIMATION_MEMORY);
        return ESP_ERR_NO_MEM;
    }
    
    // Check available heap
    size_t free_heap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (total_bytes > free_heap * 0.8) {  // Leave 20% headroom
        ESP_LOGE(TAG, "Insufficient heap: need %zu bytes, only %zu available",
                 total_bytes, free_heap);
        return ESP_ERR_NO_MEM;
    }
    
    if (!takeMutex()) {
        return ESP_ERR_TIMEOUT;
    }
    
    // Clear existing animation
    animation_data.clear();
    
    // Pre-allocate memory - use reserve + resize pattern to detect allocation failure
    animation_data.reserve(total_bytes);
    if (animation_data.capacity() < total_bytes) {
        giveMutex();
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for animation", total_bytes);
        return ESP_ERR_NO_MEM;
    }
    animation_data.resize(total_bytes, 0);
    
    animation_frame_count = total_frames;
    animation_current_frame = 0;
    animation_upload_offset = 0;
    animation_uploading = true;
    animation_led_count = RGB_LED_COUNT;
    
    giveMutex();
    
    ESP_LOGI(TAG, "Animation buffer allocated: %zu bytes for %u frames",
             animation_data.size(), total_frames);
    
    // Update read-only parameters
    if (animFrameCount) animFrameCount->setValue(0, 0, total_frames);
    if (animMemoryUsed) animMemoryUsed->setValue(0, 0, (int)total_bytes);
    
    return ESP_OK;
}

esp_err_t RgbLedComponent::animationChunk(uint16_t chunk_index, const uint8_t* data, size_t len) {
    if (!animation_uploading) {
        ESP_LOGE(TAG, "Animation chunk received but no upload in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    size_t expected_offset = chunk_index * RGB_LED_CHUNK_SIZE;
    
    if (expected_offset != animation_upload_offset) {
        ESP_LOGW(TAG, "Chunk out of order: expected offset %zu, got chunk %u (offset %zu)",
                 animation_upload_offset, chunk_index, expected_offset);
        // Allow it anyway - might be a retry
    }
    
    if (!takeMutex()) {
        return ESP_ERR_TIMEOUT;
    }
    
    // Bounds check
    if (animation_upload_offset + len > animation_data.size()) {
        giveMutex();
        ESP_LOGE(TAG, "Chunk would overflow buffer: offset %zu + len %zu > size %zu",
                 animation_upload_offset, len, animation_data.size());
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Copy data
    memcpy(animation_data.data() + animation_upload_offset, data, len);
    animation_upload_offset += len;
    
    giveMutex();
    
    ESP_LOGD(TAG, "Animation chunk %u: %zu bytes, total uploaded: %zu/%zu",
             chunk_index, len, animation_upload_offset, animation_data.size());
    
    return ESP_OK;
}

esp_err_t RgbLedComponent::animationCommit() {
    if (!animation_uploading) {
        ESP_LOGE(TAG, "Animation commit but no upload in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (animation_upload_offset != animation_data.size()) {
        ESP_LOGW(TAG, "Animation incomplete: received %zu of %zu bytes",
                 animation_upload_offset, animation_data.size());
        // Allow it - partial animations might still work
    }
    
    if (!takeMutex()) {
        return ESP_ERR_TIMEOUT;
    }
    
    animation_uploading = false;
    animation_current_frame = 0;
    animation_frame_start_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // If already playing, kick the task to restart from frame 0 with the new data.
    // Without this, the playing onChange never re-fires (value didn't change),
    // and the old animation keeps running.
    bool was_playing = playing && playing->getValue(0, 0);

    giveMutex();

    // Reset the total_frames trigger back to 0 so the next upload always fires
    // the onChange callback even when uploading the same frame count as before.
    // (onChange only fires on value *change*, so if count is the same, animationBegin
    // would never be called and the commit would fail with "no upload in progress".)
    if (animTotalFrames) animTotalFrames->setValue(0, 0, 0);

    ESP_LOGI(TAG, "Animation committed: %u frames%s",
             animation_frame_count, was_playing ? " (restarting playback)" : "");

    if (was_playing && led_task_handle) {
        xTaskNotifyGive(led_task_handle);
    }

    return ESP_OK;
}

void RgbLedComponent::animationClear() {
    if (!takeMutex()) {
        return;
    }
    
    animation_uploading = false;
    animation_frame_count = 0;
    animation_current_frame = 0;
    animation_led_count = 0;
    animation_data.clear();
    animation_data.shrink_to_fit();  // Actually free memory
    
    giveMutex();
    
    ESP_LOGI(TAG, "Animation cleared");
    
    // Update parameters
    if (animFrameCount) animFrameCount->setValue(0, 0, 0);
    if (animMemoryUsed) animMemoryUsed->setValue(0, 0, 0);
    
    // Stop playback
    if (playing) playing->setValue(0, 0, false);
}

void RgbLedComponent::playFrame() {
    if (animation_frame_count == 0 || animation_data.empty()) {
        return;  // No animation loaded
    }
    
    size_t frame_size = getAnimationFrameSize();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    
    // Get current frame data pointer
    size_t frame_offset = animation_current_frame * frame_size;
    if (frame_offset + frame_size > animation_data.size()) {
        ESP_LOGE(TAG, "Frame %u out of bounds", animation_current_frame);
        return;
    }
    
    const uint8_t* frame_data = animation_data.data() + frame_offset;
    
    // Duration is last 2 bytes of frame (little-endian)
    uint16_t duration_ms = frame_data[frame_size - 2] | (frame_data[frame_size - 1] << 8);
    
    // Check if frame expired
    uint32_t elapsed = now_ms - animation_frame_start_ms;
    if (elapsed >= duration_ms) {
        // Advance to next frame
        animation_current_frame++;
        
        if (animation_current_frame >= animation_frame_count) {
            // Check loop parameter
            bool should_loop = loop ? loop->getValue(0, 0) : true;
            if (should_loop) {
                animation_current_frame = 0;
            } else {
                // Animation complete, stop playback
                if (playing) {
                    playing->setValue(0, 0, false);
                }
                return;
            }
        }
        
        animation_frame_start_ms = now_ms;
        
        // Recalculate frame pointer
        frame_offset = animation_current_frame * frame_size;
        frame_data = animation_data.data() + frame_offset;
    }
    
    // Copy frame colors to buffer (excluding duration bytes)
    if (!takeMutex()) return;
    
    // Use the LED count the animation was built for, not the live count.
    // If the animation has fewer LEDs than the strip, zero the remainder
    // so stale data from a previous animation doesn't leak through.
    size_t anim_color_bytes = animation_led_count * 3;
    size_t buf_color_bytes  = RGB_LED_COUNT * 3;
    
    if (anim_color_bytes <= buf_color_bytes) {
        memcpy(color_buffer.data(), frame_data, anim_color_bytes);
        // Zero LEDs beyond the animation's range
        if (anim_color_bytes < buf_color_bytes) {
            memset(color_buffer.data() + anim_color_bytes, 0, buf_color_bytes - anim_color_bytes);
        }
    } else {
        // Animation has more LEDs than the strip — truncate
        memcpy(color_buffer.data(), frame_data, buf_color_bytes);
    }
    
    giveMutex();
}

void RgbLedComponent::ledTaskWrapper(void* pvParameters) {
    RgbLedComponent* self = static_cast<RgbLedComponent*>(pvParameters);
    self->ledTask();
}

bool RgbLedComponent::takeMutexBlocking() {
    // For use by the LED task - waits indefinitely
    if (!strip_mutex) return false;
    return xSemaphoreTake(strip_mutex, portMAX_DELAY) == pdTRUE;
}

void RgbLedComponent::ledTask() {
    ESP_LOGI(TAG, "RGB LED task running on core %d", xPortGetCoreID());
    
    // Initialize LED strip HERE so the RMT peripheral's ISR is registered
    // on this core (core 1), safely away from WiFi ISRs on core 0.
    esp_err_t ret = initLedStrip(RGB_LED_DEFAULT_PIN, RGB_LED_COUNT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED strip: %s", esp_err_to_name(ret));
        task_running.store(false);
        vTaskDelete(nullptr);
        return;
    }
    
    // Start with LEDs off
    ledsOff();
    
    task_running.store(true);
    const uint32_t update_interval_ms = 1;  // ~1000 FPS max — core 1 is dedicated to this
    bool was_playing = false;
    
    while (!stop_requested.load()) {
        // Check playback state
        bool is_playing = playing ? playing->getValue(0, 0) : false;
        
        if (!is_playing) {
            // Not playing - turn LEDs off (only once when transitioning to off)
            if (was_playing) {
                ledsOff();
                was_playing = false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));  // Slow poll when off
            continue;
        }
        
        // Playing - render current frame
        was_playing = true;
        playFrame();
        showBlocking();
        
        vTaskDelay(pdMS_TO_TICKS(update_interval_ms));
    }
    
    // Clean exit
    ESP_LOGI(TAG, "RGB LED task exiting");
    task_running.store(false);
    vTaskDelete(nullptr);
}
