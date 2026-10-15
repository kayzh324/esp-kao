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

## 5. Environment

Follow the environment conventions already in your instructions: the writable data root vs read-only
`/system`, `apps/<name>/` with `app.json`, `tmp/` for throwaway scripts, internal files are not task
input, and writes are versioned automatically. Do not re-derive them here.

## Reporting

End the turn with: what changed, which file(s), and the verification result. A few lines is enough.
