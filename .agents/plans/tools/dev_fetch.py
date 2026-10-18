#!/usr/bin/env python3
"""Fetch files / list directories from the device HTTP config server.

Endpoints used (see application/edge_agent/components/http_server/http_server_files_api.c):
    GET  /api/status                 -> wifi, ip, storage_base_path
    GET  /api/agents                 -> live subagents
    GET  /api/files?path=<abs>       -> JSON directory listing
    GET  /files<abs>                 -> raw file download
    POST /api/files/upload?path=<abs>
    DELETE /api/files?path=<abs>

Paths are absolute and must live under the DATA root (usually /sdcard) or /system.

Usage:
    dev_fetch.py --base http://192.168.31.120 ls /sdcard/sessions
    dev_fetch.py --base http://192.168.31.120 get /sdcard/apps/compass/main.lua -o main.lua
    dev_fetch.py --base http://192.168.31.120 status

Env: CLAW_BASE_URL (default http://192.168.31.120)
"""
import argparse
import json
import os
import sys
import urllib.parse
import urllib.request


def _base(args) -> str:
    return args.base.rstrip("/")


def _get(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=15) as r:
        return r.read()


def cmd_status(args) -> int:
    print(_get(_base(args) + "/api/status").decode("utf-8", "replace"))
    return 0


def cmd_ls(args) -> int:
    url = _base(args) + "/api/files?path=" + urllib.parse.quote(args.path)
    d = json.loads(_get(url))
    for e in d.get("entries", []):
        print(f'{"DIR " if e.get("is_dir") else "    "}{e.get("size", 0):>9}  {e.get("path")}')
    return 0


def cmd_get(args) -> int:
    url = _base(args) + "/files" + urllib.parse.quote(args.path)
    data = _get(url)
    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
        print(f"wrote {len(data)} bytes to {args.out}")
    else:
        sys.stdout.buffer.write(data)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=os.environ.get("CLAW_BASE_URL", "http://192.168.31.120"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status").set_defaults(func=cmd_status)
    p_ls = sub.add_parser("ls")
    p_ls.add_argument("path")
    p_ls.set_defaults(func=cmd_ls)
    p_get = sub.add_parser("get")
    p_get.add_argument("path")
    p_get.add_argument("-o", "--out", default=None)
    p_get.set_defaults(func=cmd_get)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
