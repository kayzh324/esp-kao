/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */


#define PCA9557_IO_COUNT (8)

/* Register addresses.
 * Note the PCA9557 has no dedicated output register read-back: the output
 * latch is write-only, so `read_output_reg` returns the cached shadow value. */
#define PCA9557_REG_INPUT      0x00  /*!< Input port register (read only) */
#define PCA9557_REG_OUTPUT     0x01  /*!< Output port register (write only) */
#define PCA9557_REG_POLARITY   0x02  /*!< Polarity inversion register */
#define PCA9557_REG_CONFIG     0x03  /*!< Configuration (direction) register: 1 = input, 0 = output */

#define PCA9557_I2C_ADDR_BASE  0x18  /*!< Base I2C address: 0b0011<A2><A1><A0> */
#define PCA9557_I2C_ADDR_A2A1A0(a2, a1, a0) \
    (PCA9557_I2C_ADDR_BASE | (((a2) & 1) << 2) | (((a1) & 1) << 1) | ((a0) & 1))

/**
 * @brief  Device structure type
 *
 */
typedef struct {
    esp_io_expander_t        base;
    i2c_master_dev_handle_t  i2c_handle;
    struct {
        uint8_t  direction;  /*!< Shadow of the configuration register */
        uint8_t  output;     /*!< Shadow of the output port register */
    } regs;
} esp_io_expander_pca9557_t;

/**
 * @brief  Create a PCA9557 IO expander object
 *
 * @note   PCA9557 has no reset pin or soft-reset command, so the device is
 *         brought to a known state by writing the configuration and output
 *         registers directly. Pull-up/down and output-mode (open-drain)
 *         requests are not supported by the hardware and are ignored by the
 *         board manager, since the corresponding function pointers are NULL.
 *
 * @param[in]   i2c_bus     I2C bus handle, obtained from `i2c_new_master_bus()`
 * @param[in]   dev_addr    I2C device address of the chip (7-bit, e.g. 0x19)
 * @param[out]  handle_ret  Handle to the created IO expander object
 *
 * @return
 *       - ESP_OK  Success, otherwise returns ESP_ERR_xxx
 */
esp_err_t esp_io_expander_new_pca9557(i2c_master_bus_handle_t i2c_bus, uint32_t dev_addr, esp_io_expander_handle_t *handle_ret);

/**
 * @brief  Create an ST7789 panel and run the extra bring-up sequence
 *
 * @note   Wraps `esp_lcd_new_panel_st7789()` and then runs the panel bring-up
 *         sequence required by this board:
 *           panel reset -> CS pulse -> panel init -> colour invert ->
 *           swap XY -> mirror (x).
 *         The CS line is driven through pin PCI9557_IO_LCD_CS of the IO
 *         expander returned by `esp_lcd_new_panel_st7789_custom_set_io_expander()`.
 *
 * @param[in]   io_handle     LCD panel IO handle created with `esp_lcd_new_panel_io_spi()`
 * @param[in]   panel_config  Panel configuration passed through to the vendor driver
 * @param[out]  panel_handle  Handle to the created panel object
 *
 * @return
 *       - ESP_OK  Success, otherwise returns ESP_ERR_xxx
 */
esp_err_t esp_lcd_new_panel_st7789_custom(esp_lcd_panel_io_handle_t io_handle,
                                          const esp_lcd_panel_dev_config_t *panel_config,
                                          esp_lcd_panel_handle_t *panel_handle);

/**
 * @brief  Register the IO expander that drives the panel CS line
 *
 * @note   Must be called before `esp_lcd_new_panel_st7789_custom()`, otherwise
 *         the bring-up sequence cannot pulse CS.
 *
 * @param[in]  io_expander  PCA9557 IO expander handle
 * @param[in]  cs_pin_mask  Bit mask of the expander pin wired to LCD_CS
 *
 * @return
 *       - ESP_OK  Success, otherwise returns ESP_ERR_xxx
 */
esp_err_t esp_lcd_new_panel_st7789_custom_set_io_expander(esp_io_expander_handle_t io_expander, uint32_t cs_pin_mask);

/**
 * @brief  Write data to a PCA9557 register
 *
 * @param[in]  handle    IO expander handle
 * @param[in]  reg_addr  Register address to write
 * @param[in]  data      Data buffer to write
 * @param[in]  data_len  Length of `data`
 *
 * @return
 *       - ESP_OK          Success
 *       - ESP_ERR_INVALID_ARG  Invalid handle or arguments
 *       - ESP_FAIL        I2C transmission failed
 */
esp_err_t esp_io_expander_pca9557_write_reg(esp_io_expander_handle_t handle, uint8_t reg_addr, const uint8_t *data, size_t data_len);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
