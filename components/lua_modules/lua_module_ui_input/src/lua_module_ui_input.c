/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `ui_input` Lua binding (added): inject USB HID reports into the system UI.
 * The HID transport stays in Lua (usb_host_hid); this module only forwards
 * events, so touch and HID can coexist.
 */
#include "lua_module_ui_input.h"

#include "cap_lua.h"
#include "lauxlib.h"
#include "system_ui_input.h"

static int lua_ui_input_mouse(lua_State *L)
{
    int dx = (int)luaL_optinteger(L, 1, 0);
    int dy = (int)luaL_optinteger(L, 2, 0);
    uint32_t buttons = (uint32_t)luaL_optinteger(L, 3, 0);
    int wheel = (int)luaL_optinteger(L, 4, 0);

    system_ui_input_mouse(dx, dy, buttons, wheel);
    return 0;
}

static int lua_ui_input_key(lua_State *L)
{
    uint32_t usage = (uint32_t)luaL_checkinteger(L, 1);
    bool pressed = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2);

    system_ui_input_key(usage, pressed);
    return 0;
}

static int lua_ui_input_set_hid(lua_State *L)
{
    bool keyboard = lua_toboolean(L, 1);
    bool mouse = lua_toboolean(L, 2);

    system_ui_input_set_hid(keyboard, mouse);
    return 0;
}

static int lua_ui_input_status(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, system_ui_input_has_hid());
    lua_setfield(L, -2, "hid");
    lua_pushboolean(L, system_ui_input_no_touch());
    lua_setfield(L, -2, "no_touch");
    return 1;
}

int luaopen_ui_input(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        { "mouse", lua_ui_input_mouse },
        { "key", lua_ui_input_key },
        { "set_hid", lua_ui_input_set_hid },
        { "status", lua_ui_input_status },
        { NULL, NULL },
    };

    luaL_newlib(L, funcs);
    return 1;
}

esp_err_t lua_module_ui_input_register(void)
{
    return cap_lua_register_module("ui_input", luaopen_ui_input);
}
