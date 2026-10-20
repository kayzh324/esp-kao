# esp-claw agent 优化里程碑 M2

- 日期：2026-10-20
- 分支：`feat/agent-rework`（本地，未推）
- **M2 里程碑提交：`22616fa4`**（M1 = `b1897259`，M1 文档 = `d18208c3`）
- 设备：ESP32-S3 N16R8（16 MB flash / 8 MB octal PSRAM），DATA 根 `/sdcard`

**读法**：新对话先读本文件。**判据与设计总览在 [`agent-harness-milestone.md`](./agent-harness-milestone.md)
（M1），未变，不必重读**；本文件只记 M1 之后的变化、验证情况与未决项。
逐项证据在 [`m1-next-plan.md`](./m1-next-plan.md) §6–§14；实测数字在
[`m1-device-eval-2026-10-19.md`](./m1-device-eval-2026-10-19.md)。

---

## 0. 判据（未变，一句话版）

设备端**每一轮都要重发全部上下文**，重取内容 = 一整轮往返；逐出机制是把 RAM 压力换算成轮次压力。
改动若导致重读/重推就是负债。**一号指标是轮数**（M1 实测：逐出已不再制造重读）。

## 1. M1 之后做了什么

| 提交 | 内容 | 状态 |
|---|---|---|
| `9c60a008` | M1 设备实测评估 + 下一步计划 + `tools/session_eval.py` | — |
| `a281ea40` | **C1** 子 agent 终局文本全文落 `<DATA>/.reports/`，通知带路径；诊断串不再被 96 B 缓冲截断；`inspect_agent` 增 `report_path` | ✅ 真实任务端到端验证 |
| `7b729a13` | **C2** `lua_tail/get_async_job` 增 `wait_ms`（tail 等新输出、get 等终局），消掉轮询轮；bench 提示词删掉 "poll it" | ✅ 设备计时 + 真实任务 |
| `3b6ef312` | **C3** 序列化连续块预检（先判可行度再判预算，refuse 带可执行建议）；**C4** `get_system_info` 增 `context` 段 + 每轮日志带 `W=`/`block=`；**C5** spawn 结果附同类型活跃 agent 复用提示；**C6** skill_creator→skill_slimming 交叉引用 | C4 ✅；C3 见下；C5 仅编译 |
| `d194b30b` | **C8** 硬件信息（芯片型号 / I2C 地址 / 引脚）由 `board_hardware_info` 技能从板级原始 yaml 生成 | ✅ 设备验证 |
| `ef514a7c` | **C3 系数回调** 1.5× → 2.2×（现场实测证伪） | 修正已落地，refuse 路径未触发 |
| `afe46e6d` | `session_eval.py`：区分"真轮询"与"阻塞等待" | — |
| `3906332a` | **C9** 网关抖动：任意轮次传输层重试 + 指数退避（800→1600→3200→6400 ms，±25% 抖动，1+4 次） | ✅ 设备验证 |
| `4be3d106` | **软性三修**：技能按需激活；分阶段可观察交付（`dev_workflow` §1 改名 *Deliver in observable stages*）；路径近失明自纠（read/edit 失败时给最近存在目录） | 路径提示 ✅ 设备验证；两条提示词待观察 |
| `22616fa4` | 板级清理：删掉 `board_devices.yaml` 过时的引脚注释 | — |

## 2. 验证矩阵（别重复验证）

| 机制 | 怎么验的 | 结果 |
|---|---|---|
| C1 落盘 | 真实任务：完成通知带 `[full report: /sdcard/.reports/...]`；root 用 `read_file limit=60..70` 窗口化读回；文件 2 510 B 完整 | ✅ |
| C1 诊断 | 失败通知含全部字段 `~238679 B; largest_int=63488 largest_psram=466944 free_int=… free_psram=…` | ✅ |
| C2 阻塞 | 6 s 探针作业：`tail wait=9000` 在 1.9 s / 2.0 s 返回对应 tick；`get wait=15000` 等到 `done`；不传 `wait_ms` 0.2 s 返回 | ✅ |
| C2 实际使用 | 真实任务 13 次作业读取**全部带 `wait_ms`**（12–30 s），真轮询仅 2/78 轮 | ✅ |
| C3 系数 | 用现场数字复算：238 KB 体 + 467 KB 最大块仍 OOM ⇒ 需求 ≈2.0×；已改 2.2× | ✅ 系数正确；**refuse 未真正触发过** |
| C4 可观测 | `cap call get_system_info {"sections":["context"]}` → W=262144、预算=344064、最大块=1638400；每轮日志有 `W=/block=` | ✅ |
| C5 复用提示 | 设备 CLI 被 "root-agent only" 拒绝，无法在不驱动任务时触发 | ⚠️ **仅编译**；但行为目标已达成（见 §3） |
| C8 硬件信息 | 三个 agent 全部激活 `board_hardware_info`；会话里的 i2c 命中全是技能正文，无盲扫 | ✅ |
| C9 重试 | 端点临时指向 `127.0.0.1:1`：重试间隔 792/1408/3200/4992 ms，`(gave up after 5 attempts)`；还原后正常路径零重试 | ✅ |
| 路径自纠 | 用失败原路径验证：`… (nearest existing directory /system/skills/builtin_lua_modules contains: skill.md, scripts/)` | ✅ |

## 3. 两轮真实任务（session `firmware1019-1`，番茄钟 app）

同一 session 追加两轮，共 3 个 agent（root + 3× build，其中 1 个因 OOM 被恢复流程替换）。

| 轮 | 轮数 | 输入（估算/校准） | 真轮询 | 备注 |
|---|---|---|---|---|
| R1 | 50（root 16 + sub 34） | 4.5 M / 5.7 M | 0 | sub01 因 **interrupt-followup 后首次请求 238 KB** OOM；root 走恢复流程救回半成品 |
| R2 | 113（root 19 + sub02 80 + sub03 14） | 11.6 M / 14.8 M | 2/78 | 分阶段 S1→S4 上屏；两次 `ESP_ERR_HTTP_FETCH_HEADER` → 触发 C9 |

**过程结论（无异常行为）**
- **失败恢复两次生效**：`<subagent_failed>` → `inspect_agent` + `grep` 查盘上实际产物 → `close_agent`
  → `pin_note` 更新任务卡 → 派**有界**恢复 agent（"Do NOT rewrite the app"）。
- **root 自己总结出上下文纪律**："保持小上下文：定点 grep / 窗口读，不整份读技能"，并写进之后每一次
  followup。规则不是我们加的。
- **分阶段上屏**（用户中途要求）推进干净：每阶段报告"屏幕上应看到什么" + 留下的 job id。
- 读:写 2.1（M1 的病态是 60:0）；80 轮里 10 次摘要、间隔 9–25 条，无塌缩。
- 一个工具错误：技能文档路径漏了 `scripts/` → 促成 §1 的路径自纠。

## 4. 未决（按优先级）

1. **C3 的 refuse 路径从未真正触发**：需要一次接近上限的场景，或人为压 PSRAM 碎片。触发时应看到
   "需要的块 vs 实际有的块" + 可执行建议。
2. **C7 弱硬件兼容**（M1 期间发现的既有缺陷，未修）：
   - spawn 准入无条件要求 PSRAM 连续块 ⇒ **无 PSRAM 板永远无法派发子 agent**；
   - 核心任务栈在 PSRAM，写**内部 flash** 上的 DATA 会触发 `esp_task_stack_is_sane_cache_disabled()`
     断言重启（影响会话持久化 / `pin_note` / `.toolout` / `.versions` / `.reports`；只有"PSRAM 且无 SD"
     的板型会中招）。**P4 板级测试会直接碰到这两条。**
3. **C5 提示与两条新提示词的效果未观察**（技能按需激活、分阶段交付）。
4. `.versions` 洞（脚本写文件不留快照）；会话 reset 留痕 + 回注。
5. Stage 2 显示所有权；Stage 3 无进展熔断（误报风险，暂不做）。
6. 网关若"连上但不回"，要等满 `timeout_ms`（当前 300 s）才重试——需要的是更短的首包超时，
   留给实际遇到再评估。

## 5. 运维与测量要点（M2 新增）

- **两套路径约定**：HTTP 文件 API **相对 DATA 根**（`?path=/tmp/x`），能力层用**绝对路径**
  （`/sdcard/tmp/x`）。用 `?path=/sdcard/...` 会在根下造出一个名为 `sdcard` 的目录。
- **打开串口会复位板子**，且 SD/服务要 ~30 s：脚本必须先等 `GET /api/status` 200 再发命令。
- **测试 app 与主 app 分区表不同**（factory 0x10000 vs 0x20000）：切回主 app 必须
  `bootloader-flash partition-table-flash app-flash`；测试 app 还需 80 MHz flash + 开 PSRAM。
- **`tools/session_eval.py`**：`metrics / roles / poll / paging / cost`，支持 `start_record` 切片；
  `poll` 已区分"真轮询"与"带 wait_ms 的阻塞等待"。
- **故障注入法验证重试**：临时把 `llm_base_url` 指向 `http://127.0.0.1:1/v1`，发一条消息看日志，
  再还原（脚本必须在异常路径也还原，否则设备会留在死端点上）。
- 串口每轮可见：`built context: … W=262144 block=… free_int=… largest_int=… free_spiram=… largest_spiram=…`。

## 6. 下一步：P4 板级

- **P4 档是估算值**（`claw_tier.h`：`CONFIG_CLAW_TIER_ESP32P4` 分支，S3 档是实测基线不动）。
  P4 板级测试的第一目标是**按实测回填**：W 上限、连续块需求、spawn 准入阈值、工具结果/参数上限。
- 同时会把 **C7 两条**（无 PSRAM 回落、内部 flash 写入）从"理论缺陷"变成"必须面对"——
  P4 模组的 PSRAM/存储形态与 S3 板不同，先确认硬件形态再决定是否先修 C7。
- 纪律：板级适配只改板目录文件；项目代码改动加 `(added)` 注释。

---

## 7. P4 首次上电基线（2026-10-20，板 `jc_esp32_p4_m3_dev`）

固件 `v0.1.0-159-g357f6702`（含窗口上限按档位提供、板级描述、peripheral 地址渲染）。
烧录方式：只写 bootloader / 分区表 / app / system，**不碰 NVS 与 storage**，因此 WiFi 与
LLM 配置原样保留（NVS 在 0xa000，新旧分区布局一致）。

### 硬件

| 项 | 值 |
|---|---|
| 芯片 | ESP32-P4 eco2, rev v1.3 |
| Flash | 16 MB（Boya，走 generic driver） |
| PSRAM | 32 MB hex @200 MHz |
| 内部 RAM | total 577 KB / free 259 KB / **largest free block 131 KB** |
| PSRAM 可用 | total 29.8 MB / free 21.4 MB / **largest free block 20.9 MB** |
| 存储 | SDMMC 4-bit，`/sdcard`，卡 29.7 GB |
| 显示 | JD9365 DSI 初始化成功；**swap_xy 不支持**（启动警告属预期） |
| 音频 | ES8311（I2C 0x30）双向初始化成功 |
| 网络 | WiFi 正常（192.168.31.135）；桌面自检 7/7 online |

### context 基线（`get_system_info {"sections":["context"]}`）

```
window_bytes      = 262144        (W，被档位上限夹住)
est_budget_bytes  = 491520        (W + 投影触发 160K + 前导 64K)
block_have_bytes  = 20971520      (20 MB)
trim_count = refuse_count = 0
```

### 由此得到的三条待定

1. **W 的约束已经不是内存**：硬件能一次给出 20 MB 连续块，W 卡在 256 KB 完全是档位上限。
   但放大 W 的代价是每轮 token 与模型上下文长度——要用真实任务的总成本来判，不能凭内存余量加。
2. **P4 档的 est_budget（480 KB）可能先撞模型上下文，而不是 RAM**：S3 档是 336 KB，已接近
   128K-token 量级。第一次任务要盯 `last_body_bytes` 与后端是否报上下文超限；若超限，应先降
   `PROJECT_TRIGGER_CHARS`/`PREAMBLE`，而不是降 W。
3. C7 的两条在这块板上**不会咬人**：DATA 在 SD（不触发内部 flash 写入断言），PSRAM 充足
   （不触发 spawn 准入拒绝）。C7 留给真正的弱板。

### 按模型窗口重算 P4 档（2026-10-20）

模型事实：`deepseek-v4.1-flash` 上下文 **1M tokens**、输出上限 **384K tokens**。
按 input + max_tokens ≤ 窗口 算，输入预算上限 = 616K tokens；取中英/代码混合 **2.5 字符/token**
换算，约 **1.54 MB 字符**。参考值：Anthropic 服务端压缩的默认触发是 150K tokens / 1M 窗口（15%）。

| 宏 | 旧（估算） | 新 | 依据 |
|---|---|---|---|
| `RUNTIME_BUDGET_BYTES` | 256 KB | **640 KB** | 运行时窗口 = W；≈256K tokens = 窗口 25%；序列化峰值 2.2× ≈ 1.4 MB ≪ 20.9 MB |
| `CTX_W_CEILING` | 256 KB | **640 KB** | 同上（P4 上 W 由 PSRAM 推导后必被此值夹住） |
| `PROJECT_TRIGGER_CHARS` | 160 KB | **320 KB** | 回放压缩触发 ≈128K tokens = 窗口 13%，贴近参考的 15% |
| `PROJECT_KEEP_CHARS` | 96 KB | **192 KB** | 保持原有 60% 比例 |
| `SPAWN_MIN_INTERNAL_FREE_BYTES` | 256 KB | **96 KB** | **实测缺陷**：本板 idle 时 internal_free 只有 259 015 B < 262 144，**按原值每次 spawn 都会被拒** |

合计预算 = 640 + 320 + 64 = **1.0 MB** ≈ 419K tokens = 窗口 42%；加 384K 输出后 80%，留 20% 余量。
（若取 768 KB 则是 88%，太贴边，否掉。）

设备实测（`get_system_info {"sections":["context"]}`）：
`window_bytes=655360`、`est_budget_bytes=1048576`、`block_have_bytes=20971520`。

未动：`TOOL_RESULT_MAX_BYTES` 48 KB、`RUNTIME_ARG_MAX_BYTES` 600、`PREAMBLE` 64 KB、
`CAP_FILES_*` 128/128/256 KB —— 先观察第一轮任务再定。

已清理（同日）：`SERIALIZE_REFUSE_BYTES`、`SERIALIZE_EST_BUDGET_BYTES`、`CLAW_TIER_NAME`
三个档位宏无人使用（闸门自己按 W+trigger+preamble 算），已从三档删除；重新扫描确认
`CLAW_TIER_*` 无死宏，且清理后 `get_system_info` 的数值不变（证明它们确实没参与运行）。

---

## 8. 工具层小修（2026-10-20，来自 radioTest 实测）

| 改动 | 依据 | 设备验证（非任务调用） |
|---|---|---|
| `grep_files`：首次无匹配且 pattern 像正则时，把正则语法中和成字面候选再搜一次，命中则在结果末尾标注；两次都无匹配则提示 `\w`/`\d`/`\.` 是字面字符。描述与 schema 同步 | radioTest 里 `display\.\w+` 静默零匹配、白烧一轮；工具本就没有正则引擎，只有 `|` 或运算 | `display.+` → 回退命中 3 条并标注；`zzz+` → 无匹配并提示 |
| `edit_file`：`old_text` 未命中时，找出包含其**首行**的位置并附 `hint: line N contains ...` | 同一任务出现 5 次未命中，每次约一轮 | 经 Lua `capability` 桥构造带换行的 `old_text` → 返回 `hint: line 1 contains the first line of old_text: ALPHA line one` |

**新增的操作/测量陷阱（都踩过）：**
- **串口 CLI 按空格切分参数**：`cap call` 的 JSON 里**不能有空格**（会被当成下一个参数，报 `invalid json`），
  引号也不被识别。要传含空格/换行的 JSON，走 Lua 桥：
  `capability.call("<cap>", "<json string>", {max_output_bytes=N})`，脚本经 HTTP 上传后
  `cap call lua_run_script {"path":...}` 执行。
- **读串口会复位板子**，而 `context` 计数器（trim/refuse/last_body）是每次开机清零的 ——
  测试后想读这些数，必须在**任何串口操作之前**读，或在测试期间抓日志。
