# M1 之后的优化计划（节奏锚定在真实任务实测）

- 基线：M1 里程碑，提交 `b1897259`（文档 `d18208c3`）
- 依据：[`m1-device-eval-2026-10-19.md`](./m1-device-eval-2026-10-19.md)（本轮实测）、
  [`agent-harness-milestone.md`](./agent-harness-milestone.md)（设计与未决项）
- 核心变更（相对里程碑）：**一号指标从"上下文大小"改为"轮数"。**
  实测证明逐出已不再制造重读（重复读 100% 是分页），**每轮重发全部上下文的乘数现在只由轮数决定**。

---

## 0. 交付协议（每个 cycle 的固定动作）

> 节奏锚点：**agent 的实际效果由用户用真实编程任务判定**，不由 Claude 判定。
> 一个 cycle 只交付一个改动集，不叠加开发；上一个没验收完，不开始下一个。

1. **Claude 实现一个改动**（单一主题，可回退）。
2. **Claude 程序性验收**（见 §1）：编译 + 单测/测试 app + 离线回放 + 非任务性设备自检。
3. **Claude 烧录**（agent-rework 周期默认由 Claude 编译烧录；串口/HTTP 自检用非任务性调用）。
4. **交给你一次真实编程任务**：本 cycle 明确写出"要你回答的验收问题"，尽量用同一个基准任务族（见 §4）。
5. **你回报**（数字 + 观感 + 有没有新的怪现象）。
6. **按回报决定**：接受 → 记入基线并开始下一 cycle；不接受 → 在同一 cycle 内调整，**不回退到已接受的旧结论**。

**禁止**：Claude 用"跑一个编程任务看结果"来自证 agent 效果；那属于第 4 步，归你。

## 1. Claude 的程序性验收范围

**可以做**（都不驱动 agent 跑任务）：
- `idf.py build`（受影响板）+ 组件 test app（`components/claw_modules/*/test_apps/`）+ Lua 模块测试（`components/lua_modules/<m>/test/`）
- **离线回放**：拿真实 session + 固件算法复算逐出/预算/请求体/轮数（`tools/session_eval.py`）；这是最便宜、最可复现的尺子
- **非任务性设备调用**：串口 `cap call`（`get_system_info` / `read_file` / `grep_files` / `list_dir`）、HTTP 文件与状态 API、启动日志检查
- 静态检查：`pnpm typecheck`/`build`（前端）、编译告警

**不做**：替你判定"agent 编程能力有没有变好"。

## 2. M1 基线（后续每个 cycle 与它对比）

| 指标 | M1 实测（当前版本切片） |
|---|---|
| 总轮次 | 382（root 98 + 子 agent 284） |
| 上下文重发总量 | 39.3 M 字符（下界）→ 50.0 M（校准） |
| 固定前言占比 | 29% |
| 角色轮次占比 | bench 47% / root 26% / build 16% / recon+verify 11% |
| 单 agent 最差 | `sa12` bench：93 轮 / 9.56 M 字符（24%），其中 88 个工具轮里 44 轮纯轮询 |
| OOM | 1 次（`subagent_07`，req10，体 ~200 858 B；gate=336 KB 拦不住） |
| 重复读 | **100% 是分页**（逐出负债已消除） |
| 人工介入 | 10 条用户消息 / 4 次重启 |
| 摘要 | 28 条，2.5–3.2 KB，无雪球 |

---

## 3. Cycle 清单

### C1 — 回报不截断 + 诊断不截断（最小改动，先解除诊断盲区）✅ **已实施（`a281ea40`）**

- **依据**：OOM 诊断串被 768 B 通知上限截掉最有用的 `largest_psram`，导致无法区分"PSRAM 碎片"还是"内部 RAM 不足"；方案 B1 未做。
- **改动**：子 agent 终局文本全文落盘 `<DATA>/.reports/<session>_<req>.md`（复用 `.toolout` 的 sanitize/prune 思路）；通知 = head(≤512 B) + `[full report: <path>]`；`inspect_agent` 增加 `report_path` 字段；**OOM/拒绝的错误串走同一套，保证字段不被截**。
- **程序性验收**：test app 构造 >768 B 的终局文本 → 断言落盘完整、通知含路径；设备上 `cap call` 触发一次 `inspect_agent` 核对 `report_path`。
- **你的实测问题**：真实任务里子 agent 的长回报是否完整到达 root（不再出现"报告被截断"）？
- **风险/回退**：`.reports` 占空间 → 复用 10 min prune；回退 = 恢复直发通知。

### C2 — 轮数：长跑作业的等待语义（最大杠杆）✅ **已实施（`7b729a13`）**

- **依据**：`sa12` 的 44/88 轮纯轮询 = 全任务 **24%** 输入；同批 `sa07`/`sa04` 用阻塞式 `lua_tail_async_job` 只查 3 次——**工具存在但默认做法不对**。
- **改动**：(a) 让"等一个作业"只有一种自然做法：`lua_get_async_job` 支持阻塞等待（等到完成或超时），或规定测量类 agent 只能用 tail/wait；(b) 根提示词 + bench/recon 类型提示词 + `dev_workflow` 明确禁止轮询 `list/get` 循环。
- **程序性验收**：工具单测（完成/超时/无作业三分支）；**离线回放 sa12 的轮次序列**，用新策略复算轮数（预期 88 → ≲30）。
- **你的实测问题**：一次含设备实测的真实任务里，测量阶段的轮数是否明显下降，且没有出现"等太久/漏结果"？
- **风险/回退**：阻塞会占住 agent core → 需长轮询 + 让出，而不是死等；回退 = 保留轮询但加提示词。

### C3 — 基座：序列化"连续块预检"（把 OOM 变成可续跑的干净拒绝）✅ **已实施（见 §8）**

- **依据**：200 KB 请求体通过了 336 KB 的闸门，却仍在序列化时 OOM；方案 A2 只落了日志没落 refuse。
- **改动**：序列化前 `largest_free_block(target heap) ≥ est × f` 预检 → 不足则先有序逐出（复用 `claw_core_evict_to_budget`）→ 复检 → 仍不足则 refuse，错误串带**可执行建议**（缩小任务 / 新开会话 / 关闭空闲 agent）。
- **程序性验收**：组件 test app 构造高碎片场景（分配若干小块再释放）验证 refuse 触发；用设备当时真实的 `largest_int/largest_psram` 离线复算 200 KB 体应被判定为"拒绝"还是"可序列化"——**先在设备上量出该板可序列化的真实上限**（非任务性调用即可）。
- **你的实测问题**：真实任务里是否还出现 `Out of memory serializing request`（终态判据 2）？
- **风险/回退**：阈值定标；过严会把本可完成的任务拒掉。回退 = 只记日志不 refuse。

### C4 — 可观测性 + 基准任务固化（让后续每个 cycle 可度量）✅ **可观测性已实施（见 §8）**

> **基准任务固化已放弃（用户决定，2026-10-19）**：历史 session 各自带着不同的问题，轮数暴涨不是
> 同一条件下的可比量，拿它们当基准价值不大。**改以"用户主观感受 + 新 session 的过程分析"来量化**：
> 每次实测用 `tools/session_eval.py` 出同一套结构指标（轮数 / 角色分布 / 纯轮询轮 / 分页 vs 重读 /
> 输入量），与 M1 那份基线并列对比即可，不再冻结一个"标准任务"。

- **依据**：`W` 在会话里完全不可见，本轮只能靠 200 KB 请求体反推；没有可复跑的基准任务，策略调整无法比较。
- **改动**：`get_system_info` 增 context 段（`W` / `est_budget` / `refuse_limit` / 本轮是否触发逐出或拒绝 / PSRAM 与内部 largest）；构建日志带同样字段；把 compass 任务固化为可复跑基准（seed prompt + SPEC_V2 + 起始代码，见 §4）。
- **程序性验收**：`cap call get_system_info` 直读（非任务性）；日志核对数值随内存变化而变。
- **你的实测问题**：无需额外动作——跑基准任务时顺带产出可对比的一组数字。
- **风险/回退**：无。**建议 C4 与 C2 合并交付**，否则 C2 的收益无法量化。

### C5 — 分工效率（测量预算 + 隔离冗余）✅ **已实施（见 §9）**

- **依据**：同轮重复委派（rec197/rec201 两个 bench 同一目标）；5 个只读 agent 重复建图 ≈ 8% 输入；bench 占 53% 输入而 build 只 16%。
- **改动**：(a) spawn 前检测"同 turn 内已有同类型且目标高度相似"→ 拒绝并提示复用（`send_agent_followup`）；(b) 只读 agent 产出"文件地图/结构摘要"落盘，后续只读 agent **先读地图再读源码**；(c) 测量类 agent 的提示词要求先声明预期轮数/预算。
- **程序性验收**：离线比对历史会话，复算新策略下可省下的轮次。
- **你的实测问题**：真实任务里 recon/verify 的重复读与重复委派是否下降，且判断质量没有变差？
- **风险/回退**：(b) 有"用摘要代替源码"而降低判断质量的风险 → **只做地图（结构），不做结论**。

### C6 — 遗留项（按需插入，不占主节奏）

| 项 | 验收方式 |
|---|---|
| `.versions` 洞（脚本写文件不留快照） | 程序性：脚本写文件后检查 `.versions`；复发就动机制 |
| 会话 reset 留痕 + 任务书回注（A4） | 程序性：破坏 `.idx` → 有 `.bak` + RESET summary + 回注提示 |
| P4 档校准 | **需 P4 硬件**；S3 档数值是实测基线，不动 |
| Stage 2 显示所有权（持有者 + 单仲裁者 + 抢占恢复） | 设计见 `~/.claude/plans/resource-lease-design.md` |
| Stage 3 无进展熔断 | 兜底，不用于可预期路径（误报风险高） |
| `skill_creator` 与 `skill_slimming` 交叉引用 | 文档改动 |

### C7 — 弱硬件兼容（**C1 实施中实测发现**，直接影响"弱硬件能否跑"）

两条都是既有缺陷，与 C1 无关，但都在"基座硬件适配"这条评估主线上，且**当前会让某类板子整体不可用**：

1. **spawn 准入是 PSRAM 硬要求**：`claw_agent_mgr.c` 无条件检查
   `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= CLAW_TIER_SPAWN_MIN_PSRAM_BLOCK_BYTES`。
   无 PSRAM 时该值为 0 ⇒ **每一次 `spawn_agent` 都被拒**，即"无 PSRAM 的板子永远无法派发子 agent"。
   方案 §A0 说"无 PSRAM 板自动回落"，但准入没有这条回落。测试 app 未开 PSRAM 时 13/15 用例
   因此失败，这是直接证据。
2. **DATA 在内部 flash 时，核心任务写文件会断言重启**：`claw_task.c` 优先把核心任务栈分配在 PSRAM
   （`MALLOC_CAP_SPIRAM`），而写内部 flash 时必须关 cache，`spi_flash_disable_interrupts_caches_and_other_cpu`
   的 `esp_task_stack_is_sane_cache_disabled()` 会断言失败 → `RTC_SW_CPU_RST`。
   **凡是从核心任务写 DATA 的路径都受影响**：会话持久化、`pin_note`、`.toolout`、`.versions`、`.reports`。
   只有"PSRAM **且** DATA 在内部 flash"（即无 SD 的 PSRAM 板）才触发——本板走 SD 所以从未暴露。

- **验收**：程序性——无 PSRAM 构建下 `spawn_agent` 不再一律被拒；DATA 指向内部 FATFS 时
  写文件路径不再断言（可用测试 app 复现：开 PSRAM + DATA=/fatfs）。
- **风险**：回落阈值需要定标（内部 RAM 的连续块比 PSRAM 紧张得多）；写路径可能要为内部 flash 情况
  降低核心任务栈位置或改由一个内部栈的辅助任务落盘。
- **优先级由你定**：你的板子（PSRAM + SD）不受影响，但这两条决定了"弱硬件兼容"能覆盖到哪些板型。

### C8 — 硬件信息暴露（用户新增需求）✅ **已实施（见 §12）**
- **需求**：agent 想拿硬件细节（芯片型号、I2C 地址）没有好途径；这些事实手写在 `board_info.yaml`
  的 description 里，而那个文件不在"先读路径"上；`board_devices.yaml` 明明有却没有暴露。
- **根因**：数据在生成链路里被丢两次——① bmgr 的 `gen_board_metadata.yaml` 只保留 type/peripherals，
  丢掉 `chip` 与 `config`；② 生成脚本只渲染引脚。于是技能里只有"设备名 + GPIO"，型号与地址只存在于
  手写 prose，且两处已经开始漂移（`board_devices.yaml` 顶部注释与实际 config 不一致）。
- **方案（已实施）**：生成脚本直接读板级原始 yaml，把 `chip` / `i2c_addr` / 频率 / 配置里的引脚渲染进
  `board_hardware_info` 技能；description 收敛为纯 prose；常驻约定加一句"操作硬件前先激活该技能"。

---

## 4. 基准任务（验收统一用这个场景）

- **场景**：给定一句自然语言需求 + 一份书面规格（`SPEC_V2`），agent 在设备上无人值守完成一个中等复杂度 Lua 任务。
  沿用 compass 任务（已有里程碑前后两次实测可对比）：2–4 文件 / ~300–600 行 / ≥2 硬件模块 / ≥3 条数值验收 / ≥1 轮"改→验"。
- **参数**：S3 板，DATA 根 `/sdcard`，`chat_id` 固定，seed prompt 固定（含 SPEC 与起始代码路径）。
- **每次记录**：`(T_in, T_out, Wall, Steps, ReReads, Resends, OOM, Reset, Success, HumanHits)` + 角色轮次分布。
- **注意**：报告截断的旧经验（`/api/files` 不支持 Range，大文件随机截断）——取数后用 size 校验，保留最大者。

## 5. 纪律

- **一次一个改动**，等你的设备实测再启下一项（[[test-dev-alternating]]）。
- 项目代码改动加 `(added)` 注释，便于日后 upstream merge 判断取舍（[[upstream-merge-comments]]）。
- 判据（[[agent-opt-milestone]] §0）：改动要让模型**少发**还是**少记**；导致重读/重推就是负债。
- 里程碑未达标的两条（零 OOM、零人工介入）是硬目标，任何 cycle 不得以牺牲它们换取 token 数字。

---

## 6. C1 实施记录（2026-10-19）

### 交付

| 改动 | 文件 |
|---|---|
| 终局文本全文落盘 `<DATA>/.reports/`（带 60 min prune、SD 瞬时失败重试一次） | 新增 `claw_manager/src/claw_agent_mgr_report.{c,h}` |
| 通知 = 开头 + `[full report: <path>]`；落盘失败时显式写 `[report truncated: spill unavailable]` | `claw_agent_mgr.c` |
| 通知正文按 UTF-8 边界裁剪，**保证结尾标签与路径永不被截** | `claw_agent_mgr.c`（`claw_agent_mgr_append_body`） |
| `last_error` 缓冲 96 → 192；`inspect_agent` 增加 `report_path`（并改用 cJSON 生成，避免错误串里的引号破坏 JSON） | `claw_agent_mgr.h` / `cap_agent_mgr.c` |
| OOM 诊断串字段重排：`largest_int` / `largest_psram` 提前 | 两个 LLM 后端 |

### 程序性验收结果

- **编译**：主 app 与 agent_mgr 测试 app 均零警告。
- **组件测试**：新增用例 `subagent report spill writes the whole text to a readable path` → **PASS**
  （本板 15 例 6 失败；**回退我的测试改动后这 6 个失败依旧存在**，属测试 app 在本板配置下的既有问题，
  与本 cycle 无关）。
- **设备**：`.reports` 可创建，且 dot 目录内的文件**能被 `read_file` 读回**
  （`cap call read_file {"path":"/sdcard/.reports/probe.md"}` 返回内容）——回读契约成立；
  主 app 烧回后正常运行（`/api/status` 200，DATA=/sdcard）。
- **未验证**：通知端到端（子 agent 长回报 → 通知携带路径、root 能 `read_file` 取回），
  **需要你的真实任务**。

### 你的验收问题

> 一次真实任务里，子 agent 的长回报是否**完整到达** root？
> 具体看：通知是否出现 `[full report: /sdcard/.reports/...]`；root 是否读回该文件；
> 是否还出现"报告被截断"的观感。

### 测试 app 环境备注（本板）

测试 app 的 `sdkconfig`（gitignored，已就地修改）需要：flash 频率 **80 MHz**（原 120 MHz 会挂在
flash timing tuning 并 RTCWDT 重启）、**开启 PSRAM**（否则 spawn 准入一律拒绝，见 C7）。
另：测试 app 的分区表与主 app 不同（factory 在 0x10000 vs 0x20000），**从测试 app 切回主 app 必须
`bootloader-flash partition-table-flash app-flash`**，只烧 app 会让板子起不来。

---

## 7. C2 实施记录（2026-10-19）

### 交付

| 改动 | 文件 |
|---|---|
| `lua_tail_async_job` / `lua_get_async_job` 新增可选 `wait_ms`（≤30 s，默认 0＝原行为）：tail 等到**有新输出**或作业终局，get 等到**终局**；等待期间以 100 ms 本地定时轮询，且每轮按 id 重新解析作业（防槽位复用误判） | `cap_lua_async.c` / `cap_lua_internal.h` |
| 两个工具的 schema/描述改为"用 wait_ms 等，不要在循环里反复读"；`lua_list_async_jobs` 明确"快照，不要用它等待" | `cap_lua.c` |
| bench 类型提示词：删掉"poll it"，改为"在工具内等"并说明代价；常驻约定加一条"never poll: wait once, with a deadline"；`dev_workflow` 增同义条目 | `claw_agent_mgr.c` / `app_claw.c` / `dev_workflow/SKILL.md` |

### 程序性验收（设备实测计时探针）

用一个 ~6 s、每 2 s 打一行的探针作业：

| 调用 | 观测 |
|---|---|
| `lua_run_script_async` | 起作业，返回 job_id |
| `tail {"wait_ms":9000}` | **1.9 s 返回**，输出恰好到 `tick 1`（等到第一条新输出） |
| `tail {"wait_ms":9000}` | **2.0 s 返回**，输出到 `tick 2` |
| `get {"wait_ms":15000}` | 输出含 `tick 3` + `done` ⇒ **等到终局**才返回 |
| `get {"wait_ms":15000}`（已终局） | **0.2 s** 立即返回 |
| `tail`（无 wait_ms） | **0.2 s** 立即返回（原行为不变） |

⇒ 阻塞语义与"不传则立即返回"的兼容性都成立；**单次等待替代一整串轮询轮**。

### 你的验收问题

> 一次真实任务（含设备实测的那种）里，**测量阶段的轮数是否明显下降**？
> 具体看：bench/子 agent 是否还在反复 `lua_list_async_jobs` / `lua_get_async_job`；
> 有没有出现"等太久"或"漏掉结果"的新问题（wait_ms 上限 30 s，超时会返回当前快照，不会丢）。

### 顺带修正 + 一个操作陷阱

- **HTTP 文件 API 的路径是相对 DATA 根的**（`/` 就是根），而**能力层（`read_file` 等）用的是绝对 VFS 路径**（`/sdcard/...`）。
  C1 的设备验证里我用 `?path=/sdcard/...` 走 HTTP，实际在 DATA 根下造了一个名为 `sdcard` 的目录——
  已删除，并在**真实路径**上重做了 `read_file` 回读验证（见 §6）。
  ⇒ 后续所有设备文件操作：HTTP 用 `/tmp/x`，能力调用用 `/sdcard/tmp/x`。

---

## 8. C3 + C4 实施记录（2026-10-19）

### C3 — 序列化连续块预检

- **交付**：`claw_core_context_window.c` 新增 `claw_core_serialize_block_have()`（两个堆的最大连续块取大者
  —— malloc 优先 PSRAM 但会回落内部 RAM，只报首选堆会拒掉本来能发的请求）与
  `claw_core_serialize_block_need()`（1.5 × body）；`claw_core_context_guard_request()` 重写为
  **先判可行度、再判预算**，任一越界即降级，降级后仍越界则 refuse，错误串给出
  body / 需要的块 / 实际有的块 / 预算，并列出可执行动作（pin_note、开新会话、拆子 agent、
  close 空闲子 agent）。
- **关键修正**：原实现里那段"连续块预检"是**死代码**——它把 `largest_int + largest_psram` 相加
  （两个堆不可能同时用），且只在日志里出现；更要命的是它在 `bytes < est_budget` 时**根本不会执行**。
  M1 的 OOM（200 KB 体、336 KB 预算、63 KB 最大块）正是从这个缝隙穿过去的。
- **为什么用 1.5× 而不是 W 推导里的 2.2×**：2.2 是给 W 自己留的余量；序列化时必须一次性拿到的是
  **输出串**那一个连续块（cJSON 树是小块分配、碎片容忍），用 2.2 会把设备其实能发的请求拒掉。
  两个数都进日志，便于用现场数据回调。
- **⚠️ 现场数据已把 1.5× 证伪（2026-10-19，session `firmware1019-1`）**：一个 **238 679 B** 的请求在
  最大连续块 **466 944 B** 时仍然 OOM ⇒ 真实需求介于 **1.96×–2.0×** body（树与输出串同时持有），
  而 1.5× 的门（358 KB）对它放行。**已改为 2.2×**（`claw_core_serialize_block_need`），并为该值补了
  实测注释。按 2.2× 复算：need=525 KB > 467 KB ⇒ 会干净拒绝并给出"需要的块 vs 实际有的块"。
  这也是计划里说的"两个数进日志、用现场数据回调"的第一次兑现。

### C4 — 可观测性

- **交付**：`claw_core_context_stats_t` + `claw_core_get_context_stats()`；`get_system_info` 新增
  `context` 段（也进默认全量输出）；每轮 `built context:` 日志增加 `W=` 与 `block=`。
- **设备实测**：`cap call get_system_info {"sections":["context"]}` →
  `window_bytes=262144`（W 取到上限）、`est_budget_bytes=344064`、`block_have_bytes=1638400`、计数器为 0。
- **未做**：基准任务固化成可复跑 seed —— 需要你指定用哪个任务/规格作为冻结基准。

## 9. C5 实施记录

- **交付**：spawn 成功后的返回体改为 cJSON，并附 `other_agents_of_this_type`（同类型且活跃的其它
  agent）与 `hint`（提示用 `send_agent_followup` 复用：新 agent 要从零重读，复用的那个已经持有上下文）。
  **仅提示，不阻断**——第二个 agent 有时确实是对的。
- **依据**：同一轮内两次近乎重复的 bench 委派；5 个只读 agent 各自从零重读同一个 62 KB 文件（≈8% 输入）。
  root 提示词里已经有"复用同一个 bench agent"，再加文字无效——提示要出现在**做决定的那个工具结果**里。
- **计划里的另两项未做**（附理由）："只读 agent 产出文件地图、后续先读地图"有"用摘要代替源码、
  降低判断质量"的风险，而 (a) 一旦奏效就不再需要它；"测量类 agent 先声明预期轮数"只是提示词，无机制。
- **验证状态：仅编译验证**。设备 CLI 的 `cap call` 被 `subagent management tools are root-agent only`
  拒绝，无法在不驱动任务的前提下触发 —— 需要你的实测。

## 10. C6（按需项）

- **做了**：`skill_creator` 增加 `skill_slimming` 交叉引用，并写明"`SKILL.md` 控制在一轮预算的约 1/8"。
- **未做（各有理由，优先级等你定）**：`.versions` 洞（脚本写文件不留快照 —— 要给 Lua 存储路径接上
  版本机制，属结构性改动）；会话 reset 留痕 + 回注（M1 的 4 次重启是人为的，不是静默 reset，
  本轮无现场证据）；P4 档校准（需 P4 硬件）；Stage 2 显示所有权（设计已定，改动大）；
  Stage 3 无进展熔断（误报风险高，兜底用）。

---

## 11. 本批次验收清单（可以一次测完）

| # | 看什么 | 判据 |
|---|---|---|
| C1 | 子 agent 长回报是否完整到达 root | 通知里出现 `[full report: /sdcard/.reports/...]`；root 能读回该文件；没有"报告被截断"的观感 |
| C2 | 测量阶段轮数是否下降 | bench/子 agent 不再反复 `lua_list_async_jobs`/`lua_get_async_job`；无"等太久/漏结果"。可用 `session_eval.py poll <log>` 定量核对 |
| C3 | 是否还出现 `Out of memory serializing request` | 终态判据 2；若仍出现，错误串现在会给出"需要的块 vs 实际有的块" |
| C4 | 容量决策是否可见 | `cap call get_system_info {"sections":["context"]}` 的 `trim_count`/`refuse_count` 与观感一致；串口日志每轮有 `W=`/`block=` |
| C5 | 是否还出现同轮重复委派 | spawn 结果里出现 `other_agents_of_this_type` 后，是否改用 followup；recon/verify 重复读是否下降 |

**本轮未测**：C1/C2/C3/C5 的端到端效果、C4 的基准任务固化。板子上是本批次最终固件（含 C1–C5 + C6 的 skill 改动）。

---

## 12. C8 实施记录：硬件信息暴露（2026-10-19）
### 数据怎么会丢的

```
boards/<board>/board_devices.yaml ──bmgr──▶ gen_bmgr_codes/gen_board_metadata.yaml ──脚本──▶ /system/skills/board_hardware_info/SKILL.md
   (chip: bmm150, i2c_addr: 0x13)                (只剩 type + peripherals)                    (只剩 设备名 + 占用 GPIO)
```

两处丢失：**① bmgr 的 metadata 导出**（丢 `chip`/`config`，所以脚本想渲染也拿不到）；**② 生成脚本**
只渲染 IO/引脚，连模板里已有的 `description`/`manufacturer` 槽位都没用上（生成物里写着
`Manufacturer: unknown`）。结果型号与地址只存在于手写 description，而那不在"先读路径"上。

### 改动

| 改动 | 文件 |
|---|---|
| 生成脚本新增 `--board-devices-yaml` / `--board-info-yaml`，**直接读板级原始 yaml**（不依赖有损的 metadata 导出）：渲染 `chip`、`i2c_addr`（回写成 `0x..` 十六进制，因为 YAML 把 `0x13` 解析成 19）、频率/几何，以及配置里声明的引脚（补回了此前丢失的 touch CS=18、SD CS=4） | `cap_boards/tools/generate_board_skill.py` |
| 渲染 `## Board Notes`（来自 board_info.yaml 的 description），并把技能 frontmatter 的 description 改成"含芯片型号/总线地址/占用 GPIO" | 同上 |
| CMake：按 app 同样的方式从 `gen_bmgr_codes/CMakeLists.txt` 取板目录，把两个 yaml 作为参数与 DEPENDS 传入（缺文件时优雅退化为旧的纯清单输出） | `cap_boards/CMakeLists.txt` |
| description 收敛为纯 prose（去掉 I2C 清单与引脚枚举——那些现在由生成物渲染），保留生成物推不出的东西：不要盲扫总线、背光 GPIO16、touch IRQ GPIO5、DATA 根随 SD 变化、USB 摄像头/音频 | `boards/kayzh/esp32_S3_test_board/board_info.yaml` |
| 常驻约定加一句"操作硬件/分配 GPIO/写板级代码前先激活 board_hardware_info；型号、地址、引脚一律不许猜" | `app_claw.c` |

### 效果与代价

- 技能从 78 行 → **113 行 / 3.5 KB**，多出的全是型号/地址/此前缺失的引脚；在本板"文档类目标 ≈
  一轮预算 1/8（≈6 KB）"以内。
- 设备已验证：`cap call read_file {"path":"/system/skills/board_hardware_info/SKILL.md", ...}` 能读到
  `Chip: \`bmm150\`` / `i2c_addr -> 0x13` / touch `cs -> GPIO18` / SD `cs -> GPIO4`。
- 生成物是**构建产物**（不进 git）；改板级 yaml 会因其进入 DEPENDS 而自动重生成。

### 遗留

- `board_devices.yaml` 顶部那段注释已经与实际 config 不符（touch CS、SD 引脚、display RST），
  现在有了单一生成来源，建议把那段注释删掉或改成指向生成物，以免又变成第二个事实来源。
- 未做（用户选择）：把原始 yaml 拷进 `/system/board/`、以及运行时枚举设备的 capability。

---

## 13. C9：网关抖动的传输层重试（2026-10-19）

### 问题

第二轮实测里同一个子 agent 连续两次死于 `ESP_ERR_HTTP_FETCH_HEADER`（网关抖动，与代码无关），
旧策略只在 **turn 的第一次** LLM 调用重试一次，于是两次抖动就让一个跑了 80 轮的 agent 被替换。

### 策略

**可重试 = 传输层失败，与"第几轮"无关。** 失败的调用没有拿到任何响应，也就没有任何结果被应用；
重发的是**同一份请求**（工具结果被重放，工具**不会**重跑），所以任意轮次重试都是安全的。

- 错误分类（`claw_core_llm_failure_is_transient`）：`ESP_ERR_HTTP_CONNECT / FETCH_HEADER /
  CONNECTION_CLOSED / READ_TIMEOUT / EAGAIN / CONNECTING / WRITE_DATA`、`ESP_ERR_TIMEOUT`；
  另外 **HTTP 5xx 与 429** 也算（网关答了但服务不了）。**4xx 不重试**（请求本身有问题）。
- 为此让传输层在非 200 时也回填 `status_code`，并由两个 chat 后端把它带进
  `claw_llm_response_t.http_status`（0 = 根本没拿到响应），避免靠解析错误文本判断。
- 次数与间隔：**1 次首发 + 4 次重试**，间隔 800 ms → 1.6 s → 3.2 s → 6.4 s（上限 8 s），
  每次 ±25% 抖动——设备上可能有多个 agent 同时重试，固定间隔会一起砸同一个抖动中的网关。
- 放弃时错误串追加 `(gave up after N attempts)`，让父 agent 和用户都能看出是网关问题。
- 串行化 OOM、4xx、调用方 abort 一律不重试。

### 设备验证

把 `llm_base_url` 临时指向 `http://127.0.0.1:1/v1`（必然拒连）后发一条消息：

```
LLM call failed (HTTP request failed: ESP_ERR_HTTP_CONNECT, http=0); retry 1/4 in 792 ms
... retry 2/4 in 1408 ms
... retry 3/4 in 3200 ms
... retry 4/4 in 4992 ms
failed: HTTP request failed: ESP_ERR_HTTP_CONNECT (gave up after 5 attempts)
```

随后把 base_url 还原并确认正常路径：`status=done raw=pong`，**零重试**。
（测试脚本 `retry_check.py` 带异常路径的还原保护，避免把设备留在死端点上。）

### 遗留

- 单轮最长增加约 10 s 退避；LLM 超时本身是 300 s，若网关是"连上了但一直不回"，一次失败要
  等满超时再重试——那种场景需要的是**更短的连接/首包超时**，不是更多重试。可后续单独评估。
