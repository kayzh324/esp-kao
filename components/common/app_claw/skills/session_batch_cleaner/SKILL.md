---
{
  "name": "session_batch_cleaner",
  "description": "Load when the user wants to clear chat history in bulk, asks where sessions are stored, or asks what /session delete does (it is partial: it leaves child session files and subagent_map entries behind).",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "manage_mode": "readonly"
  }
}
---

# Session Storage and Batch Cleanup

Use this when the user wants to delete many stored chats at once, asks where chat history lives, or asks
what `/session delete` actually removes.

## Where sessions live

```text
<data_root>/sessions/
├── chat_map/...                    {chat_key, current_alias, sessions:[alias...]}
├── subagent_map/...                {parent_session_id, next_suffix, child_ids}
├── s_<prefix>_<hash>.log           message records (JSONL)
├── s_<prefix>_<hash>.log.idx       binary offset index
└── s_<prefix>_<hash>.skills.json   skills activated in that session
```

A session id is `<chat_key>:<alias>`; the 8-hex suffix in file names is the FNV-1a hash of the full id.

## `/session delete` is partial

It removes that alias's `.log`, `.log.idx` and `.skills.json` and drops the alias from `chat_map`, but it
does **not** remove the `subagent_map` entry and does **not** remove child session files. What it leaves
behind is exactly what later shows up as an orphan.

## Tool

`{CUR_SKILL_DIR}/scripts/session_batch_cleaner.lua` lists, dry-runs or batch-deletes stored sessions, and
can also remove orphans. Defaults are `mode="list"`, `dry_run=true`, and
`keep=current_alias of every chat` (i.e. delete everything except the session in use).

## Safety

1. Report first: run with the defaults and show the user what would be removed.
2. Never delete the active session; prefer the default `keep`.
3. Only pass `dry_run=false` once the user has agreed.
