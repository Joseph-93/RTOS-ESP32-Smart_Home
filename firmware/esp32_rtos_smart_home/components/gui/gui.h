#pragma once

#include "../component.h"
#include "lvgl.h"
#include <vector>
#include <map>

#ifdef __cplusplus

// Forward declarations
struct MenuNode;

// GUIComponent - manages the user interface and component menu system
class GUIComponent : public Component {
public:
    GUIComponent();
    ~GUIComponent() override;
    
    void initialize() override;
    
    // Component registry - GUI needs to know about all components
    void registerComponent(Component* component);
    
    // Build the complete menu tree (call after all components registered)
    void buildMenuTree();
    
    // Navigate to a specific menu node (creates screen if not yet created)
    void navigateToNode(MenuNode* node);
    
    // Get current active node
    MenuNode* getCurrentNode() const { return current_node; }
    
    // Ensure a node's screen is created (lazy creation)
    void ensureScreenCreated(MenuNode* node);

    // Task function for lower-priority gui operations
    static void guiStatusTaskWrapper(void* pvParameters);
    void guiStatusTask();

    // LVGL touch callback (static trampoline)
    static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);

private:
    // Menu tree root
    MenuNode* root_node;
    MenuNode* current_node;
    
    // All registered components
    std::vector<Component*> registered_components;

    IntParameter* light_sensor_current_light_level; // Pointer to light sensor param for brightness adjustment
    TaskHandle_t gui_status_task_handle = nullptr;
    TimerHandle_t gui_status_timer_handle = nullptr;
    TickType_t last_interaction_tick = 0;
    
    // All menu nodes (for memory management)
    std::vector<MenuNode*> all_nodes;
    
    // Create menu nodes for a component
    MenuNode* createComponentNode(Component* component, MenuNode* parent);
    MenuNode* createParametersNode(Component* component, MenuNode* parent);
    MenuNode* createActionsNode(Component* component, MenuNode* parent);
    
    // LVGL screen creation
    void createHomeScreen(MenuNode* node);
    void createComponentScreen(MenuNode* node, Component* component);
    void createParametersScreen(MenuNode* node, Component* component);
    void createParameterDetailScreen(MenuNode* node, Component* component, 
                                     const std::string& param_name, const std::string& param_type);
    void createParameterEditScreen(MenuNode* node, Component* component);
    void createActionsScreen(MenuNode* node, Component* component);
    
    // Event handlers
    static void menu_button_event_cb(lv_event_t* e);
    static void back_button_event_cb(lv_event_t* e);
    static void action_button_event_cb(lv_event_t* e);
    static void param_edit_button_event_cb(lv_event_t* e);
    static void param_save_button_event_cb(lv_event_t* e);
    
    // Touch reading implementation (non-static)
    void handleTouchRead(lv_indev_data_t *data);
};

// MenuNode - represents a node in the menu tree
struct MenuNode {
    std::string name;
    MenuNode* parent;
    std::vector<MenuNode*> children;
    lv_obj_t* screen;              // Pre-created LVGL screen
    GUIComponent* gui_instance;    // Back-reference to GUI
    Component* associated_component; // Component this node represents (if any)
    std::string action_name;       // Action name if this is an action button
    std::string param_name;        // Parameter name if this is a parameter detail screen
    std::string param_type;        // Parameter type: "int", "float", "bool", "string"
    size_t param_row;              // Row index for parameter edit screen
    size_t param_col;              // Col index for parameter edit screen
    
    MenuNode(const std::string& name, MenuNode* parent, GUIComponent* gui)
        : name(name), parent(parent), screen(nullptr), gui_instance(gui),
          associated_component(nullptr), action_name(""), param_name(""), param_type(""),
          param_row(0), param_col(0) {}
    
    ~MenuNode() {
        // LVGL screens are deleted automatically when parent is deleted
        for (auto* child : children) {
            delete child;
        }
    }
};

extern "C" {
#endif

/**
 * @brief Initialize GUI subsystem (lcd, touch, and LVGL)
 * @param gui_component The GUIComponent instance for touch callbacks
 */
void gui_init(GUIComponent* gui_component);

/**
 * @brief Create the main UI - now managed by GUIComponent
 * @param gui_component The GUIComponent instance to use
 */
void gui_create_ui(GUIComponent* gui_component);

#ifdef __cplusplus
}
#endif
