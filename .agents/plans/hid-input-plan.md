# USB HID Input Bridge (added)

## Goal

Make the system UI usable on boards without a touch panel while keeping touch as
the primary input on boards that have one:

- Touch is never disabled; it and HID drive the same LVGL screens.
- A USB keyboard or mouse is detected automatically and attached at runtime.
- Hot-plug works: unplugging falls back to the hint, plugging back restores input.
- If neither touch nor HID exists, the home screen shows a hint instead of looking
  frozen.

## Architecture

```
USB keyboard/mouse
        |
usb_host_hid (Lua driver, already used by apps)
        |
/system/scripts/hid_input.lua   (boot job, owns HID transport + hot-plug)
        |
ui_input Lua module             (new, thin binding)
        |
system_ui_input.c               (new, LVGL pointer + keypad integration)
        |
LVGL display / launcher
```

Keeping the HID transport in Lua reuses the driver that was already validated on
the P4 board and avoids installing a second USB host HID class driver in C. The C
side only owns LVGL input devices and focus movement.

Boot order: `app_claw_start()` starts the bridge after capabilities (and therefore
the Lua runtime) are ready. Starting it earlier from `app_claw_ui_start()` fails
because the Lua runner does not exist yet.

## Input model

- Mouse: a second LVGL pointer indev with an on-screen cursor. Relative HID
  movement is accumulated and clamped to the display. Left button bit 0 maps to
  pressed state.
- Keyboard: an LVGL keypad indev with a small press/release queue.
- Launcher grid: arrows move focus by the launcher's 2x4 grid instead of LVGL's
  linear group order. Left/right change columns and cross pages; up/down change
  rows and do nothing when the row is empty (for example a single row of apps).
- Focus targets are the clickable icon objects, so Enter triggers the same
  `LV_EVENT_CLICKED` path as touch/mouse. The focused icon gets an outline.
- Tab toggles between the clock page and the launcher page.
- Enter opens the focused app.
- Home key (HID usage 0x4A) returns to the clock. Tab already provides the same
  round trip, so keyboards without a Home key are still fully usable.

## Hint

`system_ui_input` creates a top-layer label refreshed every 2 s:

| Touch | HID | Label |
| --- | --- | --- |
| yes | any | hidden |
| no | yes | `TAB apps/home | ARROWS move | ENTER open` |
| no | no | `No touch input. Connect a USB keyboard or mouse.` |

The hint is created before the app registry finishes loading, so it is
independent of launcher content.

## Compatibility / known quirks

- `system_ui_input_start()` is called from `system_ui_start()` while the LVGL lock
  is held. The injection functions are called from the Lua job task and only
  update state under a critical section; LVGL objects are only touched on the
  LVGL task or under `system_ui_lock()`.
- The tested 2.4G receiver (`1915:1025`) reports the left/right arrow usages
  swapped. `hid_input.lua` flags that VID/PID through
  `ui_input.set_arrow_swap(true)`; standard keyboards keep the normal mapping.
- Mouse wheel is accepted by the Lua API but currently reserved.
- Multi-page launcher navigation is implemented in the grid math but only the
  single-page case has been exercised.

## Verification

P4 (`jc_esp32_p4_m3_dev`, no touch, USB HID):

- Keyboard and mouse enumerate at boot; bridge job stays `running`.
- Tab switches clock/apps; arrows move the focus outline by grid position; Enter
  opens an app; mouse cursor moves and clicks.
- Unplugging the receiver shows the no-input hint; plugging it back changes the
  hint to the key map and restores input.

S3 (`esp32_S3_test_board`, touch):

- Touch page swipes, app launch and exit remain normal.
- No hint is shown because touch is available.
- The bridge job runs without disturbing touch.
