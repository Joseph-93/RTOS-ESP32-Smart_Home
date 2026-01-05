#include "gui.h"
#include "lcd/lcd.h"
#include "touch/touch.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "GUI";

// Display dimensions
#define LCD_H_RES    320
#define LCD_V_RES    240

// LVGL driver structures
static lv_disp_drv_t lvgl_disp_drv;
static lv_disp_draw_buf_t lvgl_disp_buf;

static esp_lcd_panel_handle_t panel_handle_ref = NULL;
static esp_lcd_touch_handle_t touch_handle_ref = NULL;

// Keep reference to a square to test dynamic updates
static lv_obj_t *test_square = NULL;

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
    if (gaussian_initialized) return;
    
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
}

// Custom animation setter for opacity
static void set_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

// Create visual touch feedback at the given position
static void create_touch_feedback(int16_t x, int16_t y) {
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

// LVGL timer task - also polls touch directly
static void lvgl_timer_task(void *arg) {
    static bool was_pressed = false;
    uint16_t touchpad_x[1] = {0};
    uint16_t touchpad_y[1] = {0};
    uint8_t touchpad_cnt = 0;
    
    ESP_LOGI(TAG, "LVGL timer task started");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));

        // Increment LVGL tick by 10ms
        lv_tick_inc(10);

        // Call LVGL timer handler to process rendering and animations
        lv_timer_handler();
        
        // Manually delete old feedback objects after 1100ms
        uint32_t now = lv_tick_get();
        static uint32_t log_counter = 0;
        if (feedback_count > 0 && log_counter++ % 100 == 0) {
            ESP_LOGI(TAG, "Deletion check: now=%lu, feedback_count=%d", now, feedback_count);
        }
        for (int i = 0; i < feedback_count; i++) {
            uint32_t age = now - feedback_list[i].created_time;
            if (feedback_list[i].obj && age > 225) {  // 225ms (after 125ms animation)
                ESP_LOGI(TAG, "Deleting feedback %p (age=%lu ms)", feedback_list[i].obj, age);
                lv_obj_del(feedback_list[i].obj);
                // Shift remaining items down
                for (int j = i; j < feedback_count - 1; j++) {
                    feedback_list[j] = feedback_list[j + 1];
                }
                feedback_count--;
                ESP_LOGI(TAG, "Feedback deleted, count now=%d", feedback_count);
                i--; // Recheck this index
            }
        }
        
        // Poll touch controller directly over SPI
        esp_lcd_touch_read_data(touch_handle_ref);
        bool touched = esp_lcd_touch_get_coordinates(touch_handle_ref, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);
        
        if (touched && touchpad_cnt > 0) {
            ESP_LOGI(TAG, "Touch detected at (%d, %d)", touchpad_x[0], touchpad_y[0]);
            // Create touch feedback on new press only
            if (!was_pressed) {
                create_touch_feedback(touchpad_x[0], touchpad_y[0]);
                ESP_LOGI(TAG, "Touch at (%d, %d)", touchpad_x[0], touchpad_y[0]);
            }
            was_pressed = true;
        } else {
            was_pressed = false;
        }
    }
}

void gui_init(void) {
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
    
    ESP_LOGI(TAG, "LVGL initialized");
    
    // Create LVGL timer task
    xTaskCreate(lvgl_timer_task, "lvgl_timer", 4096, NULL, 5, NULL);
}

void gui_create_ui(void) {
    // Set background to black
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    
    // Create RED square
    lv_obj_t *red_square = lv_obj_create(lv_scr_act());
    lv_obj_set_size(red_square, 80, 80);
    lv_obj_set_pos(red_square, 20, 80);
    lv_obj_set_style_bg_color(red_square, lv_color_make(255, 0, 0), 0);
    lv_obj_set_style_border_width(red_square, 0, 0);
    
    // Create GREEN square - save reference for testing
    test_square = lv_obj_create(lv_scr_act());
    lv_obj_set_size(test_square, 80, 80);
    lv_obj_set_pos(test_square, 120, 80);
    lv_obj_set_style_bg_color(test_square, lv_color_make(0, 255, 0), 0);
    lv_obj_set_style_border_width(test_square, 0, 0);
    
    // Create BLUE square
    lv_obj_t *blue_square = lv_obj_create(lv_scr_act());
    lv_obj_set_size(blue_square, 80, 80);
    lv_obj_set_pos(blue_square, 220, 80);
    lv_obj_set_style_bg_color(blue_square, lv_color_make(0, 0, 255), 0);
    lv_obj_set_style_border_width(blue_square, 0, 0);
    
    ESP_LOGI(TAG, "UI created");
}
