#include "lcd.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"

static const char *TAG = "LCD";
static uint8_t current_brightness = 100; // Default 100%

// LCD backlight PWM configuration
#define LCD_BACKLIGHT_GPIO  GPIO_NUM_33
#define LCD_PWM_FREQ_HZ     10000
#define LCD_PWM_RESOLUTION  LEDC_TIMER_8_BIT
#define LCD_PWM_SPEED_MODE  LEDC_LOW_SPEED_MODE
#define LCD_PWM_TIMER       LEDC_TIMER_0
#define LCD_PWM_CHANNEL     LEDC_CHANNEL_0

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
    
    // Initialize PWM for backlight control on GPIO 33
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LCD_PWM_SPEED_MODE,
        .duty_resolution  = LCD_PWM_RESOLUTION,
        .timer_num        = LCD_PWM_TIMER,
        .freq_hz          = LCD_PWM_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
    
    ledc_channel_config_t ledc_channel = {
        .gpio_num       = LCD_BACKLIGHT_GPIO,
        .speed_mode     = LCD_PWM_SPEED_MODE,
        .channel        = LCD_PWM_CHANNEL,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = LCD_PWM_TIMER,
        .duty           = 0,
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    lcd_set_brightness(100); // Set to full brightness initially
    
    ESP_LOGI(TAG, "LCD initialized with PWM backlight control on GPIO %d at %d Hz", LCD_BACKLIGHT_GPIO, LCD_PWM_FREQ_HZ);
    return panel_handle;
}

void lcd_set_brightness(uint8_t brightness) {
    if (brightness > 100) {
        brightness = 100;
    }
    
    // Map brightness 0-100 to PWM duty cycle (0-255 for 8-bit resolution)
    uint32_t duty = (brightness * 255) / 100;
    
    ledc_set_duty(LCD_PWM_SPEED_MODE, LCD_PWM_CHANNEL, duty);
    ledc_update_duty(LCD_PWM_SPEED_MODE, LCD_PWM_CHANNEL);
    current_brightness = brightness;
    
}

uint8_t lcd_get_brightness(void) {
    return current_brightness;
}
