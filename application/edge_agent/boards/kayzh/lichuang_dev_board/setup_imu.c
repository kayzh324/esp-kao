/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QMI8658 board sensor for the lichuang dev board.
 *
 * The upstream IMU module expects a board sensor to hand back an initialised
 * device handle. Our QMI8658 backend talks to the chip through a native
 * i2c_master_dev_handle_t, so this init creates and configures that handle and
 * returns it as the device handle.
 */
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gen_board_device_custom.h"

static const char *TAG = "lichuang_imu";

#define QMI8658_REG_WHO_AM_I   0x00
#define QMI8658_REG_CTRL1      0x02
#define QMI8658_REG_CTRL2      0x03
#define QMI8658_REG_CTRL3      0x04
#define QMI8658_REG_CTRL7      0x08
#define QMI8658_REG_RESET      0x60

#define QMI8658_WHO_AM_I_VAL   0x05
#define QMI8658_RESET_CMD      0xB0
#define QMI8658_CTRL1_VAL      0x60
#define QMI8658_CTRL2_VAL      0x18
#define QMI8658_CTRL3_VAL      0x58
#define QMI8658_CTRL7_VAL      0x03
#define QMI8658_SETTLE_MS      300

static esp_err_t qmi8658_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(dev, buf, sizeof(buf), 1000);
}

static esp_err_t qmi8658_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, 1000);
}

static int imu_sensor_init(void *config, int size, void **out)
{
    ESP_RETURN_ON_FALSE(config && out && size == sizeof(dev_custom_imu_sensor_config_t),
                        ESP_ERR_INVALID_ARG, TAG, "Invalid IMU config");
    *out = NULL;
    const dev_custom_imu_sensor_config_t *cfg = config;

    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_board_periph_ref_handle(cfg->peripheral_name, (void **)&bus),
                        TAG, "I2C bus unavailable");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->i2c_addr,
        .scl_speed_hz = cfg->frequency,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err == ESP_OK) {
        uint8_t who = 0;
        err = qmi8658_read_regs(dev, QMI8658_REG_WHO_AM_I, &who, 1);
        if (err == ESP_OK && who != QMI8658_WHO_AM_I_VAL) {
            ESP_LOGE(TAG, "Not a QMI8658: WHO_AM_I=0x%02x", who);
            err = ESP_ERR_NOT_FOUND;
        }
    }
    if (err == ESP_OK) err = qmi8658_write_reg(dev, QMI8658_REG_RESET, QMI8658_RESET_CMD);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(20));
        err = qmi8658_write_reg(dev, QMI8658_REG_CTRL1, QMI8658_CTRL1_VAL);
    }
    if (err == ESP_OK) err = qmi8658_write_reg(dev, QMI8658_REG_CTRL2, QMI8658_CTRL2_VAL);
    if (err == ESP_OK) err = qmi8658_write_reg(dev, QMI8658_REG_CTRL3, QMI8658_CTRL3_VAL);
    if (err == ESP_OK) err = qmi8658_write_reg(dev, QMI8658_REG_CTRL7, QMI8658_CTRL7_VAL);
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(QMI8658_SETTLE_MS));

    if (err != ESP_OK) {
        if (dev) (void)i2c_master_bus_rm_device(dev);
        (void)esp_board_periph_unref_handle(cfg->peripheral_name);
        ESP_LOGE(TAG, "QMI8658 init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "QMI8658 initialized at 0x%02x", cfg->i2c_addr);
    *out = dev;
    return ESP_OK;
}

static int imu_sensor_deinit(void *handle)
{
    i2c_master_dev_handle_t dev = handle;
    esp_err_t err = ESP_OK;
    if (dev) err = i2c_master_bus_rm_device(dev);
    (void)esp_board_periph_unref_handle("i2c_master");
    return err;
}

CUSTOM_DEVICE_IMPLEMENT(imu_sensor, imu_sensor_init, imu_sensor_deinit);
