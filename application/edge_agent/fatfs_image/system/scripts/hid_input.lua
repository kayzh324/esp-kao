-- USB HID input bridge (added).
--
-- Touch remains the primary input. When a USB keyboard or mouse is present, its
-- reports are forwarded to the system UI through the `ui_input` module so the
-- same LVGL screens can be driven without a touch panel. The launcher shows a
-- hint when neither touch nor HID exists.
--
-- Started by app_claw only on boards that enable the USB Host HID driver.

local ok_hid, hid = pcall(require, "usb_host_hid")
local ok_ui, ui = pcall(require, "ui_input")
if not ok_hid or not ok_ui then
  print("[hid_input] modules unavailable, bridge disabled")
  return
end

local delay = require("delay")
local system = require("system")

local ok, err = pcall(hid.init)
if not ok then
  print("[hid_input] hid.init failed: " .. tostring(err))
  return
end

local kb, ms
local kb_id, ms_id
local prev_keys = {}
local reported = nil

local function close_devices_if_gone()
  if kb and not kb:is_connected() then
    pcall(kb.close, kb)
    kb, kb_id, prev_keys = nil, nil, {}
  end
  if ms and not ms:is_connected() then
    pcall(ms.close, ms)
    ms, ms_id = nil, nil
  end
end

local function scan()
  close_devices_if_gone()
  local devs = hid.get_devices() or {}
  for _, d in ipairs(devs) do
    if d.type == "keyboard" and not kb then
      local ok_open, dev = pcall(hid.open, d.id)
      if ok_open and dev then
        kb, kb_id = dev, d.id
        prev_keys = {}
        ui.set_arrow_swap(d.vid == 0x1915 and d.pid == 0x1025)
        print("[hid_input] keyboard attached id=" .. tostring(d.id))
      end
    elseif d.type == "mouse" and not ms then
      local ok_open, dev = pcall(hid.open, d.id)
      if ok_open and dev then
        ms, ms_id = dev, d.id
        print("[hid_input] mouse attached id=" .. tostring(d.id))
      end
    end
  end

  local state = tostring(kb ~= nil) .. ":" .. tostring(ms ~= nil)
  if state ~= reported then
    reported = state
    ui.set_hid(kb ~= nil, ms ~= nil)
  end
end

scan()

local last_scan = system.millis()
while true do
  if kb then
    local ok_read, r = pcall(kb.read, kb)
    while ok_read and r do
      local cur = {}
      for _, usage in ipairs(r.keys or {}) do
        cur[usage] = true
        if not prev_keys[usage] then
          ui.key(usage, true)
        end
      end
      for usage in pairs(prev_keys) do
        if not cur[usage] then
          ui.key(usage, false)
        end
      end
      prev_keys = cur
      ok_read, r = pcall(kb.read, kb)
    end
    if not ok_read then
      pcall(kb.close, kb)
      kb, kb_id, prev_keys = nil, nil, {}
      ui.set_hid(false, ms ~= nil)
    end
  end

  if ms then
    local ok_mouse, r = pcall(ms.read, ms)
    while ok_mouse and r do
      local mask = (type(r.buttons) == "table" and r.buttons.mask) or 0
      ui.mouse(r.dx or 0, r.dy or 0, mask, r.wheel or 0)
      ok_mouse, r = pcall(ms.read, ms)
    end
    if not ok_mouse then
      pcall(ms.close, ms)
      ms, ms_id = nil, nil
      ui.set_hid(kb ~= nil, false)
    end
  end

  local now = system.millis()
  if now - last_scan >= 1000 then
    last_scan = now
    scan()
  end
  delay.delay_ms(10)
end
