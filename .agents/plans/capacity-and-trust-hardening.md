# 容量与可信双主线改造方案（评审稿 v2）

- 状态：**已通过评审，待实施**（未实现）
- 蓝本：设备 session `chat:web:w-mv830fpa-3bnpe43k:firmware1017-2`（root `s_..._6e0aa240.log`，
  build `s_..._486b95e8.log`，verify `s_..._496b977b.log`，bench `s_..._4e6b9f5a.log`，
  recon `s_..._4f6ba0ed.log` / `s_..._4b6b9aa1.log`）
- 约束：遵守 [`.agents/design.md`](../design.md)——`claw_core` 保持小，能放 capability / context provider /
  skill 的逻辑不要进核心循环。

## 评审意见采纳说明

| 评审意见 | 采纳方式 |
| --- | --- |
| 1. PSRAM 容量因芯片差异巨大；窗口应**动态计算**；大窗口从任务长程看可能更省 token，首字慢但总时长可能更短；要以**完成一个中等复杂度 Lua 任务**为基准衡量 token 与时长，不能只看单次调用 | 新增 **§0 P2 目标函数**、**§3 任务级度量方法与基准任务**、**§4 A0 动态窗口推导**；固定常量改为策略函数 |
| 2. 不要就 `fps` 特例改 harness，要从特例泛化出处理同类情况的方法 | 主线 B 重构为 **§5 B0 可观测契约（observable contract）机制**；`fps` 只作为该机制的一个实例 |
| 3. 眼光放全局、以终为始，不要管中窥豹头痛医头 | 全文改为"先定义任务级终态 → 倒推能力需求 → 再落改动"；新增 **§0 设计原则** 与 **§1 终态定义** |

---

## 0. 设计原则（以终为始）

- **P1 以终为始**：先定义"一个中等复杂度 Lua 任务成功完成的终态"，再倒推需要哪些能力；不从一个失败现象直接跳到一条代码改动。
- **P2 全局目标（任务级）**：优化对象是**整任务**的 `(总 token, 总时长, 步数, 成功率, 人类介入次数)`，不是单次调用的请求体大小。窗口大小同时影响"每次调用的大小"和"调用次数 × 每次上下文"，二者相乘才是成本，故必须整体衡量。
- **P3 先机制后实例**：遇到具体缺陷，先问"它属于哪一类"，把类做成机制，再把实例挂上去（`fps` 之争 → 可观测契约；`486b95e8` 掉头 → 会话可恢复契约）。

---

## 1. 终态定义（Definition of Done，任务级）

**终态场景**：给定一句自然语言需求 + 一份书面规格，agent 在设备上**无人值守**完成一个中等复杂度 Lua 脚本任务，并留下可复核的证据。

**"中等复杂度 Lua 任务"的界定**（本方案所有度量以此为基准）：
- 2–4 个文件、合计 ~300–600 行；
- 用到 ≥2 个硬件模块（如 display + 传感器）；
- 规格里含 ≥3 条**数值验收项**；
- 需要 ≥1 次设备实测 + 至少一轮"改→验"循环。
- 参考实例：本次 compass 任务（SPEC_V2 重排 + 视觉缺陷 + 静止乱转）。

**终态成功判据**：
1. 任务完成（规格的数值验收项全部达标）；
2. **零** `Out of memory serializing request`；
3. **零**人类介入（不需人喊"停一下/重发一下"）；
4. 每个"完成"都带 harness 可复核的证据；
5. 全过程 `(总 token, 总时长, 步数)` 在选定策略下最优。

**由终态倒推的能力需求**（→ 落到主线）：

| 终态要求 | 倒推的能力 | 归属 |
| --- | --- | --- |
| 任务装得下、丢失可恢复 | 动态容量 + PSRAM + 指名逐出 + 会话可恢复 | A |
| 零人类介入 | 回报不截断、失败可续跑 | A4 / B1 |
| 证据可复核 | 具名可观测项 + harness 计算 + 证据包 | B |
| 成本最优 | 任务级度量闭环（用度量反选策略） | §3 |

---

## 2. 蓝本证据与根因（简述）

| 现象 | 证据 |
| --- | --- |
| build 读 13 次、**0 编辑**，单块 reasoning 26.5 KB | `486b95e8` |
| 32 KB 整文件读 = 窗口 2/3（S3 `RUNTIME_BUDGET=48 KB`） | 单条结果峰值 32 626 B |
| 序列化 OOM 终止任务 | root 第 30 轮，`request_id=36` |
| 通知 768 B 截断 → ~15 次重发 | `NOTIFY_TEXT_SIZE=768`；root 21 条提及截断 |
| 会话静默掉头（user 记录=0，无 summary） | `486b95e8`；代码见 `claw_memory_session.c:1447-1464` |
| 逐出不可见 → verify 读 `main.lua` **16 次** | `496b977b` |
| 自报不可信（几何 / "20 fps"） | root 第 11、18 轮 |

根因模型：

```
单轮工作集 > 设备可承载 ──► (a) 内部 RAM 序列化失败 (b) 静默逐出→重读 (c) 崩溃后静默 reset→头部丢失
回报无结构化载体 ──► 768 B 截断 ──► 重发风暴
验收无 harness 契约 ──► 口径漂移 ──► 需独立 verify 纠偏（verify 自身也 thrash）
```

---

## 3. 目标函数与度量方法（任务级基准）

> 评审意见 1 的核心：**用一个中等复杂度 Lua 任务，度量整任务的 token 与时长**，而不是单次调用。

### 3.1 度量指标（每跑一次基准任务记录）

| 指标 | 含义 |
| --- | --- |
| `T_in` / `T_out` | 全任务输入 / 输出 token（含所有子 agent） |
| `Wall` | 全任务墙钟时长 |
| `Steps` | 轮数（root + 各子 agent 的 tool round 合计） |
| `ReReads` | 同一文件被重复读取的次数 |
| `Resends` | 因截断/失败触发的重发次数 |
| `OOM` / `Reset` | 序列化 OOM 次数 / 会话重置次数 |
| `Success` | 规格数值验收项是否全达标 |
| `HumanHits` | 人类介入次数（目标 0） |

### 3.2 基准任务与实验矩阵

- **基准任务**：把本次 compass 任务固化为可复跑的基准（seed prompt + SPEC_V2 + 起始代码），
  保证"中等复杂度"定义成立。
- **自变量**：窗口策略 `W` ∈ { `fixed-48K`（现状）, `dyn-balanced`, `dyn-aggressive` }（见 §4 A0）。
- **重复**：每档 N=3，取中位数。
- **决策规则**：在 `Success==true && HumanHits==0` 的候选里，最小化 `(Wall, T_in+T_out)`；
  若大窗口以更少 `Steps/ReReads` 换来更小总成本，则采用大窗口。

### 3.3 为什么"大窗口可能更省"（token 模型）

总成本 ≈ Σ_每轮(该轮上下文大小)。小窗口把内容逐出后：
- 模型**重读**同一文件 → 该文件（32 KB）进入上下文；
- 且该读取结果在**之后每一轮**都被重新发送，直到再次被逐出。

以蓝本量化：build 多读约 10 次、verify 读 16 次 `main.lua`；若某次读取后还有 ~8 轮，
单文件就多出 ≈ 32 KB × 8 ≈ 256 KB 的**输入**冗余。故"单次更小"可能反而"整任务更大"。
这正是 P2 要整体衡量的原因，也是 A0 动态窗口的依据。

---

## 4. 主线 A：容量（动态）

### A0 —— 动态窗口推导（替代固定常量）

- **动机**：ESP32 各芯片 PSRAM 差异巨大（S3 常见 2/8 MB，P4 可达数十 MB，部分无 PSRAM）；
  固定 48/160 KB 要么浪费、要么不够。
- **推导**（在会话开始、以及 projection 事件后重算，带滞回避免抖动）：
  ```
  B = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)      // 无 PSRAM 时退化为 INTERNAL
  R = reserve_for_other_psram_users                              // LVGL/图像/音频等，见下
  W_max = (B - R) / CTX_PSRAM_FACTOR                             // 见下，≈2.x
  W = clamp(W_max * k_policy, W_floor, W_ceiling)
  ```
  - `CTX_PSRAM_FACTOR`：上下文在 PSRAM 的峰值倍数。A1 后 messages 与序列化 body 均在 PSRAM，
    峰值 ≈ 2×请求体 + JSON 脚手架 → 初值 **2.2**，由设备实测标定。
  - `R`：为其它 PSRAM 使用者预留。取 `max(绝对值下限, 比例)`，比例初值 25%，绝对值初值 256 KB。
  - `k_policy`：`balanced=0.75`、`aggressive=0.9`（保守档可保留 `0.5` 用于对照）。
  - `W_floor`：可用性下限（初值 48 KB，即现状）。
  - `W_ceiling`：**由模型上下文与成本策略决定，而非硬件**（初值由 §3 基准任务选出）。
- **可观测性**：窗口值写入每轮 `ESP_LOGI`（已有 `built context:` 日志）与 `get_system_info`，
  便于基准任务采集。
- **Kconfig**：`CONFIG_CLAW_CTX_POLICY` ∈ {fixed, balanced, aggressive}，默认由基准任务结论确定。
- **验收**：同一固件在 S3(2 MB PSRAM) 与 P4 上分别推导出各自 `W`；无 PSRAM 板自动回落 `W_floor`。

### A1 —— 上下文整条管线走 PSRAM

- 目标：消除"内部 RAM 里同时放 messages 与序列化 body"的 2× 峰值。
- 改动：新增 `components/claw_modules/claw_core/src/claw_core_json_mem.c`，安装 `cJSON_InitHooks`
  （`heap_caps_malloc_prefer(size, SPIRAM|8BIT, INTERNAL|8BIT)`），在 `claw_core` init 调用一次；
  无 PSRAM 自动回落。
- 风险/前置验证：`esp_http_client_set_post_field()` 的 body 指针能否来自 PSRAM
  （`claw_llm_http_transport.c:382`）。若不行，改为 `esp_http_client_open/write` 分块发送。
  **这是 A 主线唯一会推翻结构的前置项，建议先做最小验证。**
- 验收：构造 `W` 量级上下文，内部 RAM 不塌陷，无 OOM。

### A2 —— 闸门改为"连续块保证"

- 改动（`claw_core_context.c`）：`claw_core_request_body_bytes()` 的 `+12.5%` 改为对含大量换行/引号文本
  更保守的系数（或对 body 试序列化取实测）；序列化前增加**连续块预检**
  `largest_free_block(target heap) >= est * 1.5`，不足则逐出→复检→refuse。
- 与 A0 的关系：闸门预算 `SERIALIZE_EST_BUDGET` 改为 **由 `W` 推导**，不再是固定 144 KB。

### A3 —— 逐出提示精确化（指名丢失）

- 改动（`claw_core_evict_to_budget` / `claw_core_trim_runtime_messages`）：记录被 omit 的
  `tool_call_id` + 工具名 + 参数摘要，`trim_note` 列出前 N≈4 条具体项（如
  `read_file /sdcard/apps/compass/main.lua`），使模型"定点重读"而非盲扫。
- 与 A0 的关系：动态窗口使逐出更少触发；A3 是触发时的兜底。

### A4 —— 会话重置留痕 + 任务书/pinned 回注

- 改动：
  - `claw_memory_session.c`：reset 前把旧文件改名 `<name>.bak`，并写一条 `SUMMARY`（`keep_from=0`，
    "历史因校验失败被重置"）。
  - 上下文构建检测到 RESET → 注入系统提示："历史已重置；以 pinned card 与任务书文件 `<brief_path>` 为准。"
  - 子 agent：把 brief 路径持久化进 `subagent_map`（与 `agent_type` 同处），resume 时回注。
- 验收：破坏 `.idx` → 有 `.bak` + RESET summary + 回注提示。

---

## 5. 主线 B：可信（泛化，不做特例补丁）

> 评审意见 2：`fps` 只是"**验收口径没有被唯一、机械地定义**"这一类问题的一个实例。
> 因此 B 的落点是**机制**，不是某几个数字。

### B0 —— 可观测契约（Observable Contract）机制

**问题类**：任务的"完成"依赖若干**可观测项**（帧率、耗时、静止时不刷新、循环内无 `present_full`…）。
当前这些项：
1. 定义含糊（`fps` = `loops/s` 还是 `presents/s`？）；
2. 由**模型自己**计算与陈述；
3. 没有机器可校验的载体。

**机制**（先建机制）：
- **具名可观测项注册表**：每个验收项有一个唯一名字 + 唯一计算口径 + 数据来源。
  例：`present_fps := count(present events) / seconds`，来源 = 作业日志统计行。
- **由 harness 计算**：数值一律由工具（`lua_get_async_job` / 统计行 / 一个 `metrics` 计算器）算出，
  模型**只能引用名字，不能自造口径**。
- **可判定断言**：验收项写成 `name OP threshold`（如 `present_fps >= 20`、`idle_presents == 0`、
  `grep(present_full, loop) == 0`），由 harness 判定 PASS/FAIL。
- **证据包**：完成的回报必须携带这些断言的判定结果与原始出处（job id / 行号）。

于是：
- **B2（原 fps 实例）**：把 `present_fps` 注册进契约；SPEC §6 引用该名字。同类项（`card_ms`、
  `idle_presents`、"循环内无 `present_full`"）一并注册。
- **B3（完成证据）**：完成的定义 = "契约里的断言全部 PASS 且证据可追溯"，而非"模型说完成了"。

### B1 —— 回报不截断（机制层）

- 改动：`claw_agent_mgr_completion_observer()` 把 `summary->final_text` 全文写入
  `<DATA>/.reports/<session_key>_<request_id>.md`（复用 `.toolout` 的 sanitize/prune 思路）；
  通知文本 = head（≤512 B）+ `\n[full report: <path>]`。
- 取回路径：`inspect_agent` 增加 `report_path` 字段；root 亦可 `read_file` 该路径。
- 与 B0 的关系：报告 = 证据包的载体；不截断才能承载契约断言。

---

## 6. 常量与策略变更清单

| 项 | 现值(S3) | 新值 | 文件 |
| --- | --- | --- | --- |
| `CLAW_TIER_RUNTIME_BUDGET_BYTES` | 48 KB 固定 | **由 A0 动态推导**（`W`） | `claw_tier.h` / 新策略模块 |
| `CLAW_TIER_TOOL_RESULT_MAX_BYTES` | 8 KB | 随 `W` 缩放（初值 32 KB） | `claw_tier.h` |
| `CLAW_TIER_SERIALIZE_EST_BUDGET_BYTES` | 144 KB 固定 | 由 `W` 推导 | `claw_tier.h` |
| `CLAW_TIER_SERIALIZE_REFUSE_BYTES` | 192 KB | 由连续块实测推导 | `claw_tier.h` |
| `CLAW_TIER_SPAWN_MIN_PSRAM_BLOCK_BYTES` | 128 KB | 随 `W` 推导（≥ 若干 `W`） | `claw_tier.h` |
| `CONFIG_CLAW_CTX_POLICY` | — | 新增：fixed/balanced/aggressive | `Kconfig` |
| `CLAW_AGENT_MGR_NOTIFY_TEXT_SIZE` | 768 B | 语义改为 head（值保留） | `claw_agent_mgr.c` |

---

## 7. 迁移、风险与回退

| 风险 | 缓解 | 回退 |
| --- | --- | --- |
| PSRAM body 无法直接 HTTP 发送（A1 前置） | 先做最小验证；不行则分块发送 | 关闭 A1 |
| 动态窗口在 PSRAM 抖动时抖动 | 会话开始 + projection 时重算，带滞回 | 固定档 `fixed` |
| 大窗口推高单次 token / 首字延迟 | 以 §3 基准任务整体衡量后再定默认 | 调低 `k_policy` / `W_ceiling` |
| 契约注册表变成新负担 | 只注册"会被验收引用"的项 | 退化为纯文档约定 |
| `.bak` / `.reports` 占空间 | 复用 `.toolout` prune（10 min） | 关闭落盘 |

---

## 8. 验收

- **组件单测**：guard 有序降级 / 连续块 refuse；notify 落盘+路径；reset 留痕+回注；A0 推导在各 PSRAM 规模下正确。
- **任务级基准**（§3）：跑 compass 基准任务，三档窗口各 N=3，产出 `(T_in,T_out,Wall,Steps,ReReads,Resends,OOM,Reset,Success,HumanHits)`
  对照表，并据此选定 `CONFIG_CLAW_CTX_POLICY` 默认值。
- **契约**：至少 `present_fps`、`card_ms`、`idle_presents`、"循环内无 `present_full`" 四项可被 harness 判定。

---

## 9. 推演：同一 compass 任务在新设计下的预期过程

### 9.1 基座特性代号

| 代号 | 特性 |
| --- | --- |
| F1 | spawn admission（prompt ≤2000B / 内部 free / PSRAM 连续块，随 `W` 推导） |
| F2 | context build（preamble + replay + runtime window = `W`） |
| F3 | session projection（trigger/keep 随 `W` 缩放） |
| F4 | 逐出 + 指名 trim note（A3） |
| F5 | serialize guard（连续块保证，A2） |
| F6 | persist（逐条 append） |
| F7 | notify（head + 报告路径，B1） |
| F8 | pinned core |
| F9 | 大输出落盘（`.toolout` / `.reports`） |
| F10 | 作业 / 显示所有权 |
| F11 | 会话 reset 留痕 + 回注（A4） |
| F12 | 动态窗口 + PSRAM context pool（A0+A1） |
| F13 | `report_blocked` |
| F14 | 可观测契约 + bench / `BENCH.md`（B0） |

### 9.2 逐步推演（设 `W` 由 A0 在 S3 上推导为可整读 `main.lua` 的量级）

| 步 | 角色 | 动作 | 触发基座 | 预期 |
| --- | --- | --- | --- | --- |
| S0 | harness | 会话开始推导 `W`，写日志/`get_system_info` | F12 | `W` 可观测 |
| S1 | root | 读 SPEC + 目录，写 `NOTES.md` | F2/F6 | 窗口充裕，无逐出 |
| S2 | root | 写 brief，`spawn_agent(build)` 短 prompt | F1/F6 | 准入通过（阈值随 `W`） |
| S3 | recon | 整读 `main.lua` + SPEC，输出 gap 报告 | F2/F9/F7 | 一次读完；报告落盘，root 取全文 |
| S4 | build | 整读 brief + `main.lua` + SPEC | F2/F8/F12 | **不重读** |
| S5 | build | 写 `probe_*` 测 present/clip 语义并运行 | F9/F10 | flush law |
| S6 | build | **增量编辑**几何（`edit_file` ×N，附 grep 证据） | F6/F9 | 每步一编辑 |
| S7 | build | 跑 sweep 验证 + 留 live job | F10/F5 | 无 OOM |
| S8 | build | 写 ARCH/PROGRESS + 证据包，完成 | F6/F7/F9/F14 | 报告落盘；契约断言 PASS |
| S9 | verify | 独立读 `main.lua` + SPEC + 报告，逐条 PASS/FAIL | F2/F9/F7/F14 | 一次读完，不再 16 次重读 |
| S10 | bench | 按契约口径跑 sweep，写 `BENCH.md` | F10/F14 | 口径唯一，无"fps 之争" |
| S11 | root | 读照片，把 2 个视觉缺陷写进 brief，followup build | F2/F6 | 缺陷含坐标证据 |
| S12 | build | 改 `draw_case_field` + 删 BMM150 行，跑 sweep，留 live | F6/F9/F10 | 一轮内完成 |
| S13 | recon | 只读诊断静止乱转 | F2/F9/F10 | 病因 = 错误 offset + 无阻尼 |
| S14 | root | 把病因 + 实测证据写进 brief，followup build | F6/F8 | pinned 更新 |
| S15 | build | 改 `mag_heading`（gate offset + 阻尼 + 打印 dx/dy spread），跑 live | F6/F9/F10/F4 | 一轮内完成；若超 `W` 则 F4 指名逐出 |
| S16 | verify/bench | 复核契约断言（`heading_pp <= 0.5°`、`detent_hops == 0`） | F2/F14 | 以契约判定 |
| S17 | root | 汇总交付 | F2/F14 | 终态达成 |

**≈ 17 步**，对比蓝本：root 30 轮 + 5 子 agent + 1 OOM + ~15 重发。

### 9.3 新旧对照

| 维度 | 蓝本（旧） | 推演（新） |
| --- | --- | --- |
| build 读取 / 编辑 | 13 / 0 | ~1–2 / N |
| 序列化 OOM | 1（终止任务） | 0 |
| 报告截断 / 重发 | 每份截断，~15 次 | 0 |
| verify 重读 `main.lua` | 16 | ~1 |
| 会话头部丢失 | 发生 | 留痕 + 回注 |
| 验收口径 | 含糊 | 契约唯一 |
| 窗口 | 固定 48 KB | 按 PSRAM 动态，且以任务级基准选默认 |

### 9.4 新设计下需观察的新失效点

- `largest_free_block(PSRAM)` 成为新瓶颈 → 监控 PSRAM 碎片。
- 窗口抬高后单次 token 与首字延迟上升 → 用 §3 基准任务整体判定是否划算。
- 报告落盘后若 root 忘记取回 → 通知里明确"如需细节 read_file `<path>`"。
- 契约注册表若膨胀 → 只注册会被验收引用的项。

---

## 10. 待评审拍板的问题

1. **`W_ceiling` 的定法**：由模型上下文定、还是由成本策略定、还是由基准任务直接定？（我倾向"基准任务定 + Kconfig 可覆盖"。）
2. **A1 前置验证**：PSRAM body 能否被 HTTP 发送——是否先做这个最小验证再定结构？
3. **契约机制的载体**：注册表放 harness（C 侧）还是放 skill（文档约定）？（我倾向 C 侧注册表 + skill 引用。）
4. **基准任务的固化形式**：作为组件测试 app，还是设备脚本 + 采集脚本？
5. **A4 备份策略**：`.bak` 全量 vs 仅 RESET summary。
6. **root 侧上下文经济**：是否纳入本方案（root 会话 254 KB、19 次 followup）。

---

## 11. 建议落地顺序（先机制、后实例，先前置验证、后结构改动）

1. **前置验证**：PSRAM body 的 HTTP 发送能力（决定 A1 结构）。
2. **A0 + A1**：动态窗口 + PSRAM 管线（根治 OOM，并让窗口随芯片伸缩）。
3. **§3 基准任务 + 度量闭环**：先建"能量化"的闭环，后续策略调整才有依据。
4. **B0 + B1**：可观测契约 + 回报不截断（消除重发与口径漂移）。
5. **A3 + A4**：指名逐出 + 会话可恢复。
6. **用基准任务反选 `CONFIG_CLAW_CTX_POLICY` 默认值**。

---

## 附录 A：触达设备与常用命令

> 这一节是实施者的"操作手册"。所有命令均在 macOS + 本仓库根目录下验证过。

### A.1 串口与 Python 环境

- 串口设备：`/dev/cu.usbserial-XXXX`（USB-UART 桥）。若换口：`ls /dev/cu.*`
- 设备控制台是 esp_console REPL，提示符 `app> `。
- pyserial 在 ESP-IDF 的 venv 里（系统 python 没有）：
  ```bash
  PY=~/.espressif/tools/python/v5.5.5/venv/bin/python
  $PY -c "import serial; print(serial.__version__)"
  ```
- **不要**在脚本读串口的同时开 `idf.py monitor`（会抢端口）。

### A.2 ESP-IDF 与编译烧录

```bash
source ~/.espressif/v5.5.5/esp-idf/export.sh      # 另有 v6.0.2 / v6.1
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1     # 生成板级配置
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

- 板级名以 `application/edge_agent/boards/` 下实际目录为准（本板 = `kayzh/esp32_S3_test_board`，构建参数用 `esp32_S3_DevKitC_1`）。
- 组件测试 app 在 `components/claw_modules/*/test_apps/`，单独 `idf.py -C <dir> build`。

### A.3 设备文件系统与关键路径

- DATA 根 = **`/sdcard`**（本板走 SD 卡）；SYSTEM = **`/system`**（只读）。
- 会话日志：`/sdcard/sessions/s_<safe>_<hash>.log`（+ `.log.idx`）
- 子 agent 映射：`/sdcard/sessions/subagent_map/subagents_*.json`
- chat→session 映射：`/sdcard/sessions/chat_map/`
- pinned 任务卡：`/.pinned/<session_key>.md`
- 大工具输出落盘：`/.toolout/`
- 应用：`/sdcard/apps/<name>/`（`main.lua` + `app.json` + `REQ/ARCH/PROGRESS.md`）

会话文件名算法（`claw_memory_utils.c`）：
`safe = session_id 中非 [A-Za-z0-9_-] 的字符换成 '_'，截断到 24 字符`；
`hash = 对完整 session_id 做 FNV-1a 32 位`；文件 = `s_<safe>_<hash:08x>.log`。
用脚本换算：`tools/session_dump.py key "<session_id>"`。

### A.4 HTTP 配置服务（读大文件/会话用这个，不要用 `cap call`）

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| GET | `/api/status` | wifi / ip / `storage_base_path` |
| GET | `/api/agents` | 在线子 agent |
| GET | `/api/capabilities` | 能力清单 |
| GET | `/api/files?path=/sdcard/...` | 目录列表（JSON） |
| GET | `/files/sdcard/...` | 下载文件（原始字节） |
| POST | `/api/files/upload?path=/sdcard/...` | 上传 |
| DELETE | `/api/files?path=/sdcard/...&recursive=1` | 删除 |

```bash
BASE=http://192.0.2.10
curl -s $BASE/api/status
curl -s "$BASE/api/files?path=/sdcard/sessions"
curl -s "$BASE/files/sdcard/apps/compass/main.lua" -o main.lua
```
脚本封装：`tools/dev_fetch.py`（见附录 B）。

### A.5 串口 CLI 常用命令

```
help
cap list                                   # 全部能力
cap groups                                 # 能力组状态
cap call <name> <json>                     # 调用能力（输出上限 ~1024 B）
session [id]                               # 查看/切换当前会话
ask <prompt>                               # 多轮提问（带会话历史）
ask_once <prompt>                          # 单轮提问（无历史）
auto rules | auto last                     # 路由规则 / 上次路由结果
```

高频 `cap call` 示例：

```bash
PY=~/.espressif/tools/python/v5.5.5/venv/bin/python
$PY tools/serial_cmd.py 'cap call get_system_info {"sections":["memory"]}'
$PY tools/serial_cmd.py 'cap call get_system_info {"sections":["chip","ip","version"]}'
$PY tools/serial_cmd.py 'cap call list_dir {"path":"/sdcard","keyword":"compass"}'
$PY tools/serial_cmd.py 'cap call read_file {"path":"/sdcard/apps/compass/main.lua","offset":1,"limit":40}'
$PY tools/serial_cmd.py 'cap call grep_files {"pattern":"present_full|set_clip_rect","path":"/sdcard/apps/compass"}'
$PY tools/serial_cmd.py 'cap call lua_run_script_async {"path":"/sdcard/apps/compass/main.lua","name":"compass","exclusive":"display","args":{"sweep":90,"read_ms":0,"duration_ms":15000},"replace":true,"log_bytes":16384}'
$PY tools/serial_cmd.py 'cap call lua_tail_async_job {"job_id":"<id>","max_bytes":8192}'
```

> 注意：`cap call` 的 `get_system_info` 可用于确认 PSRAM/内部内存；`read_file`/`list_dir` 输出超过 ~1024 B 会被截断——大内容走 HTTP（A.4）。

### A.6 网络

- 设备 STA IP：`192.0.2.10`（用 `/api/status` 或 `cap call get_system_info {"sections":["ip"]}` 复核，DHCP 可能变）。
- AP 回退：SSID `esp-claw-XXXXXX`，IP `192.168.4.1`。

---

## 附录 B：辅助脚本（已落盘 `tools/`）

| 脚本 | 用途 | 依赖 |
| --- | --- | --- |
| `tools/serial_cmd.py` | 通过串口发 CLI 命令并回显 | pyserial（用 IDF venv python） |
| `tools/dev_fetch.py` | 列目录 / 下载文件 / 查状态（HTTP） | 标准库 |
| `tools/session_dump.py` | 解析会话日志：`key` / `stats` / `timeline` | 标准库 |

示例：

```bash
PY=~/.espressif/tools/python/v5.5.5/venv/bin/python
# 会话 id -> 文件名
$PY tools/session_dump.py key "chat:web:w-mv830fpa-3bnpe43k:firmware1017-2"
# 从设备取日志后做统计/时间线
$PY tools/dev_fetch.py get /sdcard/sessions/s_xxx_yyyyyyyy.log -o s.log
$PY tools/session_dump.py stats s.log
$PY tools/session_dump.py timeline s.log 600 400 900
```

---

## 附录 C：实施前检查清单与前置验证

### C.1 检查清单

1. 串口与 IDF 环境可用（附录 A.1/A.2）。
2. 设备在线且 IP 可复核（附录 A.6）。
3. **固件版本对齐**（见附录 D）：设备当前 `v0.1.0-143-gb94178a0-dirty`，仓库 HEAD 为 `364fd129`。
   实施前先 `idf.py build && flash` 对齐，再复现基线，避免"改了半天对不上现象"。
4. 记录基线内存快照（附录 D），作为 A0 动态窗口的输入样本。

### C.2 前置验证：PSRAM body 能否被 HTTP 发送（决定 A1 结构）

- 目的：确认 `esp_http_client_set_post_field()`（`claw_llm_http_transport.c:382`）可以用 **PSRAM 指针** 作为 HTTPS body。
- 方案一（最小）：写一个组件测试 app，分配一块 PSRAM buffer（如 128 KB），
  `esp_http_client_open/write` 或 `set_post_field` 发送到一个 HTTPS echo（如 `https://httpbin.org/post`），
  校验服务端收到的字节数与内容。关注是否出现 TLS/内部缓冲相关错误。
- 方案二（最贴近真实）：临时把 LLM 请求体改为 PSRAM 分配，跑一次真实对话，观察是否 200 且无 OOM。
- 判定：
  - 成功 → A1 只需装 cJSON hooks（改动小）。
  - 失败 → A1 改为 `esp_http_client_open()` + 分块 `write()` 发送 PSRAM body（改动稍大）。
- 无论哪种，都顺带量一次：一次真实请求的 `internal_largest_free_block` 最小值（用现有 `built context:` 日志）。

---

## 附录 D：设备基线快照（本次实测，供对照）

```
chip: esp32s3 rev2, 2 cores
version: esp_claw 0.1.0, git v0.1.0-143-gb94178a0-dirty, esp_idf v5.5.5-dirty
memory:
  internal_free            124515 B (≈122 KB)
  internal_largest_block    31744 B (≈31 KB)   <-- OOM 的直接原因
  psram_total             4759672 B (≈4.5 MB)
  psram_free              1138668 B (≈1.1 MB)
  psram_largest_block     1081344 B (≈1.03 MB)
storage_base_path: /sdcard
ip: 192.0.2.10
```

**由本快照直接可得的结论**：
- 内部 RAM 最大连续块仅 **~31 KB**，而 S3 档 preamble（system prompt + tool schema）本身就 ≈30 KB →
  任何"必须在内部 RAM 序列化的 >31 KB 请求体"必然 OOM。这解释了 `request_id=36` 的失败。
- PSRAM 最大连续块 ≈**1.03 MB**：按 A0 公式 `W ≈ (1.03M − reserve) / 2.2 × k_policy`，
  balanced 档可推出 **~270 KB** 量级窗口（远大于现状 48 KB），与"大窗口可能更省"的判断一致。
- 因此 **A1（把上下文管线搬到 PSRAM）是根治项**，且 A0 的动态窗口有真实余量可分配。
