-- SCD41 Sensor Test Script
-- Usage: run via the CLI or scheduler, or place in the DATA root scripts dir.
-- Demonstrates that the module handles the ~5 s conversion cycle for you.

local scd41 = require("scd41_sensor")
local delay = require("delay")

-- Test 1: Open the device. Measurement auto-starts.
print("[Test 1] Opening default SCD41 device...")
local sensor = scd41.new()
print("Device name: " .. sensor:name())

-- Test 2: First read blocks until the first sample is ready (~5 s).
print("\n[Test 2] Reading first sample (blocks until ready)...")
local sample = sensor:read()
print(string.format("CO2: %d ppm", sample.co2))
print(string.format("Temperature: %.2f °C", sample.temperature))
print(string.format("Humidity: %.2f %%", sample.humidity))

-- Test 3: Individual readers share the latest sample; they never fail.
print("\n[Test 3] Reading individual values...")
print(string.format("CO2 only: %d ppm", sensor:read_co2()))
print(string.format("Temperature only: %.2f °C", sensor:read_temperature()))
print(string.format("Humidity only: %.2f %%", sensor:read_humidity()))

-- Test 4: Fast polling must not fail. Reads faster than the conversion cycle
-- return the newest available sample rather than raising an error.
print("\n[Test 4] Polling 10x at 1 s intervals (no failures expected)...")
for i = 1, 10 do
    local s = sensor:read()
    print(string.format("  poll %2d: CO2 %d ppm, %.2f °C, %.2f %%",
                        i, s.co2, s.temperature, s.humidity))
    delay.delay_ms(1000)
end

-- Test 5: read({fresh = true}) blocks until the next new sample is available.
print("\n[Test 5] Reading a fresh sample (blocks for the next cycle)...")
local fresh = sensor:read({ fresh = true })
print(string.format("CO2: %d ppm, %.2f °C, %.2f %%", fresh.co2, fresh.temperature, fresh.humidity))

-- Cleanup
sensor:close()
print("\n[Test 6] Sensor closed successfully")
