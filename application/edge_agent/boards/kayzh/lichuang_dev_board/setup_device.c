/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_io_expander.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_st7789_lichuang_custom.h"
#include "esp_board_manager_includes.h"

static const char *TAG = "setup_device";

/* PCA9557 pins wired to LCD_CS (IO0), the speaker amp enable (IO1) and camera PWDN (IO2). */
#define LICHUANG_LCD_CS_PIN_MASK  (1U << 0)
#define LICHUANG_PA_EN_PIN_MASK   (1U << 1)
#define LICHUANG_DVP_PWDN_PIN_MASK (1U << 2)
#define LICHUANG_I2C_NAME         "i2c_master"
#define LICHUANG_PCA9557_ADDR     0x19  /*!< 7-bit address: 0b0011<A2=0><A1=0><A0=1> */

/* The board manager does not expose the expander that drives LCD_CS. The expander
 * is created here and handed to the custom ST7789 driver, which drives CS low during init.
 * IO1 also drives the NS4150B speaker amplifier enable, so it is held high here, and
 * IO2 holds camera PWDN low to keep the GC0308 powered up. */
static esp_io_expander_handle_t s_lcd_cs_expander;

static esp_err_t lcd_cs_expander_init(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_RETURN_ON_ERROR(esp_board_periph_ref_handle(LICHUANG_I2C_NAME, (void **)&i2c_bus), TAG,
                        "Get I2C bus handle failed");
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "I2C bus handle is NULL");

    /* PCA9557 reset() initializes: OUTPUT=0x05 (CS=1 high), CONFIG=0xFF (all input) */
    ESP_RETURN_ON_ERROR(esp_io_expander_new_pca9557(i2c_bus, LICHUANG_PCA9557_ADDR, &s_lcd_cs_expander), TAG,
                        "Create PCA9557 failed");
    /* Configure IO0 (LCD_CS), IO1 (PA_EN) and IO2 (DVP_PWDN) as outputs. */
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_lcd_cs_expander,
                                                LICHUANG_LCD_CS_PIN_MASK |
                                                LICHUANG_PA_EN_PIN_MASK |
                                                LICHUANG_DVP_PWDN_PIN_MASK,
                                                IO_EXPANDER_OUTPUT),
                        TAG, "Set LCD_CS/PA_EN/DVP_PWDN direction failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_lcd_cs_expander, LICHUANG_PA_EN_PIN_MASK, 1), TAG,
                        "Enable PA failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_lcd_cs_expander, LICHUANG_DVP_PWDN_PIN_MASK, 0), TAG,
                        "Release camera PWDN failed");
    /* Register the expander so the custom ST7789 driver can drive CS low during init */
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789_custom_set_io_expander(s_lcd_cs_expander, LICHUANG_LCD_CS_PIN_MASK),
                        TAG, "Register LCD_CS expander failed");
    return ESP_OK;
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_ERROR(lcd_cs_expander_init(), TAG, "LCD_CS expander init failed");

    esp_err_t ret = esp_lcd_new_panel_st7789_custom(io, panel_dev_config, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New st7789 panel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ST7789 panel created");
    return ESP_OK;
}

/* Touch panel factory: the FT6x36 on the display module speaks the FT5x06 register set,
 * so the ESP FT5x06 driver handles it. dev_lcd_touch calls this once the I2C bus and
 * panel IO are ready. */
esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = esp_lcd_touch_new_i2c_ft5x06(io, touch_dev_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ft5x06 touch driver: %s", esp_err_to_name(ret));
    }
    return ret;
}