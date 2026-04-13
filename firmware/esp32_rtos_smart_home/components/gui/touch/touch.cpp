#include "touch.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"

static const char *TAG = "Touch";
static bool touch_initialized = false;

esp_lcd_touch_handle_t touch_init(void) {
    esp_err_t ret;
    
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

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &touch_io_config, &touch_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch panel IO init failed: %s - Touch disabled", esp_err_to_name(ret));
        return NULL;
    }
    
    esp_lcd_touch_config_t touch_config = {};
    touch_config.x_max = TOUCH_X_MAX;
    touch_config.y_max = TOUCH_Y_MAX;
    touch_config.rst_gpio_num = (gpio_num_t)-1;
    touch_config.int_gpio_num = (gpio_num_t)-1;
    touch_config.flags.swap_xy = true;  // Swap X and Y
    touch_config.flags.mirror_x = true;
    touch_config.flags.mirror_y = true;

    esp_lcd_touch_handle_t touch_handle = NULL;
    ret = esp_lcd_touch_new_spi_xpt2046(touch_io_handle, &touch_config, &touch_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "XPT2046 touch init failed: %s - Touch disabled", esp_err_to_name(ret));
        return NULL;
    }
    
    touch_initialized = true;

    // ── Diagnostic: raw SPI probe of XPT2046 ──────────────────────────
    // Read Z1 (0xB0), Z2 (0xC0), X (0xD0), Y (0x90) registers directly
    // via the panel IO handle to verify the chip responds on the SPI bus.
    // Each command returns 2 bytes (12-bit ADC value left-justified).
    {
        const uint8_t cmds[] = {0xB0, 0xC0, 0xD0, 0x90};  // Z1, Z2, X, Y (PD=00 = low power + IRQ)
        const char* names[] = {"Z1", "Z2", "X", "Y"};
        for (int i = 0; i < 4; i++) {
            uint8_t buf[2] = {0, 0};
            esp_err_t err = esp_lcd_panel_io_rx_param(touch_io_handle, cmds[i], buf, 2);
            uint16_t raw = (buf[0] << 8) | buf[1];
            ESP_LOGW(TAG, "SPI probe %s (cmd=0x%02X): err=%s raw=0x%04X (%d) bytes=[0x%02X,0x%02X]",
                     names[i], cmds[i], esp_err_to_name(err), raw, raw >> 3, buf[0], buf[1]);
        }
    }
    // ──────────────────────────────────────────────────────────────────

    return touch_handle;
}

bool touch_is_available(void) {
    return touch_initialized;
}
