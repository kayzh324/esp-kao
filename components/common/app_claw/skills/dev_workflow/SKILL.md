---
{
  "name": "dev_workflow",
  "description": "Load before writing or changing on-device Lua code (new app, feature, fix, refactor, resume). Small verified steps, grep to locate, edit_file to change, end the turn with a short status.",
  "metadata": {
    "cap_groups": ["cap_files", "cap_lua"],
    "manage_mode": "readonly"
  }
}
---

# On-Device Development Workflow

Use this skill for any turn that **writes or changes code files**: a new app or script, a feature, a bug
fix, a refactor, or resuming earlier work. Do not load it for chat, for explaining code without changing
it, or for running an existing script once.

## 1. One visible step per turn

- Aim each turn at **one change the user can see or run** - not "finish a whole layer".
- When the change is in place, **stop and report** it. Do not chain many changes in one turn: long turns
  burn the device's context and token budget, and the user cannot steer mid-way.

## 2. Locate before you read

- Use `grep_files` to find the exact place (`path`, `pattern`, optional `file_filter`); it returns
  `path:line: text`.
- Then `read_file` with `offset`/`limit` around that line - read the window you need, not the whole file.
- Do not read a whole large file "to understand it", and do not write a throwaway probe script just to
  read a file. Both are the slowest, most expensive way to find one line.

## 3. Change by editing, not rewriting

- Use `edit_file` with a small, exact, unique `old_text` -> `new_text`. Several small edits beat one big one.
- `write_file` is only for a new file, or a deliberate full rewrite.
- If `edit_file` reports the text matched zero or many places, re-read that window and include more
  surrounding context. Do not guess the text.

## 4. Verify on the device

- Running it (sync or async) and reading its output **is** the evidence. "The code looks right" is not.
- For UI work the evidence is what appears on the panel.
- Report intent and verification separately: what you *changed* vs what you *verified*.

## 5. Where files go

- A program lives in `<data_root>/apps/<name>/` with `main.lua` and `app.json` (`app.json` is what makes
  a directory an app).
- Throwaway scripts go in `<data_root>/tmp/`.
- Build paths with `storage.get_root_dir()` + `storage.join_path(...)`; never hardcode `/fatfs`.
- Keep an app's state inside that app's own `data/`; do not leave loose files in the root.

## 6. Do not use internal files as input

- `sessions/*.log`, `memory_records.jsonl`, `memory_index.json` and `MEMORY.md` are the agent's own
  plumbing, not task material. Do not read them to decide what to do.

## Reporting

End the turn with: what changed, which file(s), and the verification result. A few lines is enough.
