/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "esp_st7789_lichuang_custom.h"

/* I2C communication related */
#define I2C_TIMEOUT_MS         (1000)
#define I2C_CLK_SPEED_HZ       (400000)

/* LCD control lines wired to the PCA9557 */
#define PCI9557_IO_LCD_CS      (0)     /*!< Expander pin driving LCD_CS */

/* Power-on defaults (matching vendor reference implementation) */
#define DIR_REG_DEFAULT_VAL    (0xFF)  /*!< All pins as input (hardware reset value) */
#define OUT_REG_DEFAULT_VAL    (0x05)  /*!< IO0=1(CS high), IO1=1(PA_EN high), IO2=0(DVP_PWDN low) */
#define POLARITY_DEFAULT_VAL   (0x00)  /*!< Non-inverted input polarity */

static const char *TAG = "ST7789+PCA9557";

/* CS line source, registered by the board before the panel is created. */
static esp_io_expander_handle_t s_cs_io_expander;
static uint32_t s_cs_pin_mask = 1U << PCI9557_IO_LCD_CS;

#define PCA9557_RETURN_ON_CONTAINER(handle, ret_ctx)                                                    \
    do {                                                                                                \
        ESP_RETURN_ON_FALSE((handle), ESP_ERR_INVALID_ARG, TAG, "Invalid handle");                      \
        (ret_ctx) = __containerof((handle), esp_io_expander_pca9557_t, base);                           \
    } while (0)

static esp_err_t pca9557_write_reg_internal(esp_io_expander_pca9557_t *pca9557, uint8_t reg_addr, uint8_t value)
{
    uint8_t buf[2] = {reg_addr, value};
    return i2c_master_transmit(pca9557->i2c_handle, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t pca9557_read_reg_internal(esp_io_expander_pca9557_t *pca9557, uint8_t reg_addr, uint8_t *value)
{
    return i2c_master_transmit_receive(pca9557->i2c_handle, &reg_addr, sizeof(reg_addr), value, sizeof(*value), I2C_TIMEOUT_MS);
}

esp_err_t esp_io_expander_pca9557_write_reg(esp_io_expander_handle_t handle, uint8_t reg_addr, const uint8_t *data, size_t data_len)
{
    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid data");
    esp_io_expander_pca9557_t *pca9557 = NULL;
    PCA9557_RETURN_ON_CONTAINER(handle, pca9557);

    uint8_t buf[data_len + 1];
    buf[0] = reg_addr;
    memcpy(&buf[1], data, data_len);
    esp_err_t ret = i2c_master_transmit(pca9557->i2c_handle, buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write reg 0x%02X failed: %s", reg_addr, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t read_input_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_pca9557_t *pca9557 = NULL;
    PCA9557_RETURN_ON_CONTAINER(handle, pca9557);
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid value");

    uint8_t reg_val = 0;
    ESP_RETURN_ON_ERROR(pca9557_read_reg_internal(pca9557, PCA9557_REG_INPUT, &reg_val), TAG, "Read input reg failed");
    *value = reg_val;
    return ESP_OK;
}

static esp_err_t write_output_reg(esp_io_expander_handle_t handle, uint32_t value)
{
    esp_io_expander_pca9557_t *pca9557 = NULL;
    PCA9557_RETURN_ON_CONTAINER(handle, pca9557);

    uint8_t reg_val = (uint8_t)(value & 0xFF);
    ESP_RETURN_ON_ERROR(pca9557_write_reg_internal(pca9557, PCA9557_REG_OUTPUT, reg_val), TAG, "Write output reg failed");
    pca9557->regs.output = reg_val;
    return ESP_OK;
}

static esp_err_t read_output_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_pca9557_t *pca9557 = NULL;
    PCA9557_RETURN_ON_CONTAINER(handle, pca9557);
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid value");

    /* The output latch is write-only; return the cached shadow value. */
    *value = pca9557->regs.output;
    return ESP_OK;
}

static esp_err_t write_direction_reg(esp_io_expander_handle_t handle, uint32_t value)
{
    esp_io_expander_pca9557_t *pca9557 = NULL;
    PCA9557_RETURN_ON_CONTAINER(handle, pca9557);

    uint8_t reg_val = (uint8_t)(value & 0xFF);
    ESP_RETURN_ON_ERROR(pca9557_write_reg_internal(pca9557, PCA9557_REG_CONFIG, reg_val), TAG, "Write direction reg failed");
    pca9557->regs.direction = reg_val;
    return ESP_OK;
}

static esp_err_t read_direction_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_pca9557_t *pca9557 = NULL;
    PCA9557_RETURN_ON_CONTAINER(handle, pca9557);
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid value");

    *value = pca9557->regs.direction;
    return ESP_OK;
}

static esp_err_t reset(esp_io_expander_t *handle)
{
    /* Vendor sequence: OUTPUT → POLARITY → CONFIG (direction).
     * OUTPUT first ensures CS starts high before being configured as output. */
    ESP_RETURN_ON_ERROR(write_output_reg(handle, OUT_REG_DEFAULT_VAL), TAG, "Write output reg failed");
    ESP_RETURN_ON_ERROR(pca9557_write_reg_internal(
                            (esp_io_expander_pca9557_t *)__containerof(handle, esp_io_expander_pca9557_t, base),
                            PCA9557_REG_POLARITY, POLARITY_DEFAULT_VAL),
                        TAG, "Write polarity reg failed");
    ESP_RETURN_ON_ERROR(write_direction_reg(handle, DIR_REG_DEFAULT_VAL), TAG, "Write direction reg failed");
    return ESP_OK;
}

static esp_err_t del(esp_io_expander_t *handle)
{
    esp_io_expander_pca9557_t *pca9557 = NULL;
    PCA9557_RETURN_ON_CONTAINER(handle, pca9557);

    ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(pca9557->i2c_handle), TAG, "Remove I2C device failed");
    free(pca9557);
    return ESP_OK;
}

esp_err_t esp_io_expander_new_pca9557(i2c_master_bus_handle_t i2c_bus, uint32_t dev_addr, esp_io_expander_handle_t *handle_ret)
{
    ESP_RETURN_ON_FALSE(i2c_bus != NULL && handle_ret != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");
    ESP_RETURN_ON_FALSE(dev_addr <= 0x7F, ESP_ERR_INVALID_ARG, TAG, "Invalid I2C address: 0x%02" PRIX32, dev_addr);

    esp_io_expander_pca9557_t *pca9557 = (esp_io_expander_pca9557_t *)calloc(1, sizeof(esp_io_expander_pca9557_t));
    ESP_RETURN_ON_FALSE(pca9557 != NULL, ESP_ERR_NO_MEM, TAG, "Malloc failed");

    esp_err_t ret = ESP_OK;
    const i2c_device_config_t i2c_dev_cfg = {
        .device_address = dev_addr,
        .scl_speed_hz = I2C_CLK_SPEED_HZ,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &i2c_dev_cfg, &pca9557->i2c_handle), err, TAG, "Add new I2C device failed");

    /* PCA9557 semantics differ from chips like the AW9523B:
     *   - direction register bit 1 = input, 0 = output (dir_out_bit_zero = 0)
     *   - input register bit 1 = high level             (input_high_bit_zero = 0)
     * The hardware has no pull-up/down or high-Z registers, so those optional
     * function pointers stay NULL and the board manager skips them. */
    pca9557->base.config.io_count = PCA9557_IO_COUNT;
    pca9557->base.config.flags.dir_out_bit_zero = 0;
    pca9557->base.config.flags.input_high_bit_zero = 0;
    pca9557->base.read_input_reg = read_input_reg;
    pca9557->base.write_output_reg = write_output_reg;
    pca9557->base.read_output_reg = read_output_reg;
    pca9557->base.write_direction_reg = write_direction_reg;
    pca9557->base.read_direction_reg = read_direction_reg;
    pca9557->base.reset = reset;
    pca9557->base.del = del;

    ESP_GOTO_ON_ERROR(reset(&pca9557->base), err1, TAG, "Reset failed");

    *handle_ret = &pca9557->base;
    return ESP_OK;

err1:
    i2c_master_bus_rm_device(pca9557->i2c_handle);
err:
    free(pca9557);
    return ret;
}

esp_err_t esp_lcd_new_panel_st7789_custom_set_io_expander(esp_io_expander_handle_t io_expander, uint32_t cs_pin_mask)
{
    ESP_RETURN_ON_FALSE(io_expander != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid IO expander handle");
    ESP_RETURN_ON_FALSE(cs_pin_mask != 0, ESP_ERR_INVALID_ARG, TAG, "Invalid CS pin mask");

    s_cs_io_expander = io_expander;
    s_cs_pin_mask = cs_pin_mask;
    return ESP_OK;
}

/* Drive LCD_CS low before panel init. The panel samples its interface on
 * the falling edge during reset, so CS must transition from high to low. */
static esp_err_t lcd_cs_drive_low(void)
{
    ESP_RETURN_ON_FALSE(s_cs_io_expander != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "IO expander for LCD_CS is not registered, call esp_lcd_new_panel_st7789_custom_set_io_expander() first");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_cs_io_expander, s_cs_pin_mask, 0), TAG, "Drive LCD_CS low failed");
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_st7789_custom(esp_lcd_panel_io_handle_t io_handle,
                                          const esp_lcd_panel_dev_config_t *panel_config,
                                          esp_lcd_panel_handle_t *panel_handle)
{
    ESP_RETURN_ON_FALSE(io_handle != NULL && panel_config != NULL && panel_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid argument");

    esp_lcd_panel_dev_config_t panel_config_copy = {0};
    memcpy(&panel_config_copy, panel_config, sizeof(esp_lcd_panel_dev_config_t));

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config_copy, panel_handle),
                        TAG, "New panel failed");

    /* Extra bring-up sequence required by this board (vendor reference).
     * reset (CS transitions from high to low) -> drive CS low -> init. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel_handle), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(lcd_cs_drive_low(), TAG, "Drive LCD_CS low failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel_handle), TAG, "Panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(*panel_handle, true), TAG, "Panel invert colour failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(*panel_handle, true), TAG, "Panel swap XY failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(*panel_handle, true, false), TAG, "Panel mirror failed");
    return ESP_OK;
}
