#include "gui.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

// LVGL flush callback
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;
    
    esp_lcd_panel_draw_bitmap(panel_handle_ref, x1, y1, x2, y2, (uint16_t *)color_map);
    lv_disp_flush_ready(drv);
}

// LVGL touch input callback
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    if (esp_lcd_touch_read_data(touch_handle_ref) == ESP_OK) {
        esp_lcd_touch_point_data_t touch_data;
        uint8_t touch_cnt = 0;
        if (esp_lcd_touch_get_data(touch_handle_ref, &touch_data, &touch_cnt, 1) == ESP_OK && touch_cnt > 0) {
            data->point.x = touch_data.x;
            data->point.y = touch_data.y;
            data->state = LV_INDEV_STATE_PRESSED;
            ESP_LOGI(TAG, "Touch at (%d, %d)", touch_data.x, touch_data.y);
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// LVGL timer task
static void lvgl_timer_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        lv_timer_handler();
    }
}

void gui_init(esp_lcd_panel_handle_t panel_handle, esp_lcd_touch_handle_t touch_handle) {
    panel_handle_ref = panel_handle;
    touch_handle_ref = touch_handle;
    
    // Initialize LVGL
    lv_init();
    
    // Allocate LVGL draw buffers
    size_t buffer_size = LCD_H_RES * 50; // 50 lines buffer
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_disp_draw_buf_init(&lvgl_disp_buf, buf1, buf2, buffer_size);
    
    // Initialize LVGL display driver
    lv_disp_drv_init(&lvgl_disp_drv);
    lvgl_disp_drv.hor_res = LCD_H_RES;
    lvgl_disp_drv.ver_res = LCD_V_RES;
    lvgl_disp_drv.flush_cb = lvgl_flush_cb;
    lvgl_disp_drv.draw_buf = &lvgl_disp_buf;
    lv_disp_drv_register(&lvgl_disp_drv);
    
    // Initialize LVGL touch input driver
    lv_indev_drv_init(&lvgl_indev_drv);
    lvgl_indev_drv.type = LV_INDEV_TYPE_POINTER;
    lvgl_indev_drv.read_cb = lvgl_touch_cb;
    lv_indev_drv_register(&lvgl_indev_drv);
    
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
    
    // Create GREEN square
    lv_obj_t *green_square = lv_obj_create(lv_scr_act());
    lv_obj_set_size(green_square, 80, 80);
    lv_obj_set_pos(green_square, 120, 80);
    lv_obj_set_style_bg_color(green_square, lv_color_make(0, 255, 0), 0);
    lv_obj_set_style_border_width(green_square, 0, 0);
    
    // Create BLUE square
    lv_obj_t *blue_square = lv_obj_create(lv_scr_act());
    lv_obj_set_size(blue_square, 80, 80);
    lv_obj_set_pos(blue_square, 220, 80);
    lv_obj_set_style_bg_color(blue_square, lv_color_make(0, 0, 255), 0);
    lv_obj_set_style_border_width(blue_square, 0, 0);
    
    ESP_LOGI(TAG, "UI created");
}
