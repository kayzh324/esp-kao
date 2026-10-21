local arg_schema = require("arg_schema")
local json = require("json")
local storage = require("storage")

local ctx = arg_schema.parse(args, {
  autostart = arg_schema.bool({ default = false }),
  dry_run = arg_schema.bool({ default = false }),
  force = arg_schema.bool({ default = false }),
})

local a = type(args) == "table" and args or {}

local RESERVED_NAMES = {
  apps = true,
  tmp = true,
  skills = true,
  scripts = true,
  memory = true,
  sessions = true,
  scheduler = true,
  router_rules = true,
  static = true,
  system = true,
}

local FILE_ORDER = { "main.lua", "launcher.json", "config.json", "README.md" }

local MAIN_TEMPLATE = [[-- __APP_NAME__ entry point. This file is the only entry point of the app.
local storage = require("storage")

local APP_NAME = "__APP_NAME__"

local function app_dir()
  return storage.join_path(storage.get_root_dir(), "apps", APP_NAME)
end

local function app_file(...)
  return storage.join_path(app_dir(), ...)
end

local function run()
  -- TODO: implement __APP_NAME__ here.
  -- config.json holds user settings, data/ holds runtime data (create data/ on first use):
  --   app_file("config.json")
  --   app_file("data", "state.json")
  --   app_file("assets", "icon.png")
  print("[__APP_NAME__] app_dir=" .. app_dir())
  print("[__APP_NAME__] main.lua placeholder ran; replace this body with the real program")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
  print("[__APP_NAME__] ERROR: " .. tostring(err))
  error(err)
end
]]

local README_TEMPLATE = [[# __APP_NAME__

__DESCRIPTION__

## Layout

| Path | Purpose |
| --- | --- |
| main.lua | The only entry point of this app. |
| launcher.json | Launcher manifest: schema_version, id, display_name, entry, order, visible. Its presence is what makes the directory an app. |
| config.json | User-editable settings. |
| data/ | Runtime data once the app needs it: calibration values, recordings, logs, caches. |
| assets/ | Static files once the app ships them: images, fonts. |

`data/` and `assets/` are optional and created on demand; the app stays valid without them.

## Run it

Run it with lua_run_script, using these values:

  path = __APP_DIR__/main.lua
  args = {}

## Notes

__NOTES__
]]

local created = {}
local kept = {}

local function text_arg(key, default)
  local value = a[key]
  if type(value) ~= "string" then
    return default
  end
  local trimmed = value:gsub("^%s+", ""):gsub("%s+$", "")
  if trimmed == "" then
    return default
  end
  return trimmed
end

local function app_name_arg()
  local name = a.app_name
  if type(name) ~= "string" or name == "" then
    error("args.app_name is required")
  end
  if not name:match("^[a-z][a-z0-9_]*$") then
    error("args.app_name must be lowercase snake_case matching ^[a-z][a-z0-9_]*$: " .. tostring(name))
  end
  if #name > 32 then
    error("args.app_name must be 32 characters or fewer: " .. name)
  end
  if RESERVED_NAMES[name] then
    error("args.app_name is reserved and cannot be used for an app: " .. name)
  end
  return name
end

local function render(template, values)
  for key, value in pairs(values) do
    template = template:gsub(key, function()
      return value
    end)
  end
  return template
end

local function encode_object_pretty(obj)
  local keys = {}
  for key in pairs(obj) do
    keys[#keys + 1] = key
  end
  table.sort(keys)

  local lines = {}
  for _, key in ipairs(keys) do
    lines[#lines + 1] = string.format("  %s: %s", json.encode(key), json.encode(obj[key]))
  end
  return "{\n" .. table.concat(lines, ",\n") .. "\n}\n"
end

local function run()
  local app_name = app_name_arg()
  local description = text_arg("description", "")
  local version = text_arg("version", "0.1.0")
  local notes = text_arg("notes", "")

  local root = storage.get_root_dir()
  if root == "" or root == "/" then
    error("unexpected storage root: " .. tostring(root))
  end

  local apps_dir = storage.join_path(root, "apps")
  local app_dir = storage.join_path(apps_dir, app_name)
  if app_dir:sub(1, #apps_dir + 1) ~= apps_dir .. "/" then
    error("refusing to work outside " .. apps_dir)
  end

  local data_dir = storage.join_path(app_dir, "data")
  local assets_dir = storage.join_path(app_dir, "assets")

  if storage.exists(app_dir) and not ctx.force and not ctx.dry_run then
    error("app already exists: " .. app_dir
      .. " (pass force=true to fill in missing files, or dry_run=true to preview)")
  end

  local files = {
    ["main.lua"] = render(MAIN_TEMPLATE, { ["__APP_NAME__"] = app_name }),
    ["launcher.json"] = encode_object_pretty({
      schema_version = 1,
      id = app_name,
      display_name = description ~= "" and description or app_name,
      entry = "main.lua",
      order = 0,
      visible = true,
    }),
    ["config.json"] = encode_object_pretty({
      enabled = true,
      notes = "User-editable settings for this app.",
    }),
    ["README.md"] = render(README_TEMPLATE, {
      ["__APP_NAME__"] = app_name,
      ["__DESCRIPTION__"] = description ~= "" and description or "TODO: describe what this app does.",
      ["__NOTES__"] = notes ~= "" and notes or "TODO: notes for future maintenance of this app.",
      ["__APP_DIR__"] = app_dir,
    }),
  }

  local function ensure_dir(path)
    if storage.exists(path) then
      return
    end
    if ctx.dry_run then
      created[#created + 1] = path .. " (planned)"
      return
    end
    local ok, err = storage.mkdir(path)
    if not ok then
      error("failed to create directory " .. path .. ": " .. tostring(err))
    end
    created[#created + 1] = path
  end

  print("[app_layout] fs_new_app root=" .. root)
  print("[app_layout] app_dir=" .. app_dir)
  print("[app_layout] publish it after writing files: publish_app(app_id=" .. app_name .. ")")

  ensure_dir(apps_dir)
  ensure_dir(app_dir)
  ensure_dir(data_dir)
  ensure_dir(assets_dir)

  for _, file_name in ipairs(FILE_ORDER) do
    local path = storage.join_path(app_dir, file_name)
    if storage.exists(path) then
      kept[#kept + 1] = file_name
    elseif ctx.dry_run then
      created[#created + 1] = path .. " (planned)"
    else
      local ok, err = storage.write_file(path, files[file_name])
      if not ok then
        error("failed to write " .. path .. ": " .. tostring(err))
      end
      created[#created + 1] = path
    end
  end

  for _, path in ipairs(created) do
    print("[app_layout] create " .. path)
  end
  for _, name in ipairs(kept) do
    print("[app_layout] keep existing " .. name)
  end

  print(string.format("[app_layout] created=%d kept_existing=%d dry_run=%s",
    #created, #kept, tostring(ctx.dry_run)))
  print("[app_layout] next: edit " .. storage.join_path(app_dir, "main.lua"))
  print("[app_layout] fs_new_app done")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
  print("[app_layout] fs_new_app ERROR: " .. tostring(err))
  error(err)
end
