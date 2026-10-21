---
{
  "name": "app_layout",
  "description": "Load when deciding where an on-device file goes, creating or auditing an app directory, or cleaning up stray and temporary files (apps/<id>/ with launcher.json, tmp/ for throwaway, _lib/ for shared helpers).",
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
│   └── <app_id>/                a program
│       ├── launcher.json        manifest; its presence is what makes this an app
│       ├── main.lua             entry script (the manifest's `entry`)
│       ├── config.json          user-editable settings (optional)
│       ├── data/                state the app persists (create on demand)
│       ├── assets/              images, fonts, static files (create on demand)
│       └── README.md            human notes (optional)
└── tmp/                         throwaway scripts, safe to delete in bulk
```

Rules:

- A directory is an app **if and only if** it contains `launcher.json`; never detect by name or location.
- `<app_id>` must match the manifest `id` and use only letters, digits, `_` or `-` (1–63 chars).
- `entry` must name an existing `.lua` file inside the app directory. `main.lua` at the app root is our
  convention; `scripts/main.lua` is equally valid and is what upstream packages use.
- `data/` and `assets/` are optional: create them the first time they are actually needed.
- Keep nesting shallow (at most 3 levels); FATFS has no symlinks.
- Everything an app persists belongs in its own `data/`; nothing of its own lies loose in the root.
- **After creating or updating an app, call `publish_app` with its id.** That validates the package and
  reloads the App registry, which makes the app appear in the system launcher.

## launcher.json fields

| field | meaning |
| --- | --- |
| `schema_version` | required, must be `1` |
| `id` | required, must match the directory name |
| `entry` | required, package-relative `.lua` path (e.g. `main.lua`) |
| `display_name` | launcher label; defaults to `id` |
| `icon` | optional package-relative `.jpg`/`.jpeg` shown by the launcher |
| `args` | optional JSON object passed to the entry script |
| `order` | optional integer sort hint (smaller first) |
| `visible` | optional boolean; `false` hides it from the launcher |

An App owns every file in its directory. Do not reference another App's or a Skill's files. `app.json`
belongs to the old layout and is no longer read.

## Tools in this skill

- `{CUR_SKILL_DIR}/scripts/fs_init.lua` - create the layout (`apps/`, `apps/_lib/`, `tmp/`) on a fresh
  device or after a reflash.
- `{CUR_SKILL_DIR}/scripts/fs_new_app.lua` - scaffold a new app (directory + `main.lua` + `launcher.json`).
- `{CUR_SKILL_DIR}/scripts/fs_sweep.lua` - audit: list apps and flag misplaced or stray files.
- `{CUR_SKILL_DIR}/scripts/fs_clean_tmp.lua` - clean `tmp/` (dry-run by default).

## Audit workflow

1. Run `fs_sweep.lua` to see what exists and what is misplaced.
2. Move misplaced files to the right place with `move_file`, or delete junk.
3. Re-run `fs_sweep.lua` to confirm.
