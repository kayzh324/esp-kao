#!/usr/bin/env python3
"""Evaluate an ESP-Claw agent session:结构度量 / 轮次冗余 / 逐出负债 / 成本估算。

面向"里程碑后每一轮优化都要有同一把尺子"这个需求。所有子命令都接受可选的
start_record，用来只看某个固件版本对应的切片（一个新版本之后的记录）。

用法（先在仓库根目录备好 session 日志）：
    session_eval.py metrics <log> [start_record]
        rounds / reasoning / tool_results / read:write / lua exec / summaries / errors
    session_eval.py roles   <log1,log2,...>
        多 session 合并，按角色汇总轮次与输入占比
    session_eval.py poll    <log> [start_record]
        找出"只调 list/get/tail_async_job"的纯轮询轮（长跑作业的等待浪费）
    session_eval.py paging  <log> [start_record]
        区分重复读取是"分页"（distinct offset/limit）还是"逐出后重读"（同一窗口）
    session_eval.py cost    <log> [start_record]
        回放估算：preamble + 投影后回放 + 轮内累积，逐轮求和（下界）

取日志见 tools/dev_fetch.py；诊断版 OOM 串里出现的 "~N B" 是设备实测请求体，
可用来校准 cost 的偏差系数（实测 ≈ 估算 × 1.27）。

会话文件格式：一串非行分隔的 JSON 值（user / assistant / tool 结果数组 / summary）。
"""
import json
import os
import sys
from collections import Counter, defaultdict

READ_LIKE = {"read_file", "grep_files", "list_dir", "activate_skill"}
WRITE_LIKE = {"write_file", "edit_file", "restore_file"}
# S3 档 preamble：系统提示词 + 工具 schema（见 .agents/plans/capacity-and-trust-hardening.md 附录 D）
PREAMBLE_CHARS = 30000


def load(path):
    data = open(path, encoding="utf-8", errors="replace").read()
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
        recs.append((obj, j - i))
        i = j
    return recs


def walk(recs, start=0):
    """产出 (index, kind, payload)；kind ∈ assistant|user|tool|summary|other。"""
    for i, (r, _) in enumerate(recs):
        if i < start:
            continue
        if isinstance(r, list):
            for t in r:
                if isinstance(t, dict) and t.get("role") == "tool":
                    yield i, "tool", t
            continue
        if not isinstance(r, dict):
            yield i, "other", r
            continue
        if "summary" in r:
            yield i, "summary", r
            continue
        role = r.get("role")
        yield i, role if role in ("user", "assistant") else "other", r


def metrics(recs, start=0):
    m = dict(rounds=0, rc=0, rc_max=0, tr=0, tr_max=0, n_toolres=0, summaries=0, errors=0, users=0)
    calls, reads, writes = Counter(), defaultdict(list), Counter()
    for i, kind, r in walk(recs, start):
        if kind == "tool":
            c = str(r.get("content", ""))
            m["n_toolres"] += 1
            m["tr"] += len(c)
            m["tr_max"] = max(m["tr_max"], len(c))
            if r.get("is_error"):
                m["errors"] += 1
        elif kind == "summary":
            m["summaries"] += 1
        elif kind == "user":
            m["users"] += 1
        elif kind == "assistant":
            m["rounds"] += 1
            rc = r.get("reasoning_content") or ""
            m["rc"] += len(rc)
            m["rc_max"] = max(m["rc_max"], len(rc))
            for tc in r.get("tool_calls") or []:
                fn = tc.get("function", {})
                nm = fn.get("name", "?")
                calls[nm] += 1
                try:
                    a = json.loads(fn.get("arguments") or "{}")
                except Exception:
                    a = {}
                if nm == "read_file":
                    reads[a.get("path", "")].append((a.get("offset"), a.get("limit")))
                if nm in WRITE_LIKE:
                    writes[a.get("path", "")] += 1
    rl = sum(calls[k] for k in READ_LIKE)
    wl = sum(calls[k] for k in WRITE_LIKE)
    lua = sum(v for k, v in calls.items() if k.startswith("lua_"))
    return m, calls, reads, writes, rl, wl, lua


def estimate_cost(recs):
    """逐轮估算请求字符数 = PREAMBLE + 投影后回放 + 本 turn 内已累积内容。"""
    sums = [(i, r) for i, (r, _) in enumerate(recs) if isinstance(r, dict) and "summary" in r]
    total_in, rounds = 0, []
    cur_sum_len, cur_keep = 0, 0
    base, in_turn = 0, 0
    for i, (r, blen) in enumerate(recs):
        for si, sr in sums:
            if si == i:
                cur_sum_len = len(str(sr.get("summary") or ""))
                cur_keep = sr.get("keep_from") or 0
        if isinstance(r, dict) and r.get("role") == "user" and not str(r.get("content", "")).lstrip().startswith("<"):
            replay = cur_sum_len
            for k in range(cur_keep, i):
                if isinstance(recs[k][0], dict) and "summary" in recs[k][0]:
                    continue
                replay += recs[k][1]
            base, in_turn = replay, 0
            continue
        if isinstance(r, dict) and r.get("role") == "assistant":
            req = PREAMBLE_CHARS + base + in_turn
            total_in += req
            rounds.append((i, req, r))
            in_turn += blen
        else:
            in_turn += blen
    return total_in, rounds


def cmd_metrics(path, start=0):
    recs = load(path)
    m, calls, reads, writes, rl, wl, lua = metrics(recs, start)
    print(f"{os.path.basename(path)}  bytes={sum(b for _, b in recs)}  records={len(recs)}  start={start}")
    print(f"  rounds={m['rounds']} users={m['users']} summaries={m['summaries']} errors={m['errors']}")
    print(f"  reasoning total={m['rc']} max={m['rc_max']}")
    print(f"  tool_results n={m['n_toolres']} total={m['tr']} max={m['tr_max']}")
    print(f"  read_like={rl} write_like={wl} lua={lua} read:write={round(rl / wl, 2) if wl else '-'}")
    print(f"  calls={dict(calls)}")
    return 0


def cmd_roles(paths):
    rows = []
    for p in paths:
        recs = load(p)
        m, calls, _, _, rl, wl, lua = metrics(recs)
        ti, _ = estimate_cost(recs)
        rows.append((os.path.basename(p), m, ti, rl, wl, lua))
    tot_r = sum(r[1]["rounds"] for r in rows) or 1
    tot_in = sum(r[2] for r in rows) or 1
    print(f"{'session':<22} {'rounds':>6} {'round%':>7} {'in(Mchars)':>11} {'in%':>6} {'r:w':>5}")
    for name, m, ti, rl, wl, _ in sorted(rows, key=lambda x: -x[2]):
        print(f"{name[-22:]:<22} {m['rounds']:>6} {m['rounds']*100/tot_r:>6.1f}% {ti/1e6:>11.2f} {ti*100/tot_in:>5.1f}% "
              f"{(round(rl/wl,1) if wl else '-'):>5}")
    print(f"{'TOTAL':<22} {tot_r:>6} {'':>7} {tot_in/1e6:>11.2f}")
    print(f"\n固定前言占比 ≈ {PREAMBLE_CHARS*tot_r*100/tot_in:.1f}%  (preamble {PREAMBLE_CHARS}B × {tot_r} 轮)")
    print(f"输入 tokens 下界 ≈ {tot_in/4e6:.2f} M (按 4 字符/token)")
    return 0


def _is_poll_call(name, args_json):
    """A call that burns a round just to look at a job.

    A get/tail WITH wait_ms is NOT a poll: it blocks inside the tool until there is
    something to see, which is the whole point of the wait semantics. Counting those
    as polling inflates the metric badly (measured: 13 waits, 9 of them get_async_job
    with wait_ms 12-30 s, all reported as "pure polling" by the old rule).
    """
    if name == "sleep" or name == "lua_list_async_jobs":
        return True
    if name in ("lua_get_async_job", "lua_tail_async_job"):
        try:
            args = json.loads(args_json or "{}")
        except Exception:
            return True
        wait_ms = args.get("wait_ms")
        return not (isinstance(wait_ms, (int, float)) and wait_ms > 0)
    return False


def cmd_poll(path, start=0):
    recs = load(path)
    total = poll = waits = 0
    combos = Counter()
    for _, kind, r in walk(recs, start):
        if kind != "assistant":
            continue
        calls = [(tc.get("function", {}).get("name", "?"),
                  tc.get("function", {}).get("arguments", ""))
                 for tc in (r.get("tool_calls") or [])]
        if not calls:
            continue
        total += 1
        combos[tuple(sorted({name for name, _ in calls}))] += 1
        if all(_is_poll_call(name, args) for name, args in calls):
            poll += 1
        elif any(name in ("lua_get_async_job", "lua_tail_async_job") and not
                 _is_poll_call(name, args) for name, args in calls):
            waits += 1
    print(f"{os.path.basename(path)}: tool轮={total} 真轮询轮={poll} ({poll*100//max(total,1)}%) "
          f"阻塞等待轮={waits}")
    for c, n in combos.most_common(6):
        print(f"  x{n:<3} {c}")
    return 0


def cmd_paging(path, start=0):
    recs = load(path)
    _, _, reads, _, _, _, _ = metrics(recs, start)
    print(f"{os.path.basename(path)}: 重复读取判别（分页 vs 逐出后重读）")
    for p, offs in sorted(reads.items(), key=lambda x: -len(x[1])):
        if len(offs) < 3:
            continue
        uniq = len(set(offs))
        verdict = "PAGING" if uniq > 1 else "SAME WINDOW (re-read!)"
        print(f"  calls={len(offs):>3} distinct={uniq:>3}  {verdict:<24} {p}")
    return 0


def cmd_cost(path, start=0):
    recs = load(path)
    ti, rounds = estimate_cost(recs)
    if start:
        rounds = [r for r in rounds if r[0] >= start]
        ti = sum(r[1] for r in rounds)
    out = sum(len(r.get("reasoning_content") or "") for _, _, r in rounds)
    out = int(out * 1.3)  # content + tool_call arguments 的粗略补偿
    print(f"{os.path.basename(path)}: rounds={len(rounds)}")
    print(f"  Σ输入 ≈ {ti/1e6:.2f} M字符 (下界) / {ti*1.27/1e6:.2f} M字符 (按设备实测校准 ×1.27)")
    print(f"  Σ输出 ≈ {out/1e6:.2f} M字符")
    print(f"  平均每轮 ≈ {ti/len(rounds)/1024:.1f} KB" if rounds else "")
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd, arg = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]
    if cmd == "roles":
        return cmd_roles(arg.split(","))
    start = int(rest[0]) if rest else 0
    fn = {"metrics": cmd_metrics, "poll": cmd_poll, "paging": cmd_paging, "cost": cmd_cost}.get(cmd)
    if not fn:
        print(f"unknown command: {cmd}")
        return 2
    return fn(arg, start) if cmd != "metrics" else cmd_metrics(arg, start)


if __name__ == "__main__":
    raise SystemExit(main())
