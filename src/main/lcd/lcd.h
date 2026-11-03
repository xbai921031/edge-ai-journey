/*
 *    Autor: xbai
 *    Date: 2026/01/19
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_interface.h"

/* ============================================================
 *  硬件引脚与分辨率 —— SPI 接口（ST7789VW）
 * ============================================================ */

#define LCD_SPI_HOST   SPI2_HOST

#define PIN_NUM_MOSI   20
#define PIN_NUM_CLK    19
#define PIN_NUM_CS     45
#define PIN_NUM_DC     47
#define PIN_NUM_RST    21
#define PIN_NUM_BK     38

#define LCD_WIDTH      320
#define LCD_HEIGHT     240
#define BLOCK_HEIGHT   16

extern esp_lcd_panel_handle_t panel_handle;

/* Called from the LCD SPI completion ISR after a color transfer finishes.
 * Return true only when a higher-priority task was woken. */
typedef bool (*lcd_transfer_done_cb_t)(void *user_ctx);

esp_err_t lcd_init(void);
void lcd_set_transfer_done_callback(lcd_transfer_done_cb_t callback, void *user_ctx);
void lcd_fill_color(uint16_t color);
