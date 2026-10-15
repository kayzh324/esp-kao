local arg_schema = require("arg_schema")
local storage = require("storage")

local ctx = arg_schema.parse(args, {
  dry_run = arg_schema.bool({ default = true }),
  max_depth = arg_schema.int({ default = 3, min = 1, max = 3 }),
})

local a = type(args) == "table" and args or {}

local MAX_ITEMS = 500

local keep = {}
if a.keep ~= nil then
  if type(a.keep) ~= "table" then
    error("args.keep must be an array of plain names")
  end
  for _, name in ipairs(a.keep) do
    if type(name) ~= "string" or name == "" or name:find("/", 1, true) or name:find("\\", 1, true) then
      error("args.keep entries must be plain names without slashes: " .. tostring(name))
    end
    keep[name] = true
  end
end

local files = {}
local dirs = {}
local kept = {}
local deferred = {}
local reached_limit = false
local total_bytes = 0

local function type_is_dir(value)
  if value == "dir" or value == "directory" or value == "folder" then
    return true
  end
  if value == "file" or value == "regular" then
    return false
  end
  return nil
end

local function is_dir_entry(path, entry)
  if type(entry) == "table" then
    local from_entry = type_is_dir(entry.type)
    if from_entry ~= nil then
      return from_entry
    end
  end

  local info = storage.stat(path)
  if type(info) == "table" then
    local from_stat = type_is_dir(info.type)
    if from_stat ~= nil then
      return from_stat
    end
  end
  return false
end

local function entry_size(path, entry)
  if type(entry) == "table" and type(entry.size) == "number" then
    return entry.size
  end
  local info = storage.stat(path)
  if type(info) == "table" and type(info.size) == "number" then
    return info.size
  end
  return 0
end

local function collect(dir, depth, tmp_dir)
  if depth > ctx.max_depth then
    return
  end
  if #files + #dirs >= MAX_ITEMS then
    reached_limit = true
    return
  end

  local ok, entries = pcall(storage.listdir, dir)
  if not ok or type(entries) ~= "table" then
    print("[app_layout] cannot list " .. dir)
    return
  end

  for _, entry in ipairs(entries) do
    local name = tostring(entry.name)
    local path = storage.join_path(dir, name)

    -- Safety guard: never touch anything outside <root>/tmp.
    if path:sub(1, #tmp_dir + 1) ~= tmp_dir .. "/" then
      error("refusing to touch " .. path .. " outside " .. tmp_dir)
    end

    if keep[name] then
      kept[#kept + 1] = path
    elseif is_dir_entry(path, entry) then
      if depth < ctx.max_depth then
        dirs[#dirs + 1] = { path = path, depth = depth }
        collect(path, depth + 1, tmp_dir)
      else
        deferred[#deferred + 1] = path
      end
    else
      total_bytes = total_bytes + entry_size(path, entry)
      files[#files + 1] = path
    end
  end
end

local function run()
  local root = storage.get_root_dir()
  if root == "" or root == "/" then
    error("unexpected storage root: " .. tostring(root))
  end

  local tmp_dir = storage.join_path(root, "tmp")
  print(string.format("[app_layout] fs_clean_tmp tmp_dir=%s dry_run=%s max_depth=%d",
    tmp_dir, tostring(ctx.dry_run), ctx.max_depth))

  if not storage.exists(tmp_dir) then
    print("[app_layout] tmp/ does not exist; run fs_init.lua first")
    print("[app_layout] fs_clean_tmp done")
    return
  end

  collect(tmp_dir, 1, tmp_dir)

  -- Deepest directories first so each directory is empty when it is removed.
  table.sort(dirs, function(lhs, rhs)
    if lhs.depth == rhs.depth then
      return lhs.path > rhs.path
    end
    return lhs.depth > rhs.depth
  end)

  local targets = {}
  for _, path in ipairs(files) do
    targets[#targets + 1] = path
  end
  for _, item in ipairs(dirs) do
    targets[#targets + 1] = item.path
  end

  for _, path in ipairs(kept) do
    print("[app_layout] keep " .. path)
  end
  for _, path in ipairs(deferred) do
    print("[app_layout] left in place (deeper than max_depth) " .. path)
  end
  if reached_limit then
    print(string.format("[app_layout] stopped collecting at %d entries; run again to clean the rest", MAX_ITEMS))
  end

  if ctx.dry_run then
    for index, path in ipairs(targets) do
      if index <= 20 then
        print("[app_layout] would remove " .. path)
      end
    end
    if #targets > 20 then
      print(string.format("[app_layout] ... and %d more", #targets - 20))
    end
    print(string.format("[app_layout] dry_run: nothing deleted (files=%d dirs=%d bytes=%d)",
      #files, #dirs, total_bytes))
    print("[app_layout] re-run with dry_run=false to delete these files")
    print("[app_layout] fs_clean_tmp done")
    return
  end

  local removed = 0
  local failed = 0
  for _, path in ipairs(targets) do
    local ok, err = storage.remove(path)
    if ok then
      removed = removed + 1
    else
      failed = failed + 1
      print("[app_layout] failed to remove " .. path .. ": " .. tostring(err))
    end
  end

  print(string.format("[app_layout] removed=%d failed=%d freed_bytes=%d", removed, failed, total_bytes))
  print("[app_layout] fs_clean_tmp done")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
  print("[app_layout] fs_clean_tmp ERROR: " .. tostring(err))
  error(err)
end
