/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "system_ui_input.h"

#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "system_ui_private.h"

static const char *TAG = "system_ui_input";

#define SYSTEM_UI_INPUT_KEY_QUEUE_LEN 32
#define SYSTEM_UI_INPUT_CURSOR_SIDE   12
#define SYSTEM_UI_INPUT_MOUSE_GAIN    2
#define SYSTEM_UI_INPUT_HINT_PERIOD_MS 2000

typedef struct {
    uint32_t usage;
    bool pressed;
} system_ui_input_key_event_t;

static struct {
    portMUX_TYPE lock;
    int32_t width;
    int32_t height;
    int32_t x;
    int32_t y;
    bool pressed;
    bool hid_keyboard;
    bool hid_mouse;
    bool touch_available;
    system_ui_input_key_event_t keys[SYSTEM_UI_INPUT_KEY_QUEUE_LEN];
    size_t key_head;
    size_t key_tail;
    lv_obj_t *cursor;
    lv_obj_t *hint;
    lv_obj_t *hint_label;
    lv_timer_t *hint_timer;
} s_input = { .lock = portMUX_INITIALIZER_UNLOCKED };

static uint32_t system_ui_input_map_usage(uint32_t usage)
{
    switch (usage) {
    case 0x4F: /* left arrow */
    case 0x04: /* a */
    case 0x52: /* up arrow */
    case 0x1A: /* w */
        return LV_KEY_PREV;
    case 0x50: /* right arrow */
    case 0x07: /* d */
    case 0x51: /* down arrow */
    case 0x16: /* s */
        return LV_KEY_NEXT;
    case 0x28: /* enter */
        return LV_KEY_ENTER;
    case 0x29: /* escape */
        return LV_KEY_ESC;
    case 0x2C: /* space */
        return ' ';
    default:
        return 0;
    }
}

static void system_ui_input_pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    portENTER_CRITICAL(&s_input.lock);
    data->point.x = (int32_t)s_input.x;
    data->point.y = (int32_t)s_input.y;
    data->state = s_input.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    portEXIT_CRITICAL(&s_input.lock);
    data->continue_reading = false;
}

static void system_ui_input_move_focus(int dx, int dy)
{
    if (!s_ui.input_group) {
        return;
    }
    if (system_ui_lock() != ESP_OK) {
        return;
    }

    uint32_t count = lv_group_get_obj_count(s_ui.input_group);
    if (count == 0) {
        system_ui_unlock();
        return;
    }

    lv_obj_t *focused = lv_group_get_focused(s_ui.input_group);
    uint32_t index = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (lv_group_get_obj_by_index(s_ui.input_group, i) == focused) {
            index = i;
            break;
        }
    }

    uint32_t cols = s_ui.launcher_layout.cols ? s_ui.launcher_layout.cols : 4;
    uint32_t rows = s_ui.launcher_layout.rows ? s_ui.launcher_layout.rows : 2;
    uint32_t per_page = cols * rows;
    uint32_t page = index / per_page;
    uint32_t cell = index % per_page;
    int32_t row = (int32_t)(cell / cols);
    int32_t col = (int32_t)(cell % cols);

    if (dx != 0) {
        col += dx;
        if (col < 0) {
            if (page > 0) {
                page--;
                col = (int32_t)cols - 1;
            } else {
                col = 0;
            }
        } else if (col >= (int32_t)cols) {
            if ((page + 1) * per_page < count) {
                page++;
                col = 0;
            } else {
                col = (int32_t)cols - 1;
            }
        }
    }
    if (dy != 0) {
        row += dy;
        if (row < 0) {
            row = 0;
        } else if (row >= (int32_t)rows) {
            row = (int32_t)rows - 1;
        }
    }

    uint32_t next = page * per_page + (uint32_t)row * cols + (uint32_t)col;
    if (next >= count) {
        if (dy != 0) {
            /* Empty cell on this page (for example a single row of apps): stay put. */
            system_ui_unlock();
            return;
        }
        uint32_t max_in_page = count - page * per_page;
        if (max_in_page == 0) {
            system_ui_unlock();
            return;
        }
        uint32_t cell_next = next % per_page;
        if (cell_next >= max_in_page) {
            cell_next = max_in_page - 1;
        }
        next = page * per_page + cell_next;
    }
    ESP_LOGD(TAG, "focus %u -> %u (dx=%d dy=%d count=%u)", (unsigned)index,
             (unsigned)next, dx, dy, (unsigned)count);
    lv_obj_t *target = lv_group_get_obj_by_index(s_ui.input_group, next);
    if (target) {
        lv_group_focus_obj(target);
    }
    system_ui_unlock();
}

static void system_ui_input_key_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    bool has_event = false;
    system_ui_input_key_event_t event = {0};

    portENTER_CRITICAL(&s_input.lock);
    if (s_input.key_head != s_input.key_tail) {
        event = s_input.keys[s_input.key_head];
        s_input.key_head = (s_input.key_head + 1) % SYSTEM_UI_INPUT_KEY_QUEUE_LEN;
        has_event = true;
    }
    portEXIT_CRITICAL(&s_input.lock);

    if (has_event) {
        data->key = event.usage;
        data->state = event.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    } else {
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->continue_reading = false;
}

static void system_ui_input_hint_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    bool need_hint;

    bool has_hid;
    portENTER_CRITICAL(&s_input.lock);
    need_hint = !s_input.touch_available;
    has_hid = s_input.hid_keyboard || s_input.hid_mouse;
    portEXIT_CRITICAL(&s_input.lock);

    if (s_input.cursor) {
        bool has_pointer;
        portENTER_CRITICAL(&s_input.lock);
        has_pointer = s_input.hid_mouse;
        portEXIT_CRITICAL(&s_input.lock);
        if (has_pointer) lv_obj_clear_flag(s_input.cursor, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_input.cursor, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_input.hint) {
        return;
    }
    if (need_hint) {
        if (s_input.hint_label) {
            lv_label_set_text(s_input.hint_label,
                              has_hid ? "TAB apps/home  |  ARROWS move  |  ENTER open"
                                      : "No touch input.\nConnect a USB keyboard or mouse.");
        }
        lv_obj_clear_flag(s_input.hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_input.hint);
    } else {
        lv_obj_add_flag(s_input.hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static esp_err_t system_ui_input_create_hint(void)
{
    s_input.hint = lv_obj_create(lv_layer_top());
    if (!s_input.hint) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_input.hint);
    lv_obj_set_size(s_input.hint, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_input.hint, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_input.hint, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_input.hint, 8, 0);
    lv_obj_set_style_pad_all(s_input.hint, 10, 0);
    lv_obj_clear_flag(s_input.hint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_input.hint, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_obj_t *label = lv_label_create(s_input.hint);
    if (!label) {
        lv_obj_delete(s_input.hint);
        s_input.hint = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_input.hint_label = label;
    lv_label_set_text(label, "No touch input.\nConnect a USB keyboard or mouse.");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    lv_obj_add_flag(s_input.hint, LV_OBJ_FLAG_HIDDEN);

    s_input.hint_timer = lv_timer_create(system_ui_input_hint_timer_cb,
                                         SYSTEM_UI_INPUT_HINT_PERIOD_MS, NULL);
    return s_input.hint_timer ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t system_ui_input_create_cursor(void)
{
    lv_display_t *display = lv_display_get_default();

    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    lv_indev_t *pointer = lv_indev_create();
    if (!pointer) {
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, system_ui_input_pointer_read);
    lv_indev_set_display(pointer, display);

    s_input.cursor = lv_obj_create(lv_layer_top());
    if (!s_input.cursor) {
        lv_indev_delete(pointer);
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(s_input.cursor);
    lv_obj_set_size(s_input.cursor, SYSTEM_UI_INPUT_CURSOR_SIDE, SYSTEM_UI_INPUT_CURSOR_SIDE);
    lv_obj_set_style_radius(s_input.cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_input.cursor, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_input.cursor, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_input.cursor, 2, 0);
    lv_obj_set_style_border_color(s_input.cursor, lv_color_black(), 0);
    lv_obj_clear_flag(s_input.cursor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_input.cursor, LV_OBJ_FLAG_HIDDEN);
    lv_indev_set_cursor(pointer, s_input.cursor);

    lv_indev_t *keypad = lv_indev_create();
    if (!keypad) {
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keypad, system_ui_input_key_read);
    lv_indev_set_display(keypad, display);
    if (s_ui.input_group) {
        lv_indev_set_group(keypad, s_ui.input_group);
    }
    return ESP_OK;
}

esp_err_t system_ui_input_start(void)
{
    bool touch_available;
    portENTER_CRITICAL(&s_input.lock);
    touch_available = s_input.touch_available;
    portEXIT_CRITICAL(&s_input.lock);

    memset((void *)&s_input, 0, sizeof(s_input));
    s_input.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    s_input.touch_available = touch_available;

    lv_display_t *display = lv_display_get_default();
    if (display) {
        s_input.width = lv_display_get_horizontal_resolution(display);
        s_input.height = lv_display_get_vertical_resolution(display);
        s_input.x = s_input.width / 2;
        s_input.y = s_input.height / 2;
    }

    if (!s_ui.input_group) {
        s_ui.input_group = lv_group_create();
        if (!s_ui.input_group) {
            return ESP_ERR_NO_MEM;
        }
    }
    lv_group_set_default(s_ui.input_group);

    esp_err_t err = system_ui_input_create_cursor();
    if (err != ESP_OK) {
        return err;
    }
    err = system_ui_input_create_hint();
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "HID input ready (touch=%s group=%u)", s_input.touch_available ? "yes" : "no",
             (unsigned)lv_group_get_obj_count(s_ui.input_group));
    return ESP_OK;
}

void system_ui_input_note_touch(bool available)
{
    portENTER_CRITICAL(&s_input.lock);
    s_input.touch_available = available;
    portEXIT_CRITICAL(&s_input.lock);
}

void system_ui_input_mouse(int dx, int dy, uint32_t buttons, int wheel)
{
    portENTER_CRITICAL(&s_input.lock);
    if (dx || dy) {
        int32_t nx = s_input.x + dx * SYSTEM_UI_INPUT_MOUSE_GAIN;
        int32_t ny = s_input.y + dy * SYSTEM_UI_INPUT_MOUSE_GAIN;

        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx >= s_input.width) nx = s_input.width - 1;
        if (ny >= s_input.height) ny = s_input.height - 1;
        s_input.x = nx;
        s_input.y = ny;
    }
    s_input.pressed = (buttons & 0x01U) != 0;
    (void)wheel;
    portEXIT_CRITICAL(&s_input.lock);
}

void system_ui_input_key(uint32_t usage, bool pressed)
{
    uint32_t key = system_ui_input_map_usage(usage);
    ESP_LOGD(TAG, "key usage=0x%02x pressed=%d mapped=0x%lx", (unsigned)usage,
             (int)pressed, (unsigned long)key);
    if (pressed && usage == 0x2B) { /* Tab toggles clock <-> launcher */
        system_ui_toggle_page();
        return;
    }
    if (pressed && usage == 0x4A) { /* Home returns to the clock */
        system_ui_show_home();
        return;
    }
    if (pressed) {
        /* Some 2.4G receivers (seen with 1915:1025) report the arrow cluster
         * with left/right usage ids swapped. Keep A/D standard and map the
         * reported right/left usages to the visual direction. */
        if (usage == 0x50 || usage == 0x04) {
            system_ui_input_move_focus(-1, 0);
            return;
        }
        if (usage == 0x4F || usage == 0x07) {
            system_ui_input_move_focus(1, 0);
            return;
        }
        if (usage == 0x51 || usage == 0x16) {
            system_ui_input_move_focus(0, 1);
            return;
        }
        if (usage == 0x52 || usage == 0x1A) {
            system_ui_input_move_focus(0, -1);
            return;
        }
    }
    if (key == 0) {
        return;
    }

    portENTER_CRITICAL(&s_input.lock);
    size_t next = (s_input.key_tail + 1) % SYSTEM_UI_INPUT_KEY_QUEUE_LEN;
    if (next != s_input.key_head) {
        s_input.keys[s_input.key_tail].usage = key;
        s_input.keys[s_input.key_tail].pressed = pressed;
        s_input.key_tail = next;
    }
    portEXIT_CRITICAL(&s_input.lock);
}

void system_ui_input_set_hid(bool keyboard, bool mouse)
{
    bool changed;

    portENTER_CRITICAL(&s_input.lock);
    changed = s_input.hid_keyboard != keyboard || s_input.hid_mouse != mouse;
    s_input.hid_keyboard = keyboard;
    s_input.hid_mouse = mouse;
    portEXIT_CRITICAL(&s_input.lock);

    if (changed) {
        ESP_LOGI(TAG, "HID devices: keyboard=%d mouse=%d", keyboard, mouse);
    }
}

bool system_ui_input_has_hid(void)
{
    bool has;
    portENTER_CRITICAL(&s_input.lock);
    has = s_input.hid_keyboard || s_input.hid_mouse;
    portEXIT_CRITICAL(&s_input.lock);
    return has;
}

bool system_ui_input_no_touch(void)
{
    bool no_touch;
    portENTER_CRITICAL(&s_input.lock);
    no_touch = !s_input.touch_available;
    portEXIT_CRITICAL(&s_input.lock);
    return no_touch;
}
