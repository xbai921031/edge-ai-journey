/*
 *    Autor: xbai
 *    Date: 2026/01/19
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/spi_common.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lcd.h"

static const char *TAG = "lcd";

esp_lcd_panel_handle_t panel_handle = NULL;

static lcd_transfer_done_cb_t s_transfer_done_cb = NULL;
static void *s_transfer_done_ctx = NULL;

static bool lcd_color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;

    return s_transfer_done_cb ? s_transfer_done_cb(s_transfer_done_ctx) : false;
}

void lcd_set_transfer_done_callback(lcd_transfer_done_cb_t callback, void *user_ctx)
{
    /* This is configured once during startup, before the first LVGL flush. */
    s_transfer_done_cb = callback;
    s_transfer_done_ctx = user_ctx;
}

esp_err_t lcd_init(void)
{
    /* SPI init */
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_CLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * BLOCK_HEIGHT * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

    /* IO panel */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_color_trans_done_cb,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &io_handle),
                        TAG, "panel_io_spi failed");

    /* ST7789 panel config */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle),
                        TAG, "panel_st7789 failed");

    /* Panel init */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "init failed");

    /* Landscape (320x240) via ST7789 MADCTL: swap XY + mirror X.
     * This orientation (A) is known-good. The extra 180-degree flip the
     * user wants is applied in software by LVGL's flush callback (display.c),
     * which avoids the ST7789 MADCTL mirror-Y quirk that garbled the screen. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel_handle, true), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel_handle, true, false), TAG, "mirror failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel_handle, 0, 0), TAG, "set_gap failed");

    /* Backlight init */
    gpio_set_direction(PIN_NUM_BK, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_BK, 1);

    ESP_LOGI(TAG, "LCD ready %dx%d (SPI)", LCD_WIDTH, LCD_HEIGHT);
    return ESP_OK;
}

void lcd_fill_color(uint16_t color)
{
    if (!panel_handle) {
        return;
    }

    uint16_t *buf = heap_caps_malloc(LCD_WIDTH * BLOCK_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) {
        ESP_LOGE(TAG, "fill color alloc failed");
        return;
    }

    for (int i = 0; i < LCD_WIDTH * BLOCK_HEIGHT; i++) {
        buf[i] = color;
    }

    for (int y = 0; y < LCD_HEIGHT; y += BLOCK_HEIGHT) {
        int h = (y + BLOCK_HEIGHT <= LCD_HEIGHT) ? BLOCK_HEIGHT : (LCD_HEIGHT - y);
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_WIDTH, y + h, buf);
    }

    free(buf);
}
