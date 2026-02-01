#include "gui.h"
#include "component_graph.h"
#include "lcd/lcd.h"
#include "touch/touch.h"
#include "wifi_init.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include <math.h>
#include <algorithm>

// XPT2046 touch interrupt configuration
#define TOUCH_IRQ_GPIO  GPIO_NUM_22
static volatile bool touch_irq_triggered = false;

// Display dimensions
#define LCD_H_RES    320
#define LCD_V_RES    240

// LVGL driver structures
static lv_disp_drv_t lvgl_disp_drv;
static lv_disp_draw_buf_t lvgl_disp_buf;
static lv_indev_drv_t lvgl_indev_drv;

static esp_lcd_panel_handle_t panel_handle_ref = NULL;
static esp_lcd_touch_handle_t touch_handle_ref = NULL;

// GUI component instance for LVGL task
static GUIComponent* g_gui_component = NULL;

// Track feedback objects for manual deletion
#define MAX_FEEDBACK_OBJS 10
typedef struct {
    lv_obj_t *obj;
    uint32_t created_time;
} feedback_tracker_t;

static feedback_tracker_t feedback_list[MAX_FEEDBACK_OBJS] = {0};
static int feedback_count = 0;

// Gaussian touch feedback lookup table
#define GAUSSIAN_SIZE 63  // 63x63 pixel square (25% larger)
static uint8_t gaussian_lookup[GAUSSIAN_SIZE * GAUSSIAN_SIZE];
static bool gaussian_initialized = false;

// Initialize gaussian lookup table at startup
void GUIComponent::init_gaussian_lookup() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] init_gaussian_lookup");
#endif
    if (gaussian_initialized) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] init_gaussian_lookup - already initialized");
#endif
        return;
    }
    
    const float center = GAUSSIAN_SIZE / 2.0f;
    const float sigma = GAUSSIAN_SIZE / 6.0f;  // 3 sigma rule
    const float max_opa = 255.0f;  // 100% max opacity
    
    for (int y = 0; y < GAUSSIAN_SIZE; y++) {
        for (int x = 0; x < GAUSSIAN_SIZE; x++) {
            float dx = x - center;
            float dy = y - center;
            float r_squared = dx * dx + dy * dy;
            float radius = GAUSSIAN_SIZE / 2.0f;
            
            if (r_squared > radius * radius) {
                // Outside circle - transparent
                gaussian_lookup[y * GAUSSIAN_SIZE + x] = 0;
            } else {
                // Gaussian: e^(-(r^2)/(2*sigma^2))
                float gaussian = expf(-r_squared / (2.0f * sigma * sigma));
                uint8_t opa = (uint8_t)(max_opa * gaussian);
                gaussian_lookup[y * GAUSSIAN_SIZE + x] = opa;
            }
        }
    }
    
    gaussian_initialized = true;
    ESP_LOGI(TAG, "Gaussian lookup table initialized");
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] init_gaussian_lookup");
#endif
}

// ============================================================================
// GUIComponent Implementation
// ============================================================================

GUIComponent::GUIComponent() 
    : Component("GUI"), main_screen(nullptr) {
    ESP_LOGI(TAG, "GUIComponent created");
}

GUIComponent::~GUIComponent() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] ~GUIComponent");
#endif
    // LVGL cleans up screens automatically
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] ~GUIComponent");
#endif
}

void GUIComponent::setUpDependencies(ComponentGraph* graph) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] GUIComponent::setUpDependencies");
#endif
    // Store component graph reference (sensor parameters will be looked up later in guiStatusTask)
    this->component_graph = graph;
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] GUIComponent::setUpDependencies");
#endif
}

void GUIComponent::onInitialize() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] GUIComponent::initialize");
#endif
    ESP_LOGI(TAG, "Initializing GUIComponent...");

    // Add string parameter for button names (6 rows, 1 column) and store pointer
    buttonNames = addStringParam("button_names", NUM_BUTTONS, 1, "Button");
    
    // Initialize each button name
    if (buttonNames) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            std::string default_name = "Button " + std::to_string(i + 1);
            buttonNames->setValue(i, 0, default_name);
        }
        
        // Set onChange callback to trigger label update in LVGL task
        buttonNames->setOnChange([this](size_t row, size_t col, const std::string& val) {
            // Signal LVGL task to update the button label
            button_label_update_pending = true;
            ESP_LOGI(TAG, "Button name changed for button %zu: %s", row, val.c_str());
        });
    }
    
    // Register button pressed states as read-only bool params
    // These pulse true when a button is pressed, allowing external subscribers to react
    for (int i = 0; i < NUM_BUTTONS; i++) {
        std::string param_name = "button_" + std::to_string(i) + "_pressed";
        buttonPressed[i] = addBoolParam(param_name, 1, 1, false, true);  // read-only
    }

    // Brightness parameters - store member pointers
    userSetBrightness = addIntParam("user_set_brightness", 1, 1, 0, 100, 100); // User-controlled manual brightness (never touched by code)
    autoSetBrightness = addIntParam("auto_set_brightness", 1, 1, 0, 100, 100, true); // Auto-brightness from light sensor (read-only)
    desiredLcdBrightness = addIntParam("desired_lcd_brightness", 1, 1, 0, 100, 100, true); // Final target brightness (what current chases) (read-only)
    currentLcdBrightness = addIntParam("current_lcd_brightness", 1, 1, 0, 100, 100, true); // Actual current brightness (read-only)
    brightnessChangePerSecond = addIntParam("brighness_change_per_second", 1, 1, 10, 100, 50); // Rate at which current chases desired (0-100% per second)
    lcdScreenTimeoutSeconds = addIntParam("lcd_screen_timeout_seconds", 1, 1, 10, 600, 10); // Touch inactivity timeout in seconds
    motionInactivityScreenTimeoutSeconds = addIntParam("motion_inactivity_screen_timeout_seconds", 1, 1, 10, 600, 10); // Motion inactivity timeout in seconds
    lcdScreenOn = addBoolParam("lcd_screen_on", 1, 1, true); // When false, forces desired to 0. When true, relinquishes control
    overrideAutoBrightness = addBoolParam("override_auto_brightness", 1, 1, true); // When true, use user_set. When false, use auto_set
    overrideScreenTimeout = addBoolParam("override_screen_timeout", 1, 1, true); // When false, touch timeout can zero desired. When true, relinquishes
    overrideMotionInactivityScreenTimeout = addBoolParam("override_motion_inactivity_screen_timeout", 1, 1, true); // When false, motion timeout can zero desired. When true, relinquishes
    
    if (currentLcdBrightness) {
        currentLcdBrightness->setOnChange([this](size_t row, size_t col, int val) {
            int brightness = val;
            lcd_set_brightness(brightness);
        });
        // Set initial brightness directly (setValue won't trigger callback if value unchanged)
        lcd_set_brightness(100);
    }

    // Create GUI status task and its timer for lower-priority operations
    BaseType_t result = xTaskCreate(
        GUIComponent::guiStatusTaskWrapper,
        "gui_status_task",
        3072, // Stack depth - simple status updates
        this,
        tskIDLE_PRIORITY + 1,
        &gui_status_task_handle
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GUI status task");
    } else {
        ESP_LOGI(TAG, "GUI status task created successfully");
    }

    gui_status_timer_handle = xTimerCreate(
        "gui_status_timer",
        pdMS_TO_TICKS(100), // 100 millisecond interval
        pdTRUE,              // Auto-reload
        this,                // Pass 'this' as timer ID so we can access it in callback
        [](TimerHandle_t timer) {
            // Get the GUIComponent pointer from timer ID
            GUIComponent* gui = static_cast<GUIComponent*>(pvTimerGetTimerID(timer));
            xTaskNotifyGive(gui->gui_status_task_handle);
        }
    );

    result = xTimerStart(gui_status_timer_handle, 0);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to start GUI status timer");
    } else {
        ESP_LOGI(TAG, "GUI status timer started successfully");
    }

    // Initialize Notification Task (reads from ComponentGraph notification queue)
    result = xTaskCreate(
        notificationTaskWrapper,
        "notification_task",
        3072, // Stack depth - queue processing
        this, // Pass 'this' pointer
        tskIDLE_PRIORITY + 1,
        &notification_task_handle
    );
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create notification task");
        assert(false && "Task creation failed");
    }

    // Initialize hardware components
    panel_handle_ref = lcd_init();
    touch_handle_ref = touch_init();
    
    // Configure XPT2046 touch interrupt on GPIO 22 (PENIRQ - active low)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << TOUCH_IRQ_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // PENIRQ needs pull-up
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;  // Interrupt on falling edge (touch detected)
    gpio_config(&io_conf);
    
    // Install GPIO ISR service and add handler
    gpio_install_isr_service(0);
    gpio_isr_handler_add(TOUCH_IRQ_GPIO, GUIComponent::touch_irq_handler, NULL);
    
    ESP_LOGI(TAG, "XPT2046 touch IRQ configured on GPIO %d", TOUCH_IRQ_GPIO);
    
    // Initialize LVGL
    lv_init();
    
    // Allocate LVGL draw buffers
    size_t buffer_size = LCD_H_RES * 50; // 50 lines buffer
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_disp_draw_buf_init(&lvgl_disp_buf, buf1, buf2, buffer_size);
    
    // Initialize gaussian lookup table
    GUIComponent::init_gaussian_lookup();
    
    // Initialize LVGL display driver
    lv_disp_drv_init(&lvgl_disp_drv);
    lvgl_disp_drv.hor_res = LCD_H_RES;
    lvgl_disp_drv.ver_res = LCD_V_RES;
    lvgl_disp_drv.flush_cb = GUIComponent::lvgl_flush_cb;
    lvgl_disp_drv.draw_buf = &lvgl_disp_buf;
    lv_disp_drv_register(&lvgl_disp_drv);
    
    // Initialize LVGL theme (must be done AFTER display registration)
    lv_theme_t* theme = lv_theme_default_init(NULL, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), true, LV_FONT_DEFAULT);
    lv_disp_set_theme(NULL, theme);  // NULL = use default display (now it exists!)
    ESP_LOGI(TAG, "LVGL default theme initialized");
    
    // Initialize LVGL input device driver (touch)
    lv_indev_drv_init(&lvgl_indev_drv);
    lvgl_indev_drv.type = LV_INDEV_TYPE_POINTER;
    lvgl_indev_drv.read_cb = GUIComponent::lvgl_touch_read_cb;
    lvgl_indev_drv.user_data = this; // Store GUIComponent instance for callback
    lv_indev_drv_register(&lvgl_indev_drv);
    
    ESP_LOGI(TAG, "LVGL initialized");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    
    // Create LVGL timer task with larger stack
    xTaskCreate(GUIComponent::lvgl_timer_task, "lvgl_timer", 6144, NULL, 5, NULL);

    // Store the component instance for the LVGL task
    g_gui_component = this;

    initialized = true;
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] GUIComponent::initialize");
#endif
}

void GUIComponent::notificationTask() {
#ifdef DEBUG
    ESP_LOGI(TAG, "Notification task started");
#endif
    ESP_LOGI(TAG, "Notification task running - waiting for notifications...");
    
    if (!component_graph) {
        ESP_LOGE(TAG, "ComponentGraph not available - notification task exiting");
        return;
    }
    
    QueueHandle_t queue = component_graph->getGuiNotificationQueue();
    if (!queue) {
        ESP_LOGE(TAG, "GUI notification queue not available - task exiting");
        return;
    }
    
    while (true) {
        ComponentGraph::NotificationQueueItem item;
        ESP_LOGI(TAG, "Waiting to receive from notification queue...");
        if (xQueueReceive(queue, &item, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Received notification from queue: '%s', priority=%d, display_time=%lu ticks",
                     item.message, item.priority, item.ticks_to_display);
            
            if (!isInitialized()) {
                ESP_LOGW(TAG, "GUI not initialized yet - skipping notification");
                continue;
            }
            
            // Only process if higher or equal priority than current notification
            if (current_notification_priority > item.priority) {
                ESP_LOGI(TAG, "Skipping notification (priority %d < %d): %s", 
                         item.priority, current_notification_priority, item.message);
                continue;
            }
            
            // Store notification data for LVGL task to process
            // (LVGL objects can only be created from LVGL task - not thread-safe!)
            pending_notification_item = item;
            pending_notification = true;
            ESP_LOGI(TAG, "Notification queued for display by LVGL task");
        } else {
            ESP_LOGE(TAG, "xQueueReceive failed unexpectedly");
        }
    }
}

void GUIComponent::notificationTaskWrapper(void* pvParameters) {
    GUIComponent* gui_component = static_cast<GUIComponent*>(pvParameters);
    gui_component->notificationTask();
}

void GUIComponent::createPendingNotification() {
    if (!pending_notification) {
        return;
    }
    
    ComponentGraph::NotificationQueueItem item = pending_notification_item;
    pending_notification = false;  // Clear flag
    
    ESP_LOGI(TAG, "LVGL task creating notification overlay...");
    
    // Delete existing notification if present
    if (notification_overlay) {
        ESP_LOGI(TAG, "Deleting existing notification overlay");
        lv_obj_del(notification_overlay);
        notification_overlay = nullptr;
    }
    
    // Create notification overlay
    notification_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(notification_overlay, 280, 30);
    lv_obj_align(notification_overlay, LV_ALIGN_TOP_MID, 0, 20);
    
    // Set color based on notification level
    lv_color_t bg_color;
    switch (item.level) {
        case ComponentGraph::NotificationQueueItem::NotificationLevel::ERROR:
            bg_color = lv_color_make(150, 30, 30);  // Red
            break;
        case ComponentGraph::NotificationQueueItem::NotificationLevel::WARNING:
            bg_color = lv_color_make(150, 100, 0);  // Orange
            break;
        case ComponentGraph::NotificationQueueItem::NotificationLevel::INFO:
        default:
            bg_color = lv_color_make(40, 100, 40);  // Blue
            break;
    }
    
    lv_obj_set_style_bg_color(notification_overlay, bg_color, 0);
    lv_obj_set_style_border_color(notification_overlay, lv_color_white(), 0);
    lv_obj_set_style_border_width(notification_overlay, 2, 0);
    lv_obj_set_style_radius(notification_overlay, 10, 0);
    lv_obj_clear_flag(notification_overlay, LV_OBJ_FLAG_SCROLLABLE);
    
    // Add notification text
    lv_obj_t* label = lv_label_create(notification_overlay);
    lv_label_set_text(label, item.message);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 260);
    lv_obj_center(label);
    
    // Set expiration and priority
    notification_expire_tick = xTaskGetTickCount() + item.ticks_to_display;
    current_notification_priority = item.priority;
}

void GUIComponent::guiStatusTaskWrapper(void* pvParameters) {
    GUIComponent* gui_component = static_cast<GUIComponent*>(pvParameters);
    gui_component->guiStatusTask();
}

void GUIComponent::guiStatusTask() {
    ESP_LOGI(TAG, "GUI status task started");
    
    // Sensor parameter pointers (will be looked up from other components)
    static IntParameter* light_sensor_param = nullptr;
    static IntParameter* motion_sensor_param = nullptr;
    static bool first_run = true;
    static bool touch_timeout_was_active_last_cycle = false;
    static bool motion_timeout_was_active_last_cycle = false;
    
    while (true) {
        // Wait for notification from timer
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Initialize last interaction tick on first run
        if (first_run) {
            last_interaction_tick = xTaskGetTickCount();
            first_run = false;
        }
        
        if (!isInitialized()) {
            continue;
        }

        // Verify member parameter pointers are valid
        if (!(userSetBrightness && autoSetBrightness && desiredLcdBrightness && currentLcdBrightness && 
            brightnessChangePerSecond && lcdScreenTimeoutSeconds && motionInactivityScreenTimeoutSeconds &&
            lcdScreenOn && overrideAutoBrightness && overrideScreenTimeout && 
            overrideMotionInactivityScreenTimeout)) {
            ESP_LOGW(TAG, "Some member parameter pointers are null - skipping cycle");
            continue;
        }
        
        // Try to get sensor parameters once (they may not exist yet or at all)
        if (component_graph && !(light_sensor_param && motion_sensor_param)) {
            light_sensor_param = component_graph->getIntParam("LightSensor", "current_light_level");
            if (light_sensor_param) {
                ESP_LOGI(TAG, "Successfully linked to LightSensor parameter");
            } else {
                ESP_LOGW(TAG, "LightSensor parameter not found - auto-brightness disabled");
            }
            
            motion_sensor_param = component_graph->getIntParam("MotionSensor", "last_motion_detected_seconds");
            if (motion_sensor_param) {
                ESP_LOGI(TAG, "Successfully linked to MotionSensor parameter");
            } else {
                ESP_LOGW(TAG, "MotionSensor/last_motion_detected_seconds parameter not found - motion-based screen timeout disabled");
            }
        }

        // ===========================================
        // BRIGHTNESS CONTROL FLOW (as specified)
        // ===========================================
        
        // STEP 1: Update auto_set_brightness from light sensor
        if (light_sensor_param && autoSetBrightness) {
            int light_level = light_sensor_param->getValue(0, 0);
            
            // QUADRATIC MAPPING (x^2 normalized to 0-100%):
            float normalized = (float)light_level / 4095.0f;
            int brightness = (int)(pow(normalized, 2.0f) * 100);
            
            // Enforce minimum brightness for auto mode (never go to 0)
            const int MIN_AUTO_BRIGHTNESS = 1;  // Minimum 1% brightness
            if (brightness < MIN_AUTO_BRIGHTNESS) {
                brightness = MIN_AUTO_BRIGHTNESS;
            }
            
            autoSetBrightness->setValue(0, 0, brightness);
        }
        
        // STEP 2: Choose which brightness to use (user vs auto) and set desired_lcd_brightness
        int base_brightness = 100;  // Default
        if (overrideAutoBrightness && overrideAutoBrightness->getValue(0, 0) == true) {
            // Use manual (user_set_brightness)
            if (userSetBrightness) {
                base_brightness = userSetBrightness->getValue(0, 0);
            }
        } else {
            // Use auto (auto_set_brightness)
            if (autoSetBrightness) {
                base_brightness = autoSetBrightness->getValue(0, 0);
            }
        }
        
        int desired_brightness = base_brightness;
        
        // STEP 3: Apply zeroing logic based on lcd_screen_on, timeouts, etc.
        
        // 3a) lcd_screen_on - when false, forces desired to 0. When true, relinquishes control
        if (lcdScreenOn && lcdScreenOn->getValue(0, 0) == false) {
            desired_brightness = 0;
        }
        
        // 3b) Touch inactivity timeout
        bool touch_timeout_active = false;
        if (overrideScreenTimeout && overrideScreenTimeout->getValue(0, 0) == false) {
            // Touch timeout is enabled - check if timeout reached
            TickType_t tick_delta = xTaskGetTickCount() - last_interaction_tick;
            TickType_t timeout_ticks = pdMS_TO_TICKS(lcdScreenTimeoutSeconds->getValue(0, 0) * 1000);
            
            if (tick_delta >= timeout_ticks) {
                // Timeout reached - zero desired
                touch_timeout_active = true;
                desired_brightness = 0;
            }
        } else {
            // Override is true - relinquish control
            if (touch_timeout_was_active_last_cycle) {
                // Screen was timed out, now override is back to true - restore brightness
                desired_brightness = base_brightness;
            }
        }
        touch_timeout_was_active_last_cycle = touch_timeout_active;
        
        // 3c) Motion inactivity timeout
        bool motion_timeout_active = false;
        if (overrideMotionInactivityScreenTimeout && 
            overrideMotionInactivityScreenTimeout->getValue(0, 0) == false) {
            // Motion timeout is enabled - check if timeout reached
            if (motion_sensor_param && motionInactivityScreenTimeoutSeconds) {
                int current_time_seconds = (int)(esp_timer_get_time() / 1000000);
                int last_motion_time = motion_sensor_param->getValue(0, 0);
                int motion_inactive_seconds = current_time_seconds - last_motion_time;
                
                if (motion_inactive_seconds >= motionInactivityScreenTimeoutSeconds->getValue(0, 0)) {
                    // Motion timeout reached - zero desired
                    motion_timeout_active = true;
                    desired_brightness = 0;
                }
            }
        } else {
            // Override is true - relinquish control
            if (motion_timeout_was_active_last_cycle) {
                // Screen was timed out, now override is back to true - restore brightness
                desired_brightness = base_brightness;
            }
        }
        motion_timeout_was_active_last_cycle = motion_timeout_active;

        desiredLcdBrightness->setValue(0, 0, desired_brightness);
        
        // STEP 4: Update current_lcd_brightness to chase desired_lcd_brightness at specified rate
        int current_brightness = currentLcdBrightness->getValue(0, 0);
        int change_rate = brightnessChangePerSecond->getValue(0, 0)/10; // percent per second
        int brightness_diff = desired_brightness - current_brightness;
        if (abs(brightness_diff) <= change_rate) {
            // Do not allow deadband oscillation - snap to desired
            currentLcdBrightness->setValue(0, 0, desired_brightness);
            continue;
        }
        else if (current_brightness < desired_brightness) {
            current_brightness += change_rate;
            if (current_brightness > desired_brightness) {
                current_brightness = desired_brightness;
            }
            currentLcdBrightness->setValue(0, 0, current_brightness);
        } else if (current_brightness > desired_brightness) {
            current_brightness -= change_rate;
            if (current_brightness < desired_brightness) {
                current_brightness = desired_brightness;
            }
            currentLcdBrightness->setValue(0, 0, current_brightness);
        }

        // Clean up expired notification
        if (notification_overlay && xTaskGetTickCount() >= notification_expire_tick) {
            lv_obj_del(notification_overlay);
            notification_overlay = nullptr;
            current_notification_priority = -1;
        }
    }
}

// ============================================================================
// Simple Button Grid (3x2 = 6 buttons)
// ============================================================================

void GUIComponent::createSimpleButtonGrid() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] createSimpleButtonGrid");
#endif
    ESP_LOGI(TAG, "Creating simple 3x2 button grid...");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    
    // Create main screen
    main_screen = lv_obj_create(NULL);
    if (!main_screen) {
        ESP_LOGE(TAG, "Failed to create main screen - Out of memory?");
        ESP_LOGE(TAG, "Free heap: %lu bytes, Minimum ever: %lu bytes", 
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
        return;
    }
    lv_obj_set_style_bg_color(main_screen, lv_color_black(), 0);
    
    // Title
    lv_obj_t* title = lv_label_create(main_screen);
    lv_label_set_text(title, "Smart Home Controls");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Create 3x2 grid of buttons
    const int btn_width = 90;
    const int btn_height = 60;
    const int h_spacing = 10;
    const int v_spacing = 15;
    const int start_y = 50;
    
    for (int i = 0; i < NUM_BUTTONS; i++) {
        int row = i / 3;  // 0 or 1
        int col = i % 3;  // 0, 1, or 2
        
        lv_obj_t* btn = lv_btn_create(main_screen);
        lv_obj_set_size(btn, btn_width, btn_height);
        
        // Calculate position
        int x = 10 + col * (btn_width + h_spacing);
        int y = start_y + row * (btn_height + v_spacing);
        lv_obj_set_pos(btn, x, y);
        
        // Set button color (different colors for visual distinction)
        lv_color_t colors[NUM_BUTTONS] = {
            lv_color_make(0, 100, 200),   // Blue
            lv_color_make(200, 100, 0),   // Orange
            lv_color_make(0, 150, 100),   // Teal
            lv_color_make(150, 0, 150),   // Purple
            lv_color_make(200, 0, 0),     // Red
            lv_color_make(0, 200, 0)      // Green
        };
        lv_obj_set_style_bg_color(btn, colors[i], 0);
        
        // Button label - get text from parameter using member pointer (row i, column 0)
        lv_obj_t* label = lv_label_create(btn);
        if (buttonNames) {
            lv_label_set_text(label, buttonNames->getValue(i, 0).c_str());
        } else {
            lv_label_set_text(label, ("Button " + std::to_string(i + 1)).c_str());
        }
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);          // Enable text wrapping
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0); // Center-align text
        lv_obj_set_width(label, btn_width - 10);                     // Wrap within button (with 5px padding each side)
        lv_obj_center(label);
        
        // Store label reference for dynamic updates
        button_labels[i] = label;
        
        // Add click event with button index as user data
        lv_obj_add_event_cb(btn, simple_button_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    
    // IP address label at bottom
    lv_obj_t* ip_label = lv_label_create(main_screen);
    char ip_str[32];
    if (wifi_get_ip_string(ip_str, sizeof(ip_str))) {
        char full_text[48];
        snprintf(full_text, sizeof(full_text), "IP: %s", ip_str);
        lv_label_set_text(ip_label, full_text);
    } else {
        lv_label_set_text(ip_label, "IP: Not connected");
    }
    lv_obj_set_style_text_color(ip_label, lv_color_make(150, 150, 150), 0);
    lv_obj_align(ip_label, LV_ALIGN_BOTTOM_MID, 0, -5);
    
    // Load the screen
    lv_scr_load(main_screen);
    
    ESP_LOGI(TAG, "Simple button grid created successfully");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] createSimpleButtonGrid");
#endif
}

void GUIComponent::simple_button_event_cb(lv_event_t* e) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] simple_button_event_cb");
#endif
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] simple_button_event_cb - not clicked");
#endif
        return;
    }
    
    // Get button index from user data
    int button_index = (int)(intptr_t)lv_event_get_user_data(e);
    
    // Pulse the button pressed state: set to true, notify subscribers
    // External systems can subscribe to this and react accordingly
    if (g_gui_component && g_gui_component->buttonPressed[button_index]) {
        BoolParameter* btn = g_gui_component->buttonPressed[button_index];
        btn->setValue(0, 0, true);  // Triggers onChange which broadcasts to subscribers
        ESP_LOGI(TAG, "Button %d pressed - notifying subscribers", button_index);
        
        // Hold true briefly so it's visible in UI before resetting
        vTaskDelay(pdMS_TO_TICKS(150));
        
        // Reset back to false (pulse behavior)
        // Subscribers will see the true→false transition
        btn->setValue(0, 0, false);
    }
    
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] simple_button_event_cb");
#endif
}

// ============================================================================
// End GUIComponent Implementation
// ============================================================================

// Custom animation setter for opacity
void GUIComponent::set_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

// Create visual touch feedback at the given position
void GUIComponent::create_touch_feedback(int16_t x, int16_t y) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] create_touch_feedback - x: %d, y: %d", x, y);
#endif
    // Create a canvas to draw the gaussian pattern
    lv_obj_t *canvas = lv_canvas_create(lv_scr_act());
    
    // Allocate buffer for canvas (TRUE_COLOR_ALPHA = RGB565 + 8bit alpha = 3 bytes per pixel)
    static uint8_t cbuf[GAUSSIAN_SIZE * GAUSSIAN_SIZE * 3];
    lv_canvas_set_buffer(canvas, cbuf, GAUSSIAN_SIZE, GAUSSIAN_SIZE, LV_IMG_CF_TRUE_COLOR_ALPHA);
    
    // Fill canvas with gaussian pattern using direct buffer access
    lv_color_t white = lv_color_white();
    for (int py = 0; py < GAUSSIAN_SIZE; py++) {
        for (int px = 0; px < GAUSSIAN_SIZE; px++) {
            uint8_t opa = gaussian_lookup[py * GAUSSIAN_SIZE + px];
            
            // For TRUE_COLOR_ALPHA: 2 bytes color (RGB565) + 1 byte alpha
            int offset = (py * GAUSSIAN_SIZE + px) * 3;
            cbuf[offset] = white.full & 0xFF;           // Low byte of RGB565
            cbuf[offset + 1] = (white.full >> 8) & 0xFF; // High byte of RGB565
            cbuf[offset + 2] = opa;                      // Alpha channel
        }
    }
    
    // Position canvas centered on touch point
    lv_obj_set_pos(canvas, x - GAUSSIAN_SIZE / 2, y - GAUSSIAN_SIZE / 2);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    
    // Add to tracking list
    if (feedback_count < MAX_FEEDBACK_OBJS) {
        feedback_list[feedback_count].obj = canvas;
        feedback_list[feedback_count].created_time = lv_tick_get();
        feedback_count++;
    }
    
    // Animate overall opacity fade
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, canvas);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&anim, 125);
    lv_anim_set_exec_cb(&anim, GUIComponent::set_opa);
    lv_anim_start(&anim);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] create_touch_feedback");
#endif
}

// XPT2046 touch interrupt handler (PENIRQ is active low)
void IRAM_ATTR GUIComponent::touch_irq_handler(void* arg) {
    touch_irq_triggered = true;
}

// LVGL input device read callback (static trampoline)
void GUIComponent::lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    // Retrieve the GUIComponent instance from user_data
    GUIComponent* gui = static_cast<GUIComponent*>(drv->user_data);
    if (gui) {
        gui->handleTouchRead(data);
    }
}

// Touch reading implementation (non-static member function)
void GUIComponent::handleTouchRead(lv_indev_data_t *data) {
    // State machine states
    enum TouchState {
        IDLE,           // No touch, waiting for interrupt
        TOUCHING,       // Actively tracking a touch
        BLOCKED         // Touch started while screen off - ignore entire gesture
    };
    
    static TouchState state = IDLE;
    static uint16_t touchpad_x[1] = {0};
    static uint16_t touchpad_y[1] = {0};
    static uint8_t touchpad_cnt = 0;
    static BoolParameter* lcd_screen_on_param = nullptr;
    
    // Initialize screen on parameter once
    if (!lcd_screen_on_param) {
        lcd_screen_on_param = getBoolParam("lcd_screen_on");
    }
    
    // STATE MACHINE
    switch (state) {
        case IDLE: {
            // Waiting for touch interrupt
            if (touch_irq_triggered) {
                touch_irq_triggered = false;  // Clear flag immediately
                
                // Read touch controller
                esp_lcd_touch_read_data(touch_handle_ref);
                bool touched = esp_lcd_touch_get_coordinates(touch_handle_ref, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);
                
                if (touched && touchpad_cnt > 0) {
                    // Check if screen is off
                    if (lcd_screen_on_param && !lcd_screen_on_param->getValue(0, 0)) {
                        // Screen off - wake it up and block this touch gesture
                        ESP_LOGI(TAG, "Touch detected while screen off - waking screen, blocking gesture");
                        last_interaction_tick = xTaskGetTickCount();  // Wake screen by updating interaction time
                        state = BLOCKED;
                        data->state = LV_INDEV_STATE_RELEASED;
                    } else {
                        // Screen on - start tracking touch
                        ESP_LOGI(TAG, "Touch started - entering TOUCHING state");
                        state = TOUCHING;
                        last_interaction_tick = xTaskGetTickCount();
                        data->state = LV_INDEV_STATE_PRESSED;
                        data->point.x = touchpad_x[0];
                        data->point.y = touchpad_y[0];
                        GUIComponent::create_touch_feedback(touchpad_x[0], touchpad_y[0]);
                    }
                } else {
                    // False alarm - stay IDLE
                    data->state = LV_INDEV_STATE_RELEASED;
                }
            } else {
                // No interrupt, no touch
                data->state = LV_INDEV_STATE_RELEASED;
            }
            break;
        }
            
        case TOUCHING: {
            // Continue tracking touch
            esp_lcd_touch_read_data(touch_handle_ref);
            bool touched = esp_lcd_touch_get_coordinates(touch_handle_ref, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);
            
            if (touched && touchpad_cnt > 0) {
                // Still touching - update position
                last_interaction_tick = xTaskGetTickCount();
                data->state = LV_INDEV_STATE_PRESSED;
                data->point.x = touchpad_x[0];
                data->point.y = touchpad_y[0];
            } else {
                // Touch released - return to IDLE
                state = IDLE;
                data->state = LV_INDEV_STATE_RELEASED;
            }
            break;
        }
            
        case BLOCKED: {
            // Touch started while screen off - keep checking if released
            esp_lcd_touch_read_data(touch_handle_ref);
            bool still_touched = esp_lcd_touch_get_coordinates(touch_handle_ref, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);
            
            if (!still_touched || touchpad_cnt == 0) {
                // Touch released - return to IDLE
                ESP_LOGI(TAG, "Blocked touch released - returning to IDLE");
                state = IDLE;
            }
            // Always report released to LVGL (touch blocked)
            data->state = LV_INDEV_STATE_RELEASED;
            break;
        }
    }
}

// LVGL flush callback
void GUIComponent::lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;
    
    esp_lcd_panel_draw_bitmap(panel_handle_ref, x1, y1, x2, y2, (uint16_t *)color_map);
    lv_disp_flush_ready(drv);
}

// LVGL timer task
void GUIComponent::lvgl_timer_task(void *arg) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] lvgl_timer_task");
#endif
    ESP_LOGI(TAG, "LVGL timer task started");
    
    // Main LVGL timer loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        
        // Check if there's a pending notification to create
        if (g_gui_component->pending_notification) {
            g_gui_component->createPendingNotification();
        }
        
        // Check if button labels need updating (thread-safe from LVGL task)
        if (g_gui_component->button_label_update_pending) {
            g_gui_component->button_label_update_pending = false;
            StringParameter* names = g_gui_component->getStringParam("button_names");
            if (names) {
                for (int i = 0; i < NUM_BUTTONS; i++) {
                    if (g_gui_component->button_labels[i]) {
                        std::string name = names->getValue(i, 0);
                        lv_label_set_text(g_gui_component->button_labels[i], name.c_str());
                    }
                }
            }
        }

        // Increment LVGL tick by 10ms
        lv_tick_inc(10);

        // Call LVGL timer handler to process rendering and animations
        lv_timer_handler();
        
        // Manually delete old feedback objects after 225ms
        uint32_t now = lv_tick_get();
        for (int i = 0; i < feedback_count; i++) {
            uint32_t age = now - feedback_list[i].created_time;
            if (feedback_list[i].obj && age > 225) {  // 225ms (after 125ms animation)
                lv_obj_del(feedback_list[i].obj);
                // Shift remaining items down
                for (int j = i; j < feedback_count - 1; j++) {
                    feedback_list[j] = feedback_list[j + 1];
                }
                feedback_count--;
                i--; // Recheck this index
            }
        }
    }
}
