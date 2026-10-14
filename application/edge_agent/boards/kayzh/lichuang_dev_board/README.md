# Lichuang ESP32-S3 (立创实战派 XD-ESP32S3-V1.0)

ESP-Claw board package for the Lichuang 实战派 ESP32-S3 development board.

## Hardware Overview

| Feature | Specification |
|---------|---------------|
| SoC | ESP32-S3-WROOM-1-N16R8 (240 MHz, 16 MB flash, 8 MB octal PSRAM) |
| Display | 2.4" ST7789 SPI TFT, 320x240 logical landscape |
| Touch | FT6x36 capacitive touch on I2C (driven by the `ft5x06` driver) |
| Audio out | ES8311 codec (I2S + I2C) → NS4150B speaker amp |
| Audio in | ES7210 + dual analog mic (present but not enabled) |
| IMU | QMI8658A 6-axis (I2C 0x6a) |
| Camera | GC0308 DVP (VGA) |
| Storage | microSD, SDMMC 1-bit |
| IO expander | PCA9557 at I2C 0x19: IO0=LCD_CS, IO1=PA_EN, IO2=DVP_PWDN |

## GPIO Mapping

| Function | GPIO |
|----------|------|
| I2C SDA / SCL | 1 / 2 |
| LCD DC / MOSI / SCK | 39 / 40 / 41 |
| LCD backlight (LEDC) | 42 |
| LCD CS | PCA9557 IO0 (no direct GPIO) |
| LCD reset | none — RC network only (10k + 100 nF) |
| Speaker amp enable | PCA9557 IO1 (active high) |
| Camera PWDN | PCA9557 IO2 (drive low to power up) |
| I2S MCLK / BCLK / WS | 38 / 14 / 13 |
| I2S DIN / DOUT | 12 / 45 |
| Camera VSYNC / HREF / PCLK / XCLK | 3 / 46 / 7 / 5 |
| Camera D0..D7 | 16, 18, 8, 17, 15, 6, 4, 9 |
| SD CLK / CMD / D0 | 47 / 48 / 21 |

## I2C bus notes

**Do not blind-scan this bus (`lua` `bus:scan()`).** `i2c_bus_scan()` probes
addresses 1..126 while holding the bus mutex, and on ESP-IDF ≥ 5.3 it runs each
probe through `i2c_master_probe()` with the `CONFIG_I2C_MS_TO_WAIT` timeout (set
to 100 ms here). If a device wedges the bus, every probe burns the full timeout
and the touch / IMU / codec all stall behind the mutex. Use the known addresses
instead:

| 7-bit addr | Device |
|-----------|--------|
| 0x18 | ES8311 (audio DAC) |
| 0x19 | PCA9557 (IO expander) |
| 0x38 | FT6x36 (touch) |
| 0x41 | ES7210 (audio ADC) |
| 0x6a | QMI8658 (IMU) |

## Other board-specific notes

- **Touch range**: `esp_lcd_touch` applies `mirror` as `x_max - x` **before**
  `swap_xy`, so `x_max`/`y_max` are the *mirror axes* and must be the panel's raw
  touch ranges (240/320), not the 320x240 display size. Getting this wrong
  collapses coordinates toward one corner.
- **LCD CS** is driven low once by the custom ST7789 driver through the PCA9557
  (`components/esp_st7789_lichuang_custom/`). The display factory in
  `setup_device.c` also enables the speaker amp and releases camera PWDN.
- **Console** runs on the native USB-Serial-JTAG (the onboard CH340 UART bridge
  needs a host driver that macOS does not ship). The port name is location-based
  and changes when the board is re-plugged.
- **SD card**: when a card is inserted the DATA root becomes `/sdcard` and the
  flash fatfs partition is left unmounted (`app_fs`). Without a card the device
  falls back to `/fatfs` after a non-fatal mount failure.
- The IMU has no `lua_module_imu` chip backend upstream; this board ships one
  (`components/lua_modules/lua_module_imu/src/qmi8658/`) and enables it via
  `CONFIG_LUA_MODULE_IMU_CHIP_QMI8658`.
- `lua_module_lcd`'s `test/lcd_text.lua` hardcodes reference pins. Use the
  board_manager-driven display tests (`display_shapes.lua`,
  `display_color_bars.lua`) instead — they resolve pins from this board.
