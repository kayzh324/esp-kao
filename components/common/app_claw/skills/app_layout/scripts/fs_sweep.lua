local arg_schema = require("arg_schema")
local storage = require("storage")

local ctx = arg_schema.parse(args, {
  include_legacy = arg_schema.bool({ default = true }),
  include_tmp = arg_schema.bool({ default = true }),
  max_depth = arg_schema.int({ default = 3, min = 1, max = 3 }),
  max_items = arg_schema.int({ default = 200, min = 10, max = 1000 }),
})

local ROOT = storage.get_root_dir()

local SCRIPT_EXT = { lua = true }

local DATA_EXT = {
  json = true, jsonl = true, csv = true, tsv = true, log = true, txt = true,
  dat = true, bin = true, wav = true, mp3 = true, aac = true, cfg = true,
  ini = true, conf = true, yaml = true, yml = true, db = true, sqlite = true,
  idx = true, bak = true, jpg = true, jpeg = true, png = true, bmp = true,
  gif = true, ico = true,
}

local APP_REQUIRED_FILES = { "main.lua", "config.json", "README.md" }
-- data/ and assets/ are created on demand: their absence is a note, never a warning.
local APP_OPTIONAL_DIRS = {
  { name = "data", note = "keeps no state" },
  { name = "assets", note = "ships no static files" },
}
local APP_KNOWN_FILES = { ["main.lua"] = true, ["launcher.json"] = true, ["config.json"] = true }
local EXPECTED_ROOT_DIRS = {
  apps = true, tmp = true, skills = true, scripts = true, memory = true,
  sessions = true, scheduler = true, router_rules = true, static = true,
}

local printed = 0
local truncated = false
local counts = { app = 0, warn = 0, stray = 0, legacy = 0, skip = 0, note = 0, tmp_files = 0, tmp_dirs = 0, tmp_bytes = 0 }

local function emit(tag, text)
  if printed >= ctx.max_items then
    truncated = true
    return
  end
  printed = printed + 1
  print(string.format("[app_layout] %s %s", tag, text))
end

local function rel(path)
  if path == ROOT then
    return "."
  end
  local prefix = ROOT .. "/"
  if path:sub(1, #prefix) == prefix then
    return path:sub(#prefix + 1)
  end
  return path
end

local function ext_of(name)
  local ext = name:match("%.([%w]+)$")
  if type(ext) == "string" then
    return ext:lower()
  end
  return nil
end

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

local function list_entries(dir)
  local ok, entries = pcall(storage.listdir, dir)
  if not ok or type(entries) ~= "table" then
    return {}
  end
  return entries
end

local function scan_app(app_path)
  local missing = {}
  for _, file_name in ipairs(APP_REQUIRED_FILES) do
    if not storage.exists(storage.join_path(app_path, file_name)) then
      missing[#missing + 1] = file_name
    end
  end

  counts.app = counts.app + 1
  if #missing > 0 then
    counts.warn = counts.warn + 1
    emit("WARN", string.format("app %s is missing %s; run fs_new_app.lua with force=true",
      rel(app_path), table.concat(missing, ", ")))
  else
    emit("app OK", rel(app_path))
  end

  -- Optional structure: absent data/ or assets/ is informational, never a warning.
  for _, dir in ipairs(APP_OPTIONAL_DIRS) do
    if not storage.exists(storage.join_path(app_path, dir.name)) then
      counts.note = counts.note + 1
      emit("note", string.format("%s %s; %s/ not needed", rel(app_path), dir.note, dir.name))
    end
  end

  for _, entry in ipairs(list_entries(app_path)) do
    local name = tostring(entry.name)
    local child = storage.join_path(app_path, name)
    local lower = name:lower()

    if is_dir_entry(child, entry) then
      if lower == "data" or lower == "assets" then
        -- Expected structure.
      elseif name:sub(1, 1) == "_" then
        counts.skip = counts.skip + 1
        emit("skip", rel(child) .. " (underscore-prefixed, not an app)")
      else
        counts.warn = counts.warn + 1
        emit("WARN", rel(child) .. " is an unexpected app subdirectory; keep only data/ and assets/")
      end
    else
      local ext = ext_of(name)
      if APP_KNOWN_FILES[name] or lower == "readme.md" then
        -- Expected files.
      elseif SCRIPT_EXT[ext] then
        counts.skip = counts.skip + 1
        emit("skip", rel(child) .. " (extra module inside the app; shared helpers belong in apps/_lib/)")
      elseif DATA_EXT[ext] then
        counts.stray = counts.stray + 1
        emit("STRAY", string.format(
          "data file %s should live in %s/data/ or %s/assets/; create that directory on demand and move the file into it",
          rel(child), rel(app_path), rel(app_path)))
      else
        counts.skip = counts.skip + 1
        emit("skip", rel(child) .. " (unclassified file)")
      end
    end
  end
end

local function scan_apps()
  local apps_dir = storage.join_path(ROOT, "apps")
  if not storage.exists(apps_dir) then
    emit("WARN", "apps/ does not exist; run fs_init.lua")
    return
  end

  for _, entry in ipairs(list_entries(apps_dir)) do
    local name = tostring(entry.name)
    local path = storage.join_path(apps_dir, name)

    if is_dir_entry(path, entry) then
      if name:sub(1, 1) == "_" then
        counts.skip = counts.skip + 1
        emit("skip", rel(path) .. " (underscore-prefixed lib, not an app)")
      elseif storage.exists(storage.join_path(path, "launcher.json")) then
        scan_app(path)
      else
        counts.stray = counts.stray + 1
        emit("STRAY", rel(path) .. " has no launcher.json so it is not an app; scaffold it with fs_new_app.lua or move it to tmp/")
      end
    else
      counts.stray = counts.stray + 1
      emit("STRAY", rel(path) .. " is a loose file directly in apps/; apps are directories with launcher.json")
    end
  end
end

local function classify_legacy(path, entry)
  counts.legacy = counts.legacy + 1
  local ext = ext_of(tostring(entry.name))
  if SCRIPT_EXT[ext] then
    emit("legacy", rel(path) .. " (script outside apps/ and tmp/; report-only)")
  elseif DATA_EXT[ext] then
    emit("legacy", rel(path) .. " (data outside apps/<app_name>/data/; report-only)")
  else
    emit("legacy", rel(path) .. " (report-only)")
  end
end

local function scan_root_files()
  for _, entry in ipairs(list_entries(ROOT)) do
    local name = tostring(entry.name)
    local path = storage.join_path(ROOT, name)
    if not is_dir_entry(path, entry) then
      local ext = ext_of(name)
      if SCRIPT_EXT[ext] then
        counts.stray = counts.stray + 1
        emit("STRAY", rel(path) .. " is a loose script in the data root; move it to tmp/ or apps/<app_name>/main.lua")
      elseif DATA_EXT[ext] then
        counts.stray = counts.stray + 1
        emit("STRAY", rel(path) .. " is a loose data file in the data root; move it into apps/<app_name>/data/")
      end
    elseif not EXPECTED_ROOT_DIRS[name] and name:sub(1, 1) ~= "_" then
      counts.skip = counts.skip + 1
      emit("skip", rel(path) .. " (top-level directory outside this layout; not managed)")
    end
  end
end

local function scan_legacy_area()
  local dir = storage.join_path(ROOT, "scripts")
  if not storage.exists(dir) then
    return
  end

  emit("legacy", rel(dir) .. " is the official scripts area and is not managed by this skill (report-only)")

  for _, entry in ipairs(list_entries(dir)) do
    local path = storage.join_path(dir, tostring(entry.name))
    if is_dir_entry(path, entry) then
      if ctx.max_depth >= 3 then
        for _, sub in ipairs(list_entries(path)) do
          classify_legacy(storage.join_path(path, tostring(sub.name)), sub)
        end
      else
        classify_legacy(path, entry)
      end
    else
      classify_legacy(path, entry)
    end
  end
end

local function scan_tmp()
  local dir = storage.join_path(ROOT, "tmp")
  if not storage.exists(dir) then
    emit("tmp", "tmp/ does not exist; run fs_init.lua")
    return
  end

  local names = {}
  for _, entry in ipairs(list_entries(dir)) do
    local path = storage.join_path(dir, tostring(entry.name))
    if is_dir_entry(path, entry) then
      counts.tmp_dirs = counts.tmp_dirs + 1
    else
      counts.tmp_files = counts.tmp_files + 1
      if type(entry.size) == "number" then
        counts.tmp_bytes = counts.tmp_bytes + entry.size
      else
        local info = storage.stat(path)
        if type(info) == "table" and type(info.size) == "number" then
          counts.tmp_bytes = counts.tmp_bytes + info.size
        end
      end
      if #names < 10 then
        names[#names + 1] = tostring(entry.name)
      end
    end
  end

  emit("tmp", string.format("%s holds %d files, %d dirs, %d bytes; safe to delete in bulk with fs_clean_tmp.lua",
    rel(dir), counts.tmp_files, counts.tmp_dirs, counts.tmp_bytes))
  if #names > 0 then
    emit("tmp", "sample: " .. table.concat(names, ", "))
  end
end

local function run()
  if ROOT == "" or ROOT == "/" then
    error("unexpected storage root: " .. tostring(ROOT))
  end

  print(string.format("[app_layout] sweep root=%s max_depth=%d", ROOT, ctx.max_depth))

  scan_root_files()
  scan_apps()
  if ctx.include_legacy then
    scan_legacy_area()
  end
  if ctx.include_tmp then
    scan_tmp()
  end

  local space = storage.get_free_space()
  local free = type(space) == "table" and space.free or nil
  emit("summary", string.format("apps=%d warn=%d stray=%d legacy=%d note=%d tmp_files=%d free_bytes=%s",
    counts.app,
    counts.warn,
    counts.stray,
    counts.legacy,
    counts.note,
    counts.tmp_files,
    type(free) == "number" and tostring(free) or "unknown"))

  if truncated then
    print(string.format("[app_layout] output truncated at max_items=%d; raise max_items for a full list", ctx.max_items))
  end

  print("[app_layout] fs_sweep done (report-only, nothing was moved or deleted)")
end

local ok, err = xpcall(run, debug.traceback)
if not ok then
  print("[app_layout] fs_sweep ERROR: " .. tostring(err))
  error(err)
end
