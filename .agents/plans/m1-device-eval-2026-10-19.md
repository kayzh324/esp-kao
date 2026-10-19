# M1 里程碑设备实测评估（2026-10-19）

- 上游：里程碑总览 [`agent-harness-milestone.md`](./agent-harness-milestone.md)（M1 = 提交 `b1897259` / 文档 `d18208c3`）
- 被测固件：**当前版本**（判据见下"切片边界"）
- 任务：`cjsonTest` 长程多阶段任务（compass 应用的 S-B 阶段 → 校准 → 诊断）
- 结论用于：[`m1-next-plan.md`](./m1-next-plan.md)（下一步优化计划）

## 0. 取数口径（可复现）

| 项 | 值 |
|---|---|
| 设备 | S3_test_board（16 MB flash / 8 MB octal PSRAM），DATA 根 `/sdcard`，IP 192.168.31.120 |
| 会话 | root `304ea942` + `subagent_04`–`subagent_14`（11 个），共 12 份；按 `/api/files` 精确 size 校验下载 |
| 未纳入 | `subagent_01`（旧世代 2 MB 会话）下载截断；其分析已在里程碑前完成，不参与本轮结论 |
| 工具 | [`tools/session_eval.py`](./tools/session_eval.py)（本轮新增：metrics / roles / poll / paging / cost） |

**切片边界（关键）**：root 记录 `0–166` 属里程碑**前**那轮（实现记录 §11 已分析）；`167–384` 才是当前版本。
判据不是估计——**只有当前版本才会输出的新格式 OOM 诊断串**（`~200858 B; free_int=… largest_int=…`）
出现在 rec213，而 §11 记录的旧 OOM 是裸字符串。

**成本估算模型**：逐轮 `preamble(30 KB) + 投影后回放 + 本 turn 内已累积内容`，求和。
用设备**实测的一个真实请求体**校准：模型估 157.9 KB vs 实测 200.9 KB（**低估 21%**，系数 1.27）。
故所有输入量按其"下界"读，同时给出校准值。

---

## 1. 基座：硬件性能的自动适配

### 生效

| 机制 | 证据 | 判读 |
|---|---|---|
| 内部 RAM 保底（64 KB reserve） | 全程 `largest_int=63488` 稳定（rec241/250/251/267） | ✅ 生效；本轮**没有**出现方案 §4.3 那类 `phy_track_pll` / SPI DMA 分配失败崩溃 |
| **动态窗口 W** | 一次实测请求体 **200 858 B** = 旧固定窗口 48 KB 的 **4.2×** | ✅ 强硬件确实在释放 |
| 工作集装得下 | build 类子 agent 全部产出写入（sa08 3 写 / sa09 2 写 / sa11 9 写 / sa10 7 写），**"59 reads : 0 writes"死循环未再出现** | ✅ §9 修复成立 |
| 逐出不再造成重读 | **全部重复读取都是分页**：`read_file` 同路径多次调用中 `distinct(offset,limit)` 全部等于调用次数（sa06 12/12、sa13 10/10、sa11 7/7、sa10 8/8） | ✅ 窗口没有偷走模型的证据 |
| 档位框架 | 请求体可长到 200 KB 而不崩，预算常量确实按硬件伸缩 | ✅ 弱硬件兼容与强硬件释放两个方向都动了 |

### 未达成（关键）

**当前版本仍会 OOM**：

```
subagent_07 (bench) request_id=10
Out of memory serializing request (~200858 B; free_int=133519 largest_int=63488 free_psram=1480…)
```

1. **闸门不是"内存可行性"闸门。** `est_budget = W(256K) + 48K + 32K = 336 KB`，而失败体只有 200 KB
   —— 闸门**拦不住、也没打算拦**它。方案 A2 要求的"连续块预检（`largest_free_block ≥ est×f`）"
   只落了日志、没落 refuse（里程碑未决项 4）。**OOM 从"崩溃"变成了"带诊断的崩溃"，还没变成"可续跑的干净拒绝"。**
2. **诊断串本身被 768 B 通知上限截断**：最有用的 `largest_psram` 字段被切掉，只剩 `free_psram=1480…`。
   而判断"PSRAM 碎片 vs 内部 RAM 不足"恰恰需要它。→ **B1「回报不截断」不是文档整洁问题，它正在挡住故障定位。**
3. 印证方案 §9.4 的预测：**`largest_free_block(PSRAM)` 成为新瓶颈**。窗口放大解决了"装不下"，
   把风险从"工作集超窗"搬到了"序列化时的连续块"。

---

## 2. 长程任务的宏观效率（当前版本切片）

### 总量

| 指标 | 值 |
|---|---|
| 总轮次 | **382**（root 98 + 子 agent 284） |
| 上下文重发总量 | **39.3 M 字符**（下界）→ **50.0 M 字符**（设备校准后） |
| 输出总量 | ≈ 1.1 M 字符 |
| reasoning 总量 / 单块最大 | 838 KB / 49 KB |
| 工具结果总量 / 单块最大 | 744 KB / 12 KB |
| 摘要 | 28 条，每条 2.5–3.2 KB（**无雪球**） |
| OOM / 卡死 / 轮次上限 | 1 / 0 / 0 |
| 人工介入 | 切片内 10 条用户消息、4 次人为重启 |

### 成本结构：轮数是主乘数

```
30 KB preamble × 382 轮 = 11.5 M 字符 ≈ 总输入的 29%
```

对照 baseline 报告的 24%（当时 112 轮）：**固定每轮前言占比不降反升**。
原因不是逐出（逐出已被证明不制造重读），而是**上下文变大了、轮数没降下来**。
→ **判据的直接推论：当逐出不再是负债，唯一的大杠杆就是轮数。**

### 轮次按角色分解（本轮最反直觉的发现）

| 角色 | 轮次 | 轮次占比 | 输入占比 |
|---|---|---|---|
| **bench（测量）** | **181** | **47%** | **53%** |
| root（编排 + 写交接/PROGRESS） | 98 | 26% | 24% |
| build（实现） | 60 | 16% | 16% |
| recon + verify（只读） | 43 | 11% | 8% |

**近一半轮次、一半以上输入花在"测量"上，真正写代码的只有 16%。**

- `sa12`（bench）单个体：**93 轮 / 9.56 M 字符 / 占全任务输入 24%**，而其 88 个工具轮里
  **44 轮（50%）是纯轮询**：`lua_list_async_jobs` ×21 + `lua_get_async_job` ×19，只是在等一个长跑作业。
- 同批 agent 里 `sa07`/`sa04` 用的是 **`lua_tail_async_job`（阻塞式 tail）**，全程只查 3 次。
  **工具存在、有人用对、有人不用对** → 这是提示词/默认值问题，不是机制缺失。

### 冗余与分工

- ✅ **类型化委派被正确采用**：4 bench / 3 recon / 3 build / 1 verify；recon 明确 "READ-ONLY, no writes, no runs"，
  只回结论 → `539621bf` 的"类型=能力边界"在真实任务里落地了。
- ⚠️ **隔离换来重复**：5 个只读类 agent 都在分页读同一个 62 KB `main.lua`，各自从头建立理解
  ≈ **3.05 M 字符（8%）**。这是子 agent 隔离的固有代价，值得作为独立指标监控。
- ⚠️ **一次重复委派**：同一轮（rec197 / rec201）派了两个 bench agent，目标都是
  "measure the remaining S-B numbers on the real panel"、读同一份 brief → 面板争用 + 其中一个是白跑。
- ✅ root 自己写 8 次 `write_file` + 8 次 `pin_note`（交接文件）——**这是"人为重启 4 次还能接上"的原因**，
  代价计入 root 的 24%。

### 完成度：未达 DoD

正常部分：UI 达标、B4 策略经设备实测并有 A/B 结论、诊断结论有原始数据支撑（`mag_magnet_test.log` 1623 条）。
未达标：**零 OOM ❌**（1 次）；**零人工介入 ❌**（10 次介入 + 4 次重启）；任务本身未收口（用户主动叫停转入诊断）。

---

## 3. 补充评估维度（框架设计视角）

**3.1 失败可恢复性 —— 本轮最大亮点。**
新增的恢复纪律被**真实执行**且顺序正确：root 收到 `<subagent_failed>` → 推理 "check what landed on disk (BENCH.md)"
→ grep 发现 S-B1 已写入 → `close_agent` → 派有界恢复 agent。**"OOM 后检查半成品"从提示词变成了行为。**
它比机制更有价值，因为对**所有**失败模式通用。

**3.2 授权通道 —— 第二个"首次"。**
`report_blocked` 首次被**真实资源冲突**触发：root 报"重启后有一个桌面异步任务占着屏幕，请确认能否停它"，
用户回"能停，桌面任务是开机自启的"。handoff 里标记的"资源争抢型未验"这一格，现在补齐了。
副作用：root 在 turn 8/14 花多轮推理纠结"这该不该用 report_blocked"（授权/资源/产品决策 vs 普通提问），
说明判据在提示词里还不够干脆。

**3.3 可观测性缺口。**
`W` 在会话里**完全不可见**。方案 §A0 写了"窗口值写入每轮日志与 `get_system_info`"，
本轮只能靠"200 KB 请求体"反推窗口存在。**基座的自适应如果不可观测，就无法用基准任务反选策略。**

**3.4 每轮有效载荷比。** 平均每轮 ≈ 103 KB（root 96 KB），其中 30 KB 是固定前言 → 载荷比 ≈ 71%。
`tools_json` 缓存已吃掉一部分，但**提示词 + 工具 schema 仍是单轮最大常量**。

**3.5 指令服从度。** 模块化纪律（`b1897259`）**无从验证**：本轮没有新写大文件，
`main.lua` 反而从 61 KB 长到 62 KB / 1345 行，仍是单文件。约束进了提示词，但还没有一次真实的"拆分"行为作证据。

---

## 4. 里程碑判据打分

| 判据 | 结果 |
|---|---|
| 零 OOM | ❌ 1 次；且诊断证明闸门拦不住 |
| 零人工介入 | ❌ 10 次介入 / 4 次重启 |
| 任务完成 | ⚠️ 部分（UI 达标，指向未达标，用户叫停） |
| 装得下、不因逐出重读 | ✅ 强证据（重复读 100% 分页） |
| 失败可续跑 | ✅ 恢复纪律已成行为 |
| 证据可复核 | ⚠️ 有（BENCH.md / 报告 / 交接文件），但通知仍截断 |

## 5. 由实测得出的优先结论（下一步见 plan）

1. **一号指标改为轮数**：`sa12` 的 44 轮纯轮询 + 同轮重复委派 ≈ 全任务 **15%** 输入。
2. **A2 连续块预检 + B1 回报不截断**：同一个 OOM 事件同时证明这两件事必要。
3. **度量闭环前置**：把 `W` / `est_budget` / 逐出标志暴露出来，否则"反选策略"没有输入。
4. **测量与实现的预算配比**（bench 53% vs build 16%）应作为一条设计原则来管。

## 6. 复现命令

```bash
# 列会话并按 size 校验下载（大文件会随机截断，重试保留最大者）
curl -s "http://<ip>/api/files?path=/sessions"
curl -s "http://<ip>/files/sessions/<name>.log" -o raw/<name>.log

# 会话 id -> 文件名
python3 .agents/plans/tools/session_dump.py key "chat:web:w-mv830fpa-3bnpe43k:cjsonTest"

# 度量（root 用 start=167 只看当前版本切片）
python3 .agents/plans/tools/session_eval.py metrics  <log> [start]
python3 .agents/plans/tools/session_eval.py roles    <log1,log2,...>
python3 .agents/plans/tools/session_eval.py poll     <log> [start]
python3 .agents/plans/tools/session_eval.py paging   <log> [start]
python3 .agents/plans/tools/session_eval.py cost     <log> [start]
```
