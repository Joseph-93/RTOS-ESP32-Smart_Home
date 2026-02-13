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
 * RGB LED Component (WS2812/WS2812B Controller)
 * 
 * Uses ESP-IDF led_strip component with RMT backend for deterministic timing.
 * This approach is hardware-driven (not bit-banging) and works reliably under
 * FreeRTOS with WiFi/BLE interrupts.
 * 
 * WHY RMT BACKEND:
 * - Hardware peripheral generates precise WS2812 timing signals
 * - No CPU involvement during data transmission
 * - Immune to FreeRTOS task scheduling and interrupt latency
 * 
 * WHY GRB PIXEL FORMAT:
 * - WS2812/WS2812B LEDs expect Green-Red-Blue data order
 * - The led_strip component handles this automatically
 * 
 * WHY MUTEX:
 * - Multiple tasks may call setLedColor/refresh concurrently
 * - Protects led_strip handle and color_buffer from race conditions
 * 
 * Parameters exposed:
 * - led_count: Number of LEDs in the strip (configurable)
 * - brightness: Global brightness multiplier (0-100%)
 * - led_colors: RGB values for each LED (Nx3 matrix: rows=LEDs, cols=R,G,B)
 * - effect: Current effect mode (0=static, 1=rainbow, 2=breathing, 3=chase)
 * - effect_speed: Speed of animated effects (1-100)
 * - power_on: Master on/off switch
 */

// Default configuration
#define RGB_LED_DEFAULT_PIN     GPIO_NUM_13  // Data pin for LED strip
#define RGB_LED_DEFAULT_COUNT   30           // Default number of LEDs
#define RGB_LED_MAX_COUNT       300          // Maximum supported LEDs

// Effect modes
enum class RgbLedEffect : uint8_t {
    STATIC = 0,      // Static colors from led_colors parameter
    RAINBOW = 1,     // Rainbow cycle
    BREATHING = 2,   // Breathing/pulsing effect
    CHASE = 3,       // Chase/running light effect
    SOLID = 4,       // All LEDs same color (uses first LED's color)
    TEST_RGB = 5     // RED-OFF-GREEN-OFF-BLUE-OFF cycle for circuit testing
};

class RgbLedComponent : public Component {
public:
    RgbLedComponent();
    ~RgbLedComponent();
    
    void onInitialize() override;
    
    // Thread-safe LED control API
    // Set a single LED color (0-255 per channel)
    void setLedColor(uint16_t led_index, uint8_t r, uint8_t g, uint8_t b);
    
    // Set all LEDs to the same color
    void setAllLeds(uint8_t r, uint8_t g, uint8_t b);
    
    // Clear all LEDs (turn off)
    void clearAll();
    
    // Force an immediate refresh of the LED strip
    void refresh();
    
    // Low-level API (thread-safe, returns esp_err_t)
    esp_err_t setPixel(uint16_t led_index, uint8_t r, uint8_t g, uint8_t b);
    esp_err_t show();

private:
    // Parameters (exposed to component system)
    IntParameter* ledCount;           // Number of LEDs
    IntParameter* brightness;         // Global brightness (0-100)
    IntParameter* ledColors;          // RGB values matrix (led_count x 3)
    IntParameter* effect;             // Current effect mode
    IntParameter* effectSpeed;        // Effect animation speed
    BoolParameter* powerOn;           // Master power switch
    
    // ESP-IDF led_strip handle (RMT backend)
    led_strip_handle_t led_strip;
    
    // Thread safety: mutex protects led_strip and color_buffer
    SemaphoreHandle_t strip_mutex;
    
    // Internal color buffer (RGB format, 3 bytes per LED)
    std::vector<uint8_t> color_buffer;
    uint16_t current_led_count;
    
    // Task handles and synchronization
    TaskHandle_t led_task_handle;
    std::atomic_bool refresh_pending{false};   // Atomic: written by callbacks, read by task
    std::atomic_bool stop_requested{false};    // Atomic: signals task to exit cleanly
    std::atomic_bool task_running{false};      // Atomic: task sets false before exiting
    
    // Bulk update guard to prevent callback re-entrancy churn
    bool in_bulk_update;
    
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
    
    // Effect functions
    void applyStaticEffect();
    void applyRainbowEffect(uint32_t tick);
    void applyBreathingEffect(uint32_t tick);
    void applyChaseEffect(uint32_t tick);
    void applySolidEffect();
    void applyTestRgbEffect(uint32_t tick);
    
    // HSV to RGB conversion for effects
    void hsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b);
    
    // Task functions
    static void ledTaskWrapper(void* pvParameters);
    void ledTask();
    
    // Resize LED buffer when count changes
    void resizeLedBuffer(uint16_t new_count);
};
