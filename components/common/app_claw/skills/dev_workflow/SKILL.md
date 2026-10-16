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
- Then `read_file` **the whole file in one call** whenever it fits - most do. Splitting a readable file
  into several windows is not cheaper: every extra round re-sends the whole conversation, so it costs
  more, not less.
- Use `offset`/`limit` for exactly two things: continuing past a truncated read, and re-checking a known
  region after an edit.
- Do not write a throwaway probe script just to read a file.

## 3. Change by editing, not rewriting

- Use `edit_file` with a small, exact, unique `old_text` -> `new_text`. Several small edits beat one big one.
- `write_file` is only for a new file, or a deliberate full rewrite.
- If `edit_file` reports the text matched zero or many places, re-read that window and include more
  surrounding context. Do not guess the text.

## 4. Verify on the device

- Running it (sync or async) and reading its output **is** the evidence. "The code looks right" is not.
- For UI work the evidence is what appears on the panel.
- Report intent and verification separately: what you *changed* vs what you *verified*.

## 5. Multi-session programs keep three short docs

A one-off script (`tmp/`, run once) needs no docs. A **program** - multi-file, or worked on across
sessions - keeps three short files in its own directory (`apps/<name>/`):

| file | holds | size |
| --- | --- | --- |
| `REQ.md` | what the user wants | tens of lines |
| `ARCH.md` | the files and who owns what | tens of lines |
| `PROGRESS.md` | built / verified / todo / broken | append only |

Why: your context dies between sessions, the disk does not. One fact per line, no filler, no restating
code, `A -> B` for relations.

- **Read budget per session**: the relevant `REQ.md` lines, the `ARCH.md` rows you will touch, the tail of
  `PROGRESS.md`, and the one or two code files you will edit. Nothing else.
- Do **not** write a plan document (`PLAN.md`, a design essay): every session would have to re-read it,
  and it costs more than it returns. Working state that changes minute to minute belongs in `pin_note`
  (see §8), not in a disk document.
- Record intent and evidence apart: **user-confirmed = verified**; you only ran it yourself = **built**.
- Locate by `ARCH.md` rows before opening code; never read the whole tree or every doc.

## 6. Environment

Follow the environment conventions already in your instructions: the writable data root vs read-only
`/system`, `apps/<name>/` with `app.json`, `tmp/` for throwaway scripts, internal files are not task
input, and writes are versioned automatically. Do not re-derive them here.

## 7. Delegating to a subagent

Use a subagent for a bounded task you can name in one sentence; do the exploratory part yourself.

- **Keep the `prompt` short** (a couple of sentences): the goal, the exact file path(s), the
  done-condition. A long prompt fails the spawn and wastes a round trip.
- **Long spec goes in a file, not the prompt**: `write_file` the spec first, then pass its path. The
  subagent reads it with `read_file`.
- **One bounded step per subagent** - not "build the whole feature". Split rather than over-explain.
- **The subagent needs the task, not the workflow**: do not tell it to load heavy skills.
- **Delegation is async**: after `spawn_agent`, end your turn and wait to be woken by a
  completion/failure notification. Do not poll `inspect_agent`.

## 8. Long work: put your state on the device, not in your head

Anything that runs over many rounds will have its context compacted several times. Two habits keep the
work recoverable:

- **Pin the task card first.** Before a long read-and-reason stretch, call `pin_note` with the goal, the
  constraints, the file paths, the API you looked up, and the current plan. It is replayed into context on
  every round and survives compaction, so after a compaction you re-read the pinned card, not the whole
  file tree. Keep it short; rewrite it whole as the work moves on.
- **Write drafts to a file, not to your reasoning.** A design or a 100-line function written out inside
  your reasoning is the first thing compaction drops - and then you re-derive it at full price. Put it in
  a working file with `write_file` as soon as it exists, then turn it into the real file with `edit_file`.

## Reporting

End the turn with: what changed, which file(s), and the verification result. A few lines is enough.
