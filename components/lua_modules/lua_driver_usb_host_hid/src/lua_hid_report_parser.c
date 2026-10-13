/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_hid_report_parser.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "usb_hid_parse";

void lua_hid_layout_init(lua_hid_report_layout_t *layout, uint8_t report_id)
{
    memset(layout, 0, sizeof(*layout));
    layout->valid = true;
    layout->report_id = report_id;
    layout->kb_modifier_off = LUA_HID_OFF_NONE;
    layout->kb_keys_off = LUA_HID_OFF_NONE;
    layout->ms_buttons_off = LUA_HID_OFF_NONE;
    layout->ms_x_off = LUA_HID_OFF_NONE;
    layout->ms_y_off = LUA_HID_OFF_NONE;
    layout->ms_wheel_off = LUA_HID_OFF_NONE;
}

/* Returns the layout for a report ID, creating it on first use. NULL when the
 * per-interface report ID budget is exhausted. */
static lua_hid_report_layout_t *lua_hid_layout_get(lua_hid_descriptor_analysis_t *analysis,
                                                   uint8_t report_id)
{
    for (int i = 0; i < analysis->count; i++) {
        if (analysis->reports[i].report_id == report_id) {
            return &analysis->reports[i];
        }
    }
    if (analysis->count >= LUA_USB_HID_MAX_REPORT_IDS) {
        return NULL;
    }
    lua_hid_report_layout_t *layout = &analysis->reports[analysis->count++];
    lua_hid_layout_init(layout, report_id);
    return layout;
}

/**
 * @brief Classify one Input main item into the layout of its report.
 *
 * @param layout       Layout of the report the item belongs to
 * @param usage_page   Usage page in effect for the item
 * @param usages       Explicit local usages, in declaration order
 * @param usage_count  Number of entries in @p usages
 * @param usage_min    Usage minimum (0 when not declared)
 * @param bit_off      Bit offset of the item inside the report payload
 * @param report_size  Bit width of one field
 * @param report_count Number of fields
 * @param flags        Input item data flags
 */
static void lua_hid_layout_add_input(lua_hid_report_layout_t *layout,
                                     uint16_t usage_page,
                                     const uint16_t *usages,
                                     int usage_count,
                                     uint32_t usage_min,
                                     int bit_off,
                                     uint32_t report_size,
                                     uint32_t report_count,
                                     uint32_t flags)
{
    const bool variable = (flags & LUA_HID_INPUT_VARIABLE) != 0;

    if (flags & LUA_HID_INPUT_CONSTANT) {
        return; /* Padding. */
    }

    switch (usage_page) {
    case LUA_HID_USAGE_PAGE_KEYBOARD:
        if (variable && usage_min >= LUA_HID_USAGE_MODIFIER_MIN) {
            /* Modifier bitmap (LeftCtrl..RightGUI). */
            if (layout->kb_modifier_off < 0) {
                layout->kb_modifier_off = (int16_t)bit_off;
            }
        } else if (!variable && report_size == 8) {
            /* Key array. */
            if (layout->kb_keys_off < 0) {
                layout->kb_keys_off = (int16_t)bit_off;
                layout->kb_keys_count = (uint8_t)report_count;
            }
        }
        break;

    case LUA_HID_USAGE_PAGE_BUTTON:
        if (variable && layout->ms_buttons_off < 0) {
            layout->ms_buttons_off = (int16_t)bit_off;
            layout->ms_buttons_count = (uint8_t)report_count;
        }
        break;

    case LUA_HID_USAGE_PAGE_DESKTOP:
        if (!variable) {
            break;
        }
        /* One field per usage: X, Y and Wheel are usually declared together. */
        for (uint32_t i = 0; i < report_count; i++) {
            uint16_t usage = 0;
            if ((int)i < usage_count) {
                usage = usages[i];
            } else if (usage_count == 1) {
                usage = usages[0];
            } else if (usage_min != 0) {
                usage = (uint16_t)(usage_min + i);
            } else {
                continue;
            }

            const int off = bit_off + (int)(i * report_size);
            if (usage == LUA_HID_USAGE_X && layout->ms_x_off < 0) {
                layout->ms_x_off = (int16_t)off;
                layout->ms_x_bits = (uint8_t)report_size;
            } else if (usage == LUA_HID_USAGE_Y && layout->ms_y_off < 0) {
                layout->ms_y_off = (int16_t)off;
                layout->ms_y_bits = (uint8_t)report_size;
            } else if (usage == LUA_HID_USAGE_WHEEL && layout->ms_wheel_off < 0) {
                layout->ms_wheel_off = (int16_t)off;
                layout->ms_wheel_bits = (uint8_t)report_size;
            }
        }
        break;

    default:
        break;
    }
}

/**
 * @brief Parse the HID report descriptor into one layout per input report ID.
 *
 * The walk keeps global state (usage page, report size/count, report ID) and
 * local state (usages) exactly as the HID spec prescribes: locals are cleared
 * after every main item, globals persist. Report IDs are discovered from the
 * items that actually carry them rather than from collection nesting, so combo
 * receivers that hide keyboard/mouse reports inside a vendor-defined
 * application collection are handled the same as well-behaved devices.
 *
 * @param handle Interface to inspect
 * @param out    Analysis result, owned by the caller (too large for a task stack)
 */
void lua_hid_parse_report_descriptor(hid_host_device_handle_t handle,
                                     lua_hid_descriptor_analysis_t *out)
{
    out->count = 0;

    size_t desc_len = 0;
    const uint8_t *descriptor = hid_host_get_report_descriptor(handle, &desc_len);

    if (!descriptor || desc_len == 0) {
        ESP_LOGW(TAG, "report descriptor unavailable (len=%u); "
                 "falling back to interface protocol", (unsigned)desc_len);
        return;
    }

    ESP_LOGD(TAG, "report descriptor (%u bytes):", (unsigned)desc_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, descriptor, desc_len, ESP_LOG_DEBUG);

    uint16_t usage_page = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    uint8_t report_id = 0;
    uint16_t collection_usage_page = 0;
    uint16_t collection_usage = 0;
    int collection_depth = 0;

    uint16_t usages[LUA_HID_MAX_LOCAL_USAGES];
    int usage_count = 0;
    uint32_t usage_min = 0;

    for (size_t i = 0; i < desc_len; ) {
        const uint8_t prefix = descriptor[i];

        if (prefix == LUA_HID_LONG_ITEM_PREFIX) {
            /* Long item: [0xFE][bDataSize][bLongItemTag][data...] */
            if (i + 1 >= desc_len) {
                break;
            }
            i += 3 + descriptor[i + 1];
            continue;
        }

        uint8_t size = prefix & 0x03;
        const uint8_t type = (prefix >> 2) & 0x03;
        const uint8_t tag = prefix >> 4;

        if (size == 3) {
            size = 4; /* Item size 3 encodes a 4-byte payload. */
        }
        if (i + 1 + size > desc_len) {
            ESP_LOGW(TAG, "truncated report descriptor item at offset %u", (unsigned)i);
            break;
        }

        uint32_t data = 0;
        for (uint8_t j = 0; j < size; j++) {
            data |= (uint32_t)descriptor[i + 1 + j] << (8 * j);
        }
        i += 1 + size;

        if (type == LUA_HID_ITEM_TYPE_GLOBAL) {
            switch (tag) {
            case LUA_HID_TAG_USAGE_PAGE:
                usage_page = (uint16_t)data;
                break;
            case LUA_HID_TAG_REPORT_SIZE:
                report_size = data;
                break;
            case LUA_HID_TAG_REPORT_COUNT:
                report_count = data;
                break;
            case LUA_HID_TAG_REPORT_ID:
                report_id = (uint8_t)data;
                break;
            default:
                break;
            }
            continue;
        }

        if (type == LUA_HID_ITEM_TYPE_LOCAL) {
            switch (tag) {
            case LUA_HID_TAG_USAGE:
                if (usage_count < LUA_HID_MAX_LOCAL_USAGES) {
                    usages[usage_count++] = (uint16_t)data;
                }
                break;
            case LUA_HID_TAG_USAGE_MIN:
                usage_min = data;
                break;
            default:
                break;
            }
            continue;
        }

        if (type != LUA_HID_ITEM_TYPE_MAIN) {
            continue;
        }

        switch (tag) {
        case LUA_HID_TAG_COLLECTION:
            if (collection_depth == 0) {
                collection_usage_page = usage_page;
                collection_usage = usage_count > 0 ? usages[0] : 0;
            }
            collection_depth++;
            break;

        case LUA_HID_TAG_END_COLLECTION:
            if (collection_depth > 0) {
                collection_depth--;
            }
            break;

        case LUA_HID_TAG_INPUT: {
            lua_hid_report_layout_t *layout = lua_hid_layout_get(out, report_id);
            if (layout) {
                if (layout->input_bits == 0) {
                    layout->usage_page = collection_usage_page;
                    layout->usage = collection_usage;
                }
                lua_hid_layout_add_input(layout, usage_page, usages, usage_count,
                                         usage_min, layout->input_bits,
                                         report_size, report_count, data);
                layout->input_bits += (uint16_t)(report_size * report_count);
            } else {
                ESP_LOGW(TAG, "more than %d report IDs on one interface; report 0x%02x ignored",
                         LUA_USB_HID_MAX_REPORT_IDS, report_id);
            }
            break;
        }

        default:
            break;
        }

        /* Local items apply to the main item that follows them only. */
        usage_count = 0;
        usage_min = 0;
    }
}

/**
 * @brief Pick the logical device type for one parsed report.
 *
 * The collection usage is authoritative when it is a known one. Cheap 2.4G
 * receivers frequently bury their reports in a vendor-defined collection, so
 * the field layout is used as a second opinion before giving up.
 */
const char *lua_hid_layout_type(const lua_hid_report_layout_t *layout)
{
    if (layout->usage_page == LUA_HID_USAGE_PAGE_DESKTOP) {
        if (layout->usage == LUA_HID_USAGE_KEYBOARD) {
            return "keyboard";
        }
        if (layout->usage == LUA_HID_USAGE_MOUSE) {
            return "mouse";
        }
    }
    if (layout->kb_keys_off >= 0 || layout->kb_modifier_off >= 0) {
        return "keyboard";
    }
    if (layout->ms_x_off >= 0 && layout->ms_y_off >= 0) {
        return "mouse";
    }
    return "generic";
}

/* Extracts an unsigned bit field. Returns false when the report is too short. */
bool lua_hid_bits_get(const uint8_t *data, size_t len,
                      int bit_off, uint8_t bit_cnt, uint32_t *out)
{
    uint32_t value = 0;

    if (bit_off < 0 || bit_cnt == 0 || bit_cnt > 32) {
        return false;
    }
    if ((size_t)(bit_off + bit_cnt + 7) / 8 > len) {
        return false;
    }

    for (uint8_t i = 0; i < bit_cnt; i++) {
        const int bit = bit_off + i;
        if (data[bit / 8] & (1u << (bit % 8))) {
            value |= (1u << i);
        }
    }
    *out = value;
    return true;
}

/* Same, sign-extending the field to int32_t. */
bool lua_hid_bits_get_signed(const uint8_t *data, size_t len,
                             int bit_off, uint8_t bit_cnt, int32_t *out)
{
    uint32_t raw = 0;

    if (!lua_hid_bits_get(data, len, bit_off, bit_cnt, &raw)) {
        return false;
    }
    if (bit_cnt < 32 && (raw & (1u << (bit_cnt - 1)))) {
        raw |= ~((1u << bit_cnt) - 1u);
    }
    *out = (int32_t)raw;
    return true;
}
