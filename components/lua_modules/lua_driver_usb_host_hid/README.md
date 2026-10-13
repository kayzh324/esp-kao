# Lua USB Host HID Driver

Exposes USB HID keyboards, mice, and generic interfaces to Lua. `init()` enumerates all plugged devices automatically. Report ID multiplexing creates virtual devices per Report ID.

## Multiplexed Interfaces

Multiplexed physical interfaces (Report ID-based) create separate logical devices:
- Device uses report descriptor to determine fields (modifier, keys, buttons, X, Y, wheel)
- Report ID determines routing to virtual device
- If descriptor unreadable, falls back to boot protocol
- Unknown report purposes exposed as `"generic"` with own id

## API Reference

### hid.init()

Starts USB host stack and HID driver. Idempotent: calling again returns immediately if already initialized. Enumeration is asynchronous (takes ~100-500ms).

**Note**: If `deinit()` was called, wait at least 2 seconds before calling `init()` again, or the board will panic and restart.

**Returns**: `true` on success, raises error if USB host/HID could not be installed.

### hid.deinit()

Closes devices, powers off USB root port, deregisters HID client. If this module installed the USB host library, releases it. Safe to cycle with `init()` in same boot.

**Critical**: After calling `deinit()`, wait at least 2 seconds before calling `init()` again. Immediate re-initialization causes panic and board restart.

Note: Omitting `deinit()` does not affect next `init()` call — re-initializing works normally without `deinit()`.

**Returns**: `true` on success, or `false, reason` on failure. Driver is unusable after failure.

### hid.find(type)

Returns first connected device of type `"keyboard"`, `"mouse"`, or `"generic"`.

**Returns**: device userdata or `nil` (poll for device after `init()`).

### hid.get_devices()

Returns array of connected interface tables. Each has: `id`, `type`, `detected_type`, `vendor_id`, `product_id`, `interface`, `boot_protocol`, `report_id`, `usage_page`, `usage`, `parsed`.

### hid.open(id)

Attaches to device by id from `get_devices()` entry. Multiple scripts may attach to same device.

**Arguments**: `id` — from device entry `id` field (not array index).

**Returns**: device userdata, raises error if id out of range.

### hid.is_device(value)

**Returns**: `true` if value is device userdata from `hid.open()` or `hid.find()`.

### dev:read()

Removes and returns oldest buffered report or `nil`.

**Returns** (when present): table with `type`, `data` (raw bytes), and parsed fields when available: `report_id`, `modifier`, `keys` (array), `dx`, `dy`, `wheel`, `buttons` (table with `left`, `right`, `middle`, `mask`).

### dev:get_info()

**Returns**: table with device metadata: `id`, `type`, `detected_type`, `vendor_id`, `product_id`, `interface`, `boot_protocol`, `report_id`, `usage_page`, `usage`, `parsed`, `pending`.

### dev:is_connected()

**Returns**: `true` while plugged in, `false` after unplug.

### dev:close()

Releases Lua reference (USB interface stays open for other scripts).

**Returns**: `true`

## Key Notes

- Enumeration is asynchronous (~100-500ms). Poll after `init()`.
- Devices buffer up to 8 reports. Poll every 10-20ms for keyboards to avoid drops.
- Report fields come from descriptor, not boot protocol. Check field existence.
- For multiplexed devices, `data` includes Report ID byte; parsed fields strip it.
- Device id is reused after unplug; old device object reports `is_connected() == false`.
- At most 8 logical devices tracked; at most 8 Report IDs per interface.

## Keyboard Mapping

Use `require("usb_hid_keymap")` for keycode-to-character and press/release detection. `dev:read()` returns currently held keys (repeats while held); use keymap tracker and check `kind == "down"` for single keypresses.

## Quick Example

```lua
local hid = require("usb_host_hid")
local delay = require("delay")

hid.init()

-- Poll for device (enumeration is async)
local kb
for _ = 1, 20 do
    kb = hid.find("keyboard")
    if kb then break end
    delay.delay_ms(100)
end

if kb then
    while kb:is_connected() do
        local report = kb:read()
        if report and report.keys then
            for _, key in ipairs(report.keys) do
                print(string.format("key=0x%02x mod=0x%02x", key, report.modifier))
            end
        end
        delay.delay_ms(10)
    end
    kb:close()
end

hid.deinit()
```
