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

// Base64 encode table
static const char base64_encode_table[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode binary data to base64 string
static std::string base64_encode(const uint8_t* input, size_t len) {
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = ((uint32_t)input[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)input[i + 1]) << 8;
        if (i + 2 < len) n |= input[i + 2];
        
        result += base64_encode_table[(n >> 18) & 0x3F];
        result += base64_encode_table[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? base64_encode_table[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? base64_encode_table[n & 0x3F] : '=';
    }
    
    return result;
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
    // Create mutex first — needed before the task starts
    strip_mutex = xSemaphoreCreateMutex();
    if (!strip_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }
    
    // Allocate color buffer (always 30 LEDs)
    color_buffer.resize(RGB_LED_COUNT * 3, 0);
    
    // Allocate transition buffers
    transition_from_buffer.resize(RGB_LED_COUNT * 3, 0);
    transition_to_buffer.resize(RGB_LED_COUNT * 3, 0);
    
    // Create parameters
    brightness = addIntParam("brightness", 1, 1, 0, 100, 100);
    playing = addBoolParam("playing", 1, 1, false);  // false=off, true=play animation
    loop = addIntParam("loop", 1, 1, -1, 1000, -1, true);  // -1=infinite, 0=play once, N=loop N times
    transitionMs = addIntParam("transition_ms", 1, 1, 0, 5000, 200);  // Slick dick transition duration
    transitionEasing = addIntParam("transition_easing", 1, 1, 0, 1, 0);  // 0=crossfade (merge), 1=through black
    
    // Animation status parameters (read-only)
    animFrameCount = addIntParam("anim_frame_count", 1, 1, 0, 65535, 0, true);  // Read-only
    animMemoryUsed = addIntParam("anim_memory_used", 1, 1, 0, INT32_MAX, 0, true);  // Read-only
    animMemoryMax = addIntParam("anim_memory_max", 1, 1, 0, INT32_MAX, RGB_LED_MAX_ANIMATION_MEMORY, true);  // Read-only
    presetCountParam = addIntParam("preset_count", 1, 1, 0, 255, 0, true);  // Read-only
    
    // Preset management parameters
    activePresetParam = addIntParam("active_preset", 1, 1, -1, 255, -1, true);  // Read-only (output of priority system)
    // 3-tier priority system: row 0 = top (immediate), row 1 = mid (manual), row 2 = low (auto)
    presetPriorityParam = addIntParam("preset_priority", 3, 1, -1, 255, -1);
    deletePresetParam = addIntParam("delete_preset", 1, 1, -1, 255, -1);
    
    // Animation upload parameters (writable)
    animTotalFrames = addIntParam("anim_total_frames", 1, 1, 0, 65535, 0);
    animUploadChunkIndex = addIntParam("anim_chunk_index", 1, 1, 0, 65535, 0);
    animUploadData = addStringParam("anim_chunk_data", 1, 1, "");
    animPresetName = addStringParam("anim_preset_name", 1, 1, "");
    animUploadLoop = addIntParam("anim_upload_loop", 1, 1, -1, 1000, -1);  // Loop count for upload
    animCommit = addBoolParam("anim_commit", 1, 1, false);
    
    // Preset query/download parameters
    queryPresetIndex = addIntParam("query_preset_index", 1, 1, -1, 255, -1);
    queryPresetName = addStringParam("query_preset_name", 1, 1, "", true);  // Read-only
    queryPresetFrameCount = addIntParam("query_preset_frame_count", 1, 1, 0, 65535, 0, true);  // Read-only
    queryPresetDataSize = addIntParam("query_preset_data_size", 1, 1, 0, INT32_MAX, 0, true);  // Read-only
    queryPresetLoop = addIntParam("query_preset_loop", 1, 1, -1, 1000, -1, true);  // Read-only
    chunkSizeParam = addIntParam("chunk_size", 1, 1, 0, INT32_MAX, RGB_LED_CHUNK_SIZE, true);  // Read-only
    queryDownloadChunkIndex = addIntParam("query_download_chunk_index", 1, 1, -1, 65535, -1);
    queryDownloadChunkData = addStringParam("query_download_chunk_data", 1, 1, "", true);  // Read-only
    presetIdsParam = addStringParam("preset_ids", 1, 1, "", true);  // Read-only: comma-separated IDs
    
    // Set up onChange callbacks
    if (playing) {
        playing->setOnChange([this](size_t row, size_t col, bool val) {
            if (val) {
                // Reset to frame 0 when starting playback
                animation_current_frame = 0;
                animation_frame_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
                // Reset loops remaining from the preset's loop count
                auto it = presets.find(active_preset_index);
                if (it != presets.end()) {
                    animation_loops_remaining = it->second.loop;
                }
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
    
    if (activePresetParam) {
        activePresetParam->setOnChange([this](size_t row, size_t col, int val) {
            if (val == -1) {
                // Transition to off
                beginTransition(-1);
                updateStatusParams();
            } else if (isPresetPlayable((int16_t)val)) {
                // Use slick dick transition
                beginTransition((int16_t)val);
                // Auto-start playback when selecting a preset
                if (playing) playing->setValue(0, 0, true);
                updateStatusParams();
            } else {
                // Preset doesn't exist or has no frames - refuse to play
                ESP_LOGW(TAG, "Cannot play preset %d: not found or empty", val);
                // Reset active_preset to -1 since we can't play it
                // (use internal var to avoid recursive onChange)
                active_preset_index = -1;
            }
        });
    }
    
    // Priority system: when any tier changes, resolve and update active_preset
    if (presetPriorityParam) {
        presetPriorityParam->setOnChange([this](size_t row, size_t col, int val) {
            // Resolve highest priority preset (only considers playable presets)
            int16_t resolved = resolvePresetPriority();
            
            // If resolved preset is different from current, switch to it
            if (resolved != active_preset_index) {
                // Use slick dick transition for priority changes too
                beginTransition(resolved);
                if (resolved >= 0 && isPresetPlayable(resolved)) {
                    if (activePresetParam) activePresetParam->setValue(0, 0, resolved);
                    if (playing) playing->setValue(0, 0, true);
                } else {
                    if (activePresetParam) activePresetParam->setValue(0, 0, -1);
                }
                updateStatusParams();
            }
        });
    }
    
    if (deletePresetParam) {
        deletePresetParam->setOnChange([this](size_t row, size_t col, int val) {
            if (val >= 0) {
                deletePresetByIndex(val);
                deletePresetParam->setValue(0, 0, -1);  // Reset trigger
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
            }
            
            animUploadData->setValue(0, 0, "");  // Clear to detect next write
        });
    }
    
    // Query preset onChange - populate read-only query results
    if (queryPresetIndex) {
        queryPresetIndex->setOnChange([this](size_t row, size_t col, int val) {
            auto it = presets.find((int16_t)val);
            if (it != presets.end()) {
                const AnimationPreset& p = it->second;
                if (queryPresetName) queryPresetName->setValue(0, 0, p.name);
                if (queryPresetFrameCount) queryPresetFrameCount->setValue(0, 0, p.frame_count);
                if (queryPresetDataSize) queryPresetDataSize->setValue(0, 0, (int)p.data.size());
                if (queryPresetLoop) queryPresetLoop->setValue(0, 0, p.loop);
            } else {
                // Clear query results
                if (queryPresetName) queryPresetName->setValue(0, 0, "");
                if (queryPresetFrameCount) queryPresetFrameCount->setValue(0, 0, 0);
                if (queryPresetDataSize) queryPresetDataSize->setValue(0, 0, 0);
                if (queryPresetLoop) queryPresetLoop->setValue(0, 0, -1);  // Default to infinite
            }
        });
    }
    
    // Download chunk onChange - return base64 chunk of queried preset data
    if (queryDownloadChunkIndex) {
        queryDownloadChunkIndex->setOnChange([this](size_t row, size_t col, int chunk_idx) {
            if (chunk_idx < 0) return;
            
            int preset_idx = queryPresetIndex ? queryPresetIndex->getValue(0, 0) : -1;
            auto it = presets.find((int16_t)preset_idx);
            if (it == presets.end()) {
                ESP_LOGW(TAG, "Download chunk %d: preset %d not found", chunk_idx, preset_idx);
                if (queryDownloadChunkData) queryDownloadChunkData->setValue(0, 0, "");
                return;
            }
            
            const AnimationPreset& p = it->second;
            size_t offset = chunk_idx * RGB_LED_CHUNK_SIZE;
            
            if (offset >= p.data.size()) {
                // Beyond end of data
                if (queryDownloadChunkData) queryDownloadChunkData->setValue(0, 0, "");
                return;
            }
            
            size_t chunk_len = std::min((size_t)RGB_LED_CHUNK_SIZE, p.data.size() - offset);
            
            // Encode to base64
            std::string b64 = base64_encode(p.data.data() + offset, chunk_len);
            if (queryDownloadChunkData) queryDownloadChunkData->setValue(0, 0, b64);
            
            ESP_LOGD(TAG, "Download preset %d chunk %d: %zu bytes -> %zu b64",
                     preset_idx, chunk_idx, chunk_len, b64.size());
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

// Resolve priority system: find highest priority tier with a valid preset
// Returns the preset ID to play, or -1 if all tiers are empty
int16_t RgbLedComponent::resolvePresetPriority() {
    if (!presetPriorityParam) return -1;
    
    // Check tiers in order: 0 (top), 1 (mid), 2 (low)
    for (int tier = 0; tier < 3; tier++) {
        int val = presetPriorityParam->getValue(tier, 0);
        if (val >= 0 && presets.find((int16_t)val) != presets.end()) {
            return (int16_t)val;
        }
    }
    return -1;
}

// Clear the tier that was playing the given preset
void RgbLedComponent::clearFinishedPriorityTier(int16_t finished_preset) {
    if (!presetPriorityParam) return;
    
    // Find which tier had this preset and clear it
    for (int tier = 0; tier < 3; tier++) {
        if (presetPriorityParam->getValue(tier, 0) == finished_preset) {
            presetPriorityParam->setValue(tier, 0, -1);
            break;  // Only clear the first match (highest priority)
        }
    }
}

esp_err_t RgbLedComponent::animationBegin(uint16_t total_frames) {
    size_t frame_size = getFrameSize();
    size_t total_bytes = total_frames * frame_size;
    
    // Check if the new preset fits in the remaining memory pool
    size_t current_usage = calcTotalMemoryUsed();
    size_t available = (current_usage < RGB_LED_MAX_ANIMATION_MEMORY)
                       ? RGB_LED_MAX_ANIMATION_MEMORY - current_usage : 0;
    
    if (total_bytes > available) {
        ESP_LOGE(TAG, "Preset too large: needs %zu bytes, only %zu available "
                 "(used %zu of %d across %d presets)",
                 total_bytes, available, current_usage,
                 RGB_LED_MAX_ANIMATION_MEMORY, (int)presets.size());
        // Update memory params so front-end can see the current state
        updateStatusParams();
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
    
    // Free any previous staging buffer before allocating new one
    { std::vector<uint8_t>().swap(upload_staging_data); }
    
    // Pre-allocate staging buffer
    upload_staging_data.reserve(total_bytes);
    if (upload_staging_data.capacity() < total_bytes) {
        giveMutex();
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for staging buffer", total_bytes);
        return ESP_ERR_NO_MEM;
    }
    upload_staging_data.resize(total_bytes, 0);
    
    upload_frame_count = total_frames;
    upload_led_count = RGB_LED_COUNT;
    upload_offset = 0;
    uploading = true;
    
    giveMutex();
    
    return ESP_OK;
}

esp_err_t RgbLedComponent::animationChunk(uint16_t chunk_index, const uint8_t* data, size_t len) {
    if (!uploading) {
        ESP_LOGE(TAG, "Animation chunk received but no upload in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    size_t expected_offset = chunk_index * RGB_LED_CHUNK_SIZE;
    
    if (expected_offset != upload_offset) {
        ESP_LOGW(TAG, "Chunk out of order: expected offset %zu, got chunk %u (offset %zu)",
                 upload_offset, chunk_index, expected_offset);
        // Allow it anyway - might be a retry
    }
    
    if (!takeMutex()) {
        return ESP_ERR_TIMEOUT;
    }
    
    // Bounds check
    if (upload_offset + len > upload_staging_data.size()) {
        giveMutex();
        ESP_LOGE(TAG, "Chunk would overflow staging buffer: offset %zu + len %zu > size %zu",
                 upload_offset, len, upload_staging_data.size());
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Copy data into staging buffer
    memcpy(upload_staging_data.data() + upload_offset, data, len);
    upload_offset += len;
    
    giveMutex();
    
    ESP_LOGD(TAG, "Upload chunk %u: %zu bytes, total: %zu/%zu",
             chunk_index, len, upload_offset, upload_staging_data.size());
    
    return ESP_OK;
}

esp_err_t RgbLedComponent::animationCommit() {
    if (!uploading) {
        ESP_LOGE(TAG, "Animation commit but no upload in progress");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (upload_offset != upload_staging_data.size()) {
        ESP_LOGW(TAG, "Upload incomplete: received %zu of %zu bytes",
                 upload_offset, upload_staging_data.size());
        // Allow it - partial animations might still work
    }
    
    if (!takeMutex()) {
        return ESP_ERR_TIMEOUT;
    }
    
    // Move staging data into a new preset
    AnimationPreset preset;
    preset.data = std::move(upload_staging_data);
    preset.frame_count = upload_frame_count;
    preset.led_count = upload_led_count;
    
    // Find the lowest unused preset ID (fills gaps from deletions)
    int16_t new_id = 0;
    while (presets.find(new_id) != presets.end()) {
        new_id++;
    }
    
    // Get the name from the param (default to "Preset N" if empty)
    std::string name = animPresetName ? animPresetName->getValue(0, 0) : "";
    if (name.empty()) {
        name = "Preset " + std::to_string(new_id);
    }
    preset.name = name;
    
    // Get loop setting from upload param (-1=infinite, 0=once, N=N times)
    preset.loop = animUploadLoop ? (int16_t)animUploadLoop->getValue(0, 0) : -1;
    
    // Insert into map
    presets[new_id] = std::move(preset);
    
    // Clear staging state
    uploading = false;
    upload_frame_count = 0;
    upload_led_count = 0;
    upload_offset = 0;
    
    // Clear the name param for next upload
    if (animPresetName) animPresetName->setValue(0, 0, "");
    
    // Don't auto-activate - let the priority system handle activation
    // (User sets preset_priority[tier] = ID to schedule playback)
    
    // If already playing, kick the task to restart from frame 0 with the new data.
    bool was_playing = playing && playing->getValue(0, 0);

    giveMutex();

    // Reset the total_frames trigger back to 0 so the next upload always fires
    // the onChange callback even when uploading the same frame count as before.
    if (animTotalFrames) animTotalFrames->setValue(0, 0, 0);

    // Update all read-only status parameters
    updateStatusParams();
    if (activePresetParam) activePresetParam->setValue(0, 0, active_preset_index);

    presetsModified = true;
    
    ESP_LOGI(TAG, "NVS: Preset #%d committed", new_id);

    if (was_playing && led_task_handle) {
        xTaskNotifyGive(led_task_handle);
    }

    return ESP_OK;
}

void RgbLedComponent::animationClear() {
    if (!takeMutex()) {
        return;
    }
    
    // Clear staging
    uploading = false;
    upload_frame_count = 0;
    upload_led_count = 0;
    upload_offset = 0;
    { std::vector<uint8_t>().swap(upload_staging_data); }
    
    // Clear all presets
    presets.clear();
    active_preset_index = -1;
    animation_current_frame = 0;
    
    giveMutex();
    
    // Update parameters
    updateStatusParams();
    if (activePresetParam) activePresetParam->setValue(0, 0, -1);
    
    // Stop playback
    if (playing) playing->setValue(0, 0, false);
}

void RgbLedComponent::playFrame() {
    auto it = presets.find(active_preset_index);
    if (it == presets.end()) {
        return;  // No active preset
    }
    
    const AnimationPreset& preset = it->second;
    if (preset.frame_count == 0 || preset.data.empty()) {
        return;  // Empty preset
    }
    
    size_t frame_size = preset.getFrameSize();
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    
    // Get current frame data pointer
    size_t frame_offset = animation_current_frame * frame_size;
    if (frame_offset + frame_size > preset.data.size()) {
        ESP_LOGE(TAG, "Frame %u out of bounds in preset %d", 
                 animation_current_frame, active_preset_index);
        return;
    }
    
    const uint8_t* frame_data = preset.data.data() + frame_offset;
    
    // Duration is last 2 bytes of frame (little-endian)
    uint16_t duration_ms = frame_data[frame_size - 2] | (frame_data[frame_size - 1] << 8);
    
    // Check if frame expired
    uint32_t elapsed = now_ms - animation_frame_start_ms;
    if (elapsed >= duration_ms) {
        // Advance to next frame
        animation_current_frame++;
        
        if (animation_current_frame >= preset.frame_count) {
            // Loop logic: -1=infinite, N=play N times total (not N additional loops)
            if (animation_loops_remaining == -1) {
                // Infinite loop
                animation_current_frame = 0;
            } else if (animation_loops_remaining > 1) {
                // More plays remaining (decrement first, then check)
                animation_loops_remaining--;
                animation_current_frame = 0;
            } else {
                // animation_loops_remaining <= 1: this was the last play
                // Use priority system: clear the tier that finished, then resolve next
                int16_t finished_preset = active_preset_index;
                clearFinishedPriorityTier(finished_preset);
                
                // Resolve next preset from priority system
                int16_t next_preset = resolvePresetPriority();
                
                // Use slick dick transition to next preset (or to off)
                beginTransition(next_preset);
                if (next_preset >= 0) {
                    if (activePresetParam) activePresetParam->setValue(0, 0, next_preset);
                } else {
                    if (activePresetParam) activePresetParam->setValue(0, 0, -1);
                }
                return;
            }
        }
        
        animation_frame_start_ms = now_ms;
        
        // Recalculate frame pointer
        frame_offset = animation_current_frame * frame_size;
        frame_data = preset.data.data() + frame_offset;
    }
    
    // Copy frame colors to buffer (excluding duration bytes)
    if (!takeMutex()) return;
    
    // Use the LED count the preset was built for, not the live count.
    // If the preset has fewer LEDs than the strip, zero the remainder
    // so stale data from a previous preset doesn't leak through.
    size_t anim_color_bytes = preset.led_count * 3;
    size_t buf_color_bytes  = RGB_LED_COUNT * 3;
    
    if (anim_color_bytes <= buf_color_bytes) {
        memcpy(color_buffer.data(), frame_data, anim_color_bytes);
        // Zero LEDs beyond the preset's range
        if (anim_color_bytes < buf_color_bytes) {
            memset(color_buffer.data() + anim_color_bytes, 0, buf_color_bytes - anim_color_bytes);
        }
    } else {
        // Preset has more LEDs than the strip — truncate
        memcpy(color_buffer.data(), frame_data, buf_color_bytes);
    }
    
    giveMutex();
}

// ============================================================================
// Preset Management Helpers
// ============================================================================

size_t RgbLedComponent::calcTotalMemoryUsed() const {
    size_t total = 0;
    for (const auto& kv : presets) {
        total += kv.second.data.size();
    }
    return total;
}

void RgbLedComponent::deletePresetByIndex(int index) {
    auto it = presets.find((int16_t)index);
    if (it == presets.end()) {
        ESP_LOGW(TAG, "Cannot delete preset %d: not found", index);
        return;
    }
    
    if (!takeMutex()) {
        return;
    }
    
    // Free the preset's memory explicitly before erasing
    { std::vector<uint8_t>().swap(it->second.data); }
    presets.erase(it);
    
    // If we deleted the active preset, stop playback
    if (index == active_preset_index) {
        active_preset_index = -1;
        if (playing) playing->setValue(0, 0, false);
    }
    // NOTE: No index shifting needed - IDs are stable!
    
    // Clear priority tiers that pointed to the deleted preset
    // (no shifting needed since IDs are stable)
    if (presetPriorityParam) {
        for (size_t tier = 0; tier < 3; tier++) {
            int16_t tierVal = (int16_t)presetPriorityParam->getValue(tier, 0);
            if (tierVal == index) {
                // This tier pointed to the deleted preset - clear it
                presetPriorityParam->setValue(tier, 0, -1);
            }
            // No shifting needed - other tiers keep their stable IDs!
        }
    }
    
    presetsModified = true;
    
    giveMutex();
    
    ESP_LOGI(TAG, "NVS: Deleted preset %d", index);
    
    updateStatusParams();
    if (activePresetParam) activePresetParam->setValue(0, 0, active_preset_index);
}

bool RgbLedComponent::isPresetPlayable(int16_t preset_id) const {
    if (preset_id < 0) return false;
    
    auto it = presets.find(preset_id);
    if (it == presets.end()) return false;
    
    const AnimationPreset& preset = it->second;
    // Must have at least 1 frame and actual data
    return preset.frame_count > 0 && !preset.data.empty();
}

void RgbLedComponent::reconcilePlaybackAfterLoad() {
    // Called after NVS load to ensure playback state matches priority queue
    // This handles the case where params were loaded but playback didn't start
    
    int16_t resolved = resolvePresetPriority();
    
    if (resolved >= 0 && isPresetPlayable(resolved)) {
        // There's a valid preset in the priority queue - start it
        if (active_preset_index != resolved) {
            active_preset_index = resolved;
            animation_current_frame = 0;
            animation_frame_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            auto it = presets.find(resolved);
            if (it != presets.end()) {
                animation_loops_remaining = it->second.loop;
            }
            if (playing) playing->setValue(0, 0, true);
            ESP_LOGI(TAG, "Reconcile: started playback of preset %d", resolved);
        }
        if (activePresetParam) activePresetParam->setValue(0, 0, resolved);
    } else {
        // Nothing playable in priority queue
        active_preset_index = -1;
        if (activePresetParam) activePresetParam->setValue(0, 0, -1);
    }
    
    updateStatusParams();
}

void RgbLedComponent::updateStatusParams() {
    size_t total_mem = calcTotalMemoryUsed();
    if (animMemoryUsed) animMemoryUsed->setValue(0, 0, (int)total_mem);
    if (presetCountParam) presetCountParam->setValue(0, 0, (int)presets.size());
    
    // Show frame count of the active preset
    if (animFrameCount) {
        auto it = presets.find(active_preset_index);
        if (it != presets.end()) {
            animFrameCount->setValue(0, 0, it->second.frame_count);
        } else {
            animFrameCount->setValue(0, 0, 0);
        }
    }
    
    // Update preset_ids: comma-separated list of all valid preset IDs
    if (presetIdsParam) {
        std::string ids;
        for (const auto& kv : presets) {
            if (!ids.empty()) ids += ",";
            ids += std::to_string(kv.first);
        }
        presetIdsParam->setValue(0, 0, ids);
    }
}

void RgbLedComponent::beginTransition(int16_t new_preset_id) {
    // Slick dick mode: capture current state and set up smooth transition
    
    int trans_ms = transitionMs ? transitionMs->getValue(0, 0) : 0;
    
    // If transition disabled, just switch immediately
    if (trans_ms <= 0) {
        if (new_preset_id >= 0) {
            auto it = presets.find(new_preset_id);
            if (it != presets.end()) {
                active_preset_index = new_preset_id;
                animation_current_frame = 0;
                animation_frame_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
                animation_loops_remaining = it->second.loop;
            }
        } else {
            active_preset_index = -1;
            if (playing) playing->setValue(0, 0, false);
        }
        return;
    }
    
    // Capture current LED state as "from" buffer
    if (takeMutex()) {
        memcpy(transition_from_buffer.data(), color_buffer.data(), RGB_LED_COUNT * 3);
        giveMutex();
    }
    
    // Build "to" buffer: first frame of new preset, or black for off
    memset(transition_to_buffer.data(), 0, RGB_LED_COUNT * 3);
    
    if (new_preset_id >= 0) {
        auto it = presets.find(new_preset_id);
        if (it != presets.end()) {
            const AnimationPreset& preset = it->second;
            if (preset.frame_count > 0 && !preset.data.empty()) {
                // Copy first frame's color data (not duration bytes)
                size_t color_bytes = std::min((size_t)(preset.led_count * 3), (size_t)(RGB_LED_COUNT * 3));
                memcpy(transition_to_buffer.data(), preset.data.data(), color_bytes);
            }
            // Set up the new preset (will start after transition)
            active_preset_index = new_preset_id;
            animation_current_frame = 0;
            animation_frame_start_ms = (uint32_t)(esp_timer_get_time() / 1000) + trans_ms;
            animation_loops_remaining = preset.loop;
        }
    } else {
        // Transitioning to off - target is already black (zeroed above)
        active_preset_index = -1;
    }
    
    // Start transition
    transitioning = true;
    transition_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

bool RgbLedComponent::processTransition() {
    if (!transitioning) return false;
    
    int trans_ms = transitionMs ? transitionMs->getValue(0, 0) : 0;
    if (trans_ms <= 0) {
        transitioning = false;
        return false;
    }
    
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t elapsed = now_ms - transition_start_ms;
    
    if (elapsed >= (uint32_t)trans_ms) {
        // Transition complete
        transitioning = false;
        
        // If we transitioned to off, stop playback
        if (active_preset_index < 0) {
            if (playing) playing->setValue(0, 0, false);
        }
        
        return false;
    }
    
    // Calculate progress (0.0 to 1.0)
    float t = (float)elapsed / (float)trans_ms;
    
    // Transition mode: 0 = crossfade (merge colors), 1 = through black (dip to dark)
    int mode = transitionEasing ? transitionEasing->getValue(0, 0) : 0;
    
    if (!takeMutex()) return true;
    
    if (mode == 1) {
        // Through black: fade out first half, fade in second half
        // t=0.0: 100% from, 0% to
        // t=0.5: 0% from, 0% to (black)
        // t=1.0: 0% from, 100% to
        float from_factor, to_factor;
        if (t < 0.5f) {
            // First half: fade out from, to stays at 0
            from_factor = 1.0f - (t * 2.0f);  // 1.0 → 0.0
            to_factor = 0.0f;
        } else {
            // Second half: from at 0, fade in to
            from_factor = 0.0f;
            to_factor = (t - 0.5f) * 2.0f;  // 0.0 → 1.0
        }
        
        for (size_t i = 0; i < RGB_LED_COUNT * 3; i++) {
            uint8_t from_val = transition_from_buffer[i];
            uint8_t to_val = transition_to_buffer[i];
            color_buffer[i] = (uint8_t)(from_val * from_factor + to_val * to_factor);
        }
    } else {
        // Crossfade: direct blend between from and to (colors merge)
        for (size_t i = 0; i < RGB_LED_COUNT * 3; i++) {
            uint8_t from_val = transition_from_buffer[i];
            uint8_t to_val = transition_to_buffer[i];
            color_buffer[i] = (uint8_t)(from_val + t * (to_val - from_val));
        }
    }
    
    giveMutex();
    
    return true;  // Still transitioning
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
        
        // Handle slick dick transitions first
        if (transitioning) {
            bool still_transitioning = processTransition();
            if (still_transitioning) {
                showBlocking();
                vTaskDelay(pdMS_TO_TICKS(update_interval_ms));
                continue;
            }
            // Transition complete - fall through to normal playback or off
        }
        
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
    task_running.store(false);
    vTaskDelete(nullptr);
}

// ============================================================================
// NVS Persistence for Animation Presets
// ============================================================================

void RgbLedComponent::saveCustomData(nvs_handle_t handle) {
    if (!presetsModified) {
        return;
    }
    
    // Save preset count
    uint16_t count = static_cast<uint16_t>(presets.size());
    nvs_set_u16(handle, "preset_cnt", count);
    
    // Save each preset with its ID as part of the key
    int idx = 0;
    for (const auto& [id, preset] : presets) {
        char key[16];
        
        // Save preset ID
        snprintf(key, sizeof(key), "pid%d", idx);
        nvs_set_i16(handle, key, id);
        
        // Save metadata: frame_count, led_count, loop
        snprintf(key, sizeof(key), "pm%d", idx);
        uint32_t meta[3] = {
            preset.frame_count,
            preset.led_count,
            static_cast<uint32_t>(static_cast<int32_t>(preset.loop))
        };
        nvs_set_blob(handle, key, meta, sizeof(meta));
        
        // Save name (truncated to 32 chars max)
        snprintf(key, sizeof(key), "pn%d", idx);
        std::string name = preset.name.substr(0, 32);
        nvs_set_str(handle, key, name.c_str());
        
        // Save animation data - may need to split if > 4000 bytes
        // NVS blob limit is ~4000 bytes, so we chunk large animations
        size_t data_size = preset.data.size();
        snprintf(key, sizeof(key), "pds%d", idx);
        nvs_set_u32(handle, key, static_cast<uint32_t>(data_size));
        
        const size_t CHUNK_SIZE = 3900;  // Leave room for NVS overhead
        size_t chunks = (data_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
        
        for (size_t chunk = 0; chunk < chunks; chunk++) {
            snprintf(key, sizeof(key), "d%02x%02x", (uint8_t)idx, (uint8_t)chunk);
            size_t offset = chunk * CHUNK_SIZE;
            size_t len = std::min(CHUNK_SIZE, data_size - offset);
            nvs_set_blob(handle, key, preset.data.data() + offset, len);
        }
        
        idx++;
    }
    
    presetsModified = false;
    ESP_LOGI(TAG, "Saved %d presets to NVS", count);
}

void RgbLedComponent::loadCustomData(nvs_handle_t handle) {
    // Load preset count
    uint16_t count = 0;
    if (nvs_get_u16(handle, "preset_cnt", &count) != ESP_OK || count == 0) {
        ESP_LOGI(TAG, "No presets in NVS");
        return;
    }
    
    if (!takeMutex()) {
        return;
    }
    
    presets.clear();
    
    for (int idx = 0; idx < count; idx++) {
        char key[16];
        AnimationPreset preset;
        
        // Load preset ID
        snprintf(key, sizeof(key), "pid%d", idx);
        int16_t id = 0;
        if (nvs_get_i16(handle, key, &id) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load preset ID at index %d", idx);
            continue;
        }
        
        // Load metadata
        snprintf(key, sizeof(key), "pm%d", idx);
        uint32_t meta[3];
        size_t meta_size = sizeof(meta);
        if (nvs_get_blob(handle, key, meta, &meta_size) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load preset metadata at index %d", idx);
            continue;
        }
        preset.frame_count = meta[0];
        preset.led_count = meta[1];
        preset.loop = static_cast<int16_t>(static_cast<int32_t>(meta[2]));
        
        // Load name
        snprintf(key, sizeof(key), "pn%d", idx);
        size_t name_len = 0;
        if (nvs_get_str(handle, key, nullptr, &name_len) == ESP_OK && name_len > 0) {
            std::vector<char> name_buf(name_len);
            nvs_get_str(handle, key, name_buf.data(), &name_len);
            preset.name = std::string(name_buf.data());
        }
        
        // Load data size
        snprintf(key, sizeof(key), "pds%d", idx);
        uint32_t data_size = 0;
        if (nvs_get_u32(handle, key, &data_size) != ESP_OK || data_size == 0) {
            ESP_LOGW(TAG, "Failed to load preset data size at index %d", idx);
            continue;
        }
        
        // Load animation data chunks
        preset.data.resize(data_size);
        const size_t CHUNK_SIZE = 3900;
        size_t chunks = (data_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
        bool load_ok = true;
        
        for (size_t chunk = 0; chunk < chunks && load_ok; chunk++) {
            snprintf(key, sizeof(key), "d%02x%02x", (uint8_t)idx, (uint8_t)chunk);
            size_t offset = chunk * CHUNK_SIZE;
            size_t len = std::min(CHUNK_SIZE, static_cast<size_t>(data_size) - offset);
            size_t actual_len = len;
            if (nvs_get_blob(handle, key, preset.data.data() + offset, &actual_len) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to load preset data chunk %d at index %d", (int)chunk, idx);
                load_ok = false;
            }
        }
        
        if (load_ok) {
            presets[id] = std::move(preset);
        }
    }
    
    giveMutex();
    
    updateStatusParams();
    ESP_LOGI(TAG, "Loaded %zu presets from NVS", presets.size());
}

void RgbLedComponent::onPostLoadReconcile() {
    reconcilePlaybackAfterLoad();
}
