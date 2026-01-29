#pragma once

#include "component.h"
#include "component_graph.h"
#include "lvgl.h"
#include <vector>
#include <map>

#define NUM_BUTTONS 6  // Number of buttons in the grid (3x2)

#ifdef __cplusplus

// GUIComponent - manages simple button grid interface
class GUIComponent : public Component {
public:
    GUIComponent();
    ~GUIComponent() override;
    
    void setUpDependencies(ComponentGraph* graph) override;
    void onInitialize() override;

    // Task function for lower-priority gui operations
    static void guiStatusTaskWrapper(void* pvParameters);
    void guiStatusTask();

    // LVGL touch callback (static trampoline)
    static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);
    
    // Create pending notification overlay (called from LVGL task only)
    void createPendingNotification();

    // Create simple button grid (3x2)
    void createSimpleButtonGrid();

    static constexpr const char* TAG = "GUI";

    // Notification task for reading from ComponentGraph's notification queue
    TaskHandle_t notification_task_handle;
    static void notificationTaskWrapper(void* pvParameters);
    void notificationTask();

    // Pending notification (set by notification task, consumed by LVGL task)
    volatile bool pending_notification = false;
    ComponentGraph::NotificationQueueItem pending_notification_item;
    
    // Pending button label update (set by parameter callback, consumed by LVGL task)
    volatile bool button_label_update_pending = false;

private:
    TaskHandle_t gui_status_task_handle = nullptr;
    TimerHandle_t gui_status_timer_handle = nullptr;
    TickType_t last_interaction_tick = 0;
    
    // Notification overlay tracking
    lv_obj_t* notification_overlay = nullptr;
    int current_notification_priority = -1;  // -1 = no notification
    TickType_t notification_expire_tick = 0;
    
    // Simple button grid screen
    lv_obj_t* main_screen = nullptr;
    
    // Button label objects for dynamic updating (6 buttons)
    lv_obj_t* button_labels[NUM_BUTTONS] = {nullptr};
    
    // Button event handler
    static void simple_button_event_cb(lv_event_t* e);
    
    // Static helpers for LVGL callbacks and utilities
    static void init_gaussian_lookup();
    static void set_opa(void *obj, int32_t v);
    static void create_touch_feedback(int16_t x, int16_t y);
    static void IRAM_ATTR touch_irq_handler(void* arg);
    static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
    static void lvgl_timer_task(void *arg);
    
    // Touch reading implementation (non-static)
    void handleTouchRead(lv_indev_data_t *data);
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
