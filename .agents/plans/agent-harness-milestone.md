# esp-claw agent 优化里程碑 M1

> **已被 [`agent-harness-milestone-m2.md`](./agent-harness-milestone-m2.md)（`22616fa4`）取代为"先读"文档。**
> 本文件的 §0 判据与 §2 时序节点仍是设计与判断依据，内容未变；M1 之后的变化、验证与未决项见 M2。

- 日期：2026-10-19
- 分支：`feat/agent-rework`（本地，未推）
- **里程碑提交：`b1897259`** —— 后续优化以该版本为基线，不必复盘之前的优化与 debug 过程
- 设备：ESP32-S3 N16R8（16 MB Flash / 8 MB Octal PSRAM），DATA 根 `/sdcard`，SYSTEM 根 `/system`
- 过程记录（**只在需要原始数字时查**，不要通读）：
  - 本目录 [`capacity-and-trust-hardening.md`](./capacity-and-trust-hardening.md) —— 容量与可信方案蓝本（含设备操作手册附录 A/B、基线快照 D）
  - 本目录 [`capacity-and-trust-hardening-impl-log.md`](./capacity-and-trust-hardening-impl-log.md) —— 逐轮实施记录 + 会话取证
  - 仓库外 `~/.claude/plans/context-optimization-baseline.md` —— 实测 token 基线复盘（§7=V2，§8=只读不写根因）
  - 仓库外 `~/.claude/plans/resource-lease-design.md` —— 授权/租约完整设计（Stage 2/3）

---

## 0. 一条判据（判断一切改动的公理）

设备端**每一轮都要重发全部上下文**，重取内容 = 一整轮 LLM 往返（7–20 s）。
**所以每个"逐出"机制都在把 RAM 压力换算成轮次压力。**

判据：一个改动是让模型**少发**还是**少记**；若它导致重读/重推，就是负债（已实测两次）。
推论：小窗口**不一定**更省 —— 被逐出的内容会在之后每一轮重新进入上下文，且以更小窗口分多次读
还会把同一份内容按倍数重发。

另一条等价说法：**能放进窗口的工作集，就是模型能同时"持有输入 + 产出编辑"的前提。**
容量不是 token 问题，是能力问题。

## 1. 目标与终态判据

北极星：**提升 agent 的 coding 能力（多快好省）**；上下文膨胀只是症状，不是目标。
不要为了截断而截断——丢掉决策会让 coding 变**更差**。

终态（Definition of Done，任务级）：一句需求 + 一份书面规格，agent 在设备上**无人值守**完成一个
中等复杂度 Lua 任务（2–4 文件 / ~300–600 行 / ≥2 硬件模块 / ≥3 条数值验收 / ≥1 轮"改→验"），且：
1. 规格数值验收项全部达标；
2. **零** `Out of memory serializing request` —— **目前唯一未达标项**；
3. **零**人类介入；
4. 每个"完成"都带 harness 可复核的证据；
5. 全过程 `(总 token, 总时长, 步数)` 在选定策略下最优。

---

## 2. 优化总览：按 agent 实际工作时序

颗粒度 = **基座**（设备侧容量/存储/序列化/档位）与 **harness**（循环/工具/委派/提示契约）。
每条后面的括号是引入它的提交。

### 1 引导装载

| 项 | 层 | 内容 |
|---|---|---|
| 性能档位 `claw_tier` | 基座 | `d98ed111`。~30 个预算常量收进编译期档位（按 `IDF_TARGET` 默认），S3 值与原常量逐字节一致。**后端一切容量策略的基座。** |
| 动态窗口 `W` | 基座 | `eaa04e96`。由 PSRAM 最大连续块推导，10% 滞回 + `CLAW_CTX_W_CEILING=256 KB` 安全上限。 |
| 内部 RAM 保底 | 基座 | `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 32 K→64 K；实测内部最大连续块 38 912→**63 488 B**。 |
| 撤销 cJSON 内存 hooks | 基座 | `eaa04e96`。查证 `SPIRAM_MALLOC_ALWAYSINTERNAL=0` 下默认 `malloc` 已优先 PSRAM ⇒ 原改动是 no-op，删除。"调阈值"是坏气味。 |
| 常驻约定提示词 | harness | `1f304843`。约定从 skill 提到系统提示词（每轮必然在场），skill 去重。 |

### 2 请求进入与准入

| 项 | 层 | 内容 |
|---|---|---|
| spawn 准入 | harness | `c65e11af`(H1) + `d98ed111`。**在产生任何副作用之前**拒绝：prompt > 2000 B、内部 free < 64 KB、PSRAM 连续块不足 → 可读懂的拒绝，而不是崩溃。 |
| 闸门预算合流 | 基座 | `230288ef`。总预算 = `W + 回放触发线 + 前导`，覆盖 `preamble + replay + runtime`；此前漏了回放项 → 只要存在回放历史就**每轮**误触发折叠。 |
| 闸门收紧 | 基座 | `eaa04e96`(A2)。`refuse_limit` 由 `est*4/3` 收紧为 `est`。 |
| 槽位回收 / TTL / LRU | harness | `364fd129`。spawn 满员先回收最老 IDLE 槽位而非直接 `NO_MEM`；完成子 agent 的 core 加 5 min TTL 回收（保留记录与磁盘 session，followup 懒重建）；`agent_type` 持久化，槽位复用后仍用原类型提示词。 |
| `tools_json` 缓存 | harness | `d98ed111`。固定工具 schema 不再每轮重建——针对实测"24% 固定每轮前言"这一最大 token 项。 |

### 3 上下文组装（发出去之前）

| 项 | 层 | 内容 |
|---|---|---|
| provider 单次采集 | harness | `6a587c8b`。profile / long-term / lightweight / skills-list 标 `REQUEST_START_ONLY`，每请求一次而非每工具轮。 |
| 稳定核 Pinned Core | harness | `d98ed111`。`pin_note` 写 `<DATA>/.pinned/<session>.md`，独立 provider 每轮回注 ⇒ 逐出/折叠 reasoning 都不会丢任务书。**已设备证毕。** |
| 回放投影 | 基座 | `9e3a63e0`。`SUMMARY{summary, keep_from}` 只追加；实测回放 74 795→5 947 字符（≈12×）。 |
| **轮内窗口 = W** | 基座 | `eaa04e96`。固定 48 KB → 动态 `W`。**修"子 agent 只读不写"**：58 KB 工作集 > 48 KB ⇒ 每轮逐出刚读的证据 ⇒ 重读同一批文件 8 轮、0 次编辑。 |
| 分级逐出（成本排序） | 基座 | `364fd129` + `2afc59d9`。轮内窗口与 guard 共用 `claw_core_evict_to_budget()`：折叠旧 reasoning → 最老工具结果优先（保护最新 4 轮与 `activate_skill`）→ 降到预算即停。修 guard"一次清空"导致的 16 轮盲跑。 |
| 折叠而非丢弃 | 基座 | `2afc59d9`。reasoning 保留头 200 + 尾 400。 |
| 指令优先于数据 | 基座 | `539621bf`。指令内容（skill/spec）不参与逐出；文件体积按窗口比例约束（源码 1/3、文档 1/8）。 |
| 逐出提示可执行 | harness | `b94178a0`。note 从"内容在会话历史里"（正确但无用）改为点名可重读的路径。 |

### 4 序列化与上行

| 项 | 层 | 内容 |
|---|---|---|
| 引用式投影 | 基座 | `b1897259`。OpenAI 后端投影由 `cJSON_Duplicate` 深拷贝改为引用 ⇒ 峰值 −M。 |
| 预分配输出缓冲 | 基座 | `b1897259`。`json_tree_bytes()` 估算 + `cJSON_PrintBuffered()`，消除倍增 realloc 瞬态。峰值 ~3M→2M。 |
| OOM 诊断串 | 基座 | `b1897259`。错误带 `~body B / free_int / largest_int / free_psram / largest_psram`。 |
| 后端形状投影 | harness | `006d9cf6`。只保留各后端认识的字段，修 Anthropic 形状 `is_error` 泄漏进 OpenAI 请求的 400。 |
| reasoning 回传规则 | 基座 | `57ef98c3` + `681fd1be`。`reasoning_content` 必须原样回传，缺失时补空字段（上游强制）。 |

### 5 轮内循环与工具层

- `read_file` 行号 + `offset`/`limit` 分页 + 续读提示（`4674886c`）
- **`edit_file`**（锚定 `old_text`→`new_text`、唯一匹配、多编辑、`.tmp`+rename）——**coding 能力第一杠杆**（`4674886c`）
- **`grep_files`** 内容搜索（`063a6078`）
- 工具结果落盘 `.toolout/`：>8 KB 溢出，模型看 4 KB 头 + 路径；**豁免 `read_file` / `activate_skill`**，避免"读→落盘→再读"自我循环（`6a587c8b`）

### 6 工具副作用与文件治理

- 写入自动版本化 + `restore_file`（`070c8e49`）；已知洞：**脚本写文件绕过版本快照**（仅靠习惯规避）
- 体积报告 + 模块化纪律（`b1897259`）：写工具报文件大小与每轮重读成本；提示词层要求拆 `apps/_lib/` 并 `require`
- 所有权与授权：`report_blocked` + `BLOCKED` 终局 + `<subagent_blocked>` + 契约 AUTHORITY 槽位（`5f5e5120`，**已设备跑通一次**）
- 显示所有权/租约：设计完成，Stage 2 未实施（`resource-lease-design.md`）

### 7 轮末落盘

- 投影触发点从"仅轮末"改为"每次持久化后"（`a19d4cf4`）⇒ 永不完结的 turn 也能压缩
- 失败不留毒（`40a6e39d`）：失败 turn 不再写合成 assistant "Session note"（曾永久污染历史）
- 首轮重试：turn 的**第一次** LLM 调用失败重试一次（尚无副作用），后续轮不重试

### 8 长程压缩

- 结构化复合摘要（`9e3a63e0`）：超阈值异步产出 目标/决策/已完成/文件/状态/下一步，保留尾部 24 KB，摘要复利叠加。**任务能跑完的前提条件。**
- 辅助会话身份隔离（`bff9d5b0`）：压缩/抽取用独立 runtime，且不再冒用业务 `session_id`

### 9 委派与子 agent 生命周期

- 类型 = 能力边界（`c74306b2`）：build / verify / recon / bench；**bench** 专门吸收测量日志，让 builder 窗口只有代码与规格（`539621bf`）
- 异步委派语义（`c65e11af`）：spawn 后结束回合等通知，不轮询 `inspect_agent`
- 可靠投递（`cbca5e3b`）：通知在锁外投递、3 次重试、失败暂存由 reaper 补投
- 存活 reaper（`cbca5e3b` + `19ebf1c3`）：只认"RUNNING 且循环 IDLE 超时"这一无歧义信号 → `<subagent_stuck>`
- 终局后可复活：子 agent 终局后 core 未死，`send_agent_followup` 在同一 session 上开新 turn

### 10 回报、终局与恢复

- 全终局可观测（`4674886c`）：`<subagent_failed>` / `<subagent_limit_reached>` / `<subagent_blocked>` 都能回到 root；`status`/`last_error` 回写
- 恢复纪律（`b1897259`）：失败=可能只写了一半 → 先查盘上实际落地 → `close_agent` → 恢复或派**有界**恢复子 agent（任务先写文件）→ 留**冷启动交接文件**；反复失败视为资源紧张信号
- 回报不截断（B1，**未做**）：768 B 通知截断曾引发 ~15 次重发风暴；计划改全文落 `.reports/` + 通知带路径
- 可观测契约（B0，**未做**）：验收口径有名、有唯一算法、由 harness 计算——根治"自报不可信 / fps 口径之争"

---

## 3. 已验证 / 未验证（**别重复验证**）

**已验证**
- 400 `unsupported field` 消失（用户实测）
- `pin_note` + Pinned Core 每轮回注（out-of-band 改 pinned 文件，模型复述了只存在于磁盘的 token）
- 摘要雪球修复（真实失败 session 离线回放）
- 授权流程 `report_blocked` 端到端跑通一次（终局消息、返回契约正常）
- 子 agent 只读不写：15 reads : 8 writes（1.9:1），产出并在设备验证脚本
- `dev_workflow` skill 生效（`grep_files` 命中）

**未验证**
- **compass 规模端到端复跑**（§1 判据 2「零 OOM」是唯一未达标项，且 `b1897259` 的降峰值尚未跑过）
- `364fd129` 的 guard 修复 / 槽位 TTL / LRU 设备复测（离线回放通过）
- bench agent 是否真被采用；体积提示是否降低 re-read 次数
- H1 spawn 准入从未在野外触发（当时 spawn prompt 只有 509 B）
- P4/S31 档数值是估算，需 P4 硬件回填 —— **S3 档是实测基线，不要动**

## 4. 未决项（建议顺序）

1. **基准任务 + 度量闭环**：把 compass 任务固化为可复跑基准（seed prompt + SPEC_V2 + 起始代码），
   记录 `(T_in, T_out, Wall, Steps, ReReads, Resends, OOM, Reset, Success, HumanHits)`，
   用它**反选**窗口策略（`W` 档 / `CLAW_CTX_POLICY`）。**这是当前最大缺口——没有它，后续每个策略调整都缺依据。**
2. **B0 可观测契约 + B1 回报不截断**（消除口径漂移与重发风暴）
3. **A3/A4 收尾**：`.versions` 洞（脚本写文件不留快照）、会话 reset 留痕 + 任务书回注
4. **闸门内存预检真正执行**（当前只记日志未 refuse）；`CLAW_TIER_TOOL_RESULT_MAX_BYTES` 与
   `RUNTIME_ARG_MAX_BYTES` 随 `W` 缩放 —— 后者会在逐出时**截断工具参数、可能损坏 edit 调用**，先评估
5. Stage 2 显示所有权（持有者 + 单一仲裁者 + 抢占恢复）；Stage 3 无进展熔断（兜底，不用于可预期路径）
6. `skill_creator`(8.9 KB) 超 1/8 规则且与 `skill_slimming` 职责重叠 → 加交叉引用

## 5. 环境与操作速查

```bash
# IDF 本机 export.sh 不可用，用 EIM 的激活脚本
. $HOME/.espressif/tools/activate_idf_v5.5.5.sh

# 构建（必须在 app 目录）；板名以 boards/ 下实际目录为准
cd application/edge_agent && idf.py bmgr -c ./boards -b esp32_S3_test_board && idf.py build

# 烧录：app + system（技能/文档）；绝不要烧 storage.bin（可写种子）
idf.py -p /dev/cu.usbserial-XXXX app-flash
python -m esptool --chip esp32s3 -p /dev/cu.usbserial-XXXX -b 460800 write_flash 0xa20000 build/system.bin
# 烧完常停在下载模式 → 必须硬复位
python -m esptool --chip esp32s3 -p /dev/cu.usbserial-XXXX --after hard_reset read_mac
```

**设备 HTTP API**（IP 见 `/api/status`）：`GET /api/status`、`/api/agents`、`/api/capabilities`、`/api/config`、
`/api/files?path=`（列目录，含精确 size）、`/files/<path>`（下载）、`POST /api/files/upload?path=`、
`POST /api/webim/send`（body `{"chat_id":"...","text":"..."}`）、`POST /api/restart`。
**串口 CLI**：`cap call <name> <json>`（输出截断 ~1024 B，大内容走 HTTP）、`session`、`ask`、`auto rules`。

**关键路径**：会话 `/sdcard/sessions/s_<safe>_<fnv32>.log`(+`.idx`)；子 agent 映射 `sessions/subagent_map/`；
pinned 卡 `/.pinned/<session_key>.md`；大工具输出 `/.toolout/`；应用 `/sdcard/apps/<name>/`。

## 6. 测量陷阱（踩过，别再踩）

- **大文件下载会随机截断**（不支持 Range）：重试数次保留最大者，并用 `/api/files?path=` 的 size 校验；
  截断尾部会解析成**空记录**——看到空记录先怀疑下载，别先怀疑固件。
- **只看总记录数会误判效率**：一个 turn 可有 75~100 轮；必须看 turns 结构与每轮工具调用数。
- **会话里的 `reasoning_content` 是逐字持久化的，但轮内窗口会折叠/清空它（只影响在途请求）**：
  要判断"模型当时看到了什么"必须**离线回放**，不能直接读 session。
- 会话 id → 文件名：`safe` = 非法字符替换为 `_` 截断 24 字符，`hash` = 完整 id 的 FNV-1a 32；
  脚本 `tools/session_dump.py key <session_id>`。
- `/system/...` 不能走 HTTP 文件 API（只暴露 DATA 根）；看系统镜像内容查 `build/system_fs_image/`。
- 读串口脚本会拉 GPIO0 把板子按进下载模式 → 读完必须硬复位。
- 不要同时开 `idf.py monitor` 与 pyserial 脚本（抢端口）；占串口期间设备 HTTP 不可达。

## 7. 工作纪律

- **一次只交付一个改动，等设备实测再开下一个**（不叠加开发）
- 项目代码的每处改动加 `(added)` 注释，便于未来 upstream merge 判断取舍
- 保持 `claw_core` 小：能放 capability / context provider / skill 的逻辑不要进核心循环
