# Lua SCD41 Sensor Module

This module provides Lua bindings for the Sensirion SCD41 CO2, temperature, and humidity sensor. The sensor communicates via I2C and integrates with the board manager for peripheral management.

The module handles the sensor's ~5 second conversion cycle for you: it starts periodic measurement on open, waits for data to become ready before reading, and shares the newest sample between all read functions. You never need to insert your own `delay` between reads.

## How to call

- Import it with `local scd41 = require("scd41_sensor")`
- Open the sensor with one of:
  - `local sensor = scd41.new()` — use the default board-configured device
  - `local sensor = scd41.new("scd41_sensor")` — choose device by name
  - `local sensor = scd41.new({ peripheral = "i2c_master", timeout_ms = 1000 })`
- Measurement starts automatically on open (disable with `auto_start = false`)
- Read all values with `local sample = sensor:read()`
  - `sensor:read({ fresh = true })` — block until the next measurement, then return it
- Or read one value at a time (all return the newest sample already read):
  - `sensor:read_co2()` — CO2 concentration in ppm (integer)
  - `sensor:read_temperature()` — temperature in °C
  - `sensor:read_humidity()` — relative humidity in %
- Manually control measurement with `sensor:start_measurement()` / `sensor:stop_measurement()` (both idempotent)
- Inspect sensor identity with `sensor:name()`
- Call `sensor:close()` when done

## Options table

All fields are optional unless the chosen backend requires them.

| Field              | Type    | Meaning                                            |
|--------------------|---------|----------------------------------------------------|
| `device`           | string  | Board device name to load defaults from            |
| `peripheral`       | string  | Board I2C peripheral name (e.g., `"i2c_master"`)   |
| `frequency`        | integer | I2C clock in Hz (default `100000`)                 |
| `timeout_ms`       | integer | Per-transfer I2C timeout in ms (default `1000`)    |
| `ready_timeout_ms` | integer | Max wait for a new measurement in ms (default `6000`) |
| `auto_start`       | boolean | Start periodic measurement on open (default `true`) |

## Data format

- `sample.co2` — CO2 concentration in parts per million (ppm), integer
- `sample.temperature` — temperature in degrees Celsius (°C), number
- `sample.humidity` — relative humidity in percent (%RH), number

## Example: SCD41

```lua
local scd41 = require("scd41_sensor")
local delay = require("delay")

local sensor = scd41.new({ peripheral = "i2c_master" })

-- Measurement auto-starts on open; read() blocks until the first sample is ready.
local sample = sensor:read()
print(string.format("CO2: %d ppm", sample.co2))
print(string.format("Temperature: %.2f °C", sample.temperature))
print(string.format("Humidity: %.2f %%", sample.humidity))

-- Read again after one conversion cycle to get a new value.
delay.delay_ms(5000)
sample = sensor:read()
print(string.format("CO2: %d ppm", sample.co2))

sensor:close()
```

## Hardware

- **Sensor I2C Address**: 0x62 (fixed)
- **Supply Voltage**: 2.4V to 5.5V (3.3V recommended)
- **Update Rate**: 5 seconds

## Notes

- `read()` blocks only until the first sample is available (or until the next sample when `fresh = true`). Once a sample exists, a call that lands between two conversion cycles returns the cached newest sample instead of failing.
- `read()`, `read_co2()`, `read_temperature()`, and `read_humidity()` share one newest sample. Each call tries to read a newer one and falls back to the shared sample when none is ready, so calling them back to back returns consistent values and never fails.
- The sensor produces a new measurement about every 5 seconds; faster polling yields the previous sample, which is expected.
- Failures raise a Lua error with a specific reason: measurement not started, timed out waiting for a sample, or the underlying I2C error name.
- The SCD41 sensor must be connected to a functional I2C bus managed by the board manager.
