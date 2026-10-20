/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QMI8658 backend for lua_module_imu. Register-level driver using the shared
 * i2c_bus device handle from the main module (same approach as MPU6050), so it
 * needs no external sensor component.
 */

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

#include "lua_module_imu_backend.h"

static const char *TAG = "lua_module_imu.qmi8658";

/* Register map */
#define QMI8658_REG_WHO_AM_I   0x00
#define QMI8658_REG_CTRL1      0x02
#define QMI8658_REG_CTRL2      0x03  /* accel: [6:4] full scale, [3:0] ODR */
#define QMI8658_REG_CTRL3      0x04  /* gyro : [6:4] full scale, [3:0] ODR */
#define QMI8658_REG_CTRL7      0x08  /* bit0 aEN, bit1 gEN */
#define QMI8658_REG_STATUS0    0x2E  /* bit0 aDA, bit1 gDA */
#define QMI8658_REG_TEMP_L     0x33
#define QMI8658_REG_ACCEL_L    0x35  /* AX..AZ then GX..GZ, little-endian int16 */

#define QMI8658_WHO_AM_I_VAL   0x05
#define QMI8658_RESET_CMD      0xB0
#define QMI8658_REG_RESET      0x60

/* CTRL1 0x60: address auto-increment, little-endian output.
 * CTRL2 0x18 = ±4 g (aFS=001), aODR=1000 (31.25 Hz).
 * CTRL3 0x58 = ±512 dps (gFS=101), gODR=1000 (31.25 Hz).
 * CTRL7 0x03 = enable accelerometer + gyroscope. */
#define QMI8658_CTRL1_VAL      0x60
#define QMI8658_CTRL2_VAL      0x18
#define QMI8658_CTRL3_VAL      0x58
#define QMI8658_CTRL7_VAL      0x03

/* At 31.25 Hz the output registers read 0 for the first few conversion periods
 * after enabling, which looks like a dead sensor. Wait long enough before the
 * first read that the caller gets real data. */
#define QMI8658_SETTLE_MS      300

#define QMI8658_ADDR_LOW       0x6A  /* SA0 = 0 */
#define QMI8658_ADDR_HIGH      0x6B  /* SA0 = 1 */

typedef struct {
    uint8_t status;
} qmi8658_state_t;

static esp_err_t qmi8658_write_reg(lua_imu_backend_ctx_t *ctx, uint8_t reg, uint8_t value)
{
    if (ctx->i2c_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(ctx->i2c_dev_handle, buf, sizeof(buf), 1000);
}

static esp_err_t qmi8658_read_regs(lua_imu_backend_ctx_t *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    if (ctx->i2c_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(ctx->i2c_dev_handle, &reg, 1, data, len, 1000);
}

static int16_t qmi8658_le16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static esp_err_t qmi8658_backend_probe(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    esp_err_t err = lua_imu_ctx_select_addr(ctx, i2c_addr);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t who = 0;
    err = qmi8658_read_regs(ctx, QMI8658_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed at 0x%02x: %s", i2c_addr, esp_err_to_name(err));
        return err;
    }
    if (who != QMI8658_WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "Not a QMI8658: WHO_AM_I=0x%02x at 0x%02x", who, i2c_addr);
        return ESP_ERR_NOT_FOUND;
    }

    /* Soft reset, then bring the chip up in ±4 g / ±512 dps at 31.25 Hz. */
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(ctx, QMI8658_REG_RESET, QMI8658_RESET_CMD), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(ctx, QMI8658_REG_CTRL1, QMI8658_CTRL1_VAL), TAG, "CTRL1 failed");
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(ctx, QMI8658_REG_CTRL2, QMI8658_CTRL2_VAL), TAG, "CTRL2 failed");
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(ctx, QMI8658_REG_CTRL3, QMI8658_CTRL3_VAL), TAG, "CTRL3 failed");
    ESP_RETURN_ON_ERROR(qmi8658_write_reg(ctx, QMI8658_REG_CTRL7, QMI8658_CTRL7_VAL), TAG, "CTRL7 failed");
    vTaskDelay(pdMS_TO_TICKS(QMI8658_SETTLE_MS));

    ESP_LOGI(TAG, "QMI8658 initialized at 0x%02x", i2c_addr);
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_sample(lua_imu_backend_ctx_t *ctx, lua_imu_sample_t *out)
{
    uint8_t raw[12] = { 0 };

    /* Accel (AX..AZ) and gyro (GX..GZ) are contiguous, read both in one burst. */
    esp_err_t err = qmi8658_read_regs(ctx, QMI8658_REG_ACCEL_L, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    out->accel.x = qmi8658_le16(&raw[0]);
    out->accel.y = qmi8658_le16(&raw[2]);
    out->accel.z = qmi8658_le16(&raw[4]);
    out->gyro.x = qmi8658_le16(&raw[6]);
    out->gyro.y = qmi8658_le16(&raw[8]);
    out->gyro.z = qmi8658_le16(&raw[10]);
    out->sens_time = esp_timer_get_time();

    qmi8658_state_t *st = (qmi8658_state_t *)ctx->state;
    uint8_t status = 0;
    if (st != NULL && qmi8658_read_regs(ctx, QMI8658_REG_STATUS0, &status, 1) == ESP_OK) {
        st->status = status;
    }
    out->status = (st != NULL) ? st->status : 0;
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_temperature(lua_imu_backend_ctx_t *ctx, int32_t *out)
{
    uint8_t raw[2] = { 0 };

    esp_err_t err = qmi8658_read_regs(ctx, QMI8658_REG_TEMP_L, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    /* Chip-defined raw units: signed 16-bit, 1 LSB = 1/256 °C. */
    *out = qmi8658_le16(raw);
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_int_status(lua_imu_backend_ctx_t *ctx, uint32_t *out)
{
    uint8_t status = 0;

    esp_err_t err = qmi8658_read_regs(ctx, QMI8658_REG_STATUS0, &status, 1);
    if (err != ESP_OK) {
        return err;
    }
    *out = status;
    return ESP_OK;
}

static bool qmi8658_backend_is_supported_addr(uint8_t i2c_addr)
{
    return i2c_addr == QMI8658_ADDR_LOW || i2c_addr == QMI8658_ADDR_HIGH;
}

static uint8_t qmi8658_backend_default_addr(void)
{
    return QMI8658_ADDR_LOW;
}

static int qmi8658_backend_sdo_level_for_addr(uint8_t i2c_addr)
{
    /* QMI8658 SA0/AD0: 0x6A (SA0=0), 0x6B (SA0=1). */
    return (i2c_addr == QMI8658_ADDR_HIGH) ? 1 : 0;
}

const lua_imu_backend_t lua_imu_backend_qmi8658 = {
    .chip_name = "qmi8658",
    .state_size = sizeof(qmi8658_state_t),
    .probe = qmi8658_backend_probe,
    .destroy = NULL,
    .read_sample = qmi8658_backend_read_sample,
    .read_temperature = qmi8658_backend_read_temperature,
    .read_int_status = qmi8658_backend_read_int_status,
    .is_supported_addr = qmi8658_backend_is_supported_addr,
    .default_addr = qmi8658_backend_default_addr,
    .sdo_level_for_addr = qmi8658_backend_sdo_level_for_addr,
};
