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
// RgbLedComponent Implementation
// ============================================================================

RgbLedComponent::RgbLedComponent() 
    : Component("RgbLed")
    , led_strip(nullptr)
    , strip_mutex(nullptr)
    , current_led_count(0)
    , led_task_handle(nullptr)
    , in_bulk_update(false)
    , ledCount(nullptr)
    , brightness(nullptr)
    , ledColors(nullptr)
    , effect(nullptr)
    , effectSpeed(nullptr)
    , powerOn(nullptr)
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
    ESP_LOGI(TAG, "Initializing LED strip: GPIO %d, %d LEDs", gpio, led_count);
    
    // Validate parameters
    if (led_count == 0 || led_count > RGB_LED_MAX_COUNT) {
        ESP_LOGE(TAG, "Invalid LED count: %d (max: %d)", led_count, RGB_LED_MAX_COUNT);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Create mutex for thread safety (protects led_strip and color_buffer)
    if (!strip_mutex) {
        strip_mutex = xSemaphoreCreateMutex();
        if (!strip_mutex) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Configure LED strip with RMT backend
    // IMPORTANT: Always allocate for max LEDs to avoid recreating driver on resize
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = RGB_LED_MAX_COUNT;  // Always max - current_led_count tracks active
    strip_config.led_model = RGB_LED_MODEL;
    strip_config.led_pixel_format = RGB_LED_PIXEL_FORMAT;
    strip_config.flags.invert_out = false;
    
    // Configure RMT backend with 10 MHz resolution
    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = RGB_LED_RMT_RESOLUTION_HZ;
    rmt_config.mem_block_symbols = 64;  // Memory block size for RMT
    rmt_config.flags.with_dma = false;  // DMA not needed for small strips
    
    // Create LED strip with RMT backend
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return ret;
    }
    
    current_led_count = led_count;
    
    // Allocate color buffer (RGB format for internal storage)
    color_buffer.resize(led_count * 3, 0);
    
    // Clear the strip initially
    ret = led_strip_clear(led_strip);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Initial clear failed: %s", esp_err_to_name(ret));
    }
    
    ESP_LOGI(TAG, "LED strip initialized successfully (RMT @ %d Hz)", RGB_LED_RMT_RESOLUTION_HZ);
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

void RgbLedComponent::resizeLedBuffer(uint16_t new_count) {
    if (new_count > RGB_LED_MAX_COUNT) {
        new_count = RGB_LED_MAX_COUNT;
    }
    
    if (!takeMutex()) {
        ESP_LOGW(TAG, "Failed to acquire mutex for resize");
        return;
    }
    
    // Resize color buffer (3 bytes per LED: R, G, B)
    color_buffer.resize(new_count * 3, 0);
    current_led_count = new_count;
    
    ESP_LOGI(TAG, "LED buffer resized to %u LEDs (%zu bytes)", new_count, color_buffer.size());
    
    giveMutex();
}

void RgbLedComponent::onInitialize() {
    ESP_LOGI(TAG, "Initializing RgbLedComponent...");
    
    // Initialize LED strip with RMT backend
    esp_err_t ret = initLedStrip(RGB_LED_DEFAULT_PIN, RGB_LED_DEFAULT_COUNT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LED strip: %s", esp_err_to_name(ret));
        return;
    }
    
    // Create parameters
    ledCount = addIntParam("led_count", 1, 1, 1, RGB_LED_MAX_COUNT, RGB_LED_DEFAULT_COUNT);
    brightness = addIntParam("brightness", 1, 1, 0, 100, 100);
    effect = addIntParam("effect", 1, 1, 0, 5, 5);  // Default to TEST_RGB (5) for circuit debugging
    effectSpeed = addIntParam("effect_speed", 1, 1, 1, 100, 50);
    powerOn = addBoolParam("power_on", 1, 1, true);
    
    // LED colors matrix: led_count rows x 3 columns (R, G, B)
    // Note: We'll resize this when led_count changes
    ledColors = addIntParam("led_colors", RGB_LED_DEFAULT_COUNT, 3, 0, 255, 0);
    
    // Set up onChange callbacks
    if (ledCount) {
        ledCount->setOnChange([this](size_t row, size_t col, int val) {
            ESP_LOGI(TAG, "LED count changed to %d", val);
            resizeLedBuffer((uint16_t)val);
            refresh_pending.store(true);
        });
    }
    
    if (brightness) {
        brightness->setOnChange([this](size_t row, size_t col, int val) {
            ESP_LOGD(TAG, "Brightness changed to %d%%", val);
            refresh_pending.store(true);
        });
    }
    
    if (powerOn) {
        powerOn->setOnChange([this](size_t row, size_t col, bool val) {
            ESP_LOGI(TAG, "Power %s", val ? "ON" : "OFF");
            refresh_pending.store(true);
        });
    }
    
    if (ledColors) {
        ledColors->setOnChange([this](size_t row, size_t col, int val) {
            // Skip if we're in a bulk update (setAllLeds, clearAll)
            if (in_bulk_update) return;
            
            // Only trigger refresh on color changes when in static mode
            if (effect && effect->getValue(0, 0) == (int)RgbLedEffect::STATIC) {
                refresh_pending.store(true);
            }
        });
    }
    
    // Create LED task
    BaseType_t result = xTaskCreate(
        ledTaskWrapper,
        "rgb_led_task",
        4096,
        this,
        tskIDLE_PRIORITY + 1,
        &led_task_handle
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RGB LED task");
    } else {
        ESP_LOGI(TAG, "RGB LED task created, %d LEDs on GPIO %d", 
                 RGB_LED_DEFAULT_COUNT, RGB_LED_DEFAULT_PIN);
    }
    
    // Initial clear
    clearAll();
    refresh();
}

esp_err_t RgbLedComponent::setPixel(uint16_t led_index, uint8_t r, uint8_t g, uint8_t b) {
    if (led_index >= current_led_count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!takeMutex()) {
        return ESP_ERR_TIMEOUT;
    }
    
    // Store in internal buffer (RGB order)
    size_t offset = led_index * 3;
    color_buffer[offset + 0] = r;
    color_buffer[offset + 1] = g;
    color_buffer[offset + 2] = b;
    
    giveMutex();
    return ESP_OK;
}

void RgbLedComponent::setLedColor(uint16_t led_index, uint8_t r, uint8_t g, uint8_t b) {
    if (led_index >= current_led_count) {
        return;
    }
    
    // Update parameter if available
    if (ledColors) {
        ledColors->setValue(led_index, 0, r);
        ledColors->setValue(led_index, 1, g);
        ledColors->setValue(led_index, 2, b);
    }
    
    // Update internal buffer (thread-safe)
    setPixel(led_index, r, g, b);
}

void RgbLedComponent::setAllLeds(uint8_t r, uint8_t g, uint8_t b) {
    if (!takeMutex()) {
        return;
    }
    
    for (uint16_t i = 0; i < current_led_count; i++) {
        size_t offset = i * 3;
        color_buffer[offset + 0] = r;
        color_buffer[offset + 1] = g;
        color_buffer[offset + 2] = b;
    }
    
    giveMutex();
    
    // Update parameters with bulk update guard to prevent callback churn
    if (ledColors) {
        in_bulk_update = true;
        for (uint16_t i = 0; i < current_led_count; i++) {
            ledColors->setValue(i, 0, r);
            ledColors->setValue(i, 1, g);
            ledColors->setValue(i, 2, b);
        }
        in_bulk_update = false;
    }
}

void RgbLedComponent::clearAll() {
    if (!takeMutex()) {
        return;
    }
    
    std::fill(color_buffer.begin(), color_buffer.end(), 0);
    
    // Use led_strip_clear() for efficient clearing
    if (led_strip) {
        led_strip_clear(led_strip);
    }
    
    giveMutex();
    
    // Update parameters with bulk update guard
    if (ledColors) {
        in_bulk_update = true;
        for (uint16_t i = 0; i < current_led_count; i++) {
            ledColors->setValue(i, 0, 0);
            ledColors->setValue(i, 1, 0);
            ledColors->setValue(i, 2, 0);
        }
        in_bulk_update = false;
    }
}

uint8_t RgbLedComponent::applyBrightness(uint8_t color) {
    if (!brightness) return color;
    int bright = brightness->getValue(0, 0);
    return (uint8_t)((color * bright) / 100);
}

esp_err_t RgbLedComponent::show() {
    if (!led_strip) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!takeMutex()) {
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t ret = transmitBuffer();
    
    giveMutex();
    return ret;
}

esp_err_t RgbLedComponent::showBlocking() {
    // For use by the LED task - waits indefinitely for mutex
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
    // Transfer color buffer to LED strip with brightness applied
    // led_strip_set_pixel() accepts RGB order; the driver handles GRB conversion
    // based on LED_PIXEL_FORMAT_GRB configuration
    for (uint16_t i = 0; i < current_led_count; i++) {
        size_t offset = i * 3;
        uint8_t r = applyBrightness(color_buffer[offset + 0]);
        uint8_t g = applyBrightness(color_buffer[offset + 1]);
        uint8_t b = applyBrightness(color_buffer[offset + 2]);
        
        esp_err_t ret = led_strip_set_pixel(led_strip, i, r, g, b);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    
    // led_strip_refresh() transmits data via RMT and handles latch timing
    return led_strip_refresh(led_strip);
}

void RgbLedComponent::refresh() {
    esp_err_t ret = show();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Refresh failed: %s", esp_err_to_name(ret));
    }
}

void RgbLedComponent::hsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
    // h: 0-359, s: 0-255, v: 0-255
    if (s == 0) {
        *r = *g = *b = v;
        return;
    }
    
    uint8_t region = h / 60;
    uint8_t remainder = (h - (region * 60)) * 255 / 60;
    
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    
    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

void RgbLedComponent::applyStaticEffect() {
    // Copy colors from parameter to buffer
    if (!ledColors) return;
    
    if (!takeMutex()) return;
    
    for (uint16_t i = 0; i < current_led_count; i++) {
        size_t offset = i * 3;
        color_buffer[offset + 0] = (uint8_t)ledColors->getValue(i, 0);
        color_buffer[offset + 1] = (uint8_t)ledColors->getValue(i, 1);
        color_buffer[offset + 2] = (uint8_t)ledColors->getValue(i, 2);
    }
    
    giveMutex();
}

void RgbLedComponent::applyRainbowEffect(uint32_t tick) {
    int speed = effectSpeed ? effectSpeed->getValue(0, 0) : 50;
    uint16_t hue_offset = (tick * speed / 10) % 360;
    
    if (!takeMutex()) return;
    
    for (uint16_t i = 0; i < current_led_count; i++) {
        uint16_t hue = (hue_offset + (i * 360 / current_led_count)) % 360;
        uint8_t r, g, b;
        hsvToRgb(hue, 255, 255, &r, &g, &b);
        
        size_t offset = i * 3;
        color_buffer[offset + 0] = r;
        color_buffer[offset + 1] = g;
        color_buffer[offset + 2] = b;
    }
    
    giveMutex();
}

void RgbLedComponent::applyBreathingEffect(uint32_t tick) {
    int speed = effectSpeed ? effectSpeed->getValue(0, 0) : 50;
    
    // Sine wave for smooth breathing (0-255)
    float phase = (tick * speed / 1000.0f);
    uint8_t breath_brightness = (uint8_t)((sinf(phase) + 1.0f) * 127.5f);
    
    // Get base color from first LED
    uint8_t base_r = ledColors ? (uint8_t)ledColors->getValue(0, 0) : 255;
    uint8_t base_g = ledColors ? (uint8_t)ledColors->getValue(0, 1) : 255;
    uint8_t base_b = ledColors ? (uint8_t)ledColors->getValue(0, 2) : 255;
    
    // If all zeros, use white
    if (base_r == 0 && base_g == 0 && base_b == 0) {
        base_r = base_g = base_b = 255;
    }
    
    if (!takeMutex()) return;
    
    for (uint16_t i = 0; i < current_led_count; i++) {
        size_t offset = i * 3;
        color_buffer[offset + 0] = (base_r * breath_brightness) / 255;
        color_buffer[offset + 1] = (base_g * breath_brightness) / 255;
        color_buffer[offset + 2] = (base_b * breath_brightness) / 255;
    }
    
    giveMutex();
}

void RgbLedComponent::applyChaseEffect(uint32_t tick) {
    int speed = effectSpeed ? effectSpeed->getValue(0, 0) : 50;
    uint16_t chase_pos = (tick * speed / 50) % current_led_count;
    
    // Get chase color from first LED
    uint8_t chase_r = ledColors ? (uint8_t)ledColors->getValue(0, 0) : 255;
    uint8_t chase_g = ledColors ? (uint8_t)ledColors->getValue(0, 1) : 255;
    uint8_t chase_b = ledColors ? (uint8_t)ledColors->getValue(0, 2) : 255;
    
    // If all zeros, use white
    if (chase_r == 0 && chase_g == 0 && chase_b == 0) {
        chase_r = chase_g = chase_b = 255;
    }
    
    if (!takeMutex()) return;
    
    // Clear buffer first
    std::fill(color_buffer.begin(), color_buffer.end(), 0);
    
    // Light up chase position with tail
    for (int tail = 0; tail < 5 && tail < current_led_count; tail++) {
        int pos = (chase_pos - tail + current_led_count) % current_led_count;
        uint8_t fade = 255 - (tail * 50);  // Fade tail
        
        size_t offset = pos * 3;
        color_buffer[offset + 0] = (chase_r * fade) / 255;
        color_buffer[offset + 1] = (chase_g * fade) / 255;
        color_buffer[offset + 2] = (chase_b * fade) / 255;
    }
    
    giveMutex();
}

void RgbLedComponent::applySolidEffect() {
    // All LEDs same color as first LED
    uint8_t r = ledColors ? (uint8_t)ledColors->getValue(0, 0) : 0;
    uint8_t g = ledColors ? (uint8_t)ledColors->getValue(0, 1) : 0;
    uint8_t b = ledColors ? (uint8_t)ledColors->getValue(0, 2) : 0;
    
    if (!takeMutex()) return;
    
    for (uint16_t i = 0; i < current_led_count; i++) {
        size_t offset = i * 3;
        color_buffer[offset + 0] = r;
        color_buffer[offset + 1] = g;
        color_buffer[offset + 2] = b;
    }
    
    giveMutex();
}

void RgbLedComponent::applyTestRgbEffect(uint32_t tick) {
    // Simple RED-OFF-GREEN-OFF-BLUE-OFF cycle for circuit testing
    // Each state lasts ~1.5 seconds (75 ticks at 20ms per tick)
    // Full cycle: 6 states * 1.5s = 9 seconds
    const uint32_t ticks_per_state = 75;  // 1.5 seconds per state
    uint32_t state = (tick / ticks_per_state) % 6;
    
    uint8_t r = 0, g = 0, b = 0;
    const char* state_name = "OFF";
    
    switch (state) {
        case 0: r = 255; state_name = "RED";   break;
        case 1: /* all off */                  break;
        case 2: g = 255; state_name = "GREEN"; break;
        case 3: /* all off */                  break;
        case 4: b = 255; state_name = "BLUE";  break;
        case 5: /* all off */                  break;
    }
    
    // Log state changes
    static uint32_t last_state = 999;
    if (state != last_state) {
        ESP_LOGI(TAG, "TEST: %s (R=%d G=%d B=%d)", state_name, r, g, b);
        last_state = state;
    }
    
    if (!takeMutex()) return;
    
    // Set all LEDs to the test color
    for (uint16_t i = 0; i < current_led_count; i++) {
        size_t offset = i * 3;
        color_buffer[offset + 0] = r;
        color_buffer[offset + 1] = g;
        color_buffer[offset + 2] = b;
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
    ESP_LOGI(TAG, "RGB LED task started");
    
    task_running.store(true);
    uint32_t tick = 0;
    const uint32_t update_interval_ms = 20;  // 50 FPS for smooth animations
    
    while (!stop_requested.load()) {
        // Check power state
        bool power = powerOn ? powerOn->getValue(0, 0) : true;
        
        if (!power) {
            // Power off - clear all LEDs (use blocking mutex since we own the task)
            if (takeMutexBlocking()) {
                std::fill(color_buffer.begin(), color_buffer.end(), 0);
                if (led_strip) {
                    led_strip_clear(led_strip);
                    led_strip_refresh(led_strip);
                }
                giveMutex();
            }
            vTaskDelay(pdMS_TO_TICKS(100));  // Slow poll when off
            continue;
        }
        
        // Get current effect
        RgbLedEffect current_effect = effect ? (RgbLedEffect)effect->getValue(0, 0) : RgbLedEffect::STATIC;
        
        // Track whether effect was applied successfully
        bool effect_applied = true;
        
        // Apply effect (each function handles its own mutex locking)
        switch (current_effect) {
            case RgbLedEffect::STATIC:
                if (refresh_pending.load()) {
                    applyStaticEffect();
                    refresh_pending.store(false);
                }
                break;
            case RgbLedEffect::RAINBOW:
                applyRainbowEffect(tick);
                break;
            case RgbLedEffect::BREATHING:
                applyBreathingEffect(tick);
                break;
            case RgbLedEffect::CHASE:
                applyChaseEffect(tick);
                break;
            case RgbLedEffect::SOLID:
                if (refresh_pending.load()) {
                    applySolidEffect();
                    refresh_pending.store(false);
                }
                break;
            case RgbLedEffect::TEST_RGB:
                applyTestRgbEffect(tick);
                break;
        }
        
        // Transmit to LEDs using led_strip_refresh() (only if effect applied)
        if (effect_applied) {
            showBlocking();  // Use blocking variant in task context
        }
        
        tick++;
        vTaskDelay(pdMS_TO_TICKS(update_interval_ms));
    }
    
    // Clean exit
    ESP_LOGI(TAG, "RGB LED task exiting");
    task_running.store(false);
    vTaskDelete(nullptr);  // Delete self
}
