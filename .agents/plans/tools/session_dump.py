#!/usr/bin/env python3
"""Parse an ESP-Claw session log and print a timeline or statistics.

Session logs are a stream of JSON values (not line-delimited): user/assistant
message objects, arrays of tool results, and summary records. We decode them with
a raw JSON decoder, skipping whitespace.

Session file naming (see claw_memory_utils.c):
    safe = session_id with every char not in [A-Za-z0-9_-] replaced by '_', cut to 24 chars
    hash = FNV-1a 32-bit over the FULL session_id
    file = <DATA>/sessions/s_<safe>_<hash:08x>.log   (+ .idx)

Usage:
    session_dump.py timeline  <log> [reason_chars] [tool_chars] [content_chars]
    session_dump.py stats     <log>
    session_dump.py key       "<session_id>"        # print the log filename

Get logs from the device first, e.g.:
    dev_fetch.py ls /sdcard/sessions
    dev_fetch.py get /sdcard/sessions/s_xxx_yyyyyyyy.log -o s.log
"""
import json
import os
import sys
from collections import Counter


def load_records(path):
    data = open(path, "r", encoding="utf-8", errors="replace").read()
    dec = json.JSONDecoder()
    recs, i, n = [], 0, len(data)
    while i < n:
        while i < n and data[i] in " \t\r\n":
            i += 1
        if i >= n:
            break
        try:
            obj, j = dec.raw_decode(data, i)
        except json.JSONDecodeError:
            nl = data.find("\n", i)
            if nl < 0:
                break
            i = nl + 1
            continue
        recs.append(obj)
        i = j
    return recs


def _clip(s, n):
    s = "" if s is None else str(s)
    return s if len(s) <= n else s[:n] + f"…(+{len(s)-n}B)"


def cmd_key(session_id: str) -> int:
    safe = "".join(c if (c.isalnum() or c in "_-") else "_" for c in session_id)[:24]
    h = 2166136261
    for b in session_id.encode():
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    print(f"s_{safe or 'default'}_{h:08x}.log")
    return 0


def cmd_stats(path: str) -> int:
    recs = load_records(path)
    kinds = Counter()
    tot_rc = max_rc = tot_tr = max_tr = 0
    calls = Counter()
    reads = Counter()
    for r in recs:
        if isinstance(r, list):
            for t in r:
                if isinstance(t, dict) and t.get("role") == "tool":
                    c = str(t.get("content", ""))
                    kinds["tool_results"] += 1
                    tot_tr += len(c)
                    max_tr = max(max_tr, len(c))
            continue
        if not isinstance(r, dict):
            kinds[type(r).__name__] += 1
            continue
        role = r.get("role")
        if role == "assistant":
            kinds["assistant"] += 1
            rc = r.get("reasoning_content") or ""
            tot_rc += len(rc)
            max_rc = max(max_rc, len(rc))
            for tc in r.get("tool_calls") or []:
                fn = tc.get("function", {})
                nm = fn.get("name", "?")
                calls[nm] += 1
                try:
                    a = json.loads(fn.get("arguments") or "{}")
                except Exception:
                    a = {}
                if nm in ("read_file", "activate_skill"):
                    reads[a.get("path") or a.get("skill_id") or ""] += 1
        elif role in ("user", "system"):
            kinds[role] += 1
        elif "summary" in r:
            kinds["summary"] += 1
        else:
            kinds["?"] += 1
    print(f"file={os.path.basename(path)} bytes={os.path.getsize(path)} records={len(recs)}")
    print(f"kinds={dict(kinds)}")
    print(f"reasoning total={tot_rc}B max_single={max_rc}B")
    print(f"tool_result total={tot_tr}B max_single={max_tr}B")
    print(f"tool calls={dict(calls)}")
    rep = {k: v for k, v in reads.items() if v > 1}
    if rep:
        print("repeated reads:")
        for k, v in sorted(rep.items(), key=lambda x: -x[1]):
            print(f"  x{v}  {k}")
    return 0


def cmd_timeline(path: str, mr: int = 400, mt: int = 400, mc: int = 700) -> int:
    recs = load_records(path)
    print(f"== {os.path.basename(path)}  records={len(recs)} ==")
    turn = 0
    for r in recs:
        if isinstance(r, list):
            for t in r:
                if isinstance(t, dict) and t.get("role") == "tool":
                    c = str(t.get("content", ""))
                    err = " (ERR)" if t.get("is_error") else ""
                    print(f"    [TOOLRESULT{err} {len(c)}B] {_clip(c, mt)}")
            continue
        if not isinstance(r, dict):
            print("  ???", _clip(r, 100))
            continue
        role = r.get("role")
        if role == "user":
            turn += 1
            print(f"\n--- TURN {turn} ---")
            print(f"  [USER {len(str(r.get('content','')))}B] {_clip(r.get('content',''), mc)}")
        elif role == "assistant":
            rc = r.get("reasoning_content") or ""
            ct = r.get("content") or ""
            tcs = r.get("tool_calls") or []
            print(f"  [ASSISTANT rc={len(rc)}B txt={len(ct)}B calls={len(tcs)}]")
            if rc:
                print(f"     rc: {_clip(rc, mr)}")
            if ct:
                print(f"     txt: {_clip(ct, mc)}")
            for tc in tcs:
                fn = tc.get("function", {})
                print(f"     -> {fn.get('name')}({_clip(fn.get('arguments'), mt)})")
        else:
            print(f"  [{role}] {_clip(json.dumps(r, ensure_ascii=False), 200)}")
    return 0


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd, arg = sys.argv[1], sys.argv[2]
    if cmd == "key":
        return cmd_key(arg)
    if cmd == "stats":
        return cmd_stats(arg)
    if cmd == "timeline":
        rest = [int(x) for x in sys.argv[3:]]
        return cmd_timeline(arg, *rest)
    print(f"unknown command: {cmd}")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
