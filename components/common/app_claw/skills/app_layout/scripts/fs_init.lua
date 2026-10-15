local arg_schema = require("arg_schema")
local storage = require("storage")

local ctx = arg_schema.parse(args, {
  verbose = arg_schema.bool({ default = true }),
})

local created = {}
local present = {}

local function ensure_dir(path)
  if storage.exists(path) then
    present[#present + 1] = path
    return
  end

  -- Build parents step by step so nested layout paths work on a fresh device.
  local current = ""
  if path:sub(1, 1) == "/" then
    current = "/"
  end

  for part in path:gmatch("[^/]+") do
    current = storage.join_path(current, part)
    if not storage.exists(current) then
      local ok, err = storage.mkdir(current)
      if not ok then
        error("failed to create directory " .. current .. ": " .. tostring(err))
      end
      created[#created + 1] = current
    end
  end
end

local function run()
  local root = storage.get_root_dir()
  if root == "" or root == "/" then
    error("unexpected storage root: " .. tostring(root))
  end

  local apps_dir = storage.join_path(root, "apps")
  local lib_dir = storage.join_path(root, "apps", "_lib")
  local tmp_dir = storage.join_path(root, "tmp")

  print("[app_layout] fs_init root=" .. root)

  ensure_dir(apps_dir)
  ensure_dir(lib_dir)
  ensure_dir(tmp_dir)

  for _, path in ipairs(created) do
    print("[app_layout] created " .. path)
  end

  if ctx.verbose then
    for _, path in ipairs(present) do
      print("[app_layout] present " .. path)
    end
  end

  print(string.format("[app_layout] files_created=%d already_present=%d", #created, #present))
  print("[app_layout] layout: apps/ apps/_lib/ tmp/ (apps/<app_name>/main.lua is the only entry point)")
  print("[app_layout] fs_init done")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
  print("[app_layout] fs_init ERROR: " .. tostring(err))
  error(err)
end
