# 上游合并价值分析报告（v2）

- 日期：2026-10-20
- 对象：`upstream/master` @ `345073de`（134 提交，基线 `fb7b2481`）
- 配套文档：`upstream-merge-analysis-2026-10-20.md`（合并会发生什么、要改哪里）
- 评估原则：**架构跟紧上游；保住 device side coding 优化；产品层定制（web / 默认启动器）留在边界上**
- 版本说明：v1 以"轮数收益 ÷ 人日"为唯一尺子，本版按 2026-10-20 同步的新原则重排。变化见 §0.2。

---

## 0. 原则

### 0.1 背景

分叉不是目的。当初二开是因为上游 device side coding 能力弱到"不可用、白烧 token"；现在我们把这块补上了，接下来的目标是：

1. **架构层面持续跟紧上游**，不再产生新的结构分叉；
2. **保住我们已经验证有效的 coding 优化**，等官方逐步跟上；
3. 分支的长期价值转向 **web 前端个性化** 与 **默认启动器产品层封装**。

### 0.2 v1 → v2 的变化

| 项 | v1 结论 | v2 结论 | 原因 |
|---|---|---|---|
| 显示子系统重写 | "成本最高，可以不做/最后做" | **必须做，列为下一里程碑** | display 的 Lua API 是公开架构；产品层启动器也建立在 display service 之上 |
| app_registry + 启动器 | "可选产品功能，优先级低" | **产品层底座，优先级上调** | 未来主战场就是默认启动器的个性化封装 |
| `post_message` / `authorizer` / history API | "维护收益" | **必须对齐的架构接缝** | 我们的优化要挂在这些接缝上，否则永远是平行实现 |
| 评价维度 | 轮数 ÷ 成本 | 架构对齐 > coding 保全 > 产品支撑 > 成本 | 原则变了，尺子要跟着变 |
| 我们的优化 | "保住即可" | "保住 + 写成可上游的形态 + 定义退场条件" | 等官方跟上，而不是永久分叉 |

### 0.3 五条决策规则

| # | 规则 | 含义 |
|---|---|---|
| R1 | **架构优先** | 上游已经有架构的（模块边界、API、数据模型），用上游的，把我们的实现**移植到它的接缝上**，不保留平行机制 |
| R2 | **优化必须保** | coding 优化是资产，但只能以"小块、独立文件、不改上游公共语义"的方式保留 |
| R3 | **功能可缓** | 与架构无关的功能（trial auth、Bocha、vision、audio）可以晚引入，但不许为了跳过它去改上游代码 |
| R4 | **产品层只碰边界** | 个性化只改：应用包数据、编译期主题宏、板级覆盖、独立页面组件。禁止深改上游核心 UI 逻辑 |
| R5 | **冲突以上游为准** | 当上游更"架构正确"时，改写我们的实现，而不是让上游绕着我们 |

一条补充规则（服务于规则 R5）：**每一项保留下来的优化，都要写清楚"上游实现到什么程度时我们就删掉自己的"**。见 §5。

---

## 1. 结论速览

**结论：全面跟随上游架构，分四个批次落地；coding 优化全部保留，但重新挂到上游接缝上。**

| 批次 | 内容 | 性质 | 人日 |
|---|---|---|---|
| 第 1 批 | 存储 / 技能注册表 / 启动期修复 / event router / Kconfig / provider flag | 纯架构对齐，低风险 | 1–2 |
| 第 2 批 | `post_message` + `authorizer` + history API + session 基础 | 运行时接缝，我们的优化改挂 | 3–4 |
| 第 3 批 | hw registry + 受管总线 + 传感器 | 板级架构 | 3–5 |
| 第 4 批 | display service + 触摸 + app_registry + 启动器 | 产品层底座（下一里程碑） | 6–9 |
| 持续 | web 前端对齐 + 产品边界 + 上游化提 PR | 长期 | 每季度 2–4 |

**建议的第一个动作：第 1+2 批（约 4–6 人日）。** 这两批做完，核心运行时架构就与上游一致了，剩下的显示/板级是两个可以独立排期的工程。

**明确不再建议"跳过显示重写"。** 它是架构项，也是产品层的地基；只是可以排在板级之后，避免同期风险叠加。

---

## 2. 重新排序的总表

评分 1–5，5 最好。**"架构"指采纳后能消除多少分叉；"coding"指对我们轮数/容量优势的保护或支撑；"产品"指对未来 web/launcher 定制的支撑。**

| # | 上游能力 | 架构 | coding | 产品 | 成本 | 结论 |
|---|---|:--:|:--:|:--:|:--:|---|
| A1 | `post_message` + receipt | 5 | 4 | 3 | 4 | **第 2 批** |
| A2 | `llm_authorizer` 授权钩子 | 5 | 4 | 2 | 5 | **第 2 批** |
| A3 | 会话历史读 API + `history_lock` | 5 | 4 | 3 | 4 | **第 2 批** |
| A5 | 技能注册表重构 | 5 | 3 | 2 | 4 | **第 1 批** |
| A7 | `storage` space provider + `remove_tree` | 4 | 3 | 2 | 5 | **第 1 批** |
| A12 | 启动期修复 + event router | 4 | 3 | 2 | 5 | **第 1 批** |
| A13 | Kconfig 迭代上限 | 3 | 3 | 1 | 5 | **第 1 批（改默认值）** |
| A6 | hw registry + 受管总线 | 5 | 4 | 4 | 2 | **第 3 批** |
| A10 | IMU / 环境传感器统一 | 4 | 3 | 3 | 2 | **随 A6** |
| A9 | display service 重写 | 5 | 3 | 5 | 1 | **第 4 批（必须）** |
| A8 | app_registry + launcher + 应用包 | 5 | 2 | 5 | 2 | **第 4 批（产品底座）** |
| A4 | session catalog + web chat 持久会话 | 4 | 2 | 5 | 3 | **第 4 批 / 随 web** |
| A11 | Responses / Bocha / trial auth | 2 | 1 | 2 | 4 | 暂缓 |
| A14 | vision core / audio hub | 1 | 1 | 2 | 3 | 暂缓 |

与 v1 的关键差异：

- **A9 从 1 分（可跳过）变成 5 分（架构）**：display 是 Lua 侧公开 API，也是 launcher 的渲染底座。
- **A8 从"可选"变成"产品底座"**：未来的启动器个性化就长在这套东西上。
- **A1/A2/A3 的"架构"都是 5**：它们是我们 coding 优化唯一合理的挂载点。

---

## 3. 第 1 批：纯架构对齐（1–2 人日）

目标：**在不碰 coding 优化的前提下，把外围架构换成上游的。**

| 项 | 上游接缝 | 我们要做的 | 验收 |
|---|---|---|---|
| A7 存储 | `claw_paths_set_space_provider` / `get_space`、`storage.remove_tree` | 采用上游接口；保留 `lua_pushnumber` 修复（上游仍整型 push） | SD >2 GiB 读数正确 |
| A5 技能 | `claw_skill_reload_registry`、元数据扫描、`publish/remove` | 整体采用上游 `cap_skill_mgr.c`；我们不再调用被删的 4 个查询 API | 启动时间下降；大小写不敏感生效 |
| A12 启动期 | 启动阶段跳过压缩/持久化、event router 规则恢复 | 直接采用 | 冷启动无压缩任务；规则损坏可自恢复 |
| A13 配置 | `CONFIG_APP_CLAW_MAX_TOOL_ITERATIONS` | 接入 Kconfig，默认值由 tier 推导（P4/S31=240，S3=100） | 长任务不被 64 截断 |
| 修正 | 上游 skills provider | 补 `.flags = CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY` | 技能清单不每轮重建 |

**为什么先做这批**：全部是"上游已有正确实现、我们没有强耦合"的项。做完之后，与上游的分叉面立即缩小一圈，且不触碰任何 coding 优化。

---

## 4. 第 2 批：运行时接缝（3–4 人日）

这是本次合并的核心：把我们的 coding 优化**从平行机制改成挂在上游接缝上**。

### 4.1 `post_message` 取代 submit + USER_INTERRUPT

| | 现在 | 对齐后 |
|---|---|---|
| 父/子通知 | `claw_core_submit` + `CLAW_CORE_REQUEST_FLAG_USER_INTERRUPT` + 失败暂存重投 | `claw_core_post_message` + receipt（`disposition` / `run_id`） |
| 会话并发 | 调用方自己判断 active | 核心判断"进 inbox 还是新 run" |
| 我们保留 | reaper、报告落盘 `.reports/`、`BLOCKED`、准入 | 同左，只是通知走 receipt |

**收益**：删掉一层自造机制（`claw_agent_mgr.c` 20 处 `(added)` 中的一部分），同时保留可靠性能力。
**验收**：子 agent 并发完成通知不丢；reaper 仍能判卡死；`BLOCKED` 仍能终止 turn。

### 4.2 `llm_authorizer` 取代硬编码拒用表

`claw_cap.c:90-120` 的 `recon` / `verify` 拒用表 → 迁到一个独立的 authorizer 实现（独立文件）。

**这正是"可上游化"的形态**：上游刚加了钩子，且全仓目前还没有任何调用方（只有声明与实现），我们的策略可以作为第一个用例提上去，接受概率高。

**验收**：recon 仍不能写文件；verify 仍不能改文件；默认/`build` 不受限。

### 4.3 历史读 API + 锁，投影挂在写路径上

| 层 | 归属 |
|---|---|
| 读（前端/API） | 上游 `claw_memory_read_session_messages` / `load_session_history_json` |
| 写（投影/压缩） | 我们的投影，作为 append-only 写路径，不改上游读语义 |
| 并发 | 上游 `history_lock` |

**注意**：`before/limit` 分页是对"原始记录"还是"投影后视图"，需要明确定义并在文档里写死。这是唯一的设计决策点。

### 4.4 API 收口

- `claw_core_submit` → `post_message` / `start_run`（调用点：`claw_agent_mgr.c` ×2、测试 ×5）
- `claw_agent_mgr_submit_root*` → `post_root_message` / `start_root_run_text`
- `claw_agent_mgr_agent_snapshot` → 保留我们的实现并挂到新 manager 结构上（web 状态灯继续可用）

---

## 5. 我们的优化如何"活在上游架构里"

这张表是"保住 coding 能力"的执行清单。**每一行都要有明确的挂载点和退场条件。**

| 我们的优化 | 上游接缝 | 保留方式 | 退场条件（上游做到什么就删我们的） |
|---|---|---|---|
| `edit_file` / `grep_files` / `restore_file` / 版本化 | `cap_files` 工具表 | 独立工具定义，不改上游工具 | 上游提供等价工具 |
| `read_file` 行号 + `offset/limit` | `cap_files.read_file` | 扩展入参，保持默认行为兼容 | 上游支持分页 |
| 动态窗口 W + `claw_tier` | `claw_core_config_t` 容量字段 | tier 生成默认值，Kconfig 可覆盖 | 上游提供按 target 的容量配置 |
| 会话投影 / 结构化压缩 | 历史写路径 + `history_lock` | append-only 投影，读走上游 API | 上游提供 compaction |
| Pinned core / `pin_note` | context provider（`REQUEST_START_ONLY`） | 独立 provider，一个文件 | 上游提供等价常驻上下文机制 |
| 序列化闸门 / 降峰 / 轮内窗口 | `claw_core_context.c` 内部 | 局部函数，不暴露新 API | 上游重构序列化路径时 |
| 工具结果落盘 `.toolout/` | context provider + cap 层 | 独立模块 | 上游提供结果溢出 |
| 子 agent 类型边界 | `llm_authorizer` | 独立策略文件 | 上游提供 agent 类型策略 |
| 准入 / reaper / 报告 / `BLOCKED` | `claw_agent_mgr` | 独立 `claw_agent_mgr_report.c` 等 | 上游提供生命周期管理 |
| `wait_ms` 作业等待 | `cap_lua_async` | 参数扩展 | 上游支持等待语义 |
| 传输重试 + opencode-go | transport / backend vtable | 独立 provider + 重试包装 | 上游提供重试 / 该 provider |
| 板级硬件事实 | `cap_boards` 生成器 | 与上游 README 打包合并输出 | 上游从 YAML 渲染 chip/addr |
| SD >2 GiB 修复 | space provider | 修上游实现 | 上游改为 64 位返回 |
| 显示分带 | display service flush | 若复现，作为内部补丁 | 上游处理好大传输 color-done |

**关键判断**：这 14 项里，只有 **动态窗口/tier、投影、序列化闸门、子 agent 管理器** 会与上游改动正面相撞（都在它最活跃的文件里）。其余都可以做成"独立文件 / 参数扩展"，冲突面小、也更容易上游化。

**反面清单（禁止的保留方式）**：
- 不要整文件 fork 上游模块再改；
- 不要复制一份上游代码改名保留；
- 不要修改上游公共结构体字段语义；
- 不要把我们的策略写死进 `claw_cap.c` / `claw_core.c` 这类核心文件。

---

## 6. 第 3 批：板级架构（3–5 人日）

范围：A6 + A10。

| 项 | 现状 | 对齐后 |
|---|---|---|
| 资源访问 | 各模块 `esp_board_periph_ref_handle` | `lua_driver_i2c_ref_bus` + `cap_lua_hw_claim` |
| 冲突处理 | 无 | `ESP_ERR_CONFLICT` 明确返回 |
| SCD41 | 裸 `i2c_master` + board ref | 租约 + 受管总线 |
| QMI8658 | `i2c_bus_*` backend | 原生 `i2c_master_*` + 新符号名 |

> 触摸 handle 形状（`dev_lcd_touch_handles_t`）**不放在本批**：它只在 display service 消费句柄时才需要，提前改会破坏现有的 `lcd_touch` Lua 路径。它与第 4 批的 display 一起做。

**为什么必须做**：`ESP_KAO.md` §6 自己写着"显示资源所有权 / 租约只完成设计"。架构跟紧上游，这一项就是最直接的补齐。
**风险**：claim 粒度如果过粗（整条 I2C 独占）会挡掉本来能共存的设备。必须在我们板上实测：屏 + 触摸 + SD + IMU + 磁力计 + SCD41 同时工作。

---

## 7. 第 4 批：产品层底座（6–9 人日）

这一批同时完成两件事：**显示架构对齐** 和 **启动器产品底座**。它们必须一起做，因为 launcher 渲染在 display service 上。

### 7.1 display service 重写（必须做）

- Lua API：`display.init(panel, io, w, h, ...)` → `display.open()` + screen 对象。
- 触摸并入 display service，`lcd_touch` 模块删除；板级触摸设备改为首成员 `dev_lcd_touch_handles_t` 的结构体。
- 我们的单 owner 保护逻辑在新 API 上重做。
- **分带修复必须在真机复验**：上游走 `esp_lv_adapter_dummy_draw_blit`，代码里没有 color-done 处理；我们的 16 KB 分带是针对实测问题的，不能假设它消失。

引用面：`display.init` 81 处、`get_display_lcd_params` 73 处、`lcd_touch` 539 处（含文档/测试/生成模板）。

### 7.2 启动器 + 应用注册表（产品底座）

上游结构：

```text
<data_root>/apps/<id>/        或  /system/apps/<id>/
    launcher.json             ← 唯一清单（schema_version/id/entry/icon/order/visible）
    scripts/                  ← 包内脚本目录（entry 指向其中之一）
    assets/                   ← 图标等
```

**对我们的调整（v1 说"双清单"，v2 改为单清单）**：

- 未来启动器是产品主界面，**双清单会产生两个真相源**；
- 直接把 `app_layout` 的约定改成 `launcher.json`，`entry` 指向 `scripts/main.lua`；
- `app_claw.c:122` 的系统提示词同步改写；
- `fs_new_app.lua` / `fs_sweep.lua` 改为生成/校验 `launcher.json`；
- 我们想要的 `notes`、`config.json`、`data/` 仍可保留为包内附属文件，不塞进清单。

**产品定制面（重要）**：上游 `system_ui` 的定制接口很薄——

| 想改什么 | 现在怎么做 | 分叉程度 |
|---|---|---|
| 默认有哪些应用、顺序、可见性 | `launcher.json` 的 `order` / `visible` + 应用包 | **零代码** |
| 图标、名称、内置应用 | `assets/` + `display_name` | **零代码** |
| 配色 / 明暗主题 | `system_ui_private.h` 里的 `SYSTEM_UI_THEME_*` 宏 | ~10 行，可上游化为 Kconfig |
| 字体 | `system_ui_config_t.font_path/font_size` | 零代码 |
| 网格布局（3×3） | `launcher.c` 里的 `DEFAULT_ROWS/COLS` | 小改，最好提 PR |
| 新页面 / 新交互 | 新应用包（走 display API） | 零代码，推荐 |

**结论：产品层 90% 应该是"应用包 + 数据"，而不是改 C 代码。** 这条边界如果守住，未来上游怎么改 launcher 都不会冲掉我们的个性化。

### 7.3 如果产品层要求超出上游定制面

上游 `system_ui` 目前没有运行时的主题/布局 API（主题是一组编译期宏），所以"默认启动器个性化"迟早会超出"数据 + 10 行宏"的范围。三条路，按推荐顺序：

| 方案 | 做法 | 分叉程度 | 评价 |
|---|---|---|---|
| A 数据优先 | 应用包、`launcher.json`、字体、少量主题宏 | 近零 | **默认选它**，能满足大部分品牌化需求 |
| B 薄补丁序列 | 维护 3–5 个 <100 行的小补丁（主题、网格、品牌区），放在 `patches/` 并随每次合并重放 | 小、可控 | 产品要求明确超出 A 时选它；每个补丁都要有测试点 |
| C 独立启动器 | 另写一套 launcher，替换 `system_ui` | 大 | **不推荐**：等于放弃上游 UI 修复，与"架构跟紧"直接冲突 |

同时建议把"主题/布局配置 API"作为上游 PR 提上去（见 §9）：一旦上游接受，方案 B 的补丁就能退场，产品层重新回到"数据优先"。

### 7.4 web 前端（同批或紧随）

- 保持 `frontend_source` 尽量贴近上游，产品定制放独立页面/组件；
- 我们的子 agent 状态灯改成独立组件，消费 agent 状态 API（不重写 `WebImPage.tsx`）；
- i18n 只加键、不改上游句式；
- 未来 web 个性化（品牌、布局、默认页）优先用主题变量与独立页面实现。

---

## 8. 阶段计划与工时

| 阶段 | 范围 | 人日 | 依赖 |
|---|---|---|---|
| 1 | 存储 / 技能 / 启动期 / event router / Kconfig / flag | 1–2 | 无 |
| 2 | post_message / authorizer / history API / API 收口 | 3–4 | 阶段 1 |
| 3 | hw registry + 传感器 + 触摸形状 | 3–5 | 阶段 2（会话/错误语义稳定） |
| 4 | display service + 触摸 + app_registry + launcher | 6–9 | 阶段 3（板级资源已受管） |
| 5 | web 对齐 + 产品边界 + 上游化 | 持续 | 阶段 4 |

- **只做 1+2：4–6 人日**，核心运行时架构对齐，coding 优化全部改挂完成。
- **1+2+3：7–11 人日**，板级架构对齐，可以开始接新板。
- **全部：13–20 人日**，产品底座就位。

建议的第一决策点仍是 **1+2**：它不碰显示，回滚成本低，而且决定了我们后续所有优化能否"挂在上游接缝上"。

---

## 9. 上游化清单（"等官方跟上"的具体动作）

不要被动等。把我们的优化整理成可提交的 PR，是缩小分叉最快的方式，也符合"未来只做产品层"的方向。

| 优先级 | 内容 | 接受概率 | 理由 |
|---|---|---|---|
| 1 | provider flag 修复、SD 容量 64 位返回 | **高** | 明确的 bug，改动小 |
| 2 | `read_file` 分页、`edit_file`、`grep_files` | **高** | 工具层独立，对 coding 收益直观 |
| 3 | 传输层重试（任意轮次 + 指数退避） | 中高 | 小、可解释、无架构争议 |
| 4 | agent_type 策略（作为 `llm_authorizer` 示例） | 中高 | 上游刚加钩子，正缺用例 |
| 5 | `wait_ms` 作业等待、`.toolout` 结果溢出 | 中 | 需要设计讨论 |
| 6 | 会话投影 / 结构化压缩 | 中 | 大特性，需要设计文档与实测数据 |
| 7 | tier / 动态窗口 | 中低 | 平台相关，可包装成 Kconfig |
| 8 | 显示分带 | 中 | 需要可复现步骤与面板型号 |

**策略**：先用 1–2 类"无争议小修复"建立提交记录，再提工具层，最后提大特性。每次上游合入我们的 PR，分叉面就少一块，退场条件也随之触发。

---

## 10. 不合并 / 慢合并的代价（按新原则重估）

| 代价 | 说明 |
|---|---|
| 产品层无处落脚 | 启动器个性化建立在 app_registry + display service 上；不对齐就得自己维护一套 UI，越做越分叉 |
| 架构债复利 | 上游约 60 提交/月；我们越晚对齐，未来"跟紧"的成本越高，且更容易被迫改上游核心 |
| 优化上游化受阻 | 平行实现越多，越难拆成可提交的 PR，"等官方跟上"就越遥不可及 |
| 板级重复投入 | 上游显示/触摸/传感器 API 持续演进，我们每接一块新板都要写旧 API 适配层 |
| 拿不到上游 bugfix | event router 恢复、存储后端、技能容错、启动期压缩/持久化 |

反过来，**慢一点也有一个真实好处**：显示路径保持现状，已验证的分带修复继续有效；这也是为什么把第 4 批单独排期，而不是和前三批混做。

---

## 11. 需要拍板的 5 个问题

| # | 问题 | 建议 |
|---|---|---|
| 1 | 是否确认"显示重写是必须项，只是排期问题"？ | 是。它是公开架构 + 产品底座 |
| 2 | 先做 1+2（4–6 人日），还是一次做到 3？ | 先 1+2；3 需要真机窗口 |
| 3 | 产品层定制是否接受"数据/打包优先、代码最小化"的边界？ | 是。守住这条，未来才不会被上游冲掉 |
| 4 | `app_layout` 是否彻底切到 `launcher.json`（单清单）？ | 是。产品启动器需要单一真相源 |
| 5 | 是否把"每季度 1–2 个上游 PR"列为固定工作项？ | 是。这是"等官方跟上"的最短路径 |

---

## 12. 结论

1. **架构跟紧上游不是"多合一点代码"，而是把我们的优化搬到上游的接缝上。** 第 1+2 批就是干这件事：`post_message` 承接通知、`authorizer` 承接类型边界、history API 承接投影。
2. **coding 优化一项都不丢**，但保留方式变了：从"平行机制"变成"小块 + 独立文件 + 明确退场条件"。
3. **显示和应用注册表从"可选"升级为"必须"**，因为它们是未来 web / 启动器产品层的底座；只是单独排期、单独真机验证。
4. **产品层要走"数据优先"路线**：应用包、清单字段、主题宏、独立页面组件；不要重写上游 launcher 和 web 页面。
5. **"等官方跟上"要主动做**：按 §9 的顺序提 PR，先小修复后大特性。每被上游接受一项，我们的分叉就少一块。
