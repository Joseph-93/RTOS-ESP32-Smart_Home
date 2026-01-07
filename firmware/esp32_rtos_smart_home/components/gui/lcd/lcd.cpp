#include "lcd.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/dac.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"

static const char *TAG = "LCD";
static uint8_t current_brightness = 100; // Default 100%

esp_lcd_panel_handle_t lcd_init(void) {
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
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    // Set to vanilla basics - no transforms
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    
    // Initialize DAC for backlight control on GPIO 25 (DAC1)
    ESP_ERROR_CHECK(dac_output_enable(DAC_CHANNEL_1)); // GPIO 25 is DAC1
    lcd_set_brightness(100); // Set to full brightness initially
    
    ESP_LOGI(TAG, "LCD initialized with DAC backlight control");
    return panel_handle;
}

void lcd_set_brightness(uint8_t brightness) {
    if (brightness > 100) {
        brightness = 100;
    }
    
    // DAC outputs 0-255, map brightness 0-100 to DAC value
    uint8_t dac_value = (brightness * 255) / 100;
    
    ESP_ERROR_CHECK(dac_output_voltage(DAC_CHANNEL_1, dac_value));
    current_brightness = brightness;
    
    ESP_LOGI(TAG, "Backlight brightness set to %d%% (DAC: %d)", brightness, dac_value);
}

uint8_t lcd_get_brightness(void) {
    return current_brightness;
}
