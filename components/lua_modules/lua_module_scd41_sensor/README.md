# Lua SCD41 Sensor Module

This module provides Lua bindings for the Sensirion SCD41 CO2, temperature, and humidity sensor. The sensor communicates via I2C and integrates with the board manager for peripheral management.

## How to call

- Import it with `local scd41 = require("scd41_sensor")`
- Open the sensor with one of:
  - `local sensor = scd41.new()` — use the default board-configured device
  - `local sensor = scd41.new("scd41_sensor")` — choose device by name
  - `local sensor = scd41.new({ peripheral = "i2c_master", timeout_ms = 1000 })`
- Start measurement with `sensor:start_measurement()`
- Read all values with `local sample = sensor:read()`
- Or read one value at a time:
  - `sensor:read_co2()` — CO2 concentration in ppm
  - `sensor:read_temperature()` — temperature in °C
  - `sensor:read_humidity()` — relative humidity in %
- Stop measurement with `sensor:stop_measurement()`
- Inspect sensor identity with `sensor:name()`
- Call `sensor:close()` when done

## Options table

All fields are optional unless the chosen backend requires them.

| Field              | Type    | Meaning                                          |
|--------------------|---------|--------------------------------------------------|
| `device`           | string  | Board device name to load defaults from          |
| `peripheral`       | string  | Board I2C peripheral name (e.g., `"i2c_master"`)|
| `frequency`        | integer | I2C clock in Hz (default `100000`)               |
| `timeout_ms`       | integer | I2C timeout in milliseconds (default `1000`)     |

## Data format

- `sample.co2` — CO2 concentration in parts per million (ppm)
- `sample.temperature` — temperature in degrees Celsius (°C)
- `sample.humidity` — relative humidity in percent (%RH)

## Example: SCD41

```lua
local scd41 = require("scd41_sensor")

local sensor = scd41.new({
    peripheral = "i2c_master",
})

sensor:start_measurement()

-- Wait for first measurement (5 seconds typical)
require("coro").sleep(5000)

local sample = sensor:read()
print(string.format("CO2: %d ppm", sample.co2))
print(string.format("Temperature: %.2f °C", sample.temperature))
print(string.format("Humidity: %.2f %%", sample.humidity))

sensor:stop_measurement()
sensor:close()
```

## Hardware

- **Sensor I2C Address**: 0x62 (fixed)
- **Supply Voltage**: 2.4V to 5.5V (3.3V recommended)
- **Update Rate**: 5 seconds

## Notes

- Reads are blocking.
- The sensor typically requires ~5 seconds between `start_measurement()` and the first valid read.
- Each read polls the sensor's current measurement data; if called more frequently than 5 seconds, you will get the same data.
- The SCD41 sensor must be connected to a functional I2C bus managed by the board manager.
- Any setup or read failure raises a Lua error.
