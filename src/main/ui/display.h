/*
 * Display service: LVGL + ST7789VW, shows the AI response on screen.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the LCD panel and LVGL, then start the render task.
 * Call once at startup. Safe to call even if the display is disabled.
 */
esp_err_t display_init(void);

/**
 * Update the on-screen AI response text. Thread-safe: may be called
 * from any task (e.g. the agent loop). The actual LVGL redraw happens
 * inside the display task.
 */
void display_show_response(const char *text);

/**
 * Update the small status line (e.g. "Thinking...", "Connected").
 */
void display_show_status(const char *text);

void display_deinit(void);

#ifdef __cplusplus
}
#endif
