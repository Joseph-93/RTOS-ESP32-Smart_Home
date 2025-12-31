#include "touch.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"

static const char *TAG = "Touch";

esp_lcd_touch_handle_t touch_init(void) {
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
    touch_config.x_max = TOUCH_X_MAX;
    touch_config.y_max = TOUCH_Y_MAX;
    touch_config.rst_gpio_num = (gpio_num_t)-1;
    touch_config.int_gpio_num = (gpio_num_t)-1;
    touch_config.flags.swap_xy = false;
    touch_config.flags.mirror_x = true;
    touch_config.flags.mirror_y = true;

    esp_lcd_touch_handle_t touch_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(touch_io_handle, &touch_config, &touch_handle));
    ESP_LOGI(TAG, "Touch initialized");
    
    return touch_handle;
}
