# Lua UI Input

`ui_input` injects USB HID keyboard and mouse events into the system UI. It is
used by the built-in `hid_input.lua` bridge; applications normally do not call it
directly.

Touch input is independent and stays active. When both touch and HID are
available, both drive the same LVGL screens.

```lua
local ui_input = require("ui_input")

ui_input.mouse(dx, dy, buttons, wheel)  -- relative movement; buttons bit0 = left
ui_input.key(usage, pressed)            -- HID usage id and press/release edge
ui_input.set_hid(keyboard, mouse)       -- report which device classes exist
ui_input.set_arrow_swap(swapped)        -- flag receivers with swapped arrow usages
local status = ui_input.status()        -- { hid = bool, no_touch = bool }
```

Mapped keys: arrows, WASD, Enter, Esc, Tab and Space. `H` returns to the home
screen.
