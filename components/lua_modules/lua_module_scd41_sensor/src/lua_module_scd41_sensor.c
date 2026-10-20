/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_scd41_sensor.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "cap_lua_hw.h"
#include "driver/i2c_master.h"
#include "esp_board_device.h"
#include "esp_board_manager.h"
#include "lua_driver_i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"

#define LUA_MODULE_SCD41_NAME                    "scd41_sensor"
#define LUA_MODULE_SCD41_METATABLE               "scd41_sensor.device"
#define LUA_MODULE_SCD41_DEFAULT_NAME            "scd41_sensor"
#define LUA_MODULE_SCD41_MAX_NAME_LEN            64
#define LUA_MODULE_SCD41_DEFAULT_FREQ_HZ         100000
#define LUA_MODULE_SCD41_DEFAULT_TIMEOUT         1000
/* One SCD41 conversion cycle is ~5 s; allow a little slack for the first read. */
#define LUA_MODULE_SCD41_DEFAULT_READY_TIMEOUT   6000
#define LUA_MODULE_SCD41_RETRY_INTERVAL_MS       500

#define SCD41_I2C_ADDR                     0x62

#define SCD41_START_MEASUREMENT            0x21B1
#define SCD41_STOP_MEASUREMENT             0x3F86
#define SCD41_READ_MEASUREMENT             0xEC05

typedef struct {
    uint16_t co2_ppm;
    float temperature;
    float humidity;
} scd41_data_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    lua_driver_i2c_bus_handle_t bus_owner;
    cap_lua_hw_lease_t lease;
    uint8_t i2c_addr;
    uint32_t timeout_ms;
    uint32_t ready_timeout_ms;
    bool initialized;
    bool measurement_started;
    /* Latest successfully read sample, shared by read()/read_co2()/... so that
     * sub-5 s reads return this instead of failing a too-early I2C read. */
    bool has_sample;
    scd41_data_t last_sample;
    char peripheral_name[LUA_MODULE_SCD41_MAX_NAME_LEN];
} lua_module_scd41_handle_t;

typedef struct {
    lua_module_scd41_handle_t *handle;
    char device_name[LUA_MODULE_SCD41_MAX_NAME_LEN];
} lua_module_scd41_ud_t;

static const char *TAG = "lua_module_scd41";

static void lua_module_scd41_release(lua_module_scd41_handle_t *handle);

static uint8_t lua_module_scd41_crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t bit = 8; bit > 0; --bit) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc = (crc << 1);
            }
        }
    }
    return crc;
}

static esp_err_t lua_module_scd41_write_command(lua_module_scd41_handle_t *handle,
                                               uint16_t command)
{
    if (handle == NULL || !handle->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t cmd_buffer[2] = {
        (uint8_t)((command >> 8) & 0xFF),
        (uint8_t)(command & 0xFF),
    };

    esp_err_t ret = i2c_master_transmit(handle->dev, cmd_buffer, sizeof(cmd_buffer),
                                        handle->timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2C write command failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t lua_module_scd41_read_data(lua_module_scd41_handle_t *handle,
                                            uint8_t *data,
                                            size_t length)
{
    if (handle == NULL || !handle->initialized || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = i2c_master_receive(handle->dev, data, length, handle->timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "I2C read data failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t lua_module_scd41_read_measurement(lua_module_scd41_handle_t *handle,
                                                   scd41_data_t *data)
{
    if (handle == NULL || !handle->initialized || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = lua_module_scd41_write_command(handle, SCD41_READ_MEASUREMENT);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t buffer[9];
    ret = lua_module_scd41_read_data(handle, buffer, 9);
    if (ret != ESP_OK) {
        return ret;
    }

    for (int i = 0; i < 3; i++) {
        uint8_t expected_crc = lua_module_scd41_crc8(&buffer[i * 3], 2);
        if (buffer[i * 3 + 2] != expected_crc) {
            ESP_LOGD(TAG, "CRC mismatch for measurement %d", i);
            return ESP_ERR_INVALID_CRC;
        }
    }

    data->co2_ppm = (buffer[0] << 8) | buffer[1];

    uint16_t temp_raw = (buffer[3] << 8) | buffer[4];
    data->temperature = -45.0f + 175.0f * ((float)temp_raw / 65535.0f);

    uint16_t hum_raw = (buffer[6] << 8) | buffer[7];
    data->humidity = 100.0f * ((float)hum_raw / 65535.0f);

    ESP_LOGD(TAG, "SCD41 - CO2: %d ppm, Temp: %.1f°C, Humidity: %.1f%%",
             data->co2_ppm, data->temperature, data->humidity);

    return ESP_OK;
}

/* ESP-IDF reports a device NACK as ESP_ERR_INVALID_STATE, and the SCD41 NACKs
 * its I2C commands while a conversion cycle is still in progress. Such failures
 * are transient: the caller keeps retrying instead of treating them as fatal
 * (treating them as fatal is what made reads fail instantly before). */
static bool lua_module_scd41_is_retryable(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE ||
           err == ESP_ERR_TIMEOUT ||
           err == ESP_ERR_INVALID_CRC;
}

/*
 * Return the newest available measurement.
 *
 * The SCD41 only answers a measurement read once a conversion cycle has
 * completed, NACKing in between. With force_fresh false and a cached sample a
 * single attempt is made: a new sample is returned when the read succeeds,
 * otherwise the cached sample is returned, so reads issued faster than the ~5 s
 * cycle never fail. Otherwise this blocks, retrying until a sample is read or
 * ready_timeout_ms elapses.
 */
static esp_err_t lua_module_scd41_acquire(lua_module_scd41_handle_t *handle,
                                          bool force_fresh,
                                          scd41_data_t *out)
{
    if (handle == NULL || !handle->initialized || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!handle->measurement_started) {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(handle->ready_timeout_ms);

    for (;;) {
        scd41_data_t fresh = { 0 };
        esp_err_t err = lua_module_scd41_read_measurement(handle, &fresh);
        if (err == ESP_OK) {
            handle->last_sample = fresh;
            handle->has_sample = true;
            *out = fresh;
            return ESP_OK;
        }
        if (!lua_module_scd41_is_retryable(err)) {
            return err;
        }
        /* No new sample yet: serve the cached one rather than block. */
        if (!force_fresh && handle->has_sample) {
            *out = handle->last_sample;
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(LUA_MODULE_SCD41_RETRY_INTERVAL_MS));
    }
}

static int lua_module_scd41_raise_read_error(lua_State *L, lua_module_scd41_ud_t *ud,
                                             const char *op, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE:
        return luaL_error(L, "scd41_sensor %s: measurement not started "
                             "(call start_measurement or use auto_start)", op);
    case ESP_ERR_TIMEOUT:
        /* lua_pushfstring only supports %d/%s/%f/%p/%U/%I, so no %u here. */
        return luaL_error(L, "scd41_sensor %s: no new measurement within %d ms "
                             "(check sensor wiring and power)", op,
                          (int)ud->handle->ready_timeout_ms);
    case ESP_ERR_INVALID_CRC:
        return luaL_error(L, "scd41_sensor %s: CRC check failed, I2C data corrupted", op);
    case ESP_ERR_INVALID_ARG:
        return luaL_error(L, "scd41_sensor %s: invalid handle or buffer", op);
    default:
        return luaL_error(L, "scd41_sensor %s: I2C error: %s", op, esp_err_to_name(err));
    }
}

static void lua_module_scd41_destroy_handle(lua_module_scd41_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }

    if (handle->initialized && handle->measurement_started) {
        lua_module_scd41_write_command(handle, SCD41_STOP_MEASUREMENT);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    lua_module_scd41_release(handle);
    free(handle);
}

typedef struct {
    char peripheral_name[LUA_MODULE_SCD41_MAX_NAME_LEN];
    int i2c_addr;
    int frequency;
    uint32_t timeout_ms;
    uint32_t ready_timeout_ms;
    bool auto_start;
    bool has_peripheral;
    bool has_frequency;
} lua_scd41_resolved_cfg_t;

/*
 * Local mirror of the dev_custom_scd41_sensor_config_t struct that the ESP
 * Board Manager auto-generates from board_devices.yaml. It MUST stay
 * byte-for-byte identical to the generated struct; the size is cross-checked
 * against the descriptor's cfg_size in lua_scd41_resolve_board_cfg().
 */
typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t i2c_addr;
    int32_t frequency;
    int8_t int_gpio_num;
    int8_t sdo_gpio_num;
    uint8_t peripheral_count;
    const char *peripheral_name;
} lua_scd41_board_cfg_t;

static esp_err_t lua_scd41_resolve_board_cfg(const char *device_name,
                                             const lua_scd41_board_cfg_t **out)
{
    extern const esp_board_device_desc_t g_esp_board_devices[];
    const esp_board_device_desc_t *desc = g_esp_board_devices;

    while (desc != NULL && desc->name != NULL) {
        if (strcmp(desc->name, device_name) == 0) {
            if (desc->cfg == NULL) {
                return ESP_ERR_NOT_FOUND;
            }
            if (desc->cfg_size != sizeof(lua_scd41_board_cfg_t)) {
                ESP_LOGE(TAG,
                         "Board device '%s' cfg_size=%u differs from expected %u",
                         device_name,
                         (unsigned)desc->cfg_size,
                         (unsigned)sizeof(lua_scd41_board_cfg_t));
                return ESP_ERR_INVALID_SIZE;
            }
            *out = (const lua_scd41_board_cfg_t *)desc->cfg;
            return ESP_OK;
        }
        desc = desc->next;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t lua_module_scd41_load_board_defaults(const char *device_name,
                                                      lua_scd41_resolved_cfg_t *out)
{
    const lua_scd41_board_cfg_t *board = NULL;
    esp_err_t err = lua_scd41_resolve_board_cfg(device_name, &board);
    if (err != ESP_OK) {
        return err;
    }

    if (board->peripheral_name != NULL && board->peripheral_name[0] != '\0') {
        snprintf(out->peripheral_name, sizeof(out->peripheral_name), "%s", board->peripheral_name);
        out->has_peripheral = true;
    }
    if (board->i2c_addr != 0) {
        out->i2c_addr = board->i2c_addr;
    }
    if (board->frequency > 0) {
        out->frequency = board->frequency;
        out->has_frequency = true;
    }

    return ESP_OK;
}

static void lua_module_scd41_apply_lua_overrides(lua_State *L, int opts_idx,
                                                 lua_scd41_resolved_cfg_t *cfg)
{
    if (opts_idx == 0 || lua_type(L, opts_idx) != LUA_TTABLE) {
        return;
    }

    lua_getfield(L, opts_idx, "peripheral");
    if (lua_isstring(L, -1)) {
        const char *p = lua_tostring(L, -1);
        snprintf(cfg->peripheral_name, sizeof(cfg->peripheral_name), "%s", p);
        cfg->has_peripheral = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "frequency");
    if (lua_isnumber(L, -1)) {
        cfg->frequency = (int)lua_tointeger(L, -1);
        cfg->has_frequency = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "timeout_ms");
    if (lua_isnumber(L, -1)) {
        cfg->timeout_ms = (uint32_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "ready_timeout_ms");
    if (lua_isnumber(L, -1)) {
        cfg->ready_timeout_ms = (uint32_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "auto_start");
    if (lua_isboolean(L, -1)) {
        cfg->auto_start = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
}

static void lua_module_scd41_release(lua_module_scd41_handle_t *handle)
{
    if (handle->dev != NULL) {
        i2c_master_bus_rm_device(handle->dev);
        handle->dev = NULL;
    }
    if (handle->lease.id) {
        (void)cap_lua_hw_release(&handle->lease);
        handle->lease.id = CLAW_HW_CLAIM_INVALID;
    }
    if (handle->bus_owner) {
        lua_driver_i2c_unref_bus(handle->bus_owner);
        handle->bus_owner = NULL;
    }
    handle->bus = NULL;
    handle->initialized = false;
}

static esp_err_t lua_module_scd41_create_handle(lua_State *L,
                                               const lua_scd41_resolved_cfg_t *cfg,
                                               lua_module_scd41_handle_t **out_handle)
{
    lua_module_scd41_handle_t *handle = calloc(1, sizeof(lua_module_scd41_handle_t));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(handle->peripheral_name, sizeof(handle->peripheral_name), "%s", cfg->peripheral_name);
    handle->i2c_addr = (uint8_t)cfg->i2c_addr;
    handle->timeout_ms = cfg->timeout_ms;
    handle->ready_timeout_ms = cfg->ready_timeout_ms;

    int port = -1;
    handle->bus = lua_driver_i2c_ref_bus(L, -1, &port, &handle->bus_owner);
    if (handle->bus == NULL) {
        ESP_LOGE(TAG, "Failed to reference the board I2C bus");
        free(handle);
        return ESP_ERR_INVALID_STATE;
    }
    char key[CLAW_HW_KEY_SIZE];
    const char *keys[] = {
        claw_hw_key_i2c_addr(key, sizeof(key), port, handle->i2c_addr),
    };
    esp_err_t err = cap_lua_hw_claim(L, &handle->lease, "lua/scd41", keys, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to claim SCD41 address 0x%02x on i2c%d: %s",
                 handle->i2c_addr, port, esp_err_to_name(err));
        lua_module_scd41_release(handle);
        free(handle);
        return err;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = handle->i2c_addr,
        .scl_speed_hz = (uint32_t)cfg->frequency,
    };
    err = i2c_master_bus_add_device(handle->bus, &dev_cfg, &handle->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SCD41 device 0x%02x on '%s': %s",
                 handle->i2c_addr, cfg->peripheral_name, esp_err_to_name(err));
        lua_module_scd41_release(handle);
        free(handle);
        return err;
    }

    handle->initialized = true;

    err = lua_module_scd41_write_command(handle, SCD41_STOP_MEASUREMENT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop SCD41 measurement: %s", esp_err_to_name(err));
        lua_module_scd41_release(handle);
        free(handle);
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    if (cfg->auto_start) {
        err = lua_module_scd41_write_command(handle, SCD41_START_MEASUREMENT);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start SCD41 measurement: %s", esp_err_to_name(err));
            lua_module_scd41_release(handle);
            free(handle);
            return err;
        }
        handle->measurement_started = true;
    }

    *out_handle = handle;
    ESP_LOGI(TAG, "SCD41 initialized on %s (I2C 0x%02x), timeout %u ms",
             cfg->peripheral_name, handle->i2c_addr, (unsigned)cfg->timeout_ms);
    return ESP_OK;
}

static lua_module_scd41_ud_t *lua_module_scd41_get_ud(lua_State *L, int idx)
{
    lua_module_scd41_ud_t *ud =
        (lua_module_scd41_ud_t *)luaL_checkudata(L, idx, LUA_MODULE_SCD41_METATABLE);
    if (!ud || !ud->handle || !ud->handle->initialized) {
        luaL_error(L, "scd41_sensor: invalid or closed handle");
    }
    return ud;
}

static int lua_module_scd41_close_impl(lua_State *L, lua_module_scd41_ud_t *ud)
{
    (void)L;
    if (ud->handle != NULL) {
        lua_module_scd41_destroy_handle(ud->handle);
        ud->handle = NULL;
    }
    ud->device_name[0] = '\0';
    return 0;
}

static int lua_module_scd41_gc(lua_State *L)
{
    lua_module_scd41_ud_t *ud =
        (lua_module_scd41_ud_t *)luaL_testudata(L, 1, LUA_MODULE_SCD41_METATABLE);
    if (ud && ud->handle) {
        return lua_module_scd41_close_impl(L, ud);
    }
    return 0;
}

static int lua_module_scd41_close(lua_State *L)
{
    lua_module_scd41_ud_t *ud =
        (lua_module_scd41_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_SCD41_METATABLE);
    if (ud->handle) {
        return lua_module_scd41_close_impl(L, ud);
    }
    return 0;
}

static int lua_module_scd41_name(lua_State *L)
{
    lua_module_scd41_ud_t *ud = lua_module_scd41_get_ud(L, 1);
    lua_pushstring(L, ud->device_name);
    return 1;
}

static int lua_module_scd41_read(lua_State *L)
{
    lua_module_scd41_ud_t *ud = lua_module_scd41_get_ud(L, 1);

    bool force_fresh = false;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "fresh");
        force_fresh = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }

    scd41_data_t data = { 0 };
    esp_err_t err = lua_module_scd41_acquire(ud->handle, force_fresh, &data);
    if (err != ESP_OK) {
        return lua_module_scd41_raise_read_error(L, ud, "read", err);
    }

    lua_newtable(L);
    lua_pushinteger(L, data.co2_ppm);
    lua_setfield(L, -2, "co2");
    lua_pushnumber(L, data.temperature);
    lua_setfield(L, -2, "temperature");
    lua_pushnumber(L, data.humidity);
    lua_setfield(L, -2, "humidity");
    return 1;
}

static int lua_module_scd41_read_co2(lua_State *L)
{
    lua_module_scd41_ud_t *ud = lua_module_scd41_get_ud(L, 1);
    scd41_data_t data = { 0 };

    esp_err_t err = lua_module_scd41_acquire(ud->handle, false, &data);
    if (err != ESP_OK) {
        return lua_module_scd41_raise_read_error(L, ud, "read_co2", err);
    }

    lua_pushinteger(L, data.co2_ppm);
    return 1;
}

static int lua_module_scd41_read_temperature(lua_State *L)
{
    lua_module_scd41_ud_t *ud = lua_module_scd41_get_ud(L, 1);
    scd41_data_t data = { 0 };

    esp_err_t err = lua_module_scd41_acquire(ud->handle, false, &data);
    if (err != ESP_OK) {
        return lua_module_scd41_raise_read_error(L, ud, "read_temperature", err);
    }

    lua_pushnumber(L, data.temperature);
    return 1;
}

static int lua_module_scd41_read_humidity(lua_State *L)
{
    lua_module_scd41_ud_t *ud = lua_module_scd41_get_ud(L, 1);
    scd41_data_t data = { 0 };

    esp_err_t err = lua_module_scd41_acquire(ud->handle, false, &data);
    if (err != ESP_OK) {
        return lua_module_scd41_raise_read_error(L, ud, "read_humidity", err);
    }

    lua_pushnumber(L, data.humidity);
    return 1;
}

static int lua_module_scd41_start_measurement(lua_State *L)
{
    lua_module_scd41_ud_t *ud = lua_module_scd41_get_ud(L, 1);

    /* Idempotent: starting an already-running measurement makes the sensor
     * NACK, so treat it as a no-op instead of raising. */
    if (ud->handle->measurement_started) {
        return 0;
    }

    if (lua_module_scd41_write_command(ud->handle, SCD41_START_MEASUREMENT) != ESP_OK) {
        return luaL_error(L, "scd41_sensor start_measurement failed");
    }

    /* Prior sample belongs to the previous measurement session. */
    ud->handle->has_sample = false;
    ud->handle->measurement_started = true;
    return 0;
}

static int lua_module_scd41_stop_measurement(lua_State *L)
{
    lua_module_scd41_ud_t *ud = lua_module_scd41_get_ud(L, 1);

    if (!ud->handle->measurement_started) {
        return 0;
    }

    if (lua_module_scd41_write_command(ud->handle, SCD41_STOP_MEASUREMENT) != ESP_OK) {
        return luaL_error(L, "scd41_sensor stop_measurement failed");
    }

    ud->handle->measurement_started = false;
    return 0;
}

static int lua_module_scd41_new(lua_State *L)
{
    const char *device_name = LUA_MODULE_SCD41_DEFAULT_NAME;
    int opts_idx = 0;

    if (lua_isstring(L, 1)) {
        device_name = lua_tostring(L, 1);
        if (lua_istable(L, 2)) {
            opts_idx = 2;
        }
    } else if (lua_istable(L, 1)) {
        opts_idx = 1;
        lua_getfield(L, 1, "device");
        if (lua_isstring(L, -1)) {
            device_name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    if (strlen(device_name) >= LUA_MODULE_SCD41_MAX_NAME_LEN) {
        return luaL_error(L, "scd41_sensor device name too long");
    }

    lua_scd41_resolved_cfg_t cfg = {
        .i2c_addr = SCD41_I2C_ADDR,
        .frequency = LUA_MODULE_SCD41_DEFAULT_FREQ_HZ,
        .timeout_ms = LUA_MODULE_SCD41_DEFAULT_TIMEOUT,
        .ready_timeout_ms = LUA_MODULE_SCD41_DEFAULT_READY_TIMEOUT,
        .auto_start = true,
    };

    esp_err_t err = lua_module_scd41_load_board_defaults(device_name, &cfg);
    const char *opened_device_name = device_name;

    if (err == ESP_ERR_INVALID_SIZE) {
        return luaL_error(L,
                          "scd41_sensor.new: board device '%s' config schema mismatch",
                          device_name);
    }
    if (err != ESP_OK && strcmp(device_name, LUA_MODULE_SCD41_DEFAULT_NAME) == 0) {
        ESP_LOGW(TAG, "Default device '%s' not declared in board config", device_name);
    }

    lua_module_scd41_apply_lua_overrides(L, opts_idx, &cfg);

    if (!cfg.has_peripheral) {
        return luaL_error(L, "scd41_sensor.new: missing 'peripheral' "
                              "(board declares no '%s', and no override given)", device_name);
    }
    if (!cfg.has_frequency) {
        cfg.frequency = LUA_MODULE_SCD41_DEFAULT_FREQ_HZ;
    }

    lua_module_scd41_handle_t *handle = NULL;
    err = lua_module_scd41_create_handle(L, &cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        return luaL_error(L, "scd41_sensor.new failed: %s",
                          esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
    }

    lua_module_scd41_ud_t *ud =
        (lua_module_scd41_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->handle = handle;
    snprintf(ud->device_name, sizeof(ud->device_name), "%s", opened_device_name);

    luaL_getmetatable(L, LUA_MODULE_SCD41_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static void lua_module_scd41_create_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_SCD41_METATABLE)) {
        lua_pushcfunction(L, lua_module_scd41_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_scd41_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_module_scd41_read_co2);
        lua_setfield(L, -2, "read_co2");
        lua_pushcfunction(L, lua_module_scd41_read_temperature);
        lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, lua_module_scd41_read_humidity);
        lua_setfield(L, -2, "read_humidity");
        lua_pushcfunction(L, lua_module_scd41_start_measurement);
        lua_setfield(L, -2, "start_measurement");
        lua_pushcfunction(L, lua_module_scd41_stop_measurement);
        lua_setfield(L, -2, "stop_measurement");
        lua_pushcfunction(L, lua_module_scd41_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_module_scd41_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);
}

int luaopen_scd41_sensor(lua_State *L)
{
    lua_module_scd41_create_metatable(L);

    lua_newtable(L);
    lua_pushcfunction(L, lua_module_scd41_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_scd41_sensor_register(void)
{
    return cap_lua_register_module(LUA_MODULE_SCD41_NAME,
                                   luaopen_scd41_sensor);
}
