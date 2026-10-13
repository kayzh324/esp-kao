/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usb/hid_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A single physical interface may multiplex several report IDs; each one is
 * exposed as its own logical device, so the registry must hold more slots than
 * the number of interfaces we support. */
#define LUA_USB_HID_MAX_REPORT_IDS  8
/* Local usages declared before one main item (e.g. X, Y, Wheel). */
#define LUA_HID_MAX_LOCAL_USAGES    16

/* HID usage pages / usages used for report classification. */
#define LUA_HID_USAGE_PAGE_DESKTOP  0x0001
#define LUA_HID_USAGE_PAGE_KEYBOARD 0x0007
#define LUA_HID_USAGE_PAGE_BUTTON   0x0009
#define LUA_HID_USAGE_MOUSE         0x0002
#define LUA_HID_USAGE_KEYBOARD      0x0006
#define LUA_HID_USAGE_X             0x0030
#define LUA_HID_USAGE_Y             0x0031
#define LUA_HID_USAGE_WHEEL         0x0038
#define LUA_HID_USAGE_MODIFIER_MIN  0x00E0

/* Report descriptor item encoding. */
#define LUA_HID_ITEM_TYPE_MAIN      0x00
#define LUA_HID_ITEM_TYPE_GLOBAL    0x01
#define LUA_HID_ITEM_TYPE_LOCAL     0x02
#define LUA_HID_TAG_INPUT           0x08
#define LUA_HID_TAG_COLLECTION      0x0A
#define LUA_HID_TAG_END_COLLECTION  0x0C
#define LUA_HID_TAG_USAGE_PAGE      0x00
#define LUA_HID_TAG_REPORT_SIZE     0x07
#define LUA_HID_TAG_REPORT_ID       0x08
#define LUA_HID_TAG_REPORT_COUNT    0x09
#define LUA_HID_TAG_USAGE           0x00
#define LUA_HID_TAG_USAGE_MIN       0x01
#define LUA_HID_INPUT_CONSTANT      0x01
#define LUA_HID_INPUT_VARIABLE      0x02
#define LUA_HID_LONG_ITEM_PREFIX    0xFE

#define LUA_HID_OFF_NONE            (-1)

/**
 * @brief Field layout of one input report, derived from the report descriptor.
 *
 * All bit offsets are relative to the first payload bit, i.e. the report ID
 * byte is already excluded. A negative offset means the field is absent.
 */
typedef struct {
    bool valid;
    uint8_t report_id;          /**< 0 when the interface uses no report IDs */
    uint16_t usage_page;        /**< Usage page of the enclosing collection */
    uint16_t usage;             /**< Usage of the enclosing collection */
    uint16_t input_bits;        /**< Total payload size in bits */

    int16_t kb_modifier_off;
    int16_t kb_keys_off;
    uint8_t kb_keys_count;

    int16_t ms_buttons_off;
    uint8_t ms_buttons_count;
    int16_t ms_x_off;
    int16_t ms_y_off;
    int16_t ms_wheel_off;
    uint8_t ms_x_bits;
    uint8_t ms_y_bits;
    uint8_t ms_wheel_bits;
} lua_hid_report_layout_t;

/**
 * @brief HID report descriptor parsing result: one layout per input report ID.
 */
typedef struct {
    int count;
    lua_hid_report_layout_t reports[LUA_USB_HID_MAX_REPORT_IDS];
} lua_hid_descriptor_analysis_t;

/**
 * @brief Parse an interface's report descriptor into one layout per input report ID.
 *
 * @param handle Interface to inspect
 * @param out    Analysis result, owned by the caller (too large for a task stack)
 */
void lua_hid_parse_report_descriptor(hid_host_device_handle_t handle,
                                     lua_hid_descriptor_analysis_t *out);

/**
 * @brief Initialize a layout to "no fields known" for the given report ID.
 */
void lua_hid_layout_init(lua_hid_report_layout_t *layout, uint8_t report_id);

/**
 * @brief Logical device type ("keyboard" / "mouse" / "generic") of a report.
 */
const char *lua_hid_layout_type(const lua_hid_report_layout_t *layout);

/**
 * @brief Extract an unsigned bit field from a report payload.
 *
 * @return false when the payload is too short for the field.
 */
bool lua_hid_bits_get(const uint8_t *data, size_t len,
                      int bit_off, uint8_t bit_cnt, uint32_t *out);

/**
 * @brief Extract a sign-extended bit field from a report payload.
 *
 * @return false when the payload is too short for the field.
 */
bool lua_hid_bits_get_signed(const uint8_t *data, size_t len,
                             int bit_off, uint8_t bit_cnt, int32_t *out);

#ifdef __cplusplus
}
#endif
