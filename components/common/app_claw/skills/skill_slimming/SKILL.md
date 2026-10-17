---
{
  "name": "skill_slimming",
  "description": "Load when a skill is too big to hold in context (its SKILL.md is a large share of one round's budget), when you are writing a skill and unsure what belongs in SKILL.md, or when the user asks to slim, split or reorganize a skill.",
  "metadata": {
    "cap_groups": ["cap_files"],
    "manage_mode": "readonly"
  }
}
---

# Skill Slimming (progressive disclosure)

Activating a skill loads its whole `SKILL.md` into context, and the context is re-sent every round -
so its size is paid over and over, and long tasks re-activate skills repeatedly. Measured here: a
19 KB skill is ~40% of one round's budget on an S3 board. The fix is not "write less"; it is putting
the right part in `SKILL.md` and keeping the rest one read away.

## What stays in SKILL.md

- The **index**: what the skill covers, and which file holds which part.
- The **rules that always apply**: invariants, forbidden things, the required order of operations.
- Anything needed *before* you know which part you need.

## What moves to `references/`

- Long tables, worked examples, per-peripheral or per-board detail.
- The steps of one variant - keep the rule that chooses the variant, move the steps.
- Field notes, measured numbers, history.

## Procedure

1. **Measure**: the file size and its share of one round. `write_file`/`edit_file` reports the share
   when it writes; for an existing skill compare its size against the board's per-round budget.
2. **Find the seam**: go through the file and mark each block *always* or *on demand*.
3. **Split**: one topic per file under `references/<topic>.md`; the first line says what it is for.
4. **Rewrite `SKILL.md`** as index + always-rules, with one line per reference file saying when to read
   it. A reference nobody can find is a reference nobody reads.
5. **Fix every path**: `{CUR_SKILL_DIR}/references/...` - never a fixed `/fatfs` or `/sdcard` path. Fix
   paths to moved scripts too. Leave `cap_groups` and `execution` frontmatter as they were.
6. **Verify**: re-measure; `SKILL.md` should end up well under a fifth of one round. The index must name
   every file you moved. Re-activate the skill so the change takes effect.
7. **Report** in one line: what moved where, and the new size.

## Boundaries

- **System skills under `/system/skills/` are read-only.** Slimming is for skills under the data root's
  `skills/`. If the oversized skill is a system one, report that instead of trying to edit it.
- **Move detail, never delete it.** The number getting smaller is not the goal; a skill that loses
  content is worse than one that is fat.
- **Do not redesign while slimming.** If the skill is also wrong, say so and fix the size first - a
  size edit and a behaviour edit at the same time cannot be verified separately.
- **Do not over-split**: if it takes three reads where one would do, you made it worse.
