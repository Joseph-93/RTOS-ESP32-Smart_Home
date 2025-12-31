/**
 * ESP32 Bare Bones Starter - Learn RTOS from scratch
 * 
 * This is a minimal starting point. Build up RTOS features as you learn:
 * - Start with simple loop (current)
 * - Add vTaskDelay() for timing
 * - Create your first task with xTaskCreate()
 * - Add queues for inter-task communication
 * - Experiment with priorities, semaphores, mutexes, etc.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "lvgl.h"

// LED GPIO pin (common: GPIO 2 for many ESP32 dev boards)
#define BLINK_GPIO 2

// VSPI pins for ESP32
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

// ILI9341 Display pins
#define LCD_PIN_DC   2   // Data/Command
#define LCD_PIN_RST  4   // Reset
#define LCD_PIN_BL   15  // Backlight

// Touch pins
#define TOUCH_CS     21  // Touch CS

// Display dimensions (swapped for XY swap mode)
#define LCD_H_RES    320
#define LCD_V_RES    240

static const char *TAG = "ESP32";

esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_touch_handle_t touch_handle = NULL;

// Persistent framebuffer for drawing
uint16_t *framebuffer = NULL;

// LVGL display driver and buffers
static lv_disp_drv_t lvgl_disp_drv;
static lv_disp_draw_buf_t lvgl_disp_buf;
static lv_indev_drv_t lvgl_indev_drv;

// LVGL flush callback
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;
    
    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2, y2, (uint16_t *)color_map);
    lv_disp_flush_ready(drv);
}

// LVGL touch input callback
static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    if (esp_lcd_touch_read_data(touch_handle) == ESP_OK) {
        esp_lcd_touch_point_data_t touch_data;
        uint8_t touch_cnt = 0;
        if (esp_lcd_touch_get_data(touch_handle, &touch_data, &touch_cnt, 1) == ESP_OK && touch_cnt > 0) {
            data->point.x = touch_data.x;
            data->point.y = touch_data.y;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            ESP_LOGI(TAG, "esp_lcd_touch_get_data failed or no touch");
            data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        ESP_LOGI(TAG, "esp_lcd_touch_read_data failed");
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


// Helper function to redraw screen from framebuffer
void redraw_screen() {
    for (int y = 0; y < LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 1, &framebuffer[y * LCD_H_RES]);
    }
}

// Helper function to reset screen to black
void reset_screen() {
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        framebuffer[i] = 0x0000; // Black
    }
    redraw_screen();
    ESP_LOGI(TAG, "Screen reset");
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting ESP32...");
    ESP_LOGI(TAG, "ESP-IDF Version: %s", esp_get_idf_version());
    
    // Initialize SPI bus (VSPI)
    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = PIN_NUM_MISO;
    buscfg.mosi_io_num = PIN_NUM_MOSI;
    buscfg.sclk_io_num = PIN_NUM_CLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 4096;
    
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "SPI bus initialized");

    // Initialize ILI9341 LCD Display
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = LCD_PIN_DC;
    io_config.cs_gpio_num = PIN_NUM_CS;
    io_config.pclk_hz = 10 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));
    
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_PIN_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;

    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    // Set to vanilla basics - no transforms
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    
    // Turn on backlight
    gpio_set_direction((gpio_num_t)LCD_PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)LCD_PIN_BL, 1);
    
    // Initialize touch controller on same SPI bus
    esp_lcd_panel_io_handle_t touch_io_handle = NULL;
    esp_lcd_panel_io_spi_config_t touch_io_config = {};
    touch_io_config.dc_gpio_num = -1;
    touch_io_config.cs_gpio_num = TOUCH_CS;
    touch_io_config.pclk_hz = 2 * 1000 * 1000;
    touch_io_config.lcd_cmd_bits = 8;
    touch_io_config.lcd_param_bits = 8;
    touch_io_config.spi_mode = 0;
    touch_io_config.trans_queue_depth = 3;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &touch_io_config, &touch_io_handle));
    
    esp_lcd_touch_config_t touch_config = {};
    touch_config.x_max = LCD_V_RES;
    touch_config.y_max = LCD_H_RES;
    touch_config.rst_gpio_num = (gpio_num_t)-1;
    touch_config.int_gpio_num = (gpio_num_t)-1;
    touch_config.flags.swap_xy = false;
    touch_config.flags.mirror_x = true;
    touch_config.flags.mirror_y = true;

    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(touch_io_handle, &touch_config, &touch_handle));
    ESP_LOGI(TAG, "Touch initialized");
    
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
    
    ESP_LOGI(TAG, "System initialized - LVGL ready");
    
    char cmd_buffer[64];
    int buf_idx = 0;
    
    // Main loop
    while (1) {
        // Check for UART commands
        int c = getchar();
        if (c != EOF) {
            // Echo character back immediately
            putchar(c);
            fflush(stdout);
            
            if (c == '\n' || c == '\r') {
                // Process command
                cmd_buffer[buf_idx] = '\0';
                
                // No commands currently implemented
                
                buf_idx = 0;
            } else if (buf_idx < sizeof(cmd_buffer) - 1) {
                cmd_buffer[buf_idx++] = (char)c;
            }
        }

        // Poll touch controller directly
        esp_lcd_touch_read_data(touch_handle);
        esp_lcd_touch_point_data_t touch_point;
        uint8_t num_points = 0;
        if (esp_lcd_touch_get_data(touch_handle, &touch_point, &num_points, 1) == ESP_OK && num_points > 0) {
            ESP_LOGI(TAG, "Poll: X=%d Y=%d", touch_point.x, touch_point.y);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
