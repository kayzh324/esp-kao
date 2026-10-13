-- USB HID input smoke test.
-- Plug in a keyboard, mouse, or 2.4G receiver, then run this script and
-- type / move the pointer. Every connected HID interface is drained.
--
-- This script tests:
--   * standard devices (keyboard, mouse)
--   * composite devices (2.4G receiver with keyboard+mouse on separate interfaces)
--   * combo devices (2.4G receiver multiplexing keyboard+mouse via Report IDs)
--   * descriptor-based device type detection and report parsing
--
-- It fails loudly when a device announces itself as a keyboard or a mouse but
-- delivers reports without parsed fields: that combination means the report
-- descriptor was not understood, which used to pass silently.
--
-- References for correct driver usage:
--   * devices are polled for, because enumeration is asynchronous
--   * hid.open() takes entry.id, never the loop index
--   * results are validated with hid.is_device(), never type(v) == "table"

local hid = require("usb_host_hid")
local delay = require("delay")

local ENUM_POLL_ATTEMPTS = 20
local ENUM_POLL_INTERVAL_MS = 100
local RUN_SECONDS = 15
local POLL_INTERVAL_MS = 10

print("[usb_hid] init...")
local ok, err = pcall(hid.init)
if not ok then
    print("[usb_hid] init failed: " .. tostring(err))
    return
end

-- Enumeration is asynchronous: poll instead of listing once.
print("[usb_hid] waiting for enumeration...")
local devices = {}
for _ = 1, ENUM_POLL_ATTEMPTS do
    devices = hid.get_devices()
    if #devices > 0 then
        break
    end
    delay.delay_ms(ENUM_POLL_INTERVAL_MS)
end

if #devices == 0 then
    print("[usb_hid] no HID interfaces found")
    hid.deinit()
    return
end

local opened = {}
for _, entry in ipairs(devices) do
    print(string.format(
        "[usb_hid] id=%d type=%s detected_type=%s vid=0x%04x pid=0x%04x iface=%d " ..
        "boot=%s report_id=0x%02x usage=0x%04x:0x%04x parsed=%s",
        entry.id, entry.type, entry.detected_type or "N/A",
        entry.vendor_id, entry.product_id,
        entry.interface, tostring(entry.boot_protocol),
        entry.report_id or 0, entry.usage_page or 0, entry.usage or 0,
        tostring(entry.parsed)
    ))

    -- Pass entry.id, not the loop position: they diverge after an unplug.
    local ok_open, dev = pcall(hid.open, entry.id)

    -- A device is userdata. hid.is_device() is the exact check;
    -- type(dev) == "table" would reject a device that opened fine.
    if ok_open and hid.is_device(dev) then
        opened[#opened + 1] = { id = entry.id, dev = dev, type = entry.type }
    else
        print(string.format("[usb_hid] id=%d open failed: %s",
                            entry.id, tostring(dev)))
    end
end

if #opened == 0 then
    print("[usb_hid] could not attach to any device")
    hid.deinit()
    return
end

print(string.format("[usb_hid] attached to %d device(s), reading for %ds...",
                    #opened, RUN_SECONDS))

local events = 0
local failures = {}
local deadline = os.time() + RUN_SECONDS

-- A keyboard/mouse whose reports carry no parsed fields means the report
-- descriptor was not understood. Report it once per device instead of quietly
-- printing raw bytes.
local function check_parsed(entry, report)
    if entry.reported_unparsed then
        return
    end
    if entry.type == "keyboard" and not report.keys then
        entry.reported_unparsed = true
        failures[#failures + 1] = string.format(
            "id=%d claims keyboard but report has no keys field", entry.id)
    elseif entry.type == "mouse" and not report.dx then
        entry.reported_unparsed = true
        failures[#failures + 1] = string.format(
            "id=%d claims mouse but report has no dx field", entry.id)
    end
end

while os.time() < deadline do
    for _, entry in ipairs(opened) do
        if entry.dev:is_connected() then
            local report = entry.dev:read()
            -- read() returns nil when nothing is buffered; that is not an error.
            if report then
                events = events + 1
                check_parsed(entry, report)
                -- Branch on the parsed field, not on report.type: a field is
                -- present only when the report descriptor described it.
                if report.keys then
                    local keys = {}
                    for _, key in ipairs(report.keys) do
                        keys[#keys + 1] = string.format("0x%02x", key)
                    end
                    print(string.format(
                        "[usb_hid] id=%d [%s] keyboard modifier=0x%02x keys=[%s]",
                        entry.id, entry.type, report.modifier, table.concat(keys, " ")
                    ))
                elseif report.dx then
                    print(string.format(
                        "[usb_hid] id=%d [%s] mouse dx=%d dy=%d wheel=%d L=%s R=%s M=%s",
                        entry.id, entry.type, report.dx, report.dy,
                        report.wheel or 0,
                        tostring(report.buttons and report.buttons.left),
                        tostring(report.buttons and report.buttons.right),
                        tostring(report.buttons and report.buttons.middle)
                    ))
                else
                    local bytes = {}
                    for _, b in ipairs(report.data) do
                        bytes[#bytes + 1] = string.format("%02x", b)
                    end
                    print(string.format("[usb_hid] id=%d [%s] generic [%s]",
                                        entry.id, entry.type, table.concat(bytes, " ")))
                end
            end
        end
    end
    delay.delay_ms(POLL_INTERVAL_MS)
end

print(string.format("[usb_hid] done, %d report(s) received", events))

for _, entry in ipairs(opened) do
    entry.dev:close()
end
local ok_deinit, deinit_err = hid.deinit()
if not ok_deinit then
    failures[#failures + 1] = "deinit failed: " .. tostring(deinit_err)
    print("[usb_hid] FAIL: deinit: " .. tostring(deinit_err))
end

-- init()/deinit() must be usable more than once per boot. Four rounds, each one
-- re-enumerating: this is the regression test for the host library being leaked
-- on deinit().
local CYCLES = 4
for cycle = 1, CYCLES do
    local ok_reinit, reinit_err = pcall(hid.init)
    if not ok_reinit then
        failures[#failures + 1] = string.format("cycle %d: init failed: %s",
                                                cycle, tostring(reinit_err))
        print(string.format("[usb_hid] FAIL: cycle %d init: %s", cycle, tostring(reinit_err)))
        break
    end

    local found = {}
    for _ = 1, ENUM_POLL_ATTEMPTS do
        found = hid.get_devices()
        if #found > 0 then
            break
        end
        delay.delay_ms(ENUM_POLL_INTERVAL_MS)
    end
    print(string.format("[usb_hid] cycle %d: init ok, %d device(s) re-enumerated",
                        cycle, #found))

    local ok_deinit, deinit_err = hid.deinit()
    if not ok_deinit then
        failures[#failures + 1] = string.format("cycle %d: deinit failed: %s",
                                                cycle, tostring(deinit_err))
        print(string.format("[usb_hid] FAIL: cycle %d deinit: %s", cycle, tostring(deinit_err)))
        break
    end
end

if #failures > 0 then
    print(string.format("[usb_hid] cleanup finished with %d failure(s):", #failures))
    for _, message in ipairs(failures) do
        print("[usb_hid]   - " .. message)
    end
else
    print("[usb_hid] cleanup finished")
end
