/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_usb_host_hid.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "lua.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lua_hid_report_parser.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_host_hid";

#define LUA_USB_HID_METATABLE       "usb_hid.device"

/* Registry sizing. Reports are buffered in pre-allocated per-slot rings so the
 * HID driver callback never allocates. */
#define LUA_USB_HID_MAX_DEVICES     8
#define LUA_USB_HID_REPORT_MAX_LEN  32
#define LUA_USB_HID_REPORT_RING_LEN 8
#define LUA_USB_HID_EVENT_QUEUE_LEN 16

#define LUA_USB_HID_USB_TASK_STACK  4096
#define LUA_USB_HID_USB_TASK_PRIO   2
#define LUA_USB_HID_EVT_TASK_STACK  4096
#define LUA_USB_HID_EVT_TASK_PRIO   4
#define LUA_USB_HID_DRV_TASK_STACK  4096
#define LUA_USB_HID_DRV_TASK_PRIO   5
#define LUA_USB_HID_TASK_CORE       0

#define LUA_USB_HID_LOCK_TIMEOUT_MS 50
#define LUA_USB_HID_USB_POLL_MS     100
#define LUA_USB_HID_SHUTDOWN_WAIT_MS 2000
#define LUA_USB_HID_CLIENT_CLEANUP_MS 20

typedef struct {
    uint8_t len;
    uint8_t data[LUA_USB_HID_REPORT_MAX_LEN];
} lua_usb_hid_report_t;

/**
 * @brief One connected HID interface.
 *
 * Slots are owned by the driver: they are filled when the HID stack reports a
 * CONNECTED interface and released on DISCONNECTED. Lua handles only reference
 * a slot by index plus generation, so a disconnect can never leave Lua holding
 * a dangling device pointer.
 *
 * For multiplexed interfaces (multiple report IDs), multiple slots can share
 * the same handle but differ by report_id.
 */
typedef struct {
    bool in_use;
    uint32_t generation;
    hid_host_device_handle_t handle;
    uint8_t proto;
    uint8_t sub_class;
    uint8_t addr;
    uint8_t iface_num;
    uint16_t vid;
    uint16_t pid;

    /* Device type recognition from the HID report descriptor. */
    const char *detected_type;      /**< "keyboard" / "mouse" / "generic" */

    /* Report ID this slot is bound to (0 = interface uses no report IDs) and
     * the field layout of that report. */
    uint8_t report_id;
    lua_hid_report_layout_t layout;

    lua_usb_hid_report_t ring[LUA_USB_HID_REPORT_RING_LEN];
    uint8_t ring_head;
    uint8_t ring_count;
} lua_usb_hid_slot_t;

/** @brief Lua-side reference to a registry slot. */
typedef struct {
    int slot;
    uint32_t generation;
} lua_usb_hid_ud_t;

/** @brief Driver-event item forwarded out of the HID driver task. */
typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} lua_usb_hid_evt_t;

/**
 * @brief Who owns the USB host library.
 *
 * The library is a process-wide singleton with a single event handler. Board
 * support (camera, audio) installs it on some boards, in which case this module
 * must share it: register the HID client, never pump events, never uninstall.
 */
typedef enum {
    LUA_USB_LIB_NONE = 0,   /**< Not installed, or released by us again */
    LUA_USB_LIB_OWNED,      /**< Installed by this module; we must release it */
    LUA_USB_LIB_FOREIGN,    /**< Installed by another component; hands off */
} lua_usb_lib_state_t;

static lua_usb_hid_slot_t s_slots[LUA_USB_HID_MAX_DEVICES];
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_evt_queue;
static SemaphoreHandle_t s_usb_ready;
static SemaphoreHandle_t s_usb_done;
static SemaphoreHandle_t s_evt_done;
static uint32_t s_generation_counter;
static volatile bool s_usb_run;
static volatile bool s_evt_run;
static volatile lua_usb_lib_state_t s_usb_lib;
/* Set when a teardown could not release something. Sticky: the driver cannot be
 * brought up again in this boot, and init() says so instead of failing later in
 * a less obvious place. */
static const char *s_fatal_error;
static bool s_hid_installed;
static bool s_initialized;

static inline bool lua_usb_hid_lock(void)
{
    return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(LUA_USB_HID_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static inline void lua_usb_hid_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* Interface-protocol based type, used when the report descriptor is missing. */
static const char *lua_usb_hid_proto_type_name(uint8_t proto)
{
    switch (proto) {
    case HID_PROTOCOL_KEYBOARD:
        return "keyboard";
    case HID_PROTOCOL_MOUSE:
        return "mouse";
    default:
        return "generic";
    }
}

static const char *lua_usb_hid_get_type_name(const lua_usb_hid_slot_t *slot)
{
    if (slot->detected_type) {
        return slot->detected_type;
    }
    return lua_usb_hid_proto_type_name(slot->proto);
}

/**
 * @brief Find the slot a report belongs to. Caller must hold the lock.
 *
 * For multiplexed interfaces several slots share one handle and differ by
 * report_id, so the report ID taken from the first data byte selects the slot.
 * A slot with report_id 0 is the non-multiplexed case and takes everything.
 * A report ID that matches no slot is dropped rather than mixed into another
 * logical device's queue.
 */
static lua_usb_hid_slot_t *lua_usb_hid_slot_by_handle_and_report_id(
    hid_host_device_handle_t handle, uint8_t report_id)
{
    lua_usb_hid_slot_t *unnumbered = NULL;

    for (int i = 0; i < LUA_USB_HID_MAX_DEVICES; i++) {
        if (!s_slots[i].in_use || s_slots[i].handle != handle) {
            continue;
        }
        if (s_slots[i].report_id == report_id) {
            return &s_slots[i];
        }
        if (s_slots[i].report_id == 0) {
            unnumbered = &s_slots[i];
        }
    }

    return unnumbered;
}

/* Caller must hold the lock. */
static void lua_usb_hid_slot_push_report(lua_usb_hid_slot_t *slot,
                                         const uint8_t *data,
                                         size_t len)
{
    uint8_t tail;

    if (len > LUA_USB_HID_REPORT_MAX_LEN) {
        len = LUA_USB_HID_REPORT_MAX_LEN;
    }

    tail = (uint8_t)((slot->ring_head + slot->ring_count) % LUA_USB_HID_REPORT_RING_LEN);
    memcpy(slot->ring[tail].data, data, len);
    slot->ring[tail].len = (uint8_t)len;

    if (slot->ring_count < LUA_USB_HID_REPORT_RING_LEN) {
        slot->ring_count++;
    } else {
        /* Ring full: drop the oldest report. */
        slot->ring_head = (uint8_t)((slot->ring_head + 1) % LUA_USB_HID_REPORT_RING_LEN);
    }
}

/**
 * @brief Per-interface callback, runs in the HID driver task.
 *
 * Input reports must be fetched here: the raw-report API is only valid while
 * handling HID_HOST_INTERFACE_EVENT_INPUT_REPORT.
 *
 * For multiplexed interfaces, this routes the report to the correct virtual slot
 * based on the report ID (first byte of data).
 */
static void lua_usb_hid_interface_cb(hid_host_device_handle_t handle,
                                     const hid_host_interface_event_t event,
                                     void *arg)
{
    uint8_t data[LUA_USB_HID_REPORT_MAX_LEN];
    size_t len = 0;
    lua_usb_hid_slot_t *slot;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        if (hid_host_device_get_raw_input_report_data(handle, data, sizeof(data), &len) != ESP_OK) {
            return;
        }
        if (!lua_usb_hid_lock()) {
            return;
        }

        // Extract report ID if present (first byte of data)
        uint8_t report_id = 0;
        if (len > 0 && data[0] != 0) {
            report_id = data[0];
        }

        // Route to the correct slot based on report ID
        slot = lua_usb_hid_slot_by_handle_and_report_id(handle, report_id);
        if (slot) {
            lua_usb_hid_slot_push_report(slot, data, len);
        }
        lua_usb_hid_unlock();
        break;

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        if (lua_usb_hid_lock()) {
            // Disconnect all slots using this handle
            for (int i = 0; i < LUA_USB_HID_MAX_DEVICES; i++) {
                if (s_slots[i].in_use && s_slots[i].handle == handle) {
                    ESP_LOGI(TAG, "device disconnected: slot=%d type=%s report_id=0x%02x",
                             i, s_slots[i].detected_type, s_slots[i].report_id);
                    s_slots[i].in_use = false;
                    s_slots[i].handle = NULL;
                    s_slots[i].ring_head = 0;
                    s_slots[i].ring_count = 0;
                }
            }
            lua_usb_hid_unlock();
        }
        hid_host_device_close(handle);
        break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "transfer error");
        break;

    default:
        break;
    }
}

/* Runs in the registry task. Opens the interface and starts the report stream. */
static void lua_usb_hid_handle_connect(hid_host_device_handle_t handle)
{
    const hid_host_device_config_t dev_config = {
        .callback = lua_usb_hid_interface_cb,
        .callback_arg = NULL,
    };
    hid_host_dev_params_t params;
    hid_host_dev_info_t info;
    lua_usb_hid_slot_t *slot = NULL;
    int index = -1;

    if (hid_host_device_get_params(handle, &params) != ESP_OK) {
        ESP_LOGE(TAG, "get_params failed on connect");
        return;
    }

    if (hid_host_device_open(handle, &dev_config) != ESP_OK) {
        ESP_LOGE(TAG, "device_open failed (iface=%u proto=%u)", params.iface_num, params.proto);
        return;
    }

    /* Boot-subclass devices report in whatever protocol they powered up in.
     * Pin them to the boot report layout so report bytes are predictable. */
    if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        if (hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT) != ESP_OK) {
            ESP_LOGW(TAG, "set_protocol(boot) failed (iface=%u)", params.iface_num);
        }
        if (params.proto == HID_PROTOCOL_KEYBOARD &&
                hid_class_request_set_idle(handle, 0, 0) != ESP_OK) {
            ESP_LOGW(TAG, "set_idle failed (iface=%u)", params.iface_num);
        }
    }

    if (hid_host_device_start(handle) != ESP_OK) {
        ESP_LOGE(TAG, "device_start failed (iface=%u)", params.iface_num);
        hid_host_device_close(handle);
        return;
    }

    if (hid_host_get_device_info(handle, &info) != ESP_OK) {
        memset(&info, 0, sizeof(info));
    }

    /* Analyze the report descriptor before touching the registry: every input
     * report ID becomes its own logical device, so a 2.4G combo receiver shows
     * up as a keyboard and a mouse even though it exposes a single physical
     * interface.
     *
     * The analysis buffer is static because it is too large for a task stack;
     * only the event task ever handles a connect, so no extra locking is
     * needed. Parsing outside the lock keeps the report callback responsive. */
    static lua_hid_descriptor_analysis_t analysis;

    lua_hid_parse_report_descriptor(handle, &analysis);

    if (!lua_usb_hid_lock()) {
        ESP_LOGE(TAG, "registry lock timeout on connect");
        hid_host_device_close(handle);
        return;
    }

    for (int i = 0; i < LUA_USB_HID_MAX_DEVICES; i++) {
        if (!s_slots[i].in_use) {
            slot = &s_slots[i];
            index = i;
            break;
        }
    }

    if (!slot) {
        lua_usb_hid_unlock();
        ESP_LOGE(TAG, "registry full, ignoring device (max=%d)", LUA_USB_HID_MAX_DEVICES);
        hid_host_device_close(handle);
        return;
    }

    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->generation = ++s_generation_counter;
    slot->handle = handle;
    slot->proto = params.proto;
    slot->sub_class = params.sub_class;
    slot->addr = params.addr;
    slot->iface_num = params.iface_num;
    slot->vid = info.VID;
    slot->pid = info.PID;
    slot->detected_type = lua_usb_hid_proto_type_name(params.proto);

    int devices_created = 0;

    for (int i = 0; i < analysis.count; i++) {
        const lua_hid_report_layout_t *layout = &analysis.reports[i];
        lua_usb_hid_slot_t *target = slot;
        int target_index = index;

        if (devices_created > 0) {
            target = NULL;
            for (int j = 0; j < LUA_USB_HID_MAX_DEVICES; j++) {
                if (!s_slots[j].in_use) {
                    target = &s_slots[j];
                    target_index = j;
                    break;
                }
            }
            if (!target) {
                ESP_LOGW(TAG, "registry full: report_id 0x%02x of iface %u not exposed",
                         layout->report_id, params.iface_num);
                break;
            }
            memcpy(target, slot, sizeof(*target));
            target->generation = ++s_generation_counter;
            memset(target->ring, 0, sizeof(target->ring));
            target->ring_head = 0;
            target->ring_count = 0;
        }

        target->report_id = layout->report_id;
        target->layout = *layout;
        target->detected_type = lua_hid_layout_type(layout);
        devices_created++;

        if (strcmp(target->detected_type, "generic") == 0) {
            ESP_LOGW(TAG, "id=%d report_id=0x%02x: no known usage "
                     "(page=0x%04x usage=0x%04x, %u payload bits); exposed as generic",
                     target_index + 1, layout->report_id, layout->usage_page,
                     layout->usage, (unsigned)layout->input_bits);
        }
        ESP_LOGI(TAG, "device ready: id=%d type=%s report_id=0x%02x vid=0x%04x pid=0x%04x iface=%u",
                 target_index + 1, target->detected_type, target->report_id,
                 slot->vid, slot->pid, params.iface_num);
    }

    if (devices_created == 0) {
        /* No usable descriptor: fall back to the interface protocol, which is
         * only meaningful for boot-subclass devices. */
        lua_hid_layout_init(&slot->layout, 0);
        if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE &&
                params.proto == HID_PROTOCOL_KEYBOARD) {
            /* Boot keyboard: [mod][reserved][key1..key6] */
            slot->layout.kb_modifier_off = 0;
            slot->layout.kb_keys_off = 16;
            slot->layout.kb_keys_count = HID_KEYBOARD_KEY_MAX;
            slot->layout.input_bits = 64;
        } else if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE &&
                   params.proto == HID_PROTOCOL_MOUSE) {
            /* Boot mouse: [buttons][dx][dy] */
            slot->layout.ms_buttons_off = 0;
            slot->layout.ms_buttons_count = 8;
            slot->layout.ms_x_off = 8;
            slot->layout.ms_x_bits = 8;
            slot->layout.ms_y_off = 16;
            slot->layout.ms_y_bits = 8;
            slot->layout.input_bits = 24;
        } else {
            slot->layout.valid = false;
        }
        ESP_LOGW(TAG, "device ready: id=%d type=%s (descriptor gave no reports) "
                 "vid=0x%04x pid=0x%04x iface=%u",
                 index + 1, slot->detected_type, slot->vid, slot->pid, params.iface_num);
    }

    lua_usb_hid_unlock();
}

/**
 * @brief Driver-event callback, runs in the HID driver task.
 *
 * Only forwards. Opening an interface issues control transfers and must not run
 * inside the driver's own event task.
 */
static void lua_usb_hid_driver_cb(hid_host_device_handle_t handle,
                                  const hid_host_driver_event_t event,
                                  void *arg)
{
    lua_usb_hid_evt_t item = {
        .handle = handle,
        .event = event,
    };

    if (s_evt_queue) {
        xQueueSend(s_evt_queue, &item, 0);
    }
}

static void lua_usb_hid_event_task(void *arg)
{
    lua_usb_hid_evt_t item;

    while (s_evt_run) {
        if (xQueueReceive(s_evt_queue, &item, pdMS_TO_TICKS(LUA_USB_HID_USB_POLL_MS)) != pdTRUE) {
            continue;
        }
        if (item.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
            lua_usb_hid_handle_connect(item.handle);
        }
    }

    xSemaphoreGive(s_evt_done);
    vTaskDelete(NULL);
}

/**
 * @brief Release the host library once our HID client is gone.
 *
 * Runs in the USB task, after the root port has been powered off by the
 * teardown path. With the port unpowered no device can re-enumerate, so
 * freeing the remaining devices settles instead of looping: freeing a device
 * while the port is still powered makes the hub recycle the port and enumerate
 * the very same device again, which keeps `process_pending_flags` non-zero and
 * makes usb_host_uninstall() fail forever.
 *
 * @return ESP_OK when the host library was released.
 */
static esp_err_t lua_usb_hid_drain_and_uninstall(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LUA_USB_HID_SHUTDOWN_WAIT_MS);
    usb_host_lib_info_t info = { 0 };
    esp_err_t err = ESP_ERR_INVALID_STATE;

    /* The device count is polled rather than waiting for ALL_FREE: that flag is
     * cleared by whoever calls handle_events first, so it can easily have been
     * consumed by the main loop already. */
    do {
        uint32_t event_flags = 0;

        usb_host_lib_handle_events(pdMS_TO_TICKS(LUA_USB_HID_CLIENT_CLEANUP_MS), &event_flags);

        if (usb_host_lib_info(&info) == ESP_OK && info.num_devices > 0) {
            usb_host_device_free_all();
            continue;
        }
        err = usb_host_uninstall();
    } while (err != ESP_OK && xTaskGetTickCount() < deadline);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "host library still busy: %d device(s), %d client(s)",
                 info.num_devices, info.num_clients);
    }

    return err;
}

static void lua_usb_hid_usb_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    esp_err_t err;

    err = usb_host_install(&host_config);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Another owner (typically the board's camera/audio support) already
         * installed the host library and runs its own event loop. Share it:
         * register the HID client only, never pump events from here (the
         * library allows a single event handler) and never uninstall. */
        ESP_LOGI(TAG, "USB host already installed by another component; sharing it");
        s_usb_lib = LUA_USB_LIB_FOREIGN;
        xSemaphoreGive(s_usb_ready);
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        s_usb_lib = LUA_USB_LIB_NONE;
        xSemaphoreGive(s_usb_ready);
        vTaskDelete(NULL);
        return;
    }

    s_usb_lib = LUA_USB_LIB_OWNED;
    xSemaphoreGive(s_usb_ready);

    while (s_usb_run) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(LUA_USB_HID_USB_POLL_MS), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }

    /* The HID client is already deregistered and the root port powered off by
     * the time the flag is cleared. */
    err = lua_usb_hid_drain_and_uninstall();
    if (err != ESP_OK) {
        /* Keeping the state set stops init() from failing later, and much less
         * clearly, inside usb_host_install(). */
        ESP_LOGE(TAG, "usb_host_uninstall failed: %s", esp_err_to_name(err));
    } else {
        s_usb_lib = LUA_USB_LIB_NONE;
        ESP_LOGI(TAG, "USB host stopped");
    }

    xSemaphoreGive(s_usb_done);
    vTaskDelete(NULL);
}

/* Caller must NOT hold the lock. Closes every open interface exactly once;
 * virtual devices of one multiplexed interface share a handle. */
static void lua_usb_hid_close_all_interfaces(void)
{
    hid_host_device_handle_t closed[LUA_USB_HID_MAX_DEVICES];
    int closed_count = 0;

    if (!lua_usb_hid_lock()) {
        return;
    }

    for (int i = 0; i < LUA_USB_HID_MAX_DEVICES; i++) {
        if (!s_slots[i].in_use) {
            continue;
        }
        bool already_closed = false;
        for (int j = 0; j < closed_count; j++) {
            if (closed[j] == s_slots[i].handle) {
                already_closed = true;
                break;
            }
        }
        if (!already_closed) {
            hid_host_device_close(s_slots[i].handle);
            closed[closed_count++] = s_slots[i].handle;
        }
        s_slots[i].in_use = false;
        s_slots[i].handle = NULL;
    }

    lua_usb_hid_unlock();
}

/**
 * @brief Release every resource so a later init() starts from a clean state.
 *
 * Order matters, and getting it wrong is what made earlier versions leak the
 * host library:
 *
 *  1. The event task issues control transfers through the HID client, so it
 *     must stop before the client goes away.
 *  2. Interfaces are closed, then the root port is powered off. Powering the
 *     port off disconnects the attached devices for good; without it, freeing
 *     a device makes the hub recycle the port and immediately re-enumerate the
 *     same device, and the host library never becomes idle enough to uninstall.
 *  3. hid_host_uninstall() refuses to run while the HID driver still tracks a
 *     device, so it is retried until the disconnects have been processed.
 *  4. Only then may the USB task free the devices and uninstall the library.
 *
 * @return NULL on success, otherwise a static string describing what could not
 *         be released.
 */
static const char *lua_usb_hid_teardown(void)
{
    const char *error = NULL;

    /* 1. Stop the event task and wait for it to leave the queue. */
    s_evt_run = false;
    if (s_evt_done) {
        xSemaphoreTake(s_evt_done, pdMS_TO_TICKS(LUA_USB_HID_SHUTDOWN_WAIT_MS));
    }

    /* 2. Close the interfaces and cut power to the root port. */
    if (s_hid_installed) {
        lua_usb_hid_close_all_interfaces();
    }

    if (s_usb_lib == LUA_USB_LIB_OWNED) {
        esp_err_t err = usb_host_lib_set_root_port_power(false);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "root port power off failed: %s", esp_err_to_name(err));
        }
    }

    /* 3. Deregister the HID client once the driver has seen the disconnects. */
    if (s_hid_installed) {
        esp_err_t err = ESP_FAIL;
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LUA_USB_HID_SHUTDOWN_WAIT_MS);
        do {
            err = hid_host_uninstall();
            if (err == ESP_OK) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(LUA_USB_HID_CLIENT_CLEANUP_MS));
        } while (xTaskGetTickCount() < deadline);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "hid_host_uninstall failed: %s", esp_err_to_name(err));
            error = "the HID client could not be deregistered";
        }
        s_hid_installed = false;
    }

    /* 4. Both the interfaces and the client are gone; release the library.
     *    A library installed by another component is left alone. */
    if (s_usb_lib == LUA_USB_LIB_OWNED) {
        s_usb_run = false;
        if (s_usb_done) {
            xSemaphoreTake(s_usb_done, pdMS_TO_TICKS(LUA_USB_HID_SHUTDOWN_WAIT_MS * 2));
        }
        if (s_usb_lib == LUA_USB_LIB_OWNED && !error) {
            error = "the USB host library could not be released";
        }
    } else if (s_usb_lib == LUA_USB_LIB_FOREIGN) {
        /* Never started an event task for a shared library. */
        s_usb_lib = LUA_USB_LIB_NONE;
    }

    s_usb_run = false;

    /* 5. Both tasks are gone; the shared objects can be released.
     *
     * Only when everything was released, though: a HID driver that is still
     * alive can still invoke our interface callback, which takes s_lock. Losing
     * a mutex is preferable to freeing one that is about to be used. The sticky
     * error below keeps init() from running again in that state. */
    if (error) {
        s_fatal_error = error;
        ESP_LOGE(TAG, "teardown incomplete: %s", error);
        return error;
    }

    if (s_evt_queue) {
        vQueueDelete(s_evt_queue);
        s_evt_queue = NULL;
    }
    if (s_usb_ready) {
        vSemaphoreDelete(s_usb_ready);
        s_usb_ready = NULL;
    }
    if (s_usb_done) {
        vSemaphoreDelete(s_usb_done);
        s_usb_done = NULL;
    }
    if (s_evt_done) {
        vSemaphoreDelete(s_evt_done);
        s_evt_done = NULL;
    }
    if (s_lock) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }

    memset(s_slots, 0, sizeof(s_slots));
    s_initialized = false;
    return NULL;
}

static int lua_usb_hid_init(lua_State *L)
{
    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = LUA_USB_HID_DRV_TASK_PRIO,
        .stack_size = LUA_USB_HID_DRV_TASK_STACK,
        .core_id = LUA_USB_HID_TASK_CORE,
        .callback = lua_usb_hid_driver_cb,
        .callback_arg = NULL,
    };

    if (s_initialized) {
        lua_pushboolean(L, 1);
        return 1;
    }

    /* A previous teardown that could not release everything must not be papered
     * over: usb_host_install() would fail with a confusing error instead. */
    if (s_fatal_error) {
        return luaL_error(L, "a previous deinit() left the USB stack in place (%s); "
                          "the device must be restarted", s_fatal_error);
    }

    /* Any earlier partial init is discarded rather than latched. */
    (void)lua_usb_hid_teardown();

    s_lock = xSemaphoreCreateMutex();
    s_evt_queue = xQueueCreate(LUA_USB_HID_EVENT_QUEUE_LEN, sizeof(lua_usb_hid_evt_t));
    s_usb_ready = xSemaphoreCreateBinary();
    s_usb_done = xSemaphoreCreateBinary();
    s_evt_done = xSemaphoreCreateBinary();

    if (!s_lock || !s_evt_queue || !s_usb_ready || !s_usb_done || !s_evt_done) {
        (void)lua_usb_hid_teardown();
        return luaL_error(L, "failed to allocate driver resources");
    }

    s_usb_run = true;
    s_evt_run = true;

    if (xTaskCreatePinnedToCore(lua_usb_hid_usb_task, "usb_hid_lib",
                                LUA_USB_HID_USB_TASK_STACK, NULL,
                                LUA_USB_HID_USB_TASK_PRIO, NULL,
                                LUA_USB_HID_TASK_CORE) != pdPASS) {
        s_usb_run = false;
        s_evt_run = false;
        (void)lua_usb_hid_teardown();
        return luaL_error(L, "failed to start USB host task");
    }

    xSemaphoreTake(s_usb_ready, portMAX_DELAY);
    if (s_usb_lib == LUA_USB_LIB_NONE) {
        s_usb_run = false;
        s_evt_run = false;
        (void)lua_usb_hid_teardown();
        return luaL_error(L, "USB host install failed (check board USB host support)");
    }

    if (xTaskCreatePinnedToCore(lua_usb_hid_event_task, "usb_hid_evt",
                                LUA_USB_HID_EVT_TASK_STACK, NULL,
                                LUA_USB_HID_EVT_TASK_PRIO, NULL,
                                LUA_USB_HID_TASK_CORE) != pdPASS) {
        s_evt_run = false;
        (void)lua_usb_hid_teardown();
        return luaL_error(L, "failed to start HID event task");
    }

    if (hid_host_install(&hid_config) != ESP_OK) {
        (void)lua_usb_hid_teardown();
        return luaL_error(L, "hid_host_install failed");
    }

    s_hid_installed = true;
    s_initialized = true;
    ESP_LOGI(TAG, "USB HID host started (%s USB host library)",
             s_usb_lib == LUA_USB_LIB_OWNED ? "own" : "shared");

    lua_pushboolean(L, 1);
    return 1;
}

/**
 * @brief Release the driver.
 *
 * Returns `false, reason` instead of `true` when something could not be
 * released: a caller that sees `true` may rely on `init()` working again.
 */
static int lua_usb_hid_deinit(lua_State *L)
{
    const char *error = lua_usb_hid_teardown();

    if (error) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, error);
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_usb_hid_get_devices(lua_State *L)
{
    int n = 0;

    if (!s_initialized) {
        return luaL_error(L, "usb_host_hid not initialized, call init() first");
    }
    if (!lua_usb_hid_lock()) {
        return luaL_error(L, "registry busy");
    }

    lua_newtable(L);
    for (int i = 0; i < LUA_USB_HID_MAX_DEVICES; i++) {
        if (!s_slots[i].in_use) {
            continue;
        }

        lua_newtable(L);
        lua_pushinteger(L, i + 1);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, lua_usb_hid_get_type_name(&s_slots[i]));
        lua_setfield(L, -2, "type");
        lua_pushstring(L, s_slots[i].detected_type);
        lua_setfield(L, -2, "detected_type");
        lua_pushinteger(L, s_slots[i].vid);
        lua_setfield(L, -2, "vendor_id");
        lua_pushinteger(L, s_slots[i].pid);
        lua_setfield(L, -2, "product_id");
        lua_pushinteger(L, s_slots[i].iface_num);
        lua_setfield(L, -2, "interface");
        lua_pushboolean(L, s_slots[i].sub_class == HID_SUBCLASS_BOOT_INTERFACE);
        lua_setfield(L, -2, "boot_protocol");
        lua_pushinteger(L, s_slots[i].report_id);
        lua_setfield(L, -2, "report_id");
        lua_pushinteger(L, s_slots[i].layout.usage_page);
        lua_setfield(L, -2, "usage_page");
        lua_pushinteger(L, s_slots[i].layout.usage);
        lua_setfield(L, -2, "usage");
        lua_pushboolean(L, s_slots[i].layout.valid);
        lua_setfield(L, -2, "parsed");

        lua_rawseti(L, -2, ++n);
    }
    lua_usb_hid_unlock();

    return 1;
}

/* Caller must NOT hold the lock: pushing may trigger a GC step. */
static void lua_usb_hid_push_device(lua_State *L, int index, uint32_t generation)
{
    lua_usb_hid_ud_t *ud = (lua_usb_hid_ud_t *)lua_newuserdata(L, sizeof(*ud));

    ud->slot = index;
    ud->generation = generation;

    luaL_getmetatable(L, LUA_USB_HID_METATABLE);
    lua_setmetatable(L, -2);

    ESP_LOGD(TAG, "device handle -> slot=%d gen=%u", index, (unsigned)generation);
}

static int lua_usb_hid_open(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    int index = (int)id - 1;
    uint32_t generation;

    if (!s_initialized) {
        return luaL_error(L, "usb_host_hid not initialized, call init() first");
    }

    luaL_argcheck(L, index >= 0 && index < LUA_USB_HID_MAX_DEVICES, 1, "invalid device id");

    if (!lua_usb_hid_lock()) {
        return luaL_error(L, "registry busy");
    }
    if (!s_slots[index].in_use) {
        lua_usb_hid_unlock();
        return luaL_error(L, "device %d is not connected", (int)id);
    }
    generation = s_slots[index].generation;
    lua_usb_hid_unlock();

    lua_usb_hid_push_device(L, index, generation);
    return 1;
}

/**
 * @brief Find the first connected device of a given type.
 *
 * Removes the need to plumb ids by hand, which is the step scripts most often
 * get wrong (passing an array position instead of the `id` field).
 */
static int lua_usb_hid_find(lua_State *L)
{
    const char *wanted = luaL_checkstring(L, 1);
    int index = -1;
    uint32_t generation = 0;

    if (!s_initialized) {
        return luaL_error(L, "usb_host_hid not initialized, call init() first");
    }

    if (!lua_usb_hid_lock()) {
        return luaL_error(L, "registry busy");
    }
    for (int i = 0; i < LUA_USB_HID_MAX_DEVICES; i++) {
        if (s_slots[i].in_use &&
                strcmp(lua_usb_hid_get_type_name(&s_slots[i]), wanted) == 0) {
            index = i;
            generation = s_slots[i].generation;
            break;
        }
    }
    lua_usb_hid_unlock();

    if (index < 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_usb_hid_push_device(L, index, generation);
    return 1;
}

/** @brief True when the value is a device handle returned by open()/find(). */
static int lua_usb_hid_is_device(lua_State *L)
{
    void *ud = lua_touserdata(L, 1);
    int match = 0;

    if (ud && lua_getmetatable(L, 1)) {
        luaL_getmetatable(L, LUA_USB_HID_METATABLE);
        match = lua_rawequal(L, -1, -2);
        lua_pop(L, 2);
    }

    lua_pushboolean(L, match);
    return 1;
}

/* Caller must hold the lock. Returns NULL if the device went away. */
static lua_usb_hid_slot_t *lua_usb_hid_resolve(lua_usb_hid_ud_t *ud)
{
    lua_usb_hid_slot_t *slot;

    if (ud->slot < 0 || ud->slot >= LUA_USB_HID_MAX_DEVICES) {
        return NULL;
    }
    slot = &s_slots[ud->slot];
    if (!slot->in_use || slot->generation != ud->generation) {
        return NULL;
    }
    return slot;
}

static void lua_hid_push_field(lua_State *L, const char *name, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name);
}

/* Pushes the keyboard view of a payload described by @p layout. */
static void lua_hid_push_keyboard(lua_State *L, const lua_hid_report_layout_t *layout,
                                  const uint8_t *payload, size_t len)
{
    uint32_t modifier = 0;

    if (layout->kb_modifier_off >= 0 &&
            lua_hid_bits_get(payload, len, layout->kb_modifier_off, 8, &modifier)) {
        lua_hid_push_field(L, "modifier", modifier);
    }

    if (layout->kb_keys_off < 0) {
        return;
    }

    lua_newtable(L);
    int n = 0;
    for (uint8_t i = 0; i < layout->kb_keys_count; i++) {
        uint32_t key = 0;
        if (!lua_hid_bits_get(payload, len, layout->kb_keys_off + i * 8, 8, &key)) {
            break;
        }
        if (key > HID_KEY_ERROR_UNDEFINED) {
            lua_pushinteger(L, key);
            lua_rawseti(L, -2, ++n);
        }
    }
    lua_setfield(L, -2, "keys");
}

/* Pushes the mouse view of a payload described by @p layout. */
static void lua_hid_push_mouse(lua_State *L, const lua_hid_report_layout_t *layout,
                               const uint8_t *payload, size_t len)
{
    int32_t value = 0;

    if (layout->ms_x_off >= 0 &&
            lua_hid_bits_get_signed(payload, len, layout->ms_x_off, layout->ms_x_bits, &value)) {
        lua_hid_push_field(L, "dx", value);
    }
    if (layout->ms_y_off >= 0 &&
            lua_hid_bits_get_signed(payload, len, layout->ms_y_off, layout->ms_y_bits, &value)) {
        lua_hid_push_field(L, "dy", value);
    }
    if (layout->ms_wheel_off >= 0 &&
            lua_hid_bits_get_signed(payload, len, layout->ms_wheel_off,
                                    layout->ms_wheel_bits, &value)) {
        lua_hid_push_field(L, "wheel", value);
    }

    if (layout->ms_buttons_off < 0) {
        return;
    }

    uint32_t buttons = 0;
    const uint8_t count = layout->ms_buttons_count > 32 ? 32 : layout->ms_buttons_count;
    if (!lua_hid_bits_get(payload, len, layout->ms_buttons_off, count, &buttons)) {
        return;
    }

    lua_newtable(L);
    lua_pushboolean(L, buttons & 0x01);
    lua_setfield(L, -2, "left");
    lua_pushboolean(L, buttons & 0x02);
    lua_setfield(L, -2, "right");
    lua_pushboolean(L, buttons & 0x04);
    lua_setfield(L, -2, "middle");
    /* Extra buttons stay available as the raw bitmap. */
    lua_pushinteger(L, buttons);
    lua_setfield(L, -2, "mask");
    lua_setfield(L, -2, "buttons");
}

/**
 * @brief Push a report table: always the raw bytes, plus parsed fields when the
 *        report layout is known.
 *
 * The raw `data` array keeps the report ID byte; parsed fields are computed on
 * the payload after it, matching the offsets taken from the report descriptor.
 * Parsing does not require the boot subclass: cheap 2.4G receivers rarely
 * declare it even though their reports are perfectly describable.
 */
static void lua_usb_hid_push_report(lua_State *L,
                                    const lua_usb_hid_report_t *report,
                                    const char *type_name,
                                    const lua_hid_report_layout_t *layout,
                                    uint8_t report_id)
{
    lua_newtable(L);

    lua_pushstring(L, type_name);
    lua_setfield(L, -2, "type");

    /* Always expose raw data including any Report ID byte. */
    lua_newtable(L);
    for (uint8_t i = 0; i < report->len; i++) {
        lua_pushinteger(L, report->data[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "data");

    if (report_id != 0) {
        lua_hid_push_field(L, "report_id", report_id);
    }

    if (!layout->valid) {
        return;
    }

    const uint8_t skip = (report_id != 0) ? 1 : 0;
    if (report->len <= skip) {
        return;
    }
    const uint8_t *payload = report->data + skip;
    const size_t payload_len = report->len - skip;

    if (strcmp(type_name, "keyboard") == 0) {
        lua_hid_push_keyboard(L, layout, payload, payload_len);
    } else if (strcmp(type_name, "mouse") == 0) {
        lua_hid_push_mouse(L, layout, payload, payload_len);
    }
}

static int lua_usb_hid_device_read(lua_State *L)
{
    lua_usb_hid_ud_t *ud = (lua_usb_hid_ud_t *)luaL_checkudata(L, 1, LUA_USB_HID_METATABLE);
    lua_usb_hid_report_t report;
    lua_hid_report_layout_t layout;
    lua_usb_hid_slot_t *slot;
    const char *type_name;
    uint8_t report_id;

    if (!lua_usb_hid_lock()) {
        return luaL_error(L, "registry busy");
    }

    slot = lua_usb_hid_resolve(ud);
    if (!slot) {
        lua_usb_hid_unlock();
        return luaL_error(L, "device disconnected");
    }

    if (slot->ring_count == 0) {
        lua_usb_hid_unlock();
        lua_pushnil(L);
        return 1;
    }

    report = slot->ring[slot->ring_head];
    slot->ring_head = (uint8_t)((slot->ring_head + 1) % LUA_USB_HID_REPORT_RING_LEN);
    slot->ring_count--;
    layout = slot->layout;
    type_name = lua_usb_hid_get_type_name(slot);
    report_id = slot->report_id;
    lua_usb_hid_unlock();

    lua_usb_hid_push_report(L, &report, type_name, &layout, report_id);
    return 1;
}

static int lua_usb_hid_device_get_info(lua_State *L)
{
    lua_usb_hid_ud_t *ud = (lua_usb_hid_ud_t *)luaL_checkudata(L, 1, LUA_USB_HID_METATABLE);
    lua_usb_hid_slot_t *slot;

    if (!lua_usb_hid_lock()) {
        return luaL_error(L, "registry busy");
    }

    slot = lua_usb_hid_resolve(ud);
    if (!slot) {
        lua_usb_hid_unlock();
        return luaL_error(L, "device disconnected");
    }

    lua_newtable(L);
    lua_pushinteger(L, ud->slot + 1);
    lua_setfield(L, -2, "id");
    lua_pushstring(L, lua_usb_hid_get_type_name(slot));
    lua_setfield(L, -2, "type");
    lua_pushstring(L, slot->detected_type);
    lua_setfield(L, -2, "detected_type");
    lua_pushinteger(L, slot->vid);
    lua_setfield(L, -2, "vendor_id");
    lua_pushinteger(L, slot->pid);
    lua_setfield(L, -2, "product_id");
    lua_pushinteger(L, slot->iface_num);
    lua_setfield(L, -2, "interface");
    lua_pushboolean(L, slot->sub_class == HID_SUBCLASS_BOOT_INTERFACE);
    lua_setfield(L, -2, "boot_protocol");
    lua_pushinteger(L, slot->report_id);
    lua_setfield(L, -2, "report_id");
    lua_pushinteger(L, slot->layout.usage_page);
    lua_setfield(L, -2, "usage_page");
    lua_pushinteger(L, slot->layout.usage);
    lua_setfield(L, -2, "usage");
    lua_pushboolean(L, slot->layout.valid);
    lua_setfield(L, -2, "parsed");
    lua_pushinteger(L, slot->ring_count);
    lua_setfield(L, -2, "pending");
    lua_usb_hid_unlock();

    return 1;
}

static int lua_usb_hid_device_connected(lua_State *L)
{
    lua_usb_hid_ud_t *ud = (lua_usb_hid_ud_t *)luaL_checkudata(L, 1, LUA_USB_HID_METATABLE);
    bool connected;

    if (!lua_usb_hid_lock()) {
        return luaL_error(L, "registry busy");
    }
    connected = lua_usb_hid_resolve(ud) != NULL;
    lua_usb_hid_unlock();

    lua_pushboolean(L, connected);
    return 1;
}

/**
 * @brief Drop the Lua reference to a device.
 *
 * The USB interface itself stays open: the registry owns it for as long as the
 * device is plugged in, so other scripts keep receiving reports.
 */
static int lua_usb_hid_device_close(lua_State *L)
{
    lua_usb_hid_ud_t *ud = (lua_usb_hid_ud_t *)luaL_checkudata(L, 1, LUA_USB_HID_METATABLE);

    ud->slot = -1;
    ud->generation = 0;
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg s_device_methods[] = {
    { "read", lua_usb_hid_device_read },
    { "get_info", lua_usb_hid_device_get_info },
    { "is_connected", lua_usb_hid_device_connected },
    { "close", lua_usb_hid_device_close },
    { NULL, NULL }
};

static const luaL_Reg s_module_functions[] = {
    { "init", lua_usb_hid_init },
    { "deinit", lua_usb_hid_deinit },
    { "get_devices", lua_usb_hid_get_devices },
    { "open", lua_usb_hid_open },
    { "find", lua_usb_hid_find },
    { "is_device", lua_usb_hid_is_device },
    { NULL, NULL }
};

int luaopen_usb_host_hid(lua_State *L)
{
    luaL_newmetatable(L, LUA_USB_HID_METATABLE);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    /* Makes tostring(dev) and type errors name the device type explicitly. */
    lua_pushstring(L, LUA_USB_HID_METATABLE);
    lua_setfield(L, -2, "__name");
    luaL_setfuncs(L, s_device_methods, 0);
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_setfuncs(L, s_module_functions, 0);
    return 1;
}

esp_err_t lua_driver_usb_host_hid_register(void)
{
    return cap_lua_register_module("usb_host_hid", luaopen_usb_host_hid);
}
