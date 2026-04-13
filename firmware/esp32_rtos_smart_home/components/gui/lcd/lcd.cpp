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
static bool lcd_initialized = false;     // Track if LCD hardware is available
static esp_lcd_panel_io_handle_t lcd_io_handle = NULL;  // Exposed for flush-done callback

// LCD backlight PWM configuration - Pin configured in common/pin_config.h
#define LCD_BACKLIGHT_GPIO  PIN_LCD_BACKLIGHT
#define LCD_PWM_FREQ_HZ     10000
#define LCD_PWM_RESOLUTION  LEDC_TIMER_8_BIT
#define LCD_PWM_SPEED_MODE  LEDC_LOW_SPEED_MODE
#define LCD_PWM_TIMER       LEDC_TIMER_0
#define LCD_PWM_CHANNEL     LEDC_CHANNEL_0

esp_lcd_panel_handle_t lcd_init(void) {
    esp_err_t ret;
    
    // Initialize SPI bus (VSPI)
    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = PIN_NUM_MISO;
    buscfg.mosi_io_num = PIN_NUM_MOSI;
    buscfg.sclk_io_num = PIN_NUM_CLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 4096;
    
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s - LCD disabled", esp_err_to_name(ret));
        return NULL;
    }

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

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed: %s - LCD disabled", esp_err_to_name(ret));
        return NULL;
    }
    lcd_io_handle = io_handle;  // Store for on_color_trans_done registration
    
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_PIN_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;

    esp_lcd_panel_handle_t panel_handle = NULL;
    ret = esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ILI9341 panel init failed: %s - LCD disabled", esp_err_to_name(ret));
        return NULL;
    }
    
    ret = esp_lcd_panel_reset(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel reset failed: %s - LCD disabled", esp_err_to_name(ret));
        return NULL;
    }
    
    ret = esp_lcd_panel_init(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed: %s - LCD disabled", esp_err_to_name(ret));
        return NULL;
    }
    
    // Set to vanilla basics - no transforms
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, false, false);
    esp_lcd_panel_invert_color(panel_handle, false);  // This panel does NOT need inversion (LV_COLOR_16_SWAP=y handles byte order)
    esp_lcd_panel_set_gap(panel_handle, 0, 0);
    esp_lcd_panel_disp_on_off(panel_handle, true);
    
    // Initialize PWM for backlight control on GPIO 33
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode      = LCD_PWM_SPEED_MODE;
    ledc_timer.duty_resolution = LCD_PWM_RESOLUTION;
    ledc_timer.timer_num       = LCD_PWM_TIMER;
    ledc_timer.freq_hz         = LCD_PWM_FREQ_HZ;
    ledc_timer.clk_cfg         = LEDC_AUTO_CLK;
    ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LEDC timer config failed: %s - backlight control disabled", esp_err_to_name(ret));
    }
    
    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num   = LCD_BACKLIGHT_GPIO;
    ledc_channel.speed_mode = LCD_PWM_SPEED_MODE;
    ledc_channel.channel    = LCD_PWM_CHANNEL;
    ledc_channel.intr_type  = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel  = LCD_PWM_TIMER;
    ledc_channel.duty       = 0;
    ledc_channel.hpoint     = 0;
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LEDC channel config failed: %s - backlight control disabled", esp_err_to_name(ret));
    }
    
    lcd_initialized = true;
    lcd_set_brightness(100); // Set to full brightness initially
    
    return panel_handle;
}

void lcd_set_brightness(uint8_t brightness) {
    if (!lcd_initialized) {
        return;  // Silently ignore if LCD not available
    }
    
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

bool lcd_is_available(void) {
    return lcd_initialized;
}

esp_lcd_panel_io_handle_t lcd_get_io_handle(void) {
    return lcd_io_handle;
}

