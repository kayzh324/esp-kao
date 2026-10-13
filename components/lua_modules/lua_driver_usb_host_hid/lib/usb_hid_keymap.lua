-- Pure-Lua HID boot keyboard keymap and edge tracker.
--
-- Translates raw USB HID keyboard report bytes into key names, characters, and
-- press/release events. Contains no hardware access and does not require the
-- usb_host_hid driver, so it can also be used against BLE HID reports, replayed
-- captures, or unit-test fixtures.
--
-- Layout is US ANSI (see M.LAYOUT).

local M = {}

M.LAYOUT = "us"

--- Modifier bit masks as they appear in the report modifier byte.
M.MOD = {
    LEFT_CTRL   = 0x01,
    LEFT_SHIFT  = 0x02,
    LEFT_ALT    = 0x04,
    LEFT_GUI    = 0x08,
    RIGHT_CTRL  = 0x10,
    RIGHT_SHIFT = 0x20,
    RIGHT_ALT   = 0x40,
    RIGHT_GUI   = 0x80,
}

local MOD_CTRL  = 0x11
local MOD_SHIFT = 0x22
local MOD_ALT   = 0x44
local MOD_GUI   = 0x88

--- Reserved codes 0x00-0x03: no key / rollover / POST fail / undefined error.
M.KEY_NONE = 0x00
M.KEY_ROLLOVER = 0x01

-- Master table: { keycode, name, plain char, shifted char }
-- `false` for a char means the key produces no graphic character.
-- Covers HID Usage Page 0x07 (Keyboard/Keypad) codes 0x04-0xA4 plus the
-- modifier codes 0xE0-0xE7, i.e. a complete 104/105-key layout.
local ENTRIES = {}

local function entry(code, name, plain, shifted)
    ENTRIES[#ENTRIES + 1] = { code = code, name = name, plain = plain, shifted = shifted }
end

-- Letters 0x04-0x1D. Generated to avoid transcription mistakes.
for i = 0, 25 do
    local lower = string.char(string.byte("a") + i)
    local upper = string.char(string.byte("A") + i)
    entry(0x04 + i, upper, lower, upper)
end

-- Digit row 0x1E-0x27
entry(0x1E, "1", "1", "!")
entry(0x1F, "2", "2", "@")
entry(0x20, "3", "3", "#")
entry(0x21, "4", "4", "$")
entry(0x22, "5", "5", "%")
entry(0x23, "6", "6", "^")
entry(0x24, "7", "7", "&")
entry(0x25, "8", "8", "*")
entry(0x26, "9", "9", "(")
entry(0x27, "0", "0", ")")

-- Control and punctuation 0x28-0x38
entry(0x28, "ENTER", false, false)
entry(0x29, "ESCAPE", false, false)
entry(0x2A, "BACKSPACE", false, false)
entry(0x2B, "TAB", false, false)
entry(0x2C, "SPACE", " ", " ")
entry(0x2D, "MINUS", "-", "_")
entry(0x2E, "EQUAL", "=", "+")
entry(0x2F, "LEFT_BRACKET", "[", "{")
entry(0x30, "RIGHT_BRACKET", "]", "}")
entry(0x31, "BACKSLASH", "\\", "|")
-- 0x32 is the non-US "#~" key; on ANSI keyboards it repeats backslash.
entry(0x32, "NON_US_HASH", "\\", "|")
entry(0x33, "SEMICOLON", ";", ":")
entry(0x34, "QUOTE", "'", "\"")
entry(0x35, "GRAVE", "`", "~")
entry(0x36, "COMMA", ",", "<")
entry(0x37, "PERIOD", ".", ">")
entry(0x38, "SLASH", "/", "?")

-- Locks and function row 0x39-0x45
entry(0x39, "CAPS_LOCK", false, false)
for i = 1, 12 do
    entry(0x39 + i, "F" .. i, false, false)
end

-- System and navigation 0x46-0x52
entry(0x46, "PRINT_SCREEN", false, false)
entry(0x47, "SCROLL_LOCK", false, false)
entry(0x48, "PAUSE", false, false)
entry(0x49, "INSERT", false, false)
entry(0x4A, "HOME", false, false)
entry(0x4B, "PAGE_UP", false, false)
entry(0x4C, "DELETE", false, false)
entry(0x4D, "END", false, false)
entry(0x4E, "PAGE_DOWN", false, false)
entry(0x4F, "RIGHT", false, false)
entry(0x50, "LEFT", false, false)
entry(0x51, "DOWN", false, false)
entry(0x52, "UP", false, false)

-- Keypad 0x53-0x63. Digit keys are Num Lock dependent; see M.char().
entry(0x53, "NUM_LOCK", false, false)
entry(0x54, "KP_SLASH", "/", "/")
entry(0x55, "KP_ASTERISK", "*", "*")
entry(0x56, "KP_MINUS", "-", "-")
entry(0x57, "KP_PLUS", "+", "+")
entry(0x58, "KP_ENTER", false, false)
entry(0x59, "KP_1", "1", false)
entry(0x5A, "KP_2", "2", false)
entry(0x5B, "KP_3", "3", false)
entry(0x5C, "KP_4", "4", false)
entry(0x5D, "KP_5", "5", false)
entry(0x5E, "KP_6", "6", false)
entry(0x5F, "KP_7", "7", false)
entry(0x60, "KP_8", "8", false)
entry(0x61, "KP_9", "9", false)
entry(0x62, "KP_0", "0", false)
entry(0x63, "KP_PERIOD", ".", false)

-- Extended 0x64-0x73
entry(0x64, "NON_US_BACKSLASH", "\\", "|")
entry(0x65, "APPLICATION", false, false)
entry(0x66, "POWER", false, false)
entry(0x67, "KP_EQUAL", "=", "=")
for i = 13, 24 do
    entry(0x68 + (i - 13), "F" .. i, false, false)
end
entry(0x74, "EXECUTE", false, false)

-- Editing and international 0x75-0x9F
entry(0x75, "HELP", false, false)
entry(0x76, "MENU", false, false)
entry(0x77, "SELECT", false, false)
entry(0x78, "STOP", false, false)
entry(0x79, "AGAIN", false, false)
entry(0x7A, "UNDO", false, false)
entry(0x7B, "CUT", false, false)
entry(0x7C, "COPY", false, false)
entry(0x7D, "PASTE", false, false)
entry(0x7E, "FIND", false, false)
entry(0x7F, "MUTE", false, false)
entry(0x80, "VOLUME_UP", false, false)
entry(0x81, "VOLUME_DOWN", false, false)
entry(0x82, "LOCKING_CAPS_LOCK", false, false)
entry(0x83, "LOCKING_NUM_LOCK", false, false)
entry(0x84, "LOCKING_SCROLL_LOCK", false, false)
entry(0x85, "KP_COMMA", ",", ",")
entry(0x86, "KP_EQUAL_AS400", "=", "=")
entry(0x87, "INTERNATIONAL1", false, false)
entry(0x88, "INTERNATIONAL2", false, false)
entry(0x89, "INTERNATIONAL3", false, false)
entry(0x8A, "INTERNATIONAL4", false, false)
entry(0x8B, "INTERNATIONAL5", false, false)
entry(0x8C, "INTERNATIONAL6", false, false)
entry(0x8D, "INTERNATIONAL7", false, false)
entry(0x8E, "INTERNATIONAL8", false, false)
entry(0x8F, "INTERNATIONAL9", false, false)
entry(0x90, "LANG1", false, false)
entry(0x91, "LANG2", false, false)
entry(0x92, "LANG3", false, false)
entry(0x93, "LANG4", false, false)
entry(0x94, "LANG5", false, false)
entry(0x95, "LANG6", false, false)
entry(0x96, "LANG7", false, false)
entry(0x97, "LANG8", false, false)
entry(0x98, "LANG9", false, false)
entry(0x99, "ALTERNATE_ERASE", false, false)
entry(0x9A, "SYSREQ_ATTENTION", false, false)
entry(0x9B, "CANCEL", false, false)
entry(0x9C, "CLEAR", false, false)
entry(0x9D, "PRIOR", false, false)
entry(0x9E, "RETURN", false, false)
entry(0x9F, "SEPARATOR", false, false)

-- Keypad extras 0xA0-0xA4
entry(0xA0, "OUT", false, false)
entry(0xA1, "OPER", false, false)
entry(0xA2, "CLEAR_AGAIN", false, false)
entry(0xA3, "CRSEL_PROPS", false, false)
entry(0xA4, "EXSEL", false, false)

-- Modifier keycodes 0xE0-0xE7. These appear in report.keys only on some
-- keyboards; the modifier byte is the reliable source. Tracker synthesizes
-- events for them from that byte.
entry(0xE0, "LEFT_CTRL", false, false)
entry(0xE1, "LEFT_SHIFT", false, false)
entry(0xE2, "LEFT_ALT", false, false)
entry(0xE3, "LEFT_GUI", false, false)
entry(0xE4, "RIGHT_CTRL", false, false)
entry(0xE5, "RIGHT_SHIFT", false, false)
entry(0xE6, "RIGHT_ALT", false, false)
entry(0xE7, "RIGHT_GUI", false, false)

local BY_CODE = {}
local BY_NAME = {}
M.KEY = {}

for _, e in ipairs(ENTRIES) do
    BY_CODE[e.code] = e
    BY_NAME[e.name] = e
    M.KEY[e.name] = e.code
end

local LOCK_KEYS = {
    [0x39] = "caps_lock",
    [0x53] = "num_lock",
    [0x47] = "scroll_lock",
}

-- Keypad digit/period codes, whose character depends on Num Lock.
local KEYPAD_NUMERIC = {
    [0x59] = true, [0x5A] = true, [0x5B] = true, [0x5C] = true, [0x5D] = true,
    [0x5E] = true, [0x5F] = true, [0x60] = true, [0x61] = true, [0x62] = true,
    [0x63] = true,
}

-- Navigation names the keypad produces when Num Lock is off.
local KEYPAD_NAV = {
    [0x59] = "END",    [0x5A] = "DOWN",  [0x5B] = "PAGE_DOWN",
    [0x5C] = "LEFT",   [0x5D] = "CLEAR", [0x5E] = "RIGHT",
    [0x5F] = "HOME",   [0x60] = "UP",    [0x61] = "PAGE_UP",
    [0x62] = "INSERT", [0x63] = "DELETE",
}

local LETTER_MIN, LETTER_MAX = 0x04, 0x1D

--- Return true when the keycode is a modifier key (0xE0-0xE7).
function M.is_modifier(keycode)
    return type(keycode) == "number" and keycode >= 0xE0 and keycode <= 0xE7
end

--- Return true when the keycode toggles a lock (Caps/Num/Scroll).
function M.is_lock(keycode)
    return LOCK_KEYS[keycode] ~= nil
end

--- Return the lock name a keycode toggles, or nil.
function M.lock_name(keycode)
    return LOCK_KEYS[keycode]
end

--- Decode a modifier byte into a flag table.
-- Side keys are reported both individually and merged: `shift` is true when
-- either shift is held.
function M.mods(modifier)
    modifier = modifier or 0
    return {
        ctrl        = (modifier & MOD_CTRL) ~= 0,
        shift       = (modifier & MOD_SHIFT) ~= 0,
        alt         = (modifier & MOD_ALT) ~= 0,
        gui         = (modifier & MOD_GUI) ~= 0,
        left_ctrl   = (modifier & M.MOD.LEFT_CTRL) ~= 0,
        left_shift  = (modifier & M.MOD.LEFT_SHIFT) ~= 0,
        left_alt    = (modifier & M.MOD.LEFT_ALT) ~= 0,
        left_gui    = (modifier & M.MOD.LEFT_GUI) ~= 0,
        right_ctrl  = (modifier & M.MOD.RIGHT_CTRL) ~= 0,
        right_shift = (modifier & M.MOD.RIGHT_SHIFT) ~= 0,
        right_alt   = (modifier & M.MOD.RIGHT_ALT) ~= 0,
        right_gui   = (modifier & M.MOD.RIGHT_GUI) ~= 0,
    }
end

--- Return the canonical name for a keycode.
-- Unknown codes yield "UNKNOWN_0x<hex>" so logs stay readable.
-- @param opts optional { num_lock = boolean } to resolve keypad navigation names
function M.name(keycode, opts)
    if type(keycode) ~= "number" then
        return nil
    end

    if opts and opts.num_lock == false and KEYPAD_NAV[keycode] then
        return KEYPAD_NAV[keycode]
    end

    local e = BY_CODE[keycode]
    if e then
        return e.name
    end
    return string.format("UNKNOWN_0x%02X", keycode)
end

--- Return the keycode for a canonical name, or nil.
function M.code(name)
    local e = BY_NAME[name]
    return e and e.code or nil
end

--- Return the graphic character a keycode produces, or nil.
--
-- Returns nil for keys that produce no character (Enter, Tab, Backspace,
-- arrows, function keys, modifiers). Enter and Tab are deliberately excluded:
-- whether they mean "\n" / "\t" or a command is the caller's decision.
--
-- @param opts optional {
--   shift = boolean,      -- shift held
--   caps_lock = boolean,  -- caps lock engaged (letters only)
--   num_lock = boolean,   -- keypad numeric mode; defaults to true
-- }
function M.char(keycode, opts)
    local e = BY_CODE[keycode]
    if not e then
        return nil
    end

    opts = opts or {}
    local shift = opts.shift and true or false

    if KEYPAD_NUMERIC[keycode] then
        -- Num Lock defaults to on: absent state, the keypad types digits.
        if opts.num_lock == false then
            return nil
        end
        return e.plain or nil
    end

    if keycode >= LETTER_MIN and keycode <= LETTER_MAX then
        -- Caps Lock and Shift cancel each other out for letters.
        local upper = shift ~= (opts.caps_lock and true or false)
        return upper and e.shifted or e.plain
    end

    local ch = shift and e.shifted or e.plain
    if ch == false then
        return nil
    end
    return ch
end

--- Map every held key in a report to its name.
-- @return array of names (empty when nothing is held)
function M.names(report, opts)
    local out = {}
    if type(report) ~= "table" or type(report.keys) ~= "table" then
        return out
    end
    for _, code in ipairs(report.keys) do
        out[#out + 1] = M.name(code, opts)
    end
    return out
end

--- Format a modifier byte and keycode as a shortcut string, e.g. "CTRL+SHIFT+C".
function M.describe(keycode, modifier, opts)
    local mods = M.mods(modifier)
    local parts = {}
    if mods.ctrl then parts[#parts + 1] = "CTRL" end
    if mods.alt then parts[#parts + 1] = "ALT" end
    if mods.shift then parts[#parts + 1] = "SHIFT" end
    if mods.gui then parts[#parts + 1] = "GUI" end
    parts[#parts + 1] = M.name(keycode, opts) or "?"
    return table.concat(parts, "+")
end

-- ---------------------------------------------------------------------------
-- Tracker: turns the held-key snapshots in reports into edge events.
-- ---------------------------------------------------------------------------

local Tracker = {}
Tracker.__index = Tracker
Tracker.__name = "usb_hid_keymap.tracker"

--- Create a keyboard edge tracker.
--
-- HID reports carry the *currently held* keys, not key events. The tracker
-- diffs consecutive reports and reports what changed.
--
-- @param opts optional {
--   caps_lock = boolean,   -- initial lock states, default false
--   num_lock = boolean,    -- default true
--   scroll_lock = boolean, -- default false
--   repeat_delay_ms = int, -- first auto-repeat after this hold time
--   repeat_rate_ms = int,  -- interval between repeats
-- }
-- Auto-repeat is emitted only when both repeat_delay_ms and repeat_rate_ms are
-- set AND update() is called with a timestamp.
function M.new_tracker(opts)
    opts = opts or {}
    local self = setmetatable({}, Tracker)

    self.prev = {}
    self.prev_modifier = 0
    self.caps_lock = opts.caps_lock and true or false
    self.num_lock = opts.num_lock ~= false
    self.scroll_lock = opts.scroll_lock and true or false
    self.repeat_delay_ms = opts.repeat_delay_ms
    self.repeat_rate_ms = opts.repeat_rate_ms
    self.hold_since = {}
    self.last_repeat = {}

    return self
end

--- Current lock states as a table.
function Tracker:locks()
    return {
        caps_lock = self.caps_lock,
        num_lock = self.num_lock,
        scroll_lock = self.scroll_lock,
    }
end

--- Sorted list of currently held non-modifier keycodes.
function Tracker:held()
    local out = {}
    for code in pairs(self.prev) do
        out[#out + 1] = code
    end
    table.sort(out)
    return out
end

--- Reset all held-key state. Call after a device replug.
function Tracker:reset()
    self.prev = {}
    self.prev_modifier = 0
    self.hold_since = {}
    self.last_repeat = {}
end

local function make_event(self, kind, code, modifier)
    local opts = {
        shift = (modifier & MOD_SHIFT) ~= 0,
        caps_lock = self.caps_lock,
        num_lock = self.num_lock,
    }
    return {
        kind = kind,
        keycode = code,
        name = M.name(code, opts),
        char = M.char(code, opts),
        modifier = modifier,
        mods = M.mods(modifier),
    }
end

--- Feed one report and get back the events it implies.
--
-- @param report a report table from the driver, or nil (treated as "no keys")
-- @param now_ms optional monotonic milliseconds, required for auto-repeat
-- @return array of event tables, each { kind, keycode, name, char, modifier, mods }
--         kind is "down", "up", or "repeat"
function Tracker:update(report, now_ms)
    local events = {}
    local keys = {}
    local modifier = 0

    if type(report) == "table" then
        modifier = report.modifier or 0
        if type(report.keys) == "table" then
            for _, code in ipairs(report.keys) do
                -- Rollover means the keyboard cannot report the real set; the
                -- standard response is to ignore the report entirely.
                if code == M.KEY_ROLLOVER then
                    return events
                end
                if code ~= M.KEY_NONE and not M.is_modifier(code) then
                    keys[code] = true
                end
            end
        end
    end

    -- Modifier edges, derived from the modifier byte.
    local changed = modifier ~ self.prev_modifier
    if changed ~= 0 then
        for bit = 0, 7 do
            local mask = 1 << bit
            if (changed & mask) ~= 0 then
                local code = 0xE0 + bit
                local kind = (modifier & mask) ~= 0 and "down" or "up"
                events[#events + 1] = make_event(self, kind, code, modifier)
            end
        end
    end

    -- Releases.
    for code in pairs(self.prev) do
        if not keys[code] then
            events[#events + 1] = make_event(self, "up", code, modifier)
            self.hold_since[code] = nil
            self.last_repeat[code] = nil
        end
    end

    -- Presses. Lock keys toggle before the event is built so the event's
    -- char already reflects the new lock state.
    for code in pairs(keys) do
        if not self.prev[code] then
            local lock = LOCK_KEYS[code]
            if lock then
                self[lock] = not self[lock]
            end
            events[#events + 1] = make_event(self, "down", code, modifier)
            if now_ms then
                self.hold_since[code] = now_ms
                self.last_repeat[code] = now_ms
            end
        end
    end

    -- Auto-repeat for keys held across updates.
    if now_ms and self.repeat_delay_ms and self.repeat_rate_ms then
        for code in pairs(keys) do
            if self.prev[code] and not LOCK_KEYS[code] then
                local since = self.hold_since[code]
                local last = self.last_repeat[code]
                if since and last and
                        (now_ms - since) >= self.repeat_delay_ms and
                        (now_ms - last) >= self.repeat_rate_ms then
                    events[#events + 1] = make_event(self, "repeat", code, modifier)
                    self.last_repeat[code] = now_ms
                end
            end
        end
    end

    self.prev = keys
    self.prev_modifier = modifier
    return events
end

return M
