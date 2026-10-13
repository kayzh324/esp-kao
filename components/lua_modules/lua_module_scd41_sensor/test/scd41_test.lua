-- SCD41 Sensor Test Script
-- Usage: place in /fatfs/scripts/ and run via CLI or scheduler

local scd41 = require("scd41_sensor")

-- Test 1: Default device
print("[Test 1] Opening default SCD41 device...")
local sensor = scd41.new()
print("Device name: " .. sensor:name())

-- Start measurement
sensor:start_measurement()
print("Measurement started, waiting 5 seconds for first data...")

-- Wait for sensor to stabilize
require("coro").sleep(5000)

-- Read data
print("\n[Test 2] Reading sensor data...")
local sample = sensor:read()
print(string.format("CO2: %d ppm", sample.co2))
print(string.format("Temperature: %.2f °C", sample.temperature))
print(string.format("Humidity: %.2f %%", sample.humidity))

-- Read individual values
print("\n[Test 3] Reading individual values...")
print(string.format("CO2 only: %d ppm", sensor:read_co2()))
print(string.format("Temperature only: %.2f °C", sensor:read_temperature()))
print(string.format("Humidity only: %.2f %%", sensor:read_humidity()))

-- Cleanup
sensor:stop_measurement()
sensor:close()
print("\n[Test 4] Sensor closed successfully")
