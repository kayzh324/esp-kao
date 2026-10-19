# 容量与可信双主线改造 —— 实施记录

- 日期：2026-10-18
- 蓝本方案：[`capacity-and-trust-hardening.md`](./capacity-and-trust-hardening.md)
- 仓库 HEAD：`364fd129`
- 设备：ESP32-S3 N16R8（16 MB Flash / 8 MB Octal PSRAM），串口 `/dev/cu.usbserial-2120`，STA IP `192.168.31.120`，DATA 根 `/sdcard`
- 板级：`application/edge_agent/boards/kayzh/esp32_S3_test_board`（`idf.py bmgr -c ./boards -b esp32_S3_test_board`）

---

## 0. 环境备注（务必先读）

- **IDF 通过 eim 安装**，`source ~/.espressif/v5.5.5/esp-idf/export.sh` 在本机需要两个软链才能工作（否则报 venv/constraints 缺失）：
  ```bash
  ln -sfn ~/.espressif/tools/python/v5.5.5/venv ~/.espressif/python_env/idf5.5_py3.13_env
  ln -sf  ~/.espressif/tools/espidf.constraints.v5.5.txt ~/.espressif/espidf.constraints.v5.5.txt
  ```
- **test app 不能用 `idf.py bmgr`**：`components/claw_modules/*/test_apps/` 没有 `esp_board_manager` 依赖，bmgr 会拒绝。板级 flash/PSRAM 参数只能手写进该 app 的 `sdkconfig.defaults`（且 **`sdkconfig` 已存在时 defaults 不生效**，需 `fullclean` 或直接改 `sdkconfig`）。
- `application/edge_agent/sdkconfig` 被 **gitignore**；持久化配置要写进 `application/edge_agent/sdkconfig.defaults`。
- 不要同时开 `idf.py monitor` 与 pyserial 脚本；**占用串口期间设备 HTTP 服务不可达**（curl 超时/连不上属正常）。

---

## 1. 前置验证 C.2：PSRAM body 能否作为 HTTPS body 发送 —— ✅ PASS

- 验证程序：`components/claw_modules/claw_core/test_apps/psram_http_body_test/`（临时验证 app，未纳入产品构建）。
- 方法：分配 128 KB PSRAM buffer → 填充确定性伪随机模式 → 本地 HTTP server 接收 → `esp_http_client_set_post_field()` 发送 → 逐字节校验。
- 结果：
  ```
  POSTing 131072 bytes to http://127.0.0.1:18182/echo (body ptr=0x3c080c54, is_psram=1)
  Server: received 131072/131072 bytes, content_ok=1
  HTTP status=200
  RESULT: PASS
  ```
- **结论：PSRAM 指针可直接用于 `esp_http_client_set_post_field()`，无需改分块发送。**
- 但见 §3：该结论虽成立，**A1 仍然被撤销**，因为它的前提本身不成立。

---

## 2. 固件对齐 + 基线复现 —— ✅

- `idf.py bmgr -c ./boards -b esp32_S3_test_board` → `idf.py build` → `idf.py -p /dev/cu.usbserial-2120 flash`
- 对齐后基线内存（`get_system_info.memory`）：

| 指标 | 对齐后基线 | 附录 D 参考 |
| --- | --- | --- |
| internal_free | 127 899 | 124 515 |
| internal_largest_free_block | 38 912 | 31 744 |
| psram_free | 2 072 796 | 1 138 668 |
| psram_largest_free_block | 1 638 400 | 1 081 344 |

---

## 3. 关键发现：A1 是 no-op（"调阈值"确实是坏气味）

`application/edge_agent/sdkconfig.defaults:12`：

```
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0
```

IDF `heap_caps.c:107-134` 的路由逻辑：

```c
if (size <= malloc_alwaysinternal_limit)          // 0 ⇒ 恒为假
    r = heap_caps_malloc_base(size, MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
else
    r = heap_caps_malloc_base(size, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM);  // ← 走这里
```

**即：本工程默认 `malloc()` 已对一切 >0 字节的分配优先走 PSRAM。**

由此：

- A1 的 cJSON hooks（`heap_caps_malloc_prefer(SPIRAM, INTERNAL)`）与默认 `malloc` **等价**，什么都没改变；
- "cJSON 小对象落在内部 RAM 造成碎片化"的假设**不成立**；
- 1 KB → 8 KB → 2 KB 的阈值调整全部建立在错误前提上 —— 已按"方案 A"实施后又整体**撤销**；
- Session History 字符串、mbedtls（`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`）等大对象**本来就在 PSRAM**。

**决定：撤销 A1，不再对分配位置做任何"阈值调参"。**

---

## 4. 崩溃根因分析（cjsonTest 会话 + panic）

### 4.1 会话内容

`cjsonTest` = `chat:web:w-mv830fpa-3bnpe43k:cjsonTest` → `s_chat_web_w-mv830fpa-3bnp_304ea942.log`（28 KB，11 条，无子 agent）：

| 轮 | 动作 |
| --- | --- |
| 1 | `list_dir(compass)` + `list_dir(tmp)` |
| 2 | `read_file(PROGRESS.md)` + `memory_recall(...)` → 11 425 B |
| 3 | `grep_files(compass,"^#")` + `read_file(NOTES.md)` → 1 224 B |
| 4 | `read_file(SPEC_V2.md)` → 9 795 B |

第 4 轮（`iteration=4`）即崩溃。**会话本身很小，无上下文累积迹象** → 与"窗口/容量"无关。

### 4.2 panic 链条

```
pm_dream → pm_tbtt_process → wifi_rf_phy_enable → esp_phy_enable
→ phy_track_pll_init → esp_timer_create → ESP_ERR_NO_MEM → abort()
```

同一时刻还有：

```
E spi_master: setup_dma_priv_buffer(1214): Failed to allocate priv TX buffer   (LCD 刷屏)
E xpt2046: XPT2046 read error!                                                (触摸)
```

### 4.3 结论

- 崩溃是**内部 RAM / DMA 分配失败**（WiFi modem-sleep 的 `phy_track_pll` 定时器 churn + SPI 显示 bounce buffer），
  **与本次容量改动无因果关系**，属既有脆弱点。
- 内部 RAM 只被三类占用者消耗（`malloc` 搬不走）：DMA 缓冲区（SPI LCD/触摸/SD、I2S）、
  WiFi/LWIP 硬件缓冲、显式 `MALLOC_CAP_INTERNAL` 分配（`claw_cap.c:575`、`cap_mcp_server.c:137`、`INTERNAL_ONLY` 栈）。
- 实测：idle `internal_free=128 003` 但 `largest=38 912`（**开机即已碎片化**）；崩溃前 `free=67 927 / largest=31 744`。

---

## 5. 本轮改动清单

| # | 改动 | 文件 | 说明 |
| --- | --- | --- | --- |
| 1 | **撤销 A1** | 删除 `src/claw_core_json_mem.c`；`CMakeLists.txt` / `claw_core_internal.h` / `claw_core.c` 同步移除 | 去 no-op 复杂度 |
| 2 | **A0 内存安全上限** | `src/claw_core_context_window.c` | 新增 `CLAW_CTX_W_CEILING`（默认 256 KB）；`W` 在按 PSRAM 推导后再夹到上限，防止"PSRAM 大就无限放大请求体" |
| 3 | **A2 收紧闸门** | `src/claw_core_context.c` | `refuse_limit` 由 `est*4/3` 改为 `est`（trim 触发即拒绝上限，不再允许 33% 过冲）；`est = W + preamble` |
| 4 | **A3 bug 修复** | `src/claw_core_context.c` | 原实现里 `claw_core_trim_runtime_messages` 会用通用文案**覆盖**刚写入的指名 note；改为 evict 写入独立 `omitted[]` 后由最终 note 拼接 |
| 5 | **内部 RAM 保底** | `sdkconfig.defaults` + `sdkconfig` | `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 32768 → **65536**（为 DMA 预留） |

### 改动 5 的实测效果（同板、刚开机）

| 指标 | 改动前 | 改动后 |
| --- | --- | --- |
| internal_largest_free_block | 38 912 B | **63 488 B** |
| internal_total | 324 131 B | 356 899 B |
| internal_free | 128 003 B | 127 647 B |

**最大连续内部块近乎翻倍**，DMA 头寸显著改善。

---

## 6. 待办 / 下一步（按优先级）

1. **验证崩溃是否稳住**：跑同一 compass 续开发任务（`cjsonTest` 场景）多轮，观察是否再出现
   `phy_track_pll_init / setup_dma_priv_buffer` 失败。
2. 若仍崩，按"独立工作流：内部 RAM / DMA 安全"处理（**不要**再回到 cJSON 阈值）：
   - 关 WiFi modem-sleep（`esp_wifi_set_ps(WIFI_PS_NONE)`）以消除 `phy_track_pll` 定时器 churn；
   - 让 LCD 刷屏 buffer 落**内部 DMA RAM**，消除 SPI bounce buffer；
   - 开 `CONFIG_HEAP_TRACING` 定位那 ~60 KB 的常驻内部占用。
3. **A0 的 `W` 与 `CLAW_CTX_W_CEILING`**：当前 256 KB 是保守值，待 §3 基准任务实测后再上调；
   同步把 `claw_core_window_update()` 接到 session projection 事件上（当前仅在 create 时算一次）。
4. A2 的 `est_budget` 与 `refuse_limit` 需随 `W` 定标后再定稿。
5. 清理临时验证 app：`components/claw_modules/claw_core/test_apps/psram_http_body_test/`（未纳入构建，可删）。
6. 后续主线：§3 基准任务与度量闭环 → B0 契约 + B1 回报不截断 → A4 会话可恢复。

---

## 7. 复现命令

```bash
# 环境
ln -sfn ~/.espressif/tools/python/v5.5.5/venv ~/.espressif/python_env/idf5.5_py3.13_env
ln -sf  ~/.espressif/tools/espidf.constraints.v5.5.txt ~/.espressif/espidf.constraints.v5.5.txt
source ~/.espressif/v5.5.5/esp-idf/export.sh

# 构建烧录（主应用）
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_S3_test_board
idf.py build
idf.py -p /dev/cu.usbserial-2120 flash

# 设备状态 / 内存（HTTP 路径相对 DATA 根，不带 /sdcard 前缀）
curl -s http://192.168.31.120/api/status
curl -s "http://192.168.31.120/api/files?path=/sessions"

# 串口 CLI（IDF venv python，勿与 monitor 同开）
PY=~/.espressif/tools/python/v5.5.5/venv/bin/python
$PY -c "import serial,time; s=serial.Serial('/dev/cu.usbserial-2120',115200); s.write(b'cap call get_system_info {\"sections\":[\"memory\"]}\r\n'); time.sleep(2.5); print(s.read(s.in_waiting or 1).decode())"

# 会话 id -> 文件名（FNV-1a 32）
python3 -c "h=2166136261
for b in 'chat:web:w-mv830fpa-3bnpe43k:cjsonTest'.encode(): h=(h^b)*16777619&0xffffffff
print(f's_chat_web_w-mv830fpa-3bnp_{h:08x}.log')"
```

---

## 8. 未纳入提交的临时产物

- `.agents/plans/`（本目录，含方案与本文档）
- `components/claw_modules/claw_core/src/claw_core_context_window.c`（A0，新增）
- `components/claw_modules/claw_core/test_apps/psram_http_body_test/`（C.2 验证 app，可删）
- `.agents/tmp/cjsonTest.log*`、`.agents/tmp/s_chat_*`（下载的会话，可删）

---

## 9. 追加（同日）：子 agent"光读不写"的根因与修复

### 9.1 现象

同一 session 继续测试：根 agent 无报错，但 **build 子 agent 陷入"只读文件、从不写文件"的无效轮**。

会话证据：`chat:web:w-mv830fpa-3bnpe43k:cjsonTest:subagent_01`（type `build`）
→ `s_chat_web_w-mv830fpa-3bnp_53e01e03.log`（537 KB，117 条记录）：

```
tool call counts: {'read_file': 59, 'grep_files': 1}     ← 0 次 edit_file / write_file
```

读取呈**完全重复的循环**（重复 8 轮）：
`SPEC_V2.md → PROGRESS.md → ARCH.md/NOTES.md → main.lua（分块）→ SPEC_V2.md → …`

```
 28  /sdcard/apps/compass/main.lua
  8  /sdcard/apps/compass/SPEC_V2.md
  8  /sdcard/apps/compass/PROGRESS.md
  8  /sdcard/apps/compass/NOTES.md
  7  /sdcard/apps/compass/ARCH.md
```

### 9.2 根因

| 项 | 值 |
| --- | --- |
| 子 agent 工作集（SPEC_V2 9272 + PROGRESS 9050 + ARCH 6386 + NOTES 1101 + main.lua 33729） | **≈ 58 KB** |
| S3 运行时窗口 `CLAW_TIER_RUNTIME_BUDGET_BYTES` | **48 KB** |

**58 KB > 48 KB** ⇒ 每轮 `claw_core_trim_runtime_messages` 都逐出最老的工具结果 ⇒ 模型下一轮
发现上下文缺了，于是**重读同一批文件** ⇒ 永远在重建上下文，从不进入"编辑"决策。

**这是 A0 只做了一半**：`claw_core_window_get()`（动态窗口 `W`）当时**只接进了序列化闸门（A2）**，
**没有接进运行时窗口** —— 而方案 §9.1 F2 明确要求 "runtime window = W"。
（`claw_core_context.c:925` 用的是固定常量 `CLAW_TIER_RUNTIME_BUDGET_BYTES`。）

> 注：`read_file` 的结果**不落盘**（`claw_core_bound_tool_output` 对 `read_file` 豁免），
> 所以整份 `main.lua`（33 KB）会完整进入窗口 —— 这使工作集必然超过 48 KB。

### 9.3 修复

`claw_core_context.c`：运行时逐出预算由固定常量改为动态窗口：

```c
    claw_core_evict_to_budget(runtime_messages, claw_core_window_get(),   /* was CLAW_TIER_RUNTIME_BUDGET_BYTES */
                              CLAW_TIER_RUNTIME_KEEP_ROUNDS, &stats,
                              omitted, sizeof(omitted));
```

`claw_core_window_get()` 返回 `W`（本次上限 256 KB）：
- 58 KB 工作集 + 若干轮 reasoning ≪ 256 KB ⇒ **不再触发逐出** ⇒ 模型能持有完整工作集并做编辑决策。
- 上限由 `CLAW_CTX_W_CEILING`（256 KB）保证内存安全。

### 9.4 遗留（下一轮处理）

- `CLAW_TIER_TOOL_RESULT_MAX_BYTES`（S3 = 8 KB）与 `CLAW_TIER_RUNTIME_ARG_MAX_BYTES`（S3 = 200）
  仍是固定值；方案 §6 要求随 `W` 缩放。其中 `ARG_MAX_BYTES` 会在逐出时截断工具参数，
  **可能损坏 edit 调用**，需一并评估。
- 待复测：同一 compass S-B 任务下，build 子 agent 是否开始出现 `edit_file`。

---

## 10. 追加（同日）：长程工作的读写比例与"每轮 trim"日志

### 10.1 会话分析（`s_chat_web_w-mv830fpa-3bnp_53e01e03.log`，1.3 MB，build 子 agent）

> 下载备注：设备 HTTP 文件下载在 SD 读失败时会中途截断（`http_server_files_api.c` 的
> chunked 流未终止）。多次重试只取到 ~1.16 MB / 1.37 MB（≈85%），尾部若干轮缺失；
> 已足够定性。

会话含**两个阶段**（同一 `subagent_01` 会话 id 跨运行追加）：

| 阶段 | 记录区间 | read-ish | write-ish | exec(lua) | read:write | main.lua 读 |
| --- | --- | --- | --- | --- | --- | --- |
| **A 修复前**（旧死循环） | 0–117 | 60 | **0** | 0 | 60:0 | 28 |
| **B 修复后**（真实推进） | 118–299 | 43 | **15** | 32 | **2.9** | 14 |

阶段 A 是 §9 的死循环（SPEC/PROGRESS/ARCH/NOTES/main.lua 整读循环 7 轮，0 写）。
阶段 B 才是本次成果：`activate_skill`→定向 `grep`→**小切片 `read_file`**（如 `off=180 lim=40`）
→`write_file`（tmp 脚本）→`lua_run_script_async`/`lua_get_async_job`（**32 次设备实测**）
→`edit_file main.lua`×3→写 `SB_B*_report.md`。

**结论**：
1. 全量 read:write=7.85 是**被阶段 A 的旧死循环拉高的**；阶段 B 实际为 **2.9**。
2. 阶段 B 的读是**手术式、必要的**（编辑前取精确文本 + 定向 grep 定位），
   **不是硬件窗口逐出造成的重读**。且见 10.2：trim 日志 `omitted=0` —— **没有任何工具结果被丢**。

### 10.2 "每轮 trim"日志的根因

```
W request body ~352881 bytes >= 294912 budget: trimming by recovery cost (free_int=126615 largest_int=63488 free_spiram=1463212 largest_spiram=983040)
W request body trimmed to ~294252 bytes (folded=7 omitted=0 blanked=0)
```

- `294912 = 288 KB = W(256 KB) + preamble(32 KB)`。
- 请求体 `= preamble + 回放历史(replay) + 运行时窗口`，而 **A2 的 `est_budget` 只加了 `W + preamble`，
  漏掉了回放历史项**（原 `CLAW_TIER_SERIALIZE_EST_BUDGET_BYTES` 公式里是有
  `CLAW_TIER_PROJECT_TRIGGER_CHARS` 的）。于是只要回放历史存在，总量必然 > 预算 ⇒ **每轮都 trim**。
- `folded=7 omitted=0 blanked=0`：只折叠了 reasoning（有损：保留首尾），**未丢任何工具结果/文件** ⇒
  说明重读**不是**数据被逐出造成的。

### 10.3 修复

`claw_core_context.c`：闸门总预算补回回放历史项：

```c
    size_t est_budget = claw_core_window_get() +
                        CLAW_TIER_PROJECT_TRIGGER_CHARS +      /* 回放历史（原缺失项） */
                        CLAW_TIER_SERIALIZE_PREAMBLE_BYTES;
```

`W(256K) + 48K + 32K = 336 KB`，覆盖 `preamble + replay + runtime` 三项，闸门不再每轮误触发。
（已 build + flash。）

### 10.4 结论与建议

- **重读必要性**：阶段 B 的读是必要的（精确编辑 + 设备实测闭环）；窗口**没有**在逐出数据。
- 遗留可优化项：`CLAW_TIER_RUNTIME_ARG_MAX_BYTES`(200) 与 `CLAW_TIER_TOOL_RESULT_MAX_BYTES`(8 KB)
  仍为固定值（§9.4）；`CLAW_CTX_W_CEILING`(256 KB) 是否上调需按 PSRAM 最大连续块与基准任务定。

---

## 11. 追加（同日）：长程多子 agent 会话分析——OOM 仍会杀死子 agent

里程碑：`230288ef`（est_budget 补回放历史项）。

### 11.1 会话构成

| 会话 | id | 大小 | 角色 |
| --- | --- | --- | --- |
| root | `…:cjsonTest` (`304ea942`) | 365 KB / 167 条 | 编排：`spawn_agent`×5、`send_agent_followup`×10、`pin_note`×7、`inspect_agent`×3 |
| subagent_01 | `…:subagent_01` (`53e01e03`) | 2.04 MB / 603 条（已完整取得） | 主线 build：B1→B2→B3→B4 |
| subagent_02 | `…:subagent_02` (`54e01f96`) | 264 KB / 70 条 | 恢复被 OOM 杀死后的 5A 树 |
| subagent_03 | `…:subagent_03` (`55e02129`) | 44 KB / 15 条 | 紧预算恢复 + 落盘状态 |

### 11.2 读写比例确实改善

| 会话/阶段 | read-ish | write-ish | exec(lua) | read:write |
| --- | --- | --- | --- | --- |
| subagent_01 **全量**（603 条） | 160 | 48 | 90 | 3.3 |
| subagent_01 阶段 A（修复前死循环） | 60 | 0 | 0 | 60:0 |
| subagent_01 阶段 B（修复后） | 100 | 48 | 90 | **2.1** |
| subagent_02 | 17 | 7 | 18 | 2.4 |
| root | 29 | 8 | 23 | 3.6 |

阶段 B：`main.lua` 读 40 次 / 写 48 次（≈0.8 读/写，编辑前取精确文本所必需），
并有 90 次设备实测（`lua_run_script*` / `lua_get_async_job` / `lua_tail_async_job`）。
**结论：修复后重读是手术式、必要的；不是窗口逐出造成（§10.2 `omitted=0`）。**

且交付真实：B1 探针→B2 修 BUG-1（真机静止 60 s 航向仅散 0.3°）→B3 成本归因→B4 RGB565 缓冲（用户目测 A/B "一样"），
`/tmp` 留下 B1–B4 报告 + 备份 + 交接文件。

### 11.3 关键问题：`Out of memory serializing request` 仍会杀死子 agent

root 会话记录（`<subagent_failed>`）：

- 记录 111：`subagent_01 … error="Out of memory serializing request"`（它在收尾时挂掉）。
- 记录 131：`subagent_02 … error="Out of memory serializing request"`（恢复任务也挂了）。

root 恢复得体的（`inspect_agent`→写对比脚本→`close_agent`→`spawn_agent` 新恢复子 agent→写
`SB_HANDOVER.md`），但**方案 §1 判据 2「零 OOM」仍未达成**。

### 11.4 根因：序列化峰值 ≈ 3× 请求体

`claw_llm_backend_openai_compatible.c`：
1. `openai_compatible_project_message()` 对**每条消息**做 `cJSON_Duplicate(field, true)`（深拷贝）→
   构造出与 `request->messages` 等大的 `body` 树；
2. `cJSON_PrintUnformatted(body)` 再分配等大的输出串（且 cJSON 内部按倍增扩容）。

于是峰值 ≈ 原树 M + 副本 M + 输出串 M ≈ **3M**。子 agent 长程运行后 M 很大（会话 2 MB），
叠加显示/会话/PSRAM 碎片 → `malloc` 回落内部 RAM（仅 ~63 KB）失败 → 返回 NULL → OOM。

### 11.5 建议修复（按性价比）

1. **消除深拷贝**：投影改用 cJSON 引用（`cJSON_AddItemReferenceToObject`），峰值 3M→2M。
2. **闸门落实内存预检**（A2 只记了日志没执行）：`est × CTX_PSRAM_FACTOR > 可用 PSRAM` 时
   **提前 refuse 并给可执行建议**，把"崩溃"变成"可续跑的干净拒绝"（对齐 §1 判据 3）。
3. **约束子 agent 上下文增长**：长程子 agent 会话 2 MB、请求体逼近上限；需对子 agent 也做
   session projection / 上限。
4. **诊断**：把 free/largest（PSRAM+internal）写进 OOM 错误串，便于下次定位。

### 11.6 取数限制（已澄清）

- 设备 HTTP 文件下载**在设备忙时会截断** chunked 流（SD 读失败，`http_server_files_api.c` 的流未终止）；
  **设备空闲时可完整下载**（本次 2.04 MB 一次取全，603 条记录 0 损坏）。
  ⇒ 取大会话前先停掉 agent/作业；失败就重试。
- `curl` Range 不受支持；`storage.read_file()` 整读 2 MB 报 `failed to allocate read buffer`
  （Lua 串放不下）——**不要对大文件整读**。若要自动化取大文件，建议给 HTTP 下载加 Range/分片。

### 11.7 进一步方案（基于完整记录）

**目标对齐 §1 判据 2「零 OOM」**：目前唯一未达标项。

**根因（代码级）**：序列化峰值 ≈ **3–4× 请求体**
- `openai_compatible_project_message()` 深拷贝每条消息 → `body` 树 ≈ M；
- `cJSON_PrintUnformatted()` 输出串按倍增扩容 → 瞬态再 ≈ M；
- 子 agent 长程运行 M 逼近闸门上限（W 256 KB + replay 48 KB + preamble 32 KB = 336 KB），
  峰值 ~1 MB，PSRAM 碎片化时 `malloc` 回落内部 RAM（~63 KB）失败 → NULL → OOM。

**改动（按性价比）**：

| # | 改动 | 预期收益 | 风险 |
| --- | --- | --- | --- |
| 1a | 投影改 cJSON 引用（`cJSON_AddItemReferenceToObject`），去掉深拷贝 | 峰值 −M | 需确认 body 生命周期短于 messages（是） |
| 1b | `cJSON_PrintBuffered(body, est, false)` 用预估值预分配 | 去掉倍增瞬态 −~M | 预估值偏小会回落倍增（无害） |
| 2 | 闸门落实**内存预检**：`est × CTX_PSRAM_FACTOR > 可用(PSRAM+internal 连续块)` → 逐出→复检→仍不足则 **refuse（带建议）** | 把"崩溃"变成"可续跑的干净拒绝" | 阈值定标 |
| 3 | OOM/拒绝错误串带 free/largest（PSRAM+internal） | 可定位 | 无 |
| 4 | 子 agent 上下文增长约束（长程 2 MB 会话） | 降 M | 需设计 |

**建议先做 1a+1b（直接降峰值）+ 3（诊断）**，复测同一 compass 任务看 OOM 是否消失；
若仍现，再做 2 兜底。1a+1b 目标是峰值 **3M → 2M**（树 + 单份输出，已是该设计下限）。

---

## 12. 追加（同日）：降序列化峰值 + 提示词/skill 层约束

### 12.1 代码（1a + 1b + 3，两个后端）

| # | 改动 | 文件 |
| --- | --- | --- |
| 1a | 投影改 **cJSON 引用**（`cJSON_AddItemReferenceToObject`）去掉每条消息的深拷贝 → 峰值 −M | `claw_llm_backend_openai_compatible.c`（`openai_compatible_project_message`） |
| 1b | `cJSON_PrintBuffered(body, json_tree_bytes(body)+64, false)` 预分配输出缓冲，去掉倍增瞬态 −~M | 同上 + `claw_llm_backend_anthropic.c` |
| 3 | OOM 错误串带 `~body B / free_int / largest_int / free_psram / largest_psram` | 同上 |

引用安全性：`request->messages` 生命周期长于临时 `body`；cJSON 删除引用项不会释放共享的
`child`/`valuestring`。anthropic 的**块级深拷贝**（`anthropic_duplicate_supported_block`）本次未改，
作为后续项（非当前后端）。

### 12.2 提示词 / skill 层（对应用户追加的两点）

| 层 | 位置 | 内容 |
| --- | --- | --- |
| 系统提示词 | `app_claw.c` `APP_ROOT_AGENT_SYSTEM_PROMPT` | 子 agent 失败（含 OOM）时：**检查盘上实际落地（可能只写了一半）**→`close_agent`→恢复或派**有界恢复子 agent**（任务先写文件）；反复失败=资源紧张信号，**缩小每个子 agent 的体量与所需上下文**；留**冷启动交接文件** |
| 系统提示词 | `app_claw.c` `APP_SYSTEM_PROMPT_CONVENTIONS` | **模块化**：按职责拆到 `apps/_lib/` 并 `require`，文件超过约 1/3 轮预算即为过大；`main.lua` 越写越长是缺陷不是进度 |
| agent 类型提示词 | `claw_agent_mgr.c` build | 同上模块化约束（写工具会报文件大小） |
| 系统 skill | `skills/dev_workflow/SKILL.md` | §9 增「Modularize, do not append」；新增 §10「When a subagent dies (OOM / timeout / limit)」 |

依据（用户追加的发现）：① root 在长程运行中自主摸索出了"资源紧张→子 agent 异常→恢复"的流程，
应固化到提示词/skill，不必每次重新发现；② compass 一直是单一 `main.lua` 不断追加到 61 KB，
说明模块化与单文件体积约束没有落到**提示词/skill 层**。

### 12.3 验证

- build 通过；flash 通过；设备启动正常。
- skill 已生效：`grep_files /system/skills/dev_workflow "Modularize"` → `skill.md:128` 命中。

### 12.4 遗留

- 1a 未应用到 anthropic 的块级拷贝；2（闸门内存预检）与 4（子 agent 上下文增长约束）未做。
- 待复测：同一 compass 任务是否还出现 `Out of memory serializing request`。

