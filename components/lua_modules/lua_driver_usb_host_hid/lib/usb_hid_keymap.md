# usb_hid_keymap.lua

Reusable pure-Lua keymap for HID boot keyboards. Translates raw report bytes
into key names, characters, and press/release events.

## Require

```lua
local keymap = require("usb_hid_keymap")
```

## Dependencies

None. This library performs no hardware access and does **not** require
`usb_host_hid`. It operates on plain report tables, so it also works against
BLE HID reports, recorded captures, and test fixtures.

Layout is US ANSI (`keymap.LAYOUT == "us"`).

## The one thing to understand first

**HID reports carry the keys currently held down, not key events.**

A report like `{ modifier = 0x02, keys = { 0x04 } }` means "shift and A are
held *right now*". If the user holds A for one second you will receive that same
report many times. Nothing in the report says "A was just pressed".

So there are two ways to use this library, and picking the wrong one is the
main source of bugs:

| You want | Use | Why |
|---|---|---|
| To know a key was *pressed* or *released* | `keymap.new_tracker()` + `tracker:update(report)` | Diffs consecutive reports and gives you edge events |
| To know what is held *at this instant* | `keymap.char()` / `keymap.name()` / `keymap.names()` | Pure lookup, no state |

For typing text, chat input, shortcuts, or anything that should fire once per
keypress, **use the tracker**. Calling `keymap.char()` in a poll loop and
appending the result will repeat every character dozens of times.

## Quick start: text entry (the common case)

Copy this shape. It is correct as written.

```lua
local hid = require("usb_host_hid")
local keymap = require("usb_hid_keymap")
local delay = require("delay")

hid.init()

local kb
for _ = 1, 20 do
    kb = hid.find("keyboard")
    if kb then break end
    delay.delay_ms(100)
end
if not kb then hid.deinit() return end

local tracker = keymap.new_tracker()   -- create ONCE, outside the loop
local text = ""

while kb:is_connected() do
    for _, ev in ipairs(tracker:update(kb:read())) do
        if ev.kind == "down" then
            if ev.char then
                text = text .. ev.char           -- printable character
            elseif ev.name == "BACKSPACE" then
                text = text:sub(1, -2)
            elseif ev.name == "ENTER" then
                print("submitted: " .. text)
                text = ""
            end
        end
    end
    delay.delay_ms(10)
end

kb:close()
hid.deinit()
```

`tracker:update()` accepts `nil` (which `kb:read()` returns when no report is
pending) and returns an empty list, so no `if report then` guard is needed.

## Type contract

| Value | Lua type | Notes |
|---|---|---|
| `keymap.name(code)` | `string` | Never `nil` for a number input; unknown codes give `"UNKNOWN_0x<hex>"` |
| `keymap.char(code, opts)` | `string` or `nil` | `nil` means "this key produces no character" |
| `keymap.code(name)` | `number` or `nil` | `nil` for an unknown name |
| `keymap.mods(byte)` | `table` | All fields are booleans, never `nil` |
| `keymap.names(report)` | `table` (array) | Empty array when nothing held, never `nil` |
| `keymap.new_tracker()` | `table` | A tracker object; call methods with `:` |
| `tracker:update(report)` | `table` (array) | Empty array when nothing changed, never `nil` |
| `ev.char` | `string` or `nil` | `nil` for Enter, arrows, F-keys, modifiers |

## Functions

### keymap.new_tracker([opts])

Create an edge tracker. **Create one tracker and reuse it** — it holds the
previous report as state. Creating a tracker inside the poll loop makes every
report look like a fresh press.

`opts`:

| Field | Default | Meaning |
|---|---|---|
| `caps_lock` | `false` | Initial Caps Lock state |
| `num_lock` | `true` | Initial Num Lock state |
| `scroll_lock` | `false` | Initial Scroll Lock state |
| `repeat_delay_ms` | none | Hold time before auto-repeat starts |
| `repeat_rate_ms` | none | Interval between auto-repeats |

### tracker:update(report [, now_ms])

Feed one report; get back the events it implies.

- `report`: a report table from `dev:read()`, or `nil`
- `now_ms`: monotonic milliseconds. Required only for auto-repeat.

**Returns**: an array of event tables:

| Field | Type | Meaning |
|---|---|---|
| `kind` | string | `"down"`, `"up"`, or `"repeat"` |
| `keycode` | integer | HID keycode |
| `name` | string | Canonical key name, e.g. `"A"`, `"ENTER"`, `"LEFT_CTRL"` |
| `char` | string or nil | Character produced, honouring shift and Caps Lock |
| `modifier` | integer | Raw modifier byte at the time of the event |
| `mods` | table | Decoded modifiers, same shape as `keymap.mods()` |

Modifier keys (Ctrl/Shift/Alt/GUI) generate their own `down` / `up` events
derived from the modifier byte, so a lone Ctrl press is observable.

Lock keys toggle tracker state on their `down` event, and the event's `char`
already reflects the new state.

Reports containing the rollover code (`0x01`, too many keys held) are ignored
entirely, as the standard requires.

### tracker:held()

**Returns**: sorted array of currently held non-modifier keycodes.

### tracker:locks()

**Returns**: `{ caps_lock, num_lock, scroll_lock }` booleans.

### tracker:reset()

Clear held-key state. Call after a device replug so a key that was down at
unplug time does not produce a stale `up` event.

### keymap.char(keycode [, opts])

Return the graphic character a keycode produces, or `nil`.

`opts`:

| Field | Default | Meaning |
|---|---|---|
| `shift` | `false` | Shift held |
| `caps_lock` | `false` | Caps Lock engaged (affects letters only) |
| `num_lock` | `true` | Keypad numeric mode |

Returns `nil` for every non-graphic key. **Enter and Tab return `nil`**, not
`"\n"` and `"\t"` — whether they mean a character or a command is the caller's
decision. Handle them by name:

```lua
if ev.char then
    text = text .. ev.char
elseif ev.name == "ENTER" then
    text = text .. "\n"       -- your choice, made explicitly
end
```

Shift and Caps Lock cancel out for letters (`shift + caps` gives lowercase).
Caps Lock does not affect digits or punctuation.

### keymap.name(keycode [, opts])

Return the canonical name. Pass `{ num_lock = false }` to get keypad navigation
names (`KP_8` becomes `UP`).

Unknown codes return `"UNKNOWN_0x<hex>"` rather than `nil`, so logging is always
safe. Do not use `keymap.name()` to test whether a code is valid — compare
against `keymap.KEY` or check `keymap.code(name)` instead.

### keymap.code(name)

Reverse lookup. Returns `nil` for unknown names.

### keymap.mods(modifier_byte)

Decode a modifier byte. All fields are booleans:

`ctrl`, `shift`, `alt`, `gui` (either side), plus `left_ctrl`, `left_shift`,
`left_alt`, `left_gui`, `right_ctrl`, `right_shift`, `right_alt`, `right_gui`.

```lua
local m = keymap.mods(report.modifier)
if m.ctrl and ev.name == "C" then --[[ Ctrl+C ]] end
```

### keymap.names(report [, opts])

Map every held key in a report to its name. Returns an empty array for `nil` or
malformed reports.

### keymap.describe(keycode, modifier [, opts])

Format a shortcut string: `"CTRL+SHIFT+C"`. Useful for logs and UI hints.

### keymap.is_modifier(keycode) / keymap.is_lock(keycode) / keymap.lock_name(keycode)

Classification helpers. `lock_name` returns `"caps_lock"`, `"num_lock"`,
`"scroll_lock"`, or `nil`.

## Constants

### keymap.KEY

Name to keycode table, for comparing without magic numbers:

```lua
if ev.keycode == keymap.KEY.ENTER then --[[ ... ]] end
```

Digit key names are the digit characters, so they need bracket syntax:

```lua
keymap.KEY.A          -- 0x04, letter A
keymap.KEY["1"]       -- 0x1E, the 1 key on the digit row
keymap.KEY.KP_1       -- 0x59, the 1 key on the keypad
```

`keymap.KEY.1` is a syntax error. `keymap.KEY["A"]` also works.

### keymap.MOD

Modifier bit masks: `LEFT_CTRL`, `LEFT_SHIFT`, `LEFT_ALT`, `LEFT_GUI`,
`RIGHT_CTRL`, `RIGHT_SHIFT`, `RIGHT_ALT`, `RIGHT_GUI`. Prefer
`keymap.mods()` over testing these by hand.

## Key coverage

Complete HID Usage Page 0x07 (Keyboard/Keypad), codes `0x04`-`0xA4` plus
modifiers `0xE0`-`0xE7`. That is a full 104/105-key layout:

| Group | Names |
|---|---|
| Letters | `A`-`Z` |
| Digit row | `"1"`-`"9"`, `"0"` |
| Punctuation | `MINUS` `EQUAL` `LEFT_BRACKET` `RIGHT_BRACKET` `BACKSLASH` `SEMICOLON` `QUOTE` `GRAVE` `COMMA` `PERIOD` `SLASH` |
| Control | `ENTER` `ESCAPE` `BACKSPACE` `TAB` `SPACE` |
| Function | `F1`-`F24` |
| Locks | `CAPS_LOCK` `NUM_LOCK` `SCROLL_LOCK` |
| Navigation | `INSERT` `HOME` `PAGE_UP` `DELETE` `END` `PAGE_DOWN` `UP` `DOWN` `LEFT` `RIGHT` |
| System | `PRINT_SCREEN` `PAUSE` `APPLICATION` `POWER` `MENU` `EXECUTE` `HELP` |
| Keypad | `KP_0`-`KP_9` `KP_PERIOD` `KP_ENTER` `KP_PLUS` `KP_MINUS` `KP_ASTERISK` `KP_SLASH` `KP_EQUAL` `KP_COMMA` |
| Editing | `AGAIN` `UNDO` `CUT` `COPY` `PASTE` `FIND` `SELECT` `STOP` `CLEAR` `CANCEL` |
| Media | `MUTE` `VOLUME_UP` `VOLUME_DOWN` |
| International | `INTERNATIONAL1`-`INTERNATIONAL9`, `LANG1`-`LANG9`, `NON_US_HASH`, `NON_US_BACKSLASH` |
| Modifiers | `LEFT_CTRL` `LEFT_SHIFT` `LEFT_ALT` `LEFT_GUI` `RIGHT_CTRL` `RIGHT_SHIFT` `RIGHT_ALT` `RIGHT_GUI` |

Non-US physical keys are mapped to their ANSI equivalents where one exists.
International and language keys resolve to names but produce no character.

## Common mistakes

| Wrong | Right | Why |
|---|---|---|
| `keymap.char()` in a poll loop to build text | `tracker:update()` and act on `kind == "down"` | Reports repeat while held; you get each character many times |
| `keymap.new_tracker()` inside the loop | Create once, outside | The tracker's whole job is remembering the previous report |
| Expecting `ev.char == "\n"` for Enter | Check `ev.name == "ENTER"` | `char` is `nil` for non-graphic keys, by design |
| `if ev.char then` to detect any keypress | Check `ev.kind == "down"` | Arrows, F-keys, and modifiers have no `char` |
| `keymap.KEY.1` | `keymap.KEY["1"]` | Digit names are not valid Lua identifiers |
| `keymap.name(code) == nil` to validate | `keymap.code(name) ~= nil` | `name()` returns `"UNKNOWN_0x.."`, never `nil` |
| `report.keys[1]` as "the key pressed" | Iterate tracker events | Up to 6 keys are held simultaneously and order is not meaningful |
| Reusing a tracker across a replug | `tracker:reset()` after reconnect | Keys held at unplug would emit stale `up` events |
| Passing `report.modifier` as `opts` to `char()` | `keymap.char(code, { shift = mods.shift })` | `opts` is a table of flags, not the raw byte |

## Example: shortcuts

```lua
local tracker = keymap.new_tracker()

for _, ev in ipairs(tracker:update(kb:read())) do
    if ev.kind == "down" then
        if ev.mods.ctrl and ev.name == "C" then
            print("copy")
        elseif ev.mods.ctrl and ev.mods.shift and ev.name == "Z" then
            print("redo")
        elseif ev.name == "F5" then
            print("refresh")
        end
    end
end
```

Modifier state comes from the event itself (`ev.mods`), so there is no need to
track shift or ctrl separately.

## Example: auto-repeat for held arrow keys

Auto-repeat needs both repeat options and a timestamp:

```lua
local system = require("system")

local tracker = keymap.new_tracker({
    repeat_delay_ms = 400,
    repeat_rate_ms = 50,
})

while kb:is_connected() do
    local now = system.millis()   -- milliseconds; system.uptime() is seconds
    for _, ev in ipairs(tracker:update(kb:read(), now)) do
        -- "down" fires once; "repeat" fires while the key stays held
        if (ev.kind == "down" or ev.kind == "repeat") and ev.name == "RIGHT" then
            cursor = cursor + 1
        end
    end
    delay.delay_ms(10)
end
```

Omit `now_ms` and no `repeat` events are ever produced — `down` and `up` still
work normally.

## Example: on-screen key display

```lua
local tracker = keymap.new_tracker()

for _, ev in ipairs(tracker:update(kb:read())) do
    if ev.kind == "down" then
        label:set_text(keymap.describe(ev.keycode, ev.modifier))
    end
end
```
