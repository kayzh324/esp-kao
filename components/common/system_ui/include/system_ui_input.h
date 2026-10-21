/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Optional USB HID input for the system UI (added).
 *
 * Touch stays the primary input and is never disabled. When a USB keyboard or
 * mouse is present, a Lua service reads its reports and injects them here so
 * the same LVGL screens can be driven without a touch panel. If neither touch
 * nor HID is available, the home screen shows a hint instead of looking dead.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Tell the input layer whether the display has a touch panel. */
void system_ui_input_note_touch(bool available);

/** Create the pointer/keypad input devices. Called by system_ui_start(). */
esp_err_t system_ui_input_start(void);

/** Relative mouse movement and button mask (bit 0 = left). */
void system_ui_input_mouse(int dx, int dy, uint32_t buttons, int wheel);

/** HID keyboard usage id with press/release edge. */
void system_ui_input_key(uint32_t usage, bool pressed);

/** Report which HID device classes are currently connected. */
void system_ui_input_set_hid(bool keyboard, bool mouse);

/** True when at least one HID device is connected. */
bool system_ui_input_has_hid(void);

/** True when the display has no touch panel. */
bool system_ui_input_no_touch(void);

#ifdef __cplusplus
}
#endif
