/*
 * Display service: LVGL + ST7789VW.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ui/display.h"
#include "lcd/lcd.h"

#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "display";

/* Full-screen double buffer in PSRAM.
 * N16R8 has 8MB PSRAM, so a full 240x240 frame (x2 for double-buffering)
 * is affordable and eliminates the partial-refresh flicker. */
#define DISP_BUF_LINES LCD_HEIGHT
static lv_color_t *s_disp_buf1 = NULL;
static lv_color_t *s_disp_buf2 = NULL;
/* Buffer used to hold the 180-degree rotated frame before sending to the
 * panel (LVGL renders in logical orientation, we rotate here). */
static lv_color_t *s_rot_buf = NULL;

static lv_obj_t *s_response_label = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_resp_cont = NULL;   /* clipped container holding the response */

/* Composite UI font: Latin glyphs come from Montserrat 20, Chinese glyphs
 * fall back to the CJK font. The struct lives in RAM because the built-in
 * fonts are in flash and their fallback pointer cannot be patched at runtime. */
static lv_font_t s_ui_font;

/* 20px CJK font (generated from SourceHanSansSC, same glyph subset as the
 * built-in 16px one but larger) so Chinese matches the 20px Latin size. */
extern const lv_font_t lv_font_source_han_sans_sc_20_cjk;

/* Thread-safe text hand-off between producer (agent task) and the LVGL task. */
static SemaphoreHandle_t s_text_mutex = NULL;
static char s_response_text[8192];
static char s_status_text[128];
static bool s_response_dirty = false;
static bool s_status_dirty = false;

/* Called by the LCD driver's SPI completion ISR. Until this runs, LVGL must
 * keep both its render buffer and s_rot_buf untouched by subsequent frames. */
static bool lvgl_flush_done_cb(void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    if (disp) {
        lv_display_flush_ready(disp);
    }
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    esp_err_t ret;

    if (rotation == LV_DISPLAY_ROTATION_0 || s_rot_buf == NULL) {
        ret = esp_lcd_panel_draw_bitmap(panel_handle,
                                        area->x1, area->y1,
                                        area->x2 + 1, area->y2 + 1,
                                        (const void *)px_map);
    } else {
        /* Rotate the rendered (logical) buffer into the native panel
         * orientation, then send it. 180 deg keeps the frame size. */
        lv_color_format_t cf = lv_display_get_color_format(disp);
        lv_area_t rotated_area = *area;
        lv_display_rotate_area(disp, &rotated_area);
        uint32_t src_stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), cf);
        uint32_t dest_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&rotated_area), cf);
        lv_draw_sw_rotate(px_map, s_rot_buf,
                          lv_area_get_width(area), lv_area_get_height(area),
                          src_stride, dest_stride, rotation, cf);
        ret = esp_lcd_panel_draw_bitmap(panel_handle,
                                        rotated_area.x1, rotated_area.y1,
                                        rotated_area.x2 + 1, rotated_area.y2 + 1,
                                        (const void *)s_rot_buf);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "panel draw failed: %s", esp_err_to_name(ret));
        /* No completion event follows a failed submission. */
        lv_display_flush_ready(disp);
    }
}

static void display_task(void *arg)
{
    (void)arg;
    uint32_t last = esp_timer_get_time();
    int32_t scroll_y = 0;        /* current vertical offset of the response */
    uint32_t scroll_acc = 0;     /* ms accumulator for the scroll step */
    uint32_t pause_acc = 0;      /* ms accumulator for the end-of-text pause */
    bool pausing = true;         /* start paused at the top so the first
                                  * lines are readable before scrolling */

    while (1) {
        uint32_t now = esp_timer_get_time();
        uint32_t elapsed_ms = (now - last) / 1000;
        last = now;
        lv_tick_inc(elapsed_ms ? elapsed_ms : 1);

        if (s_text_mutex && xSemaphoreTake(s_text_mutex, 0) == pdTRUE) {
            if (s_response_dirty) {
                lv_label_set_text(s_response_label, s_response_text);
                s_response_dirty = false;
                /* Restart auto-scroll from the top and pause there briefly
                 * so the reader can see the beginning of the new message. */
                scroll_y = 0;
                pausing = true;
                pause_acc = 0;
                scroll_acc = 0;
                lv_obj_set_y(s_response_label, 0);
            }
            if (s_status_dirty) {
                lv_label_set_text(s_status_label, s_status_text);
                s_status_dirty = false;
            }
            xSemaphoreGive(s_text_mutex);
        }

        /* Auto-scroll the response vertically so long messages are fully
         * readable on a display with no touch/button input. The motion is
         * gentle (~33 px/s) and pauses ~2 s at both the top and the bottom. */
        if (s_resp_cont && s_response_label) {
            int32_t label_h = lv_obj_get_height(s_response_label);
            int32_t cont_h = lv_obj_get_height(s_resp_cont);
            if (label_h > cont_h + 2) {
                if (pausing) {
                    pause_acc += elapsed_ms;
                    if (pause_acc >= 2000) {
                        pause_acc = 0;
                        pausing = false;
                        /* If we just finished the bottom pause, jump back to
                         * the top before resuming. */
                        if (scroll_y != 0) {
                            scroll_y = 0;
                            lv_obj_set_y(s_response_label, 0);
                        }
                    }
                } else {
                    scroll_acc += elapsed_ms;
                    if (scroll_acc >= 100) {
                        scroll_acc = 0;
                        scroll_y += 1;
                        if (scroll_y >= label_h - cont_h) {
                            scroll_y = label_h - cont_h;
                            lv_obj_set_y(s_response_label, -scroll_y);
                            pausing = true;   /* pause at the bottom */
                            pause_acc = 0;
                        } else {
                            lv_obj_set_y(s_response_label, -scroll_y);
                        }
                    }
                }
            } else if (scroll_y != 0) {
                scroll_y = 0;
                lv_obj_set_y(s_response_label, 0);
            }
        }

        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

esp_err_t display_init(void)
{
    esp_err_t ret = lcd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lcd_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Draw buffers in PSRAM to avoid exhausting internal RAM. */
    size_t buf_bytes = sizeof(lv_color_t) * LCD_WIDTH * DISP_BUF_LINES;
    s_disp_buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_disp_buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_disp_buf1 || !s_disp_buf2) {
        ESP_LOGE(TAG, "display buffer alloc (PSRAM) failed");
        return ESP_ERR_NO_MEM;
    }

    /* Rotation buffer (same size as a logical frame; 180 deg keeps dims). */
    s_rot_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rot_buf) {
        ESP_LOGE(TAG, "rotation buffer alloc (PSRAM) failed");
        return ESP_ERR_NO_MEM;
    }

    lv_init();

    /* Build the composite font: copy Montserrat 20 (descriptor + glyph tables
     * stay in flash), then chain the CJK font as its fallback so any code point
     * missing from Montserrat (i.e. Chinese) is drawn from the CJK font. Both
     * are 20px so Latin and Chinese render at a consistent size. */
    s_ui_font = lv_font_montserrat_20;
    s_ui_font.fallback = &lv_font_source_han_sans_sc_20_cjk;

    lv_display_t *disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_buffers(disp, s_disp_buf1, s_disp_buf2,
                           buf_bytes, LV_DISPLAY_RENDER_MODE_FULL);
    lcd_set_transfer_done_callback(lvgl_flush_done_cb, disp);

    /* Apply the extra 180-degree flip in software (panel stays in the
     * known-good landscape orientation A). */
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);

    /* ---- Minimal UI: status line + auto-scrolling response area ---- */
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_pad_all(scr, 4, 0);
    /* Black background, white text for maximum readability. */
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_status_label = lv_label_create(scr);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_status_label, LCD_WIDTH - 8);
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_status_label, &s_ui_font, 0);
    lv_label_set_text(s_status_label, "ESP-Claw ready");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Response area: a clipped, non-scrollable box that fills the rest of the
     * screen below the status line. Sized/positioned explicitly (no flex) so
     * it always occupies the full lower region regardless of text length. The
     * reply label inside is auto-scrolled vertically by display_task (this
     * board has no touch/button input, so a manual scroll is impossible). */
    s_resp_cont = lv_obj_create(scr);
    lv_obj_set_size(s_resp_cont, LCD_WIDTH - 8, LCD_HEIGHT - 30);
    lv_obj_align(s_resp_cont, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_pad_all(s_resp_cont, 0, 0);
    lv_obj_set_style_bg_opa(s_resp_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_resp_cont, 0, 0);
    lv_obj_set_style_radius(s_resp_cont, 0, 0);
    /* We move the child label manually, so the container itself must NOT be
     * scrollable and must not draw a scrollbar. It still clips its children
     * to its bounds (default behaviour), which is exactly what we want. */
    lv_obj_clear_flag(s_resp_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_resp_cont, LV_SCROLLBAR_MODE_OFF);

    s_response_label = lv_label_create(s_resp_cont);
    lv_label_set_long_mode(s_response_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_response_label, lv_pct(100));
    lv_obj_set_style_text_font(s_response_label, &s_ui_font, 0);
    lv_obj_set_style_text_color(s_response_label, lv_color_white(), 0);
    lv_obj_set_pos(s_response_label, 0, 0);
    lv_label_set_text(s_response_label, "Waiting for messages...");

    s_text_mutex = xSemaphoreCreateMutex();

    BaseType_t task_ret = xTaskCreate(display_task, "display", 6144, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "display task create failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "display service started");
    return ESP_OK;
}

/* Normalize text so the screen never shows missing-glyph boxes:
 *  - full-width punctuation -> half-width (rendered by Montserrat)
 *  - emoji / variation selectors / ZWJ are dropped (no emoji font on this LCD)
 *  - everything else (CJK) is passed through for the fallback font. */
static void display_normalize(const char *in, char *out, size_t out_size)
{
    size_t oi = 0;
    const unsigned char *p = (const unsigned char *)in;

    while (*p && oi + 4 < out_size) {
        uint32_t cp;
        int len;
        if (*p < 0x80) {
            cp = *p; len = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                 ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4;
        } else {
            p++; continue; /* invalid lead byte, skip */
        }

        /* Drop emoji / combining marks that have no glyph here. */
        if (cp >= 0x1F000) { p += len; continue; }                 /* astral emoji */
        if (cp == 0x200D) { p += len; continue; }                  /* ZWJ */
        if (cp >= 0xFE00 && cp <= 0xFE0F) { p += len; continue; }  /* variation selectors */
        if (cp >= 0x2600 && cp <= 0x27BF) { p += len; continue; }  /* misc symbols / dingbats */
        if (cp >= 0x2B00 && cp <= 0x2BFF) { p += len; continue; }  /* arrows + misc symbols */

        /* Full-width ASCII -> half-width. */
        if (cp >= 0xFF01 && cp <= 0xFF5E) {
            cp -= 0xFEE0;
        } else if (cp == 0x3000) { cp = ' '; }
        else if (cp == 0x3001) { cp = ','; }
        else if (cp == 0x3002) { cp = '.'; }
        else if (cp == 0x2018 || cp == 0x2019) { cp = '\''; }
        else if (cp == 0x201C || cp == 0x201D) { cp = '"'; }
        else if (cp == 0x2013 || cp == 0x2014) { cp = '-'; }
        else if (cp == 0x2026) { /* ellipsis -> ... */
            if (oi + 4 < out_size) {
                out[oi++] = '.'; out[oi++] = '.'; out[oi++] = '.';
            }
            p += len; continue;
        }
        /* else: keep as-is (CJK handled by the fallback font) */

        if (cp < 0x80) {
            out[oi++] = (char)cp;
        } else if (cp < 0x800) {
            out[oi++] = (char)(0xC0 | (cp >> 6));
            out[oi++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[oi++] = (char)(0xE0 | (cp >> 12));
            out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[oi++] = (char)(0x80 | (cp & 0x3F));
        }
        p += len;
    }
    out[oi] = '\0';
}

void display_show_response(const char *text)
{
    if (!text || !s_text_mutex) {
        return;
    }
    if (xSemaphoreTake(s_text_mutex, portMAX_DELAY) == pdTRUE) {
        display_normalize(text, s_response_text, sizeof(s_response_text));
        s_response_dirty = true;
        xSemaphoreGive(s_text_mutex);
    }
}

void display_show_status(const char *text)
{
    if (!text || !s_text_mutex) {
        return;
    }
    if (xSemaphoreTake(s_text_mutex, portMAX_DELAY) == pdTRUE) {
        display_normalize(text, s_status_text, sizeof(s_status_text));
        s_status_dirty = true;
        xSemaphoreGive(s_text_mutex);
    }
}

void display_deinit(void)
{
    /* Nothing to tear down in this framework version. */
}
