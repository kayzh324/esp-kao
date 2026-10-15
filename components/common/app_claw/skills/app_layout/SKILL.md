---
{
  "name": "app_layout",
  "description": "Load when deciding where an on-device file goes, creating or auditing an app directory, or cleaning up stray and temporary files (apps/<name>/ with app.json, tmp/ for throwaway, _lib/ for shared helpers).",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "manage_mode": "readonly"
  }
}
---

# App Layout

Load this before creating, moving or auditing files on the device, and when the user asks where something
should go or asks to tidy things up.

## The contract

```text
<data_root>/                     = storage.get_root_dir()
├── apps/
│   ├── _lib/                    shared helper libraries (a leading "_" means "not an app")
│   └── <app_name>/              a program
│       ├── main.lua             the only entry point, fixed name
│       ├── app.json             manifest; its presence is what makes this an app
│       ├── config.json          user-editable settings (optional)
│       ├── data/                state the app persists (create on demand)
│       ├── assets/              images, fonts, static files (create on demand)
│       └── README.md            human notes (optional)
└── tmp/                         throwaway scripts, safe to delete in bulk
```

Rules:

- A directory is an app **if and only if** it contains `app.json`; never detect by name or location.
- `main.lua` is the single entry point; an app runs as `<data_root>/apps/<name>/main.lua`.
- `data/` and `assets/` are optional: create them the first time they are actually needed.
- Keep nesting shallow (at most 3 levels); FATFS has no symlinks.
- Everything an app persists belongs in its own `data/`; nothing of its own lies loose in the root.

## app.json fields

| field | meaning |
| --- | --- |
| `name` | app id, matching the directory name |
| `description` | one line: what it does |
| `entry` | entry script, normally `main.lua` |
| `version` | your own version string |
| `hidden` | hide it from the launcher |
| `notes` | free-form notes for the next session |

## Tools in this skill

- `{CUR_SKILL_DIR}/scripts/fs_init.lua` - create the layout (`apps/`, `apps/_lib/`, `tmp/`) on a fresh
  device or after a reflash.
- `{CUR_SKILL_DIR}/scripts/fs_new_app.lua` - scaffold a new app (directory + `main.lua` + `app.json`).
- `{CUR_SKILL_DIR}/scripts/fs_sweep.lua` - audit: list apps and flag misplaced or stray files.
- `{CUR_SKILL_DIR}/scripts/fs_clean_tmp.lua` - clean `tmp/` (dry-run by default).

## Audit workflow

1. Run `fs_sweep.lua` to see what exists and what is misplaced.
2. Move misplaced files to the right place with `move_file`, or delete junk.
3. Re-run `fs_sweep.lua` to confirm.
