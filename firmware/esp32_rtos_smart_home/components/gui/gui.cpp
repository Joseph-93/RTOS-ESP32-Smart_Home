#include "gui.h"
#include "lcd/lcd.h"
#include "touch/touch.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/dac.h"
#include <math.h>
#include <algorithm>

static const char *TAG = "GUI";

// Display dimensions
#define LCD_H_RES    320
#define LCD_V_RES    240

// LVGL driver structures
static lv_disp_drv_t lvgl_disp_drv;
static lv_disp_draw_buf_t lvgl_disp_buf;
static lv_indev_drv_t lvgl_indev_drv;

static esp_lcd_panel_handle_t panel_handle_ref = NULL;
static esp_lcd_touch_handle_t touch_handle_ref = NULL;

// UI creation signaling (for LVGL task)
static GUIComponent* g_gui_component = NULL;
static volatile bool g_create_ui_requested = false;

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
static void init_gaussian_lookup() {
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
    : Component("GUI"), root_node(nullptr), current_node(nullptr) {
    ESP_LOGI(TAG, "GUIComponent created");
}

GUIComponent::~GUIComponent() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] ~GUIComponent");
#endif
    // Clean up menu tree
    if (root_node) {
        delete root_node;
    }
    for (auto* node : all_nodes) {
        if (node != root_node) {
            delete node;
        }
    }
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] ~GUIComponent");
#endif
}

void GUIComponent::initialize() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] GUIComponent::initialize");
#endif
    ESP_LOGI(TAG, "Initializing GUIComponent...");

    // Add a brightness parameter to control LCD backlight
    addIntParam("desired_lcd_brightness", 1, 1, 0, 100, 100); // desired target brightness 0-100%
    addIntParam("current_lcd_brightness", 1, 1, 0, 100, 100); // Read-only current brightness 0-100%
    addIntParam("brighness_change_per_second", 1, 1, 10, 100, 50); // How fast to change brightness (0-100% per second)
    addIntParam("lcd_screen_timeout_seconds", 1, 1, 10, 600, 20); // Screen timeout in seconds
    addBoolParam("lcd_screen_on", 1, 1, true); // If false, turn off screen (saves power)
    addBoolParam("override_auto_brightness", 1, 1, false); // If true, disable auto brightness adjustment
    addBoolParam("override_screen_timeout", 1, 1, false); // If true, disable screen timeout
    auto* brightness_param = getIntParam("current_lcd_brightness");
    if (brightness_param) {
        brightness_param->setOnChange([this](size_t row, size_t col, int val) {
            int brightness = val;
            // Map 0-100% to useful DAC range (26-64)
            // Below 40% (DAC 26) was too dim, so start there
            uint8_t dac_value = 26 + (brightness * 38) / 100;
            // Set DAC output on GPIO 25 (DAC channel 1)
            dac_output_voltage(DAC_CHANNEL_1, dac_value);
        });
        // Set initial brightness to 100%
        brightness_param->setValue(0, 0, 100);
    }

    // Create GUI status task and its timer for lower-priority operations
    BaseType_t result = xTaskCreate(
        GUIComponent::guiStatusTaskWrapper,
        "gui_status_task",
        4096, // Stack depth
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

    initialized = true;
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] GUIComponent::initialize");
#endif
}

void GUIComponent::guiStatusTaskWrapper(void* pvParameters) {
    GUIComponent* gui_component = static_cast<GUIComponent*>(pvParameters);
    gui_component->guiStatusTask();
}

void GUIComponent::guiStatusTask() {
    ESP_LOGI(TAG, "GUI status task started");
    
    // Declare static param pointers (will be initialized on first run AFTER component is initialized)
    static IntParameter* desired_brightness_param = nullptr;
    static IntParameter* current_brightness_param = nullptr;
    static IntParameter* change_rate_param = nullptr;
    static IntParameter* screen_timeout_param = nullptr;
    static BoolParameter* lcd_screen_on_param = nullptr;
    static BoolParameter* override_auto_brightness_param = nullptr;
    static BoolParameter* override_screen_timeout_param = nullptr;
    static bool first_run = true;
    
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

        // Initialize parameter pointers on first run (AFTER component is initialized)
        if (!(desired_brightness_param && current_brightness_param && change_rate_param &&
            screen_timeout_param && lcd_screen_on_param && override_auto_brightness_param && override_screen_timeout_param)) {
            desired_brightness_param = this->getIntParam("desired_lcd_brightness");
            current_brightness_param = this->getIntParam("current_lcd_brightness");
            change_rate_param = this->getIntParam("brighness_change_per_second");
            screen_timeout_param = this->getIntParam("lcd_screen_timeout_seconds");
            lcd_screen_on_param = this->getBoolParam("lcd_screen_on");
            override_auto_brightness_param = this->getBoolParam("override_auto_brightness");
            override_screen_timeout_param = this->getBoolParam("override_screen_timeout");
        }

        // Handle screen timeout
        if (override_screen_timeout_param && override_screen_timeout_param->getValue(0, 0) == false) {
            TickType_t tick_delta = xTaskGetTickCount() - last_interaction_tick;
            TickType_t timeout_ticks = pdMS_TO_TICKS(screen_timeout_param->getValue(0, 0) * 1000);
            if (tick_delta >= timeout_ticks && lcd_screen_on_param) {
                // Timeout reached - turn off screen
                lcd_screen_on_param->setValue(0, 0, false);
            }
            else {
                // Timeout not yet reached - ensure screen is on
                lcd_screen_on_param->setValue(0, 0, true);
            }
        }

        if (lcd_screen_on_param && lcd_screen_on_param->getValue(0, 0) == false) {
            // Screen off - set brightness to 0
            if (desired_brightness_param) {
                desired_brightness_param->setValue(0, 0, 0);
            }
            ESP_LOGI(TAG, "Screen is off - skipping brightness adjustment");
        }
        else {
            // Calculate and set desired brightness
            if (light_sensor_current_light_level && override_auto_brightness_param && override_auto_brightness_param->getValue(0, 0) == false) { // light_sensor_current_light_level is a Parameter from LightSensor
                int light_level = light_sensor_current_light_level->getValue(0, 0);
                
                // QUADRATIC MAPPING:
                // New nonlinear mapping: x^(1/4)*8.75+30
                // 30 is baseline brightness. 8.75 scales to 100. x^1/4 is for response curve.
                // int brightness = static_cast<int>(pow(light_level, 0.25) * 8.75 + 30);

                // LINEAR MAPPING:
                // int brightness = (light_level * 100) / 4095;

                // LINEAR MAPPING WITH BASELINE:
                int baseline = 30; // Minimum brightness
                int brightness = baseline + (light_level * (100 - baseline)) / 4095;
                
                // Update GUI brightness parameter
                if (desired_brightness_param) {
                    desired_brightness_param->setValue(0, 0, brightness);
                }
            }
        }

        // Update the current brightness parameter to get closer to desired brightness
        // NOTE: This should ALWAYS be triggered, as it is no longer CONTROL-LAW, but rather a CONTROL LOOP for smooth transitions!
        int current_brightness = current_brightness_param->getValue(0, 0);
        int desired_brightness = desired_brightness_param->getValue(0, 0);
        int change_rate = change_rate_param->getValue(0, 0)/10; // percent per second
        int brightness_diff = desired_brightness - current_brightness;
        if (abs(brightness_diff) <= change_rate) {
            // Do not allow deadband oscillation - snap to desired
            current_brightness_param->setValue(0, 0, desired_brightness);
            continue;
        }
        else if (current_brightness < desired_brightness) {
            current_brightness += change_rate;
            if (current_brightness > desired_brightness) {
                current_brightness = desired_brightness;
            }
            ESP_LOGI(TAG, "Increasing brightness to %d%%", current_brightness);
            current_brightness_param->setValue(0, 0, current_brightness);
        } else if (current_brightness > desired_brightness) {
            current_brightness -= change_rate;
            if (current_brightness < desired_brightness) {
                current_brightness = desired_brightness;
            }
            ESP_LOGI(TAG, "Decreasing brightness to %d%%", current_brightness);
            current_brightness_param->setValue(0, 0, current_brightness);
        }
    }
}

void GUIComponent::registerComponent(Component* component) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] GUIComponent::registerComponent - component: %p", component);
#endif
    if (!component) {
        ESP_LOGE(TAG, "Attempted to register null component");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] GUIComponent::registerComponent - null component");
#endif
        return;
    }
    if (component->getName() == "LightSensor") {
        light_sensor_current_light_level = component->getIntParam("current_light_level");
    }
    registered_components.push_back(component);
    ESP_LOGI(TAG, "Registered component: %s", component->getName().c_str());
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] GUIComponent::registerComponent");
#endif
}

void GUIComponent::buildMenuTree() {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] GUIComponent::buildMenuTree");
#endif
    ESP_LOGI(TAG, "Building menu tree...");
    ESP_LOGI(TAG, "Free heap before building tree: %lu bytes", esp_get_free_heap_size());
    
    // Create root node (home screen) - will be populated after components are added
    root_node = new MenuNode("Home", nullptr, this);
    all_nodes.push_back(root_node);
    current_node = root_node;
    
    // For each registered component, create its menu subtree
    for (Component* comp : registered_components) {
        MenuNode* comp_node = createComponentNode(comp, root_node);
        root_node->children.push_back(comp_node);
    }
    
    // Now create home screen with all component buttons
    createHomeScreen(root_node);
    
    ESP_LOGI(TAG, "Menu tree built with %zu top-level components", root_node->children.size());
    
    // Display the home screen
    lv_scr_load(root_node->screen);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] GUIComponent::buildMenuTree");
#endif
}

MenuNode* GUIComponent::createComponentNode(Component* component, MenuNode* parent) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] GUIComponent::createComponentNode - component: %s", 
             component ? component->getName().c_str() : "null");
#endif
    MenuNode* node = new MenuNode(component->getName(), parent, this);
    node->associated_component = component;
    all_nodes.push_back(node);
    
    // Create Parameters submenu (just structure, no screen yet)
    MenuNode* params_node = createParametersNode(component, node);
    node->children.push_back(params_node);
    
    // Create Actions submenu (just structure, no screen yet)
    MenuNode* actions_node = createActionsNode(component, node);
    node->children.push_back(actions_node);
    
    // Don't create screen yet - will be created on-demand when navigated to
    
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] GUIComponent::createComponentNode");
#endif
    return node;
}

MenuNode* GUIComponent::createParametersNode(Component* component, MenuNode* parent) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] GUIComponent::createParametersNode - component: %s",
             component ? component->getName().c_str() : "null");
#endif
    MenuNode* node = new MenuNode("Parameters", parent, this);
    node->associated_component = component;
    all_nodes.push_back(node);
    
    // Create child nodes for each parameter (structure only, no screens yet)
    for (auto& param : component->getIntParams()) {
        MenuNode* param_node = new MenuNode(param->getName(), node, this);
        param_node->associated_component = component;
        param_node->param_name = param->getName();
        param_node->param_type = "int";
        all_nodes.push_back(param_node);
        node->children.push_back(param_node);
    }
    
    for (auto& param : component->getFloatParams()) {
        MenuNode* param_node = new MenuNode(param->getName(), node, this);
        param_node->associated_component = component;
        param_node->param_name = param->getName();
        param_node->param_type = "float";
        all_nodes.push_back(param_node);
        node->children.push_back(param_node);
    }
    
    for (auto& param : component->getBoolParams()) {
        MenuNode* param_node = new MenuNode(param->getName(), node, this);
        param_node->associated_component = component;
        param_node->param_name = param->getName();
        param_node->param_type = "bool";
        all_nodes.push_back(param_node);
        node->children.push_back(param_node);
    }
    
    for (auto& param : component->getStringParams()) {
        MenuNode* param_node = new MenuNode(param->getName(), node, this);
        param_node->associated_component = component;
        param_node->param_name = param->getName();
        param_node->param_type = "string";
        all_nodes.push_back(param_node);
        node->children.push_back(param_node);
    }
    
    // Don't create screens yet - will be created on-demand when navigated to
    
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] GUIComponent::createParametersNode");
#endif
    return node;
}

MenuNode* GUIComponent::createActionsNode(Component* component, MenuNode* parent) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] GUIComponent::createActionsNode - component: %s",
             component ? component->getName().c_str() : "null");
#endif
    MenuNode* node = new MenuNode("Actions", parent, this);
    node->associated_component = component;
    all_nodes.push_back(node);
    
    // Don't create screen yet - will be created on-demand
    
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] GUIComponent::createActionsNode");
#endif
    return node;
}

void GUIComponent::navigateToNode(MenuNode* node) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] GUIComponent::navigateToNode - node: %s", 
             node ? node->name.c_str() : "null");
#endif
    if (!node) {
        ESP_LOGE(TAG, "Cannot navigate to null node");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] GUIComponent::navigateToNode - null node");
#endif
        return;
    }
    
    // If navigating to a parameter detail screen, delete any existing screen to force refresh
    if (node->screen && !node->param_name.empty() && node->param_type != "") {
        ESP_LOGI(TAG, "Deleting cached parameter detail screen to force refresh");
        lv_obj_del(node->screen);
        node->screen = nullptr;
    }
    
    // Create screen on-demand if it doesn't exist yet
    ensureScreenCreated(node);
    
    if (!node->screen) {
        ESP_LOGE(TAG, "Failed to create screen for node: %s", node->name.c_str());
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] GUIComponent::navigateToNode - screen creation failed");
#endif
        return;
    }
    
    current_node = node;
    lv_scr_load(node->screen);
    ESP_LOGI(TAG, "Navigated to: %s", node->name.c_str());
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] GUIComponent::navigateToNode");
#endif
}

void GUIComponent::ensureScreenCreated(MenuNode* node) {
    if (!node || node->screen) return; // Already created or invalid
    
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] ensureScreenCreated - node: %s", node->name.c_str());
#endif
    
    // Determine what type of screen to create based on node properties
    if (node == root_node) {
        // Home screen
        createHomeScreen(node);
    } else if (node->associated_component) {
        if (node->name == "Parameters") {
            createParametersScreen(node, node->associated_component);
        } else if (node->name == "Actions") {
            createActionsScreen(node, node->associated_component);
        } else if (!node->param_name.empty()) {
            // Check if this is a parameter edit screen (has row/col set) or detail screen
#ifdef DEBUG
            ESP_LOGI(TAG, "ensureScreenCreated: checking node name='%s' for '_edit'", node->name.c_str());
#endif
            if (node->name.find("_edit") != std::string::npos) {
                // Parameter edit screen
#ifdef DEBUG
                ESP_LOGI(TAG, "ensureScreenCreated: calling createParameterEditScreen");
#endif
                createParameterEditScreen(node, node->associated_component);
            } else {
                // Parameter detail screen
#ifdef DEBUG
                ESP_LOGI(TAG, "ensureScreenCreated: calling createParameterDetailScreen");
#endif
                createParameterDetailScreen(node, node->associated_component, 
                                           node->param_name, node->param_type);
            }
        } else {
            // Component detail screen
            createComponentScreen(node, node->associated_component);
        }
    }
    
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] ensureScreenCreated - screen: %p", node->screen);
#endif
}

// Event handlers

void GUIComponent::menu_button_event_cb(lv_event_t* e) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] menu_button_event_cb");
#endif
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] menu_button_event_cb - not clicked");
#endif
        return;
    }
    
    MenuNode* target_node = (MenuNode*)lv_event_get_user_data(e);
    if (!target_node || !target_node->gui_instance) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] menu_button_event_cb - invalid node");
#endif
        return;
    }
    
    target_node->gui_instance->navigateToNode(target_node);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] menu_button_event_cb");
#endif
}

void GUIComponent::back_button_event_cb(lv_event_t* e) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] back_button_event_cb");
#endif
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] back_button_event_cb - not clicked");
#endif
        return;
    }
    
    MenuNode* current = (MenuNode*)lv_event_get_user_data(e);
    if (!current || !current->parent || !current->gui_instance) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] back_button_event_cb - invalid node");
#endif
        return;
    }
    
    current->gui_instance->navigateToNode(current->parent);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] back_button_event_cb");
#endif
}

void GUIComponent::action_button_event_cb(lv_event_t* e) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] action_button_event_cb");
#endif
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] action_button_event_cb - not clicked");
#endif
        return;
    }
    
    MenuNode* node = (MenuNode*)lv_event_get_user_data(e);
    if (!node || !node->associated_component || node->action_name.empty()) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] action_button_event_cb - invalid node/component/action");
#endif
        return;
    }
    
    ESP_LOGI(TAG, "Action button clicked: %s", node->action_name.c_str());
    node->associated_component->invokeAction(node->action_name);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] action_button_event_cb");
#endif
}

void GUIComponent::param_edit_button_event_cb(lv_event_t* e) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] param_edit_button_event_cb");
#endif
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] param_edit_button_event_cb - not clicked");
#endif
        return;
    }
    
    MenuNode* node = (MenuNode*)lv_event_get_user_data(e);
    if (!node || !node->associated_component || !node->gui_instance) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] param_edit_button_event_cb - invalid node");
#endif
        return;
    }
    
    ESP_LOGI(TAG, "Edit parameter: %s[%zu,%zu]", node->param_name.c_str(), node->param_row, node->param_col);
    node->gui_instance->navigateToNode(node);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] param_edit_button_event_cb");
#endif
}

void GUIComponent::param_save_button_event_cb(lv_event_t* e) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] param_save_button_event_cb");
#endif
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] param_save_button_event_cb - not clicked");
#endif
        return;
    }
    
    MenuNode* node = (MenuNode*)lv_event_get_user_data(e);
    if (!node || !node->associated_component || !node->gui_instance) {
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] param_save_button_event_cb - invalid node");
#endif
        return;
    }
    
    // The slider value was already saved during slider change event
    // Just navigate back to parent
    ESP_LOGI(TAG, "Saving parameter: %s[%zu,%zu]", node->param_name.c_str(), node->param_row, node->param_col);
    if (node->parent) {
        node->gui_instance->navigateToNode(node->parent);
    }
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] param_save_button_event_cb");
#endif
}

// LVGL screen creation functions

void GUIComponent::createHomeScreen(MenuNode* node) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] createHomeScreen - node: %s", node ? node->name.c_str() : "null");
#endif
    if (!node) {
        ESP_LOGE("GUI", "createHomeScreen: null node");
        return;
    }
    
    ESP_LOGI(TAG, "Creating home screen - Free heap: %lu bytes", esp_get_free_heap_size());
    
    // Delete old screen if it exists
    if (node->screen) {
        lv_obj_del(node->screen);
    }
    
    node->screen = lv_obj_create(NULL);
    if (!node->screen) {
        ESP_LOGE(TAG, "Failed to create screen for home - Out of memory?");
        ESP_LOGE(TAG, "Free heap: %lu bytes, Minimum ever: %lu bytes", 
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
        return;
    }
    lv_obj_clear_flag(node->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(node->screen, lv_color_black(), 0);
    
    // Title
    lv_obj_t* title = lv_label_create(node->screen);
    ESP_LOGI(TAG, "Created title label - Free heap: %lu bytes", esp_get_free_heap_size());
    lv_label_set_text(title, "Smart Home");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Create component buttons
    int y_pos = 60;
    ESP_LOGI(TAG, "Creating %zu component buttons - Free heap: %lu bytes", 
             node->children.size(), esp_get_free_heap_size());
    for (MenuNode* child : node->children) {
        ESP_LOGI(TAG, "Creating button for: %s on screen %p", 
                 child ? child->name.c_str() : "null", node->screen);
        lv_obj_t* comp_btn = lv_btn_create(node->screen);
        ESP_LOGI(TAG, "Button %p created - Free heap: %lu bytes", comp_btn, esp_get_free_heap_size());
        lv_obj_set_size(comp_btn, 280, 50);
        lv_obj_set_pos(comp_btn, 20, y_pos);
        
        lv_obj_t* comp_label = lv_label_create(comp_btn);
        lv_label_set_text(comp_label, child->name.c_str());
        lv_obj_center(comp_label);
        
        lv_obj_add_event_cb(comp_btn, menu_button_event_cb, LV_EVENT_CLICKED, child);
        
        y_pos += 60;
    }
    
    ESP_LOGI(TAG, "Home screen created with %zu component buttons", node->children.size());
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] createHomeScreen");
#endif
}

void GUIComponent::createComponentScreen(MenuNode* node, Component* component) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] createComponentScreen - node: %s, component: %s",
             node ? node->name.c_str() : "null",
             component ? component->getName().c_str() : "null");
#endif
    if (!node) {
        ESP_LOGE(TAG, "createComponentScreen: null node");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createComponentScreen - null node");
#endif
        return;
    }
    if (!component) {
        ESP_LOGE(TAG, "createComponentScreen: null component");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createComponentScreen - null component");
#endif
        return;
    }
    
    node->screen = lv_obj_create(NULL);
    if (!node->screen) {
        ESP_LOGE(TAG, "Failed to create screen for component %s", component->getName().c_str());
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createComponentScreen - screen creation failed");
#endif
        return;
    }
    lv_obj_set_style_bg_color(node->screen, lv_color_black(), 0);
    
    // Title
    lv_obj_t* title = lv_label_create(node->screen);
    lv_label_set_text(title, component->getName().c_str());
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Back button
    lv_obj_t* back_btn = lv_btn_create(node->screen);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_set_pos(back_btn, 10, 10);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_button_event_cb, LV_EVENT_CLICKED, node);
    
    // Parameters button (only if params child exists)
    if (node->children.size() > 0 && node->children[0]) {
        lv_obj_t* params_btn = lv_btn_create(node->screen);
        lv_obj_set_size(params_btn, 280, 50);
        lv_obj_set_pos(params_btn, 20, 80);
        lv_obj_t* params_label = lv_label_create(params_btn);
        lv_label_set_text(params_label, "Parameters");
        lv_obj_center(params_label);
        lv_obj_add_event_cb(params_btn, menu_button_event_cb, LV_EVENT_CLICKED, node->children[0]);
    }
    
    // Actions button (only if actions child exists)
    if (node->children.size() > 1 && node->children[1]) {
        lv_obj_t* actions_btn = lv_btn_create(node->screen);
        lv_obj_set_size(actions_btn, 280, 50);
        lv_obj_set_pos(actions_btn, 20, 150);
        lv_obj_t* actions_label = lv_label_create(actions_btn);
        lv_label_set_text(actions_label, "Actions");
        lv_obj_center(actions_label);
        lv_obj_add_event_cb(actions_btn, menu_button_event_cb, LV_EVENT_CLICKED, node->children[1]);
    }
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] createComponentScreen");
#endif
}

void GUIComponent::createParametersScreen(MenuNode* node, Component* component) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] createParametersScreen - node: %s, component: %s",
             node ? node->name.c_str() : "null",
             component ? component->getName().c_str() : "null");
#endif
    if (!node) {
        ESP_LOGE(TAG, "createParametersScreen: null node");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createParametersScreen - null node");
#endif
        return;
    }
    if (!component) {
        ESP_LOGE(TAG, "createParametersScreen: null component");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createParametersScreen - null component");
#endif
        return;
    }
    
    node->screen = lv_obj_create(NULL);
    if (!node->screen) {
        ESP_LOGE(TAG, "Failed to create parameters screen for %s", component->getName().c_str());
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createParametersScreen - screen creation failed");
#endif
        return;
    }
    lv_obj_set_style_bg_color(node->screen, lv_color_black(), 0);
    
    // Title
    lv_obj_t* title = lv_label_create(node->screen);
    std::string title_text = component->getName() + " - Parameters";
    lv_label_set_text(title, title_text.c_str());
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Back button
    lv_obj_t* back_btn = lv_btn_create(node->screen);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_set_pos(back_btn, 10, 10);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_button_event_cb, LV_EVENT_CLICKED, node);
    
    // Create scrollable list of parameter buttons
    lv_obj_t* list = lv_obj_create(node->screen);
    lv_obj_set_size(list, 300, 160);
    lv_obj_set_pos(list, 10, 60);
    lv_obj_set_style_bg_color(list, lv_color_make(20, 20, 20), 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_style_pad_row(list, 5, 0);  // Add spacing between rows    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Add buttons for each parameter from child nodes
    if (node->children.empty()) {
        lv_obj_t* empty_label = lv_label_create(list);
        lv_label_set_text(empty_label, "No parameters");
        lv_obj_set_style_text_color(empty_label, lv_color_make(128, 128, 128), 0);
    } else {
        for (MenuNode* param_child : node->children) {
            lv_obj_t* param_btn = lv_btn_create(list);
            lv_obj_set_size(param_btn, 280, 40);
            lv_obj_set_style_bg_color(param_btn, lv_color_make(50, 50, 150), 0);
            
            lv_obj_t* param_label = lv_label_create(param_btn);
            lv_label_set_text(param_label, param_child->name.c_str());
            lv_obj_center(param_label);
            
            lv_obj_add_event_cb(param_btn, menu_button_event_cb, LV_EVENT_CLICKED, param_child);
        }
    }
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] createParametersScreen - %zu parameter buttons", node->children.size());
#endif
}

void GUIComponent::createParameterDetailScreen(MenuNode* node, Component* component,
                                               const std::string& param_name, const std::string& param_type) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] createParameterDetailScreen - param: %s, type: %s",
             param_name.c_str(), param_type.c_str());
#endif
    if (!node || !component) {
        ESP_LOGE(TAG, "createParameterDetailScreen: null node or component");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createParameterDetailScreen - null input");
#endif
        return;
    }
    
    node->screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(node->screen, lv_color_black(), 0);
    
    // Title
    lv_obj_t* title = lv_label_create(node->screen);
    lv_label_set_text(title, param_name.c_str());
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Back button
    lv_obj_t* back_btn = lv_btn_create(node->screen);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_set_pos(back_btn, 10, 10);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_button_event_cb, LV_EVENT_CLICKED, node);
    
    // Create scrollable list
    lv_obj_t* list = lv_obj_create(node->screen);
    lv_obj_set_size(list, 300, 160);
    lv_obj_set_pos(list, 10, 60);
    lv_obj_set_style_bg_color(list, lv_color_make(20, 20, 20), 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    
    // Get parameter and display all [row, col] values as buttons (for numeric) or labels (for string)
    if (param_type == "int") {
        IntParameter* param = component->getIntParam(param_name);
        if (param) {
            for (size_t row = 0; row < param->getRows(); row++) {
                for (size_t col = 0; col < param->getCols(); col++) {
                    // Create button for editing
                    lv_obj_t* value_btn = lv_btn_create(list);
                    lv_obj_set_width(value_btn, 280);
                    lv_obj_set_height(value_btn, 40);
                    lv_obj_set_style_pad_all(value_btn, 5, 0);
                    
                    lv_obj_t* value_label = lv_label_create(value_btn);
                    std::string text = "[" + std::to_string(row) + "," + std::to_string(col) + "]: " + 
                                      std::to_string(param->getValue(row, col));
                    lv_label_set_text(value_label, text.c_str());
                    lv_obj_center(value_label);
                    
                    // Create child node for editing this value
                    MenuNode* edit_node = new MenuNode(param_name + "_edit", node, this);
                    edit_node->associated_component = component;
                    edit_node->param_name = param_name;
                    edit_node->param_type = param_type;
                    edit_node->param_row = row;
                    edit_node->param_col = col;
                    all_nodes.push_back(edit_node);
                    
                    lv_obj_add_event_cb(value_btn, param_edit_button_event_cb, LV_EVENT_CLICKED, edit_node);
                }
            }
        }
    } else if (param_type == "float") {
        FloatParameter* param = component->getFloatParam(param_name);
        if (param) {
            for (size_t row = 0; row < param->getRows(); row++) {
                for (size_t col = 0; col < param->getCols(); col++) {
                    // Create button for editing
                    lv_obj_t* value_btn = lv_btn_create(list);
                    lv_obj_set_width(value_btn, 280);
                    lv_obj_set_height(value_btn, 40);
                    lv_obj_set_style_pad_all(value_btn, 5, 0);
                    
                    lv_obj_t* value_label = lv_label_create(value_btn);
                    std::string text = "[" + std::to_string(row) + "," + std::to_string(col) + "]: " + 
                                      std::to_string(param->getValue(row, col));
                    lv_label_set_text(value_label, text.c_str());
                    lv_obj_center(value_label);
                    
                    // Create child node for editing this value
                    MenuNode* edit_node = new MenuNode(param_name + "_edit", node, this);
                    edit_node->associated_component = component;
                    edit_node->param_name = param_name;
                    edit_node->param_type = param_type;
                    edit_node->param_row = row;
                    edit_node->param_col = col;
                    all_nodes.push_back(edit_node);
                    
                    lv_obj_add_event_cb(value_btn, param_edit_button_event_cb, LV_EVENT_CLICKED, edit_node);
                }
            }
        }
    } else if (param_type == "bool") {
        BoolParameter* param = component->getBoolParam(param_name);
        if (param) {
            for (size_t row = 0; row < param->getRows(); row++) {
                for (size_t col = 0; col < param->getCols(); col++) {
                    // Create button for toggling
                    lv_obj_t* value_btn = lv_btn_create(list);
                    lv_obj_set_width(value_btn, 280);
                    lv_obj_set_height(value_btn, 40);
                    lv_obj_set_style_pad_all(value_btn, 5, 0);
                    
                    lv_obj_t* value_label = lv_label_create(value_btn);
                    std::string text = "[" + std::to_string(row) + "," + std::to_string(col) + "]: " + 
                                      (param->getValue(row, col) ? "true" : "false");
                    lv_label_set_text(value_label, text.c_str());
                    lv_obj_center(value_label);
                    
                    // Create child node for editing this value
                    MenuNode* edit_node = new MenuNode(param_name + "_edit", node, this);
                    edit_node->associated_component = component;
                    edit_node->param_name = param_name;
                    edit_node->param_type = param_type;
                    edit_node->param_row = row;
                    edit_node->param_col = col;
                    all_nodes.push_back(edit_node);
                    
                    lv_obj_add_event_cb(value_btn, param_edit_button_event_cb, LV_EVENT_CLICKED, edit_node);
                }
            }
        }
    } else if (param_type == "string") {
        StringParameter* param = component->getStringParam(param_name);
        if (param) {
            for (size_t row = 0; row < param->getRows(); row++) {
                for (size_t col = 0; col < param->getCols(); col++) {
                    lv_obj_t* value_label = lv_label_create(list);
                    const std::string& val = param->getValue(row, col);
                    std::string text = "[" + std::to_string(row) + "," + std::to_string(col) + "]: " + val;
                    lv_label_set_text(value_label, text.c_str());
                    lv_obj_set_style_text_color(value_label, lv_color_white(), 0);
                    lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
                    lv_obj_set_width(value_label, 280);
                }
            }
        }
    }
    
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] createParameterDetailScreen");
#endif
}

void GUIComponent::createParameterEditScreen(MenuNode* node, Component* component) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] createParameterEditScreen - param: %s[%zu,%zu] type: %s",
             node ? node->param_name.c_str() : "null", node ? node->param_row : 0,
             node ? node->param_col : 0, node ? node->param_type.c_str() : "null");
#endif
    if (!node || !component) {
        ESP_LOGE(TAG, "createParameterEditScreen: null input");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createParameterEditScreen - null input");
#endif
        return;
    }
    
    node->screen = lv_obj_create(NULL);
    if (!node->screen) {
        ESP_LOGE(TAG, "Failed to create parameter edit screen");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createParameterEditScreen - screen creation failed");
#endif
        return;
    }
    
    lv_obj_set_style_bg_color(node->screen, lv_color_black(), 0);
    lv_obj_clear_flag(node->screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title
    lv_obj_t* title = lv_label_create(node->screen);
    std::string title_text = "Edit " + node->param_name + "[" + 
                             std::to_string(node->param_row) + "," + 
                             std::to_string(node->param_col) + "]";
    lv_label_set_text(title, title_text.c_str());
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Back button
    lv_obj_t* back_btn = lv_btn_create(node->screen);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_set_pos(back_btn, 10, 10);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_button_event_cb, LV_EVENT_CLICKED, node);
    
    // Create edit controls based on parameter type
    if (node->param_type == "int") {
        IntParameter* param = component->getIntParam(node->param_name);
        if (param) {
            int current_value = param->getValue(node->param_row, node->param_col);
            int min_val = param->getMin();
            int max_val = param->getMax();
            
            // Value label
            lv_obj_t* value_label = lv_label_create(node->screen);
            lv_label_set_text_fmt(value_label, "Value: %d", current_value);
            lv_obj_set_style_text_color(value_label, lv_color_white(), 0);
            lv_obj_align(value_label, LV_ALIGN_CENTER, 0, -40);
            
            // Slider
            lv_obj_t* slider = lv_slider_create(node->screen);
            lv_obj_set_width(slider, 260);
            lv_slider_set_range(slider, min_val, max_val);
            lv_slider_set_value(slider, current_value, LV_ANIM_OFF);
            lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
            
            // Store label pointer in slider's user_data for callback access
            lv_obj_set_user_data(slider, value_label);
            
            lv_obj_add_event_cb(slider, [](lv_event_t* e) {
                MenuNode* node = (MenuNode*)lv_event_get_user_data(e);
                lv_obj_t* slider = lv_event_get_target(e);
                int value = lv_slider_get_value(slider);
                
                // Update the label in real-time
                lv_obj_t* label = (lv_obj_t*)lv_obj_get_user_data(slider);
                if (label) {
                    lv_label_set_text_fmt(label, "Value: %d", value);
                }
                
                IntParameter* param = node->associated_component->getIntParam(node->param_name);
                if (param) {
                    param->setValue(node->param_row, node->param_col, value);
                    ESP_LOGI(TAG, "Updated %s[%zu,%zu] = %d", 
                             node->param_name.c_str(), node->param_row, node->param_col, value);
                }
            }, LV_EVENT_VALUE_CHANGED, node);
        }
    } else if (node->param_type == "float") {
        FloatParameter* param = component->getFloatParam(node->param_name);
        if (param) {
            float current_value = param->getValue(node->param_row, node->param_col);
            float min_val = param->getMin();
            float max_val = param->getMax();
            
            // Value label
            lv_obj_t* value_label = lv_label_create(node->screen);
            lv_label_set_text_fmt(value_label, "Value: %.2f", current_value);
            lv_obj_set_style_text_color(value_label, lv_color_white(), 0);
            lv_obj_align(value_label, LV_ALIGN_CENTER, 0, -40);
            
            // Slider
            lv_obj_t* slider = lv_slider_create(node->screen);
            lv_obj_set_width(slider, 260);
            lv_slider_set_range(slider, (int)(min_val * 100), (int)(max_val * 100));
            lv_slider_set_value(slider, (int)(current_value * 100), LV_ANIM_OFF);
            lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
            
            // Store label pointer in slider's user_data for callback access
            lv_obj_set_user_data(slider, value_label);
            
            lv_obj_add_event_cb(slider, [](lv_event_t* e) {
                MenuNode* node = (MenuNode*)lv_event_get_user_data(e);
                lv_obj_t* slider = lv_event_get_target(e);
                float value = lv_slider_get_value(slider) / 100.0f;
                
                // Update the label in real-time
                lv_obj_t* label = (lv_obj_t*)lv_obj_get_user_data(slider);
                if (label) {
                    lv_label_set_text_fmt(label, "Value: %.2f", value);
                }
                
                FloatParameter* param = node->associated_component->getFloatParam(node->param_name);
                if (param) {
                    param->setValue(node->param_row, node->param_col, value);
                    ESP_LOGI(TAG, "Updated %s[%zu,%zu] = %.2f", 
                             node->param_name.c_str(), node->param_row, node->param_col, value);
                }
            }, LV_EVENT_VALUE_CHANGED, node);
        }
    } else if (node->param_type == "bool") {
        BoolParameter* param = component->getBoolParam(node->param_name);
        if (param) {
            bool current_value = param->getValue(node->param_row, node->param_col);
            
            // Toggle switch
            lv_obj_t* sw = lv_switch_create(node->screen);
            if (current_value) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            }
            lv_obj_align(sw, LV_ALIGN_CENTER, 0, 0);
            
            // Label
            lv_obj_t* value_label = lv_label_create(node->screen);
            lv_label_set_text(value_label, current_value ? "true" : "false");
            lv_obj_set_style_text_color(value_label, lv_color_white(), 0);
            lv_obj_align(value_label, LV_ALIGN_CENTER, 0, -40);
            
            // Store label pointer in switch's user_data for callback access
            lv_obj_set_user_data(sw, value_label);
            
            lv_obj_add_event_cb(sw, [](lv_event_t* e) {
                MenuNode* node = (MenuNode*)lv_event_get_user_data(e);
                lv_obj_t* sw = lv_event_get_target(e);
                bool value = lv_obj_has_state(sw, LV_STATE_CHECKED);
                
                // Update the label in real-time
                lv_obj_t* label = (lv_obj_t*)lv_obj_get_user_data(sw);
                if (label) {
                    lv_label_set_text(label, value ? "true" : "false");
                }
                
                BoolParameter* param = node->associated_component->getBoolParam(node->param_name);
                if (param) {
                    param->setValue(node->param_row, node->param_col, value);
                    ESP_LOGI(TAG, "Updated %s[%zu,%zu] = %s", 
                             node->param_name.c_str(), node->param_row, node->param_col,
                             value ? "true" : "false");
                }
            }, LV_EVENT_VALUE_CHANGED, node);
        }
    }
    
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] createParameterEditScreen");
#endif
}

void GUIComponent::createActionsScreen(MenuNode* node, Component* component) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] createActionsScreen - node: %s, component: %s",
             node ? node->name.c_str() : "null",
             component ? component->getName().c_str() : "null");
#endif
    if (!node) {
        ESP_LOGE(TAG, "createActionsScreen: null node");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createActionsScreen - null node");
#endif
        return;
    }
    if (!component) {
        ESP_LOGE(TAG, "createActionsScreen: null component");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createActionsScreen - null component");
#endif
        return;
    }
    
    node->screen = lv_obj_create(NULL);
    if (!node->screen) {
        ESP_LOGE(TAG, "Failed to create actions screen for %s", component->getName().c_str());
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] createActionsScreen - screen creation failed");
#endif
        return;
    }
    lv_obj_set_style_bg_color(node->screen, lv_color_black(), 0);
    
    // Title
    lv_obj_t* title = lv_label_create(node->screen);
    std::string title_text = component->getName() + " - Actions";
    lv_label_set_text(title, title_text.c_str());
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);
    
    // Back button
    lv_obj_t* back_btn = lv_btn_create(node->screen);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_set_pos(back_btn, 10, 10);
    lv_obj_t* back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< Back");
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_button_event_cb, LV_EVENT_CLICKED, node);
    
    // Create scrollable list of action buttons
    lv_obj_t* list = lv_obj_create(node->screen);
    lv_obj_set_size(list, 300, 160);
    lv_obj_set_pos(list, 10, 60);
    lv_obj_set_style_bg_color(list, lv_color_make(20, 20, 20), 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    const auto& actions = component->getActions();
    
    if (actions.empty()) {
        lv_obj_t* empty_label = lv_label_create(list);
        lv_label_set_text(empty_label, "No actions available");
        lv_obj_set_style_text_color(empty_label, lv_color_make(128, 128, 128), 0);
    } else {
        for (const auto& action : actions) {
            // Create a child node for this action (for event callback context)
            MenuNode* action_node = new MenuNode(action.name, node, this);
            action_node->associated_component = component;
            action_node->action_name = action.name;
            all_nodes.push_back(action_node);
            
            lv_obj_t* action_btn = lv_btn_create(list);
            lv_obj_set_size(action_btn, 280, 40);
            lv_obj_set_style_bg_color(action_btn, lv_color_make(0, 100, 200), 0);
            
            lv_obj_t* action_label = lv_label_create(action_btn);
            lv_label_set_text(action_label, action.name.c_str());
            lv_obj_center(action_label);
            
            lv_obj_add_event_cb(action_btn, action_button_event_cb, LV_EVENT_CLICKED, action_node);
        }
    }
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] createActionsScreen - %zu actions", actions.size());
#endif
}

// ============================================================================
// End GUIComponent Implementation
// ============================================================================

// Custom animation setter for opacity
static void set_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

// Create visual touch feedback at the given position
static void create_touch_feedback(int16_t x, int16_t y) {
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
    lv_anim_set_exec_cb(&anim, set_opa);
    lv_anim_start(&anim);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] create_touch_feedback");
#endif
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
    static uint16_t touchpad_x[1] = {0};
    static uint16_t touchpad_y[1] = {0};
    static uint8_t touchpad_cnt = 0;
    static bool was_pressed = false;
    
    // Poll touch controller
    esp_lcd_touch_read_data(touch_handle_ref);
    bool touched = esp_lcd_touch_get_coordinates(touch_handle_ref, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);
    
    if (touched && touchpad_cnt > 0) {
        // Update the last time a touch was detected (only when actually touched!)
        last_interaction_tick = xTaskGetTickCount();
        
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = touchpad_x[0];
        data->point.y = touchpad_y[0];
        
        // Create visual feedback on new press
        if (!was_pressed) {
            create_touch_feedback(touchpad_x[0], touchpad_y[0]);
        }
        was_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        was_pressed = false;
    }
}

// LVGL flush callback
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;
    
    esp_lcd_panel_draw_bitmap(panel_handle_ref, x1, y1, x2, y2, (uint16_t *)color_map);
    lv_disp_flush_ready(drv);
}

// LVGL timer task
static void lvgl_timer_task(void *arg) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] lvgl_timer_task");
#endif
    ESP_LOGI(TAG, "LVGL timer task started");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));

        // Check if UI creation has been requested
        if (g_create_ui_requested && g_gui_component) {
            ESP_LOGI(TAG, "Creating UI from LVGL task...");
            g_gui_component->buildMenuTree();
            g_create_ui_requested = false;
            ESP_LOGI(TAG, "UI creation complete");
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

void gui_init(GUIComponent* gui_component) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] gui_init");
#endif
    // Initialize hardware components
    panel_handle_ref = lcd_init();
    touch_handle_ref = touch_init();
    
    // Initialize LVGL
    lv_init();
    
    // Allocate LVGL draw buffers
    size_t buffer_size = LCD_H_RES * 50; // 50 lines buffer
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_disp_draw_buf_init(&lvgl_disp_buf, buf1, buf2, buffer_size);
    
    // Initialize gaussian lookup table
    init_gaussian_lookup();
    
    // Initialize LVGL display driver
    lv_disp_drv_init(&lvgl_disp_drv);
    lvgl_disp_drv.hor_res = LCD_H_RES;
    lvgl_disp_drv.ver_res = LCD_V_RES;
    lvgl_disp_drv.flush_cb = lvgl_flush_cb;
    lvgl_disp_drv.draw_buf = &lvgl_disp_buf;
    lv_disp_drv_register(&lvgl_disp_drv);
    
    // Initialize LVGL input device driver (touch)
    lv_indev_drv_init(&lvgl_indev_drv);
    lvgl_indev_drv.type = LV_INDEV_TYPE_POINTER;
    lvgl_indev_drv.read_cb = GUIComponent::lvgl_touch_read_cb;
    lvgl_indev_drv.user_data = gui_component; // Store GUIComponent instance for callback
    lv_indev_drv_register(&lvgl_indev_drv);
    
    ESP_LOGI(TAG, "LVGL initialized");
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    
    // Create LVGL timer task with larger stack
    xTaskCreate(lvgl_timer_task, "lvgl_timer", 8192, NULL, 5, NULL);
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] gui_init");
#endif
}

void gui_create_ui(GUIComponent* gui_component) {
#ifdef DEBUG
    ESP_LOGI(TAG, "[ENTER] gui_create_ui - gui_component: %p", gui_component);
#endif
    if (!gui_component) {
        ESP_LOGE(TAG, "gui_create_ui called with null GUIComponent");
#ifdef DEBUG
        ESP_LOGI(TAG, "[EXIT] gui_create_ui - null component");
#endif
        return;
    }
    
    // Store the component and signal LVGL task to create UI
    g_gui_component = gui_component;
    g_create_ui_requested = true;
    
    ESP_LOGI(TAG, "UI creation requested (will be created by LVGL task)");
#ifdef DEBUG
    ESP_LOGI(TAG, "[EXIT] gui_create_ui");
#endif
}
