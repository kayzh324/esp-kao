-- session_batch_cleaner.lua
-- List / dry-run / batch-delete stored chat sessions under <root>/sessions.
--
-- args:
--   mode    : "list" (default) | "clean"
--             "list" only reports; "clean" deletes the non-kept sessions and
--             rewrites chat_map, so the alias disappears everywhere.
--   keep    : comma-separated aliases to keep, e.g. "systemsetup,newdrivertest".
--             Default (omitted): keep only current_alias of every chat, i.e.
--             "delete everything except the active session".
--   dry_run : true (default) | false
--             true  -> report only, no file is removed, no map is rewritten.
--             false -> apply the deletions.
--   orphans : true | false (default false)
--             Also delete data files that no chat_map / subagent_map references
--             (true leftovers). Safe to enable together with mode="clean".
--
-- Notes:
--   * Session id is "<chat_key>:<alias>"; the 8-hex suffix in every file name is
--     the FNV-1a 32-bit hash of that full session id. The script recomputes it,
--     so files never need to be matched by hand.
--   * The active session is never deleted unless the caller explicitly passes it
--     in `keep`-excluded form AND dry_run=false; prefer leaving the default.
--   * Long-term memory, skills, scheduler and router rules are never touched.

local storage = require("storage")
local json = require("json")

local A = (type(args) == "table") and args or {}
local mode = A.mode or "list"
local dry = (A.dry_run == nil) and true or (A.dry_run == true or A.dry_run == "true")
local purge_orphans = (A.orphans == true or A.orphans == "true")
local keep_set = {}
if type(A.keep) == "string" and #A.keep > 0 then
  for a in A.keep:gmatch("[^,%s]+") do keep_set[a] = true end
end
local explicit_keep = next(keep_set) ~= nil

local M = 0xFFFFFFFF
local function fnv1a(s)
  local h = 0x811C9DC5
  for i = 1, #s do h = (h ~ s:byte(i)); h = (h * 0x01000193) & M end
  return string.format("%08x", h)
end

local function read_json(path)
  local ok, txt = pcall(storage.read_file, path)
  if not ok or type(txt) ~= "string" then return nil end
  local ok2, obj = pcall(json.decode, txt)
  if not ok2 then return nil end
  return obj
end

local function size_of(path)
  local st = storage.stat(path)
  if st and st.size then return st.size end
  return 0
end

local root = storage.get_root_dir()
local sdir = storage.join_path(root, "sessions")
local mapdir = storage.join_path(sdir, "chat_map")
local subdir = storage.join_path(sdir, "subagent_map")

if not storage.exists(sdir) then
  print("[session_batch_cleaner] no sessions directory at " .. sdir)
  return
end

-- 1. index every data file by session hash
local files, all_names = {}, {}
for _, e in ipairs(storage.listdir(sdir) or {}) do
  if e.type == "file" then
    all_names[#all_names + 1] = e.name
    local h = e.name:match("^s_.+_([0-9a-fA-F]+)%.log$")
    if h then files[h] = files[h] or {}; files[h].log = e.name end
    h = e.name:match("^s_.+_([0-9a-fA-F]+)%.log%.idx$")
    if h then files[h] = files[h] or {}; files[h].idx = e.name end
    h = e.name:match("^s_.+_([0-9a-fA-F]+)%.skills%.json$")
    if h then files[h] = files[h] or {}; files[h].skills = e.name end
  end
end

-- 2. index subagent maps by parent-session hash, and child session hashes
local submap, child_parent = {}, {}
for _, e in ipairs(storage.listdir(subdir) or {}) do
  if e.type == "file" then
    local path = storage.join_path(subdir, e.name)
    local h = e.name:match("_(%x%x%x%x%x%x%x%x)%.json$")
    if h then submap[h] = { name = e.name, path = path } end
    local obj = read_json(path)
    if obj and obj.parent_session_id then
      for _, cid in ipairs(obj.child_ids or {}) do child_parent[fnv1a(cid)] = obj.parent_session_id end
    end
  end
end

-- 3. read chat maps
local chats = {}
for _, e in ipairs(storage.listdir(mapdir) or {}) do
  if e.type == "file" and e.name:match("%.json$") then
    local path = storage.join_path(mapdir, e.name)
    local obj = read_json(path)
    if obj and obj.chat_key and obj.sessions then
      chats[#chats + 1] = { file = e.name, path = path, data = obj }
    end
  end
end

local function bytes_of(h)
  local f = files[h] or {}
  local n = 0
  for _, k in ipairs({ "log", "idx", "skills" }) do
    if f[k] then n = n + size_of(storage.join_path(sdir, f[k])) end
  end
  return n
end

-- 4. report
local referenced, doomed = {}, {}
print(("mode=%s dry_run=%s chats=%d data_files=%d"):format(mode, tostring(dry), #chats, #all_names))
for _, c in ipairs(chats) do
  local d = c.data
  print(("chat %s  current=%s  sessions=%d"):format(d.chat_key, tostring(d.current_alias), #d.sessions))
  local kept = {}
  for _, alias in ipairs(d.sessions) do
    local sid = d.chat_key .. ":" .. alias
    local h = fnv1a(sid)
    referenced[h] = true
    local kids = {}
    for ch, parent in pairs(child_parent) do
      if parent == sid then kids[#kids + 1] = ch; referenced[ch] = true end
    end
    table.sort(kids)
    local kidbytes = 0
    for _, kh in ipairs(kids) do kidbytes = kidbytes + bytes_of(kh) end
    local keep = explicit_keep and keep_set[alias] or (not explicit_keep and alias == d.current_alias)
    if keep then kept[#kept + 1] = alias end
    print(("  [%s] %-20s %s  %7d B  %d subagent session(s)%s"):format(
      keep and "KEEP" or "del ", alias, h, bytes_of(h), #kids,
      #kids > 0 and (" (" .. table.concat(kids, ",") .. ", " .. kidbytes .. " B)") or ""))
    if not keep then
      doomed[#doomed + 1] = { alias = alias, hash = h, kids = kids }
    end
  end
  c.kept = kept
end

local orphans = {}
for h in pairs(files) do
  if not referenced[h] then
    orphans[#orphans + 1] = h
  end
end
table.sort(orphans)
if #orphans > 0 then
  print("leftover data files not referenced by any map:")
  for _, h in ipairs(orphans) do print(("  %s  %7d B"):format(h, bytes_of(h))) end
end

if mode == "list" then return end

-- 5. delete
local removed = 0
local function drop(name)
  print("  remove " .. name)
  if not dry then
    if storage.remove(storage.join_path(sdir, name)) then removed = removed + 1 end
  end
end
for _, item in ipairs(doomed) do
  print(("%s session %s [%s]"):format(dry and "would delete" or "delete", item.alias, item.hash))
  local f = files[item.hash] or {}
  for _, k in ipairs({ "log", "idx", "skills" }) do
    if f[k] then drop(f[k]) end
  end
  for _, kh in ipairs(item.kids) do
    local kf = files[kh] or {}
    for _, k in ipairs({ "log", "idx", "skills" }) do
      if kf[k] then drop(kf[k]) end
    end
    if submap[kh] then
      print("  remove subagent map " .. submap[kh].name)
      if not dry then storage.remove(submap[kh].path) end
    end
  end
  if submap[item.hash] then
    print("  remove subagent map " .. submap[item.hash].name)
    if not dry then storage.remove(submap[item.hash].path) end
  end
end

if purge_orphans then
  for _, h in ipairs(orphans) do
    local f = files[h] or {}
    for _, k in ipairs({ "log", "idx", "skills" }) do
      if f[k] then drop(f[k]) end
    end
  end
end

-- 6. rewrite chat maps
for _, c in ipairs(chats) do
  if #c.kept ~= #c.data.sessions then
    local d = c.data
    local was = d.current_alias
    d.sessions = c.kept
    local still = false
    for _, a in ipairs(c.kept) do if a == was then still = true end end
    if not still then d.current_alias = c.kept[1] or was end
    local ok, enc = pcall(json.encode, d)
    if ok then
      print(("rewrite chat_map %s -> sessions=%d current=%s%s"):format(
        c.file, #d.sessions, d.current_alias, dry and " (dry run)" or ""))
      if not dry then storage.write_file(c.path, enc) end
    else
      print("[session_batch_cleaner] chat_map encode failed: " .. tostring(enc))
    end
  end
end

print(("%s: %d session(s) deleted, %d file(s) removed"):format(
  dry and "DRY RUN - nothing changed" or "DONE", dry and 0 or #doomed, removed))
if dry and #doomed > 0 then
  print("re-run with dry_run=false to apply, then restart the device to refresh the in-memory session table.")
end
