# 上游 134 提交合并分析报告

- 日期：2026-10-20
- 本地：`feat/agent-rework` @ `6b7bcc2d`（ESP KAO，相对基线 63 提交）
- 上游：`upstream/master` @ `345073de`（2026-10-19，相对基线 134 提交）
- 共同基线：`fb7b2481`（upstream 合并点，见 `VERSIONS.md`）
- 分析方式：**只读**。仅执行 `git fetch upstream`（只更新 refs）、`git log/diff/show`、`git merge-tree --write-tree`（在对象库中生成合并树，不动工作区、不建分支、不建 worktree）。
- 配套文档：`upstream-merge-value-2026-10-20.md`（合并价值分析）

> 本报告描述"合并时会发生什么、要改哪里"，不描述"值不值得合并"。价值判断见配套文档。

---

## 0. 结论

| 项 | 结论 |
|---|---|
| 能否合并 | 能。没有不可调和的设计冲突，但需要重放我们 163 处 `(added)` 改动 |
| 冲突规模 | 25 个文本冲突（合计 44 处冲突块）+ 1 个二进制冲突、4 个 modify/delete |
| 硬断链 | 3 处：显示 Lua API 整体替换、触摸 handle 形状、SCD41 资源获取方式 |
| 编译级断链 | 4 组 API 被删/改名：`claw_core_submit`、`claw_agent_mgr_submit_root*`、`claw_skill_*` 四个查询接口、`claw_agent_mgr_agent_snapshot` |
| 软回归（上游自身也退了一点） | 2 处：新 skills provider 缺 `REQUEST_START_ONLY` flag；`storage.get_free_space` 走新 space provider 后仍用整型 push |
| 最大单项成本 | 显示子系统（含触摸、板级 README、Lua 示例/测试、LVGL 引用） |
| 建议姿态 | 分领域合并；架构对齐优先，独有优化改挂到上游接缝（取舍与顺序见价值分析 v2） |

---

## 1. 上游变更全景

### 1.1 体量

| 指标 | 上游 | 本地 |
|---|---|---|
| 提交 | 134 | 63 |
| 变更文件 | 527 | 173（含 4 份板级 PDF、1 份 UAC 补丁副本） |
| 增/删行 | +28,068 / −28,926 | +63,631 / −356（其中 PDF 39,779 行，文本改动约 +2.39 万） |
| 提交类型 | feat 39 / fix 34 / refactor 22 / merge 34 / 其他 5 | 以能力建设为主，中文提交信息 |

上游提交高度集中在几个包：

| 目录 | 增 | 删 | 文件数 |
|---|---|---|---|
| `components/lua_modules` | 10,740 | 20,143 | 209 |
| `components/common` | 5,804 | 1,196 | 66 |
| `application/edge_agent` | 4,408 | 1,733 | 68 |
| `components/claw_modules` | 3,654 | 867 | 59 |
| `components/claw_capabilities` | 1,412 | 1,949 | 52 |

`lua_modules` 净删约 9,400 行：不是功能缩水，而是把显示、触摸、LEDC、传感器统一到新的资源/呈现框架后删掉了各自的样板代码。

### 1.2 组件级增删（相对基线）

**新增组件：**

| 组件 | 作用 |
|---|---|
| `components/common/app_registry` | 应用包注册表，扫 `launcher.json`，DATA 优先 / SYSTEM 兜底 |
| `components/common/claw_hw_registry` | 硬件资源注册与租约（exclusive/shared claim） |
| `components/common/audio_hub` | 播放混音 + 采集 hub |
| `components/common/audio_playback` | 播放实例/状态机 |
| `components/claw_capabilities/cap_app_mgr` | `publish_app` / `remove_app` + `app_creator` skill |
| `components/lua_modules/lua_driver_ledc` | 原 `lua_module_ledc` 改名为 driver |

**删除组件：**

| 组件 | 替代 |
|---|---|
| `lua_module_lcd_touch` | 触摸并入 `display_service`（`display_service.c:624` 读 `dev_lcd_touch_handles_t`） |
| `lua_module_ledc` | `lua_driver_ledc`（Kconfig 同步改为 `APP_CLAW_LUA_DRIVER_LEDC`） |

**文件级删除（影响我们）：**

| 文件 | 状态 |
|---|---|
| `components/lua_modules/lua_module_display/src/display_hal.c` | 上游删除；**我们在这个文件上做了 301 行改写**（`dd348d73`）→ modify/delete 冲突 |
| `components/lua_modules/lua_module_lcd_touch/src/lua_module_lcd_touch.c` | 上游删除；我们改过 → modify/delete 冲突 |
| `.agents/spec/claw-skill-spec.md` | 上游删除；我们改过 → modify/delete 冲突 |
| `components/common/skill_builder/skills/skill_creator/references/write_lua.md` | 上游删除；我们改过 → modify/delete 冲突 |

> 注意：`lua_module_lvgl`、`lua_module_vision`、`cap_im_platform`、`cap_web_search`、`skill_builder` 这些目录**仍然存在**，只是内部 API/文件大改，不要按"被删"处理。

### 1.3 硬件抽象层（影响面最大）

| 提交 | 内容 |
|---|---|
| `1479b375` | 新增 `claw_hw_registry` 核心与测试 app |
| `1d3318c2` / `88e70cbe` | 统一资源所有权模型、简化 resource key 归属 |
| `f69c313d` / `55549d0a` | IMU、磁力计支持"板载或外接"两种来源 |
| `9ae8ed3e` | LEDC 归类为 driver |
| `14e22ce5` | SCI 改走受管 I2C 总线 |
| `2d5238ab` | 环境传感器恢复 BME690/DHT 访问 |
| `067a88aa` | board manager 延后处理被跳过的设备 |
| `9bc9208b` | **`board_manager` Lua API 收缩为纯查询** |

新的资源模型：

```c
// components/common/claw_hw_registry/include/claw_hw_registry.h
claw_hw_registry_claim(handle, &config, &claim_id);   // 独占或共享
claw_hw_registry_release(handle, claim_id);
claw_hw_registry_query(handle, resource, entries, cap, &count);
```

资源目录由 `components/common/app_claw/app_claw_hw_bridge.c` 在启动时从 board manager 的 peripheral groups **运行时生成**（不是 Python 预生成）。这意味着：

- 自定义设备（我们的 `xpt2046` / `scd41` / `qmi8658`）所挂的 **总线资源**（`i2c_master` / `spi_master`）依然会被注册，资源租约本身没有障碍；
- 但**设备级的芯片识别与地址**不再由资源表提供，而由各 Lua 模块自己读 board 配置；
- 所有上游传感器模块都改成 `lua_driver_i2c_ref_bus` + `cap_lua_hw_claim` 模式，例：
  `lua_module_environmental_sensor.c:304-307`。

`board_manager` 现存 Lua 接口只剩：

```
get_board_info, get_camera_paths
```

被移除的接口（我们仍在使用）：`init_device`、`deinit_device`、`get_device_handle`、`get_display_lcd_params`、`get_lcd_touch_handle`、音频参数、`PANEL_IF`/`PIXEL_FORMAT` 常量。

### 1.4 显示与 UI

| 提交 | 内容 |
|---|---|
| `473d5b63` | **替换旧 Lua display API**：`display.init(panel, io, w, h, ...)` → `display.open()` 返回 screen 对象 |
| `4f388612` | 触摸集中到 display service |
| `e7532b71` / `7fc983be` | 渲染与传输优化、帧指标 |
| `0a8395dd` | 帧缓冲数量可配 |
| `0fd6d972` | 退出手势移到 session 层 |
| `5676ba82` | RGB565 字节交换选项 |
| `544af15a` | LVGL 默认字体控制 |
| `69e61ecc` | DFN 度量换算 |
| `7dd01ec4` | LVGL 增加 image / interaction API |

新 API 形态（`lua_module_display/README.md`）：

```lua
local screen <close> = display.open()
screen:begin({ clear = "#101820" })
screen:fill_round_rect(...)
screen:text(24, 30, "hello", { font_size = 24 })
screen:present()
```

我们的 `display_hal.c` 改动（`dd348d73`）解决的是：**单次 SPI 传输超过 16 KB 时面板不返回 color-done**，导致桥接层永久等待。做法是按 `DISPLAY_HAL_SUBMIT_BAND_BYTES` 分带提交，并把 swap 缓冲区按带复用。

上游新路径不再经过 `display_hal`，而是 `esp_lv_adapter_dummy_draw_blit`（`display_service.c:1359-1383`）。上游代码里**没有** color-done / bounce buffer 相关处理（全仓搜索无命中）。所以：

- 不一定需要我们的分带补丁；
- 但必须用 S3 测试板实测大区域刷新（全屏 `present()`）后再下结论，**不能假设问题已消失**。

### 1.5 应用与启动器

| 提交 | 内容 |
|---|---|
| `0b5103f4` | 启动器与 skills 解耦 |
| `35f29460` | 内置应用包 |
| `3c8942c2` | 用 app store 替换内置 demo |
| `77aa3844` | skill 支持应用包下载 |
| `4cdddc90` | lab skills 移到 app 存储 |

关键事实（对上一轮结论的修正）：

- 我们的 `system_ui/launcher.c:384` 用 `claw_skill_foreach_catalog_entry`，**列的是 skills**；上游 `launcher.c:386` 改用 `app_registry_foreach`。
- 上游应用包结构是 `apps/<id>/{launcher.json, scripts/, assets/}`，`scripts/` 在应用目录内部，不是全局目录。
- `launcher.json` 的硬规则只有三条：`id` 必须等于目录名；`entry` 必须存在且是包内相对 `.lua`；清单 ≤ 4096 字节。**`entry` 写 `main.lua` 合法**，`scripts/` 只是约定。
- 我们的 `app_layout` 是**给 agent 自研程序用的开发约定**，与系统启动界面无关；合并后两套可并存，只是我们的应用不会出现在新启动器里（可发现性缺口，不是断链）。按 v2 的产品层原则，建议最终切到 `launcher.json` 单清单（见价值分析 §7.2）。
- 上游把应用开发说明从系统提示词里移走了：`app_claw.c` 的系统提示词已无 `apps/` 描述，改由 `cap_app_mgr/skills/app_creator/references/app-package-spec.md` 承载；我们仍在 `app_claw.c:122` 保留该段。

### 1.6 会话、记忆、消息

| 提交 | 内容 |
|---|---|
| `f2d6baad` | 统一会话消息投递：`claw_core_post_message()` + receipt |
| `6199125c` | 只读会话历史 API |
| `9354591b` | web chat 持久会话 |
| `a2559a70` / `3a36309b` | 启动期不做压缩、不做持久化 |
| `a6b8658b` / `5e0318e6` | 技能注册表显式 reload、扫描只读元数据 |

`post_message` 的语义（`claw_core.h:214-226`）：

> 目标会话正被一个"接受 inbox"的 run 持有时，消息进入该 run 的 inbox；否则新起一个 run。调用方因此不再需要"submit 还是 append"的二选一，也不需要查 active 状态。

返回 `claw_core_message_receipt_t{ disposition, run_id }`。

### 1.7 LLM 与网络

| 提交 | 内容 |
|---|---|
| `1e048db6` | OpenAI Responses 后端 |
| `89f2209d` | trial 鉴权 |
| `b387bcdf` | Bocha 搜索 + 显式 provider 选择 |
| `134b5780` | 改用 Espressif Lua 组件 |
| `9448f4ed` / `428d349e` | 文件系统无关的空间查询 |
| `15d02558` | `storage.remove_tree()` 安全删目录 |

### 1.8 其他

event router 规则恢复与持久化（`8499e708`、`1210d0be`、`bf24b3bd`）、camera buffer 可配 + 设备发现、vision core 检测器、audio hub/playback、ADC 可选通道、MCPWM 生命周期修复、IR 用 RMT copy encoder、CLI 精简、仿真器支持 display 应用、前端 WebIM/i18n。

---

## 2. 本地 63 提交能力地图

| 能力 | 代表提交 | 主要落点 | 上游是否覆盖 | 合并处理 |
|---|---|---|---|---|
| 精确编辑 / 内容搜索 / 自动版本化 | `a07f23e4` `063a6078` `070c8e49` | `cap_files.c` | **否**（上游仍只有 read/write/delete/copy/move/list） | 保留，无冲突 |
| `read_file` 分页（offset/limit） | `b369ad72` | `cap_files.c:2037` | 否 | 保留 |
| 传输层重试 + 退避 | `3906332a` | `claw_core_agent_loop.c`、transport | 部分（上游有 token 解析/body 清洗） | 两侧合并 |
| opencode-go provider | 分散在 6 个 LLM 文件 | `claw_llm_*` | 否 | 保留（`(added)` 标记齐全） |
| 动态窗口 W + 分档常量 | `24dc5af4` `eaa04e96` `d98ed111` | `claw_tier.h`、`claw_core_context.c` | 否 | 保留 |
| 会话投影 / 结构化压缩 | `9e3a63e0` `a19d4cf4` | `claw_memory_session.c` | 否（上游新增 history lock + 读 API） | 保留 + 吸收读 API |
| pinned core / `pin_note` | `d98ed111` `2afc59d9` | `claw_memory_session.c` | 否 | 保留 |
| 序列化闸门 / 降峰 / 轮内窗口 | `b1897259` `3b6ef312` `bff9d5b0` | `claw_core_context.c` | 否 | 保留 |
| 工具结果落盘 `.toolout/` | `6a587c8b` `acc58066` | `claw_core_context.c`、`cap_files.c` | 否 | 保留 |
| 子 agent 类型 = 能力边界 | `c74306b2` | `claw_cap.c:90-120` | 否（上游提供 `llm_authorizer` 钩子） | 迁到 authorizer |
| 派发准入 / reaper / 报告 / BLOCKED | `c65e11af` `19ebf1c3` `5f5e5120` `a281ea40` | `claw_agent_mgr.c`、`claw_core.h` | 否 | 保留 |
| 子 agent 可靠通知 | `cbca5e3b` | `claw_agent_mgr.c` | **部分**（上游 `post_message` + receipt 更彻底） | 改用上游机制 |
| 长跑作业 `wait_ms` | `7b729a13` | `cap_lua_async.c:633` | 否 | 保留 |
| 板级适配（S3 测试板 / 立创实战派） | `327d7e92` `f52b7387` `4e5dd5a2` | `boards/kayzh/*`、`generate_board_skill.py` | 部分（上游改为打包 README） | 板级优先保留，生成器合并 |
| 板级硬件事实生成 | `d194b30b` | 手写 YAML → skill 文本 | 上游无同等能力 | 保留 |
| SD 容量 >2 GiB 修复 | `5efc5676` | `lua_module_storage.c` | **否**（上游仍整型 push） | 保留，并在新 space provider 上复验 |
| SCD41 修复 | `3901d6eb` | `lua_module_scd41_sensor.c` | 上游重构了环境传感器 | 迁移 |
| QMI8658 backend | 随板级提交 | `lua_module_imu/src/qmi8658/` | 上游重构了 IMU backend 表 | 迁移 |
| 显示分带修复 | `dd348d73` | `display_hal.c` | 上游删除该文件 | 重做 / 实测 |
| 前端 agents 状态灯 + 栈溢出修复 | `34de3196` `89936e1e` | `frontend_source` | web 前端被上游大改 | 重做 |
| 系统级 skills（app_layout / dev_workflow / session_batch_cleaner / skill_slimming） | `ebea5c2b` `6154f627` | `app_claw/skills/` | 上游把 skills 移到各 cap 组件 | 保留运行时路径不变 |

---

## 3. 合并干跑结果

### 3.1 总体

```
git merge-tree --write-tree HEAD upstream/master
→ 合并树 ebd23d04（对象库内，未落工作区）
```

| 类别 | 数量 | 说明 |
|---|---|---|
| 文本冲突 | 25 文件 / 44 处冲突块 | 见 3.2（另加 1 个二进制冲突） |
| modify/delete | 4 | 见 3.3 |
| 二进制冲突 | 1 | `frontend_source/dist/index.html.gz` |
| 自动合并但语义需复核 | 27 处（估） | 改名、删除的间接引用 |

### 3.2 冲突逐项

| 文件 | 冲突块 | 性质 | 处理建议 |
|---|---:|---|---|
| `lua_module_display/src/lua_module_display.c` | 5 | API 整体换代 + 我们的单 owner 保护 | 采用上游，owner 保护逻辑在新 API 上重做 |
| `app_lua_modules.c` | 4 | `lcd_touch` 删除、`LEDC`→`DRIVER_LEDC` 改名、模块清单 | 采用上游，删除 touch 注册 |
| `app_claw.c` | 4 | 迭代上限（tier vs Kconfig）、provider 注册、应用目录 | 逐块取：tier 保留，Kconfig 接入 |
| `claw_cap.c` | 4 | 授权状态枚举、`ctx` 取代 `session_id`、agent_type 拒用表 | 拒用表迁入 authorizer |
| `generate_board_skill.py` | 3 | 我们的 authored YAML 芯片信息 vs 上游 README 打包 | 两者合并：YAML 供硬件事实，README 供文档 |
| `claw_memory_session.c` | 2 | 我们的投影/pinned core vs 上游 history lock/读 API | 保留我们实现，插在上游锁与 API 之后 |
| `claw_core_agent_loop.c` | 2 | 我们的重试策略/blocked vs 上游 run inbox | 保留重试，改用上游 ingress |
| `app_claw/idf_component.yml` | 2 | `claw_tier`、`lcd_touch` 依赖 | 保留 tier，去掉 lcd_touch |
| `claw_agent_mgr.c` | 1 | 我们的 report/reaper/notify vs 上游会话 API | 保留我们的记录字段 |
| `claw_core.h` | 1 | `agent_notice`/`mark_blocked`/`submit` vs `post_message`/receipt | 保留我们的扩展，补上游新 API |
| `claw_core_internal.h` | 1 | tier 常量 vs 固定 4/10 | 保留 tier |
| `claw_skill.c` | 1 | 我们的 skills-list provider（含 start-only flag） | 上游移到 `app_claw.c`，flag 必须补回 |
| `claw_memory_utils.c` | 1 | opencode_go 的 message format 映射 | 保留 |
| `claw_llm_http_transport.c` | 1 | 我们的 body 所有权 vs 上游 token 清洗 | 两侧合并 |
| `openai_compatible.{c,h}` | 1+1 | opencode_go 扩展 | 保留 |
| `Kconfig`（app_claw） | 1 | 上游新增 `MAX_TOOL_ITERATIONS` | 引入，默认值改 |
| `Kconfig`（imu） | 1 | 我们的 QMI8658 选项 vs 上游芯片清单 | 迁移后合并 |
| `lua_module_imu/README.md` | 1 | 文档 | 采用上游，补 QMI8658 |
| `lua_module_storage/README.md` | 1 | 我们的返回值说明 | 保留 |
| `skill_creator/SKILL.md` | 1 | 上游删除 `write_lua.md` 引用 | 按上游，保留本地副本需登记 |
| `cap_boards/CMakeLists.txt` | 1 | 生成参数变化 | 采用上游 + 我们的参数 |
| `WebImPage.tsx` | 2 | 我们的 agent 状态灯 vs 上游持久会话 UI | 重做 |
| `i18n/en.ts`、`zh-cn.ts` | 各 1 | 文案 | 合并 |
| `dist/index.html.gz` | 二进制 | 构建产物 | 合并后重新 `pnpm build` 生成 |

### 3.3 modify/delete 决策

| 文件 | 上游 | 我们 | 建议 |
|---|---|---|---|
| `display_hal.c` | 删除 | 301 行改写（分带提交） | **接受删除**；把分带思路在新 present 路径复验，必要时在上游 flush 处实现 |
| `lua_module_lcd_touch.c` | 删除 | 小改 | **接受删除**；触摸改由 display service 提供 |
| `.agents/spec/claw-skill-spec.md` | 删除 | 小改 | **保留本地**（我们的文档体系引用它），在 `VERSIONS.md` 记录"上游已删、本地保留" |
| `skill_creator/references/write_lua.md` | 删除 | 小改 | 按上游删除，或保留并在 `SKILL.md` 保留引用 |

### 3.4 自动合并但需人工复核的间接断点

| 位置 | 原因 |
|---|---|
| `app_claw.c:593` 的 `app_claw_get_agents_json`（本地 web UI 的 agent 状态灯数据源） | 依赖 `claw_agent_mgr_agent_snapshot`，上游已删除该 API |
| `cap_skill_mgr.c` 四处调用 | `render_catalog_json` / `read_document` / `get_catalog_entry` 上游已删，需整体换用上游实现 |
| `claw_core_submit` 调用点（`claw_agent_mgr.c` ×2、测试 ×5） | 上游改名 `post_message` / `start_run` |
| `claw_agent_mgr_submit_root*`（CLI、event router、测试） | 上游改名 `post_root_message` / `start_root_run_text` |
| `app_claw.c:934` provider 注册 | 上游 provider 定义移到 `app_claw.c:169`，flag 丢失 |
| `lua_module_storage` space 查询 | 新 provider 下仍整型 push，>2 GiB 截断回归 |
| 所有引用 `board_manager.get_device_handle` 的 Lua/文档 | 接口已移除（仓库内 51 处引用，含文档/测试） |

---

## 4. 合并后必须适配的清单

> 本节按"不改就坏"的严重度排序；落地批次与优先级见价值分析 v2 §8。

### P0 — 不改就编译不过或功能不可用

1. **显示 Lua API 换代**
   - 受影响引用量：`display.init` 81 处、`get_display_lcd_params` 73 处（含 `lua_module_builder` 生成的文档/测试、`lua_module_lvgl`、`lua_module_vision`）。
   - 动作：把我们的 Lua 示例、板级 README、`builtin_lua_modules` 文档生成源全部切到 `display.open()` 形态；删除对 `panel_if/pixel_format` 的依赖。

2. **触摸 handle 形状**
   - 上游 `display_service.c:624` 把 board manager 返回的句柄直接 cast 成 `dev_lcd_touch_handles_t*`，取 `.touch_handle`。
   - 我们 `kayzh/esp32_S3_test_board/setup_device.c:621-686` 返回的是裸 `esp_lcd_touch_handle_t`。
   - 动作：自定义设备改为返回"首成员是 `dev_lcd_touch_handles_t`"的结构体（参考 `lilygo_t_display_p4_v1/setup_device.c:94`）；同时确认 board manager 注册名仍是 `ESP_BOARD_DEVICE_NAME_LCD_TOUCH`。
   - 立创标准板（FT5X06，i2c）走上游标准路径，不需要自定义。

3. **SCD41 资源获取方式**
   - 现状：`lua_module_scd41_sensor.c:409` 用 `esp_board_periph_ref_handle` 直接拿总线，绕过租约。
   - 上游模式：`lua_driver_i2c_ref_bus(...)` + `cap_lua_hw_claim(L, &ud->lease, "lua/<module>", keys, 1)`，关闭时 `unref`。
   - 不改的后果：能编译能跑，但与新资源仲裁脱节（同总线被其他模块独占 claim 时行为不确定）。

4. **核心 API 收口**
   - `claw_core_submit` → `claw_core_post_message`（receipt）或 `claw_core_start_run`；
   - `claw_agent_mgr_submit_root` → `post_root_message`；`submit_root_text` → `start_root_run_text`；
   - 保留我们的 `claw_core_publish_agent_notice` / `claw_core_mark_blocked`（上游没有）。

5. **agent 状态接口**
   - 上游删了 `claw_agent_mgr_agent_snapshot`（`app_claw.c:593` 的 `app_claw_get_agents_json` 在调它，供本地 web UI 状态灯使用）。二选一：
     a. 把我们的 snapshot 实现挂回新 manager 结构（保留现有 HTTP 行为）；
     b. 改用 `claw_agent_mgr_list_agents`（需要 `cap_call_context_t`，HTTP 侧构造较别扭）。
   - 建议 a，改动最小。

6. **技能查询 API**
   - `claw_skill_read_skills_list` / `render_catalog_json` / `get_catalog_entry` / `read_document` 上游删除，调用点集中在 `cap_skill_mgr.c`。
   - 动作：整体采用上游 `cap_skill_mgr.c`，我们的调用随之消失。

### P1 — 能跑但行为退化

1. **skills provider 漏 flag**
   上游 `app_claw.c:169` 的 `s_app_claw_skills_list_provider` 没有 `.flags = CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY`（结构体字段在 `claw_core.h:133`）。合并时补回，否则技能清单每轮重建。

2. **存储容量截断回归**
   上游改用 `claw_paths_get_space`，但 `get_free_space` 仍用整型 push。保留我们的 `lua_pushnumber` 修复（`lua_module_storage.c:293`）。

3. **迭代上限**
   上游新增 `CONFIG_APP_CLAW_MAX_TOOL_ITERATIONS`（默认 64）。我们是 per-tier：P4/S31=240、S3=100（`claw_tier.h:87/149/212`）。建议 Kconfig 默认值按 tier 推导，不要退回 64。

4. **板级音频**
   上游 `lua_module_audio` 改走 `audio_hub`，`audio_hub_start` 只在 `CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT` 下工作。我们的 UAC 自定义设备需要确认在新链路下仍可播放（当前失败只是 `ESP_LOGW`，不会崩，但会静默无声）。

5. **IMU / 磁力计**
   我们的 `board_devices.yaml` 用 `type: custom` + `chip: bmi270`，与上游示例一致，声明形状兼容；但 backend 的 bus 类型从 `i2c_bus_*` 换成原生 `i2c_master_*`，且每个芯片的符号从 `lua_imu_backend` 改为 `lua_imu_backend_<chip>`。QMI8658 需按新表头重写适配。

6. **`generate_board_skill.py` 输出合并**
   我们注入 authored YAML 的 `chip`/`config`（芯片型号、I2C 地址、几何/时序），上游注入 board README 打包。两者不冲突，应同时保留；注意"每轮重发"的体积预算（我们的 `CONFIG_VALUE_KEYS` 白名单策略要保留）。

7. **`board_manager` 文档与示例**
   仓库内 73 处 `get_display_lcd_params`、51 处 `get_device_handle`、30 处 `get_lcd_touch_handle` 引用需清理（含 `docs/`、板级 README、`builtin_lua_modules` 测试）。

### P2 — 文档与工程约定

1. `.agents/spec/claw-skill-spec.md`：保留本地版本，登记"上游已删"。
2. `AGENTS.md` 中"运行时路径规则"仍有效，但 `lua_module_board_manager` 能力描述要改。
3. `VERSIONS.md` §2/§2.2/§3 按合并结果更新；`ESP_KAO.md` 的本地能力清单加"上游已覆盖项"标注。
4. `docs/` 双语页面（`reference-cap/lua-modules.mdx` 等）需同步 display/board_manager 变化。
5. 前端 `dist` 冲突：合并后 `cd frontend_source && pnpm build` 重新生成。

---

## 5. 验证清单

**构建**

```bash
. $IDF_PATH/export.sh
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1     # 或我们的 S3 测试板
idf.py build
```

至少覆盖：`kayzh/esp32_S3_test_board`、立创实战派板、P4（tier 上限路径）。

**真机**

| 项 | 判定 |
|---|---|
| 显示全屏刷新 | 无撕裂、无卡死（重点验证 color-done 问题是否复现） |
| 触摸 | XPT2046 坐标正确、与显示方向一致 |
| IMU / 磁力计 / SCD41 | 读数正常，且与 display 共用 SPI/I2C 时不互相破坏 |
| SD 卡容量 | >2 GiB 时数值正确 |
| 长会话 | 投影触发正常，pinned core 不被压缩 |
| 子 agent | 派发、通知、reaper、报告落盘 |
| 音频（如启用） | UAC 播放不静默失败 |

**前端**

```bash
cd application/edge_agent/components/http_server/frontend_source
pnpm build && pnpm typecheck
```

**回归指标**（与 M1/M2 基线对比）

- 首轮上下文 token、序列化峰值、单轮工具迭代数、首 token 延迟、启动耗时、最小空闲堆。

---

## 6. 风险登记

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| 显示换代后 SPI 大传输卡死复现 | 中 | 高（黑屏/卡死） | 真机全屏刷新测试；必要时把分带逻辑移植到上游 flush 路径 |
| 触摸自定义设备未适配 | 高 | 中（触摸失效） | P0 第 2 项，先改 S3 板 |
| 资源租约冲突（SCD41 未迁移） | 中 | 中（偶发 I2C 失败） | 迁移到 `cap_lua_hw_claim` |
| 重放 `(added)` 时漏改 | 中 | 高（静默丢能力） | 以 163 处标记为清单逐条核对；每阶段跑回归 |
| 上游自身小回归被带入 | 中 | 低-中 | 合并时主动修正（provider flag、storage push） |
| 前端 dist 冲突处理不当 | 低 | 低 | 重新构建，不手工合并 |
| 板级音频静默失效 | 中 | 中 | 板级冒烟测试加音频用例 |

---

## 附录 A：冲突文件完整清单（文本）

> `claw_llm_backend_openai_compatible.{c,h}` 是 2 个文件；`dist/index.html.gz` 为二进制冲突，未在此列。合计 25 个文本文件 / 44 处冲突块。

```
components/lua_modules/lua_module_display/src/lua_module_display.c            5
components/common/app_claw/app_lua_modules.c                                  4
components/common/app_claw/app_claw.c                                         4
components/claw_modules/claw_cap/src/claw_cap.c                               4
components/claw_capabilities/cap_boards/tools/generate_board_skill.py         3
components/common/app_claw/idf_component.yml                                  2
components/claw_modules/claw_memory/src/claw_memory_session.c                 2
components/claw_modules/claw_core/src/claw_core_agent_loop.c                  2
application/.../frontend_source/src/pages/WebImPage.tsx                        2
components/lua_modules/lua_module_storage/README.md                           1
components/lua_modules/lua_module_imu/README.md                               1
components/lua_modules/lua_module_imu/Kconfig                                 1
components/common/skill_builder/skills/skill_creator/SKILL.md                 1
components/common/app_claw/Kconfig                                            1
components/claw_modules/claw_skill/src/claw_skill.c                           1
components/claw_modules/claw_memory/src/claw_memory_utils.c                   1
components/claw_modules/claw_manager/src/claw_agent_mgr.c                     1
components/claw_modules/claw_core/src/llm/claw_llm_http_transport.c           1
.../llm/backends/claw_llm_backend_openai_compatible.{c,h}                     1+1
components/claw_modules/claw_core/src/claw_core_internal.h                    1
components/claw_modules/claw_core/include/claw_core.h                         1
components/claw_capabilities/cap_boards/CMakeLists.txt                        1
application/.../frontend_source/src/i18n/{en,zh-cn}.ts                        1+1
```

modify/delete：

```
.agents/spec/claw-skill-spec.md
components/common/skill_builder/skills/skill_creator/references/write_lua.md
components/lua_modules/lua_module_display/src/display_hal.c
components/lua_modules/lua_module_lcd_touch/src/lua_module_lcd_touch.c
```

## 附录 B：分析命令

```bash
git fetch upstream
git log --oneline fb7b2481..upstream/master
git diff --stat fb7b2481 upstream/master
T=$(git merge-tree --write-tree HEAD upstream/master | head -1)
git ls-tree -r --name-only $T | while read f; do
  c=$(git show "$T:$f" | grep -c '^<<<<<<< '); [ "$c" -gt 0 ] && echo "$c $f"
done | sort -rn
git diff fb7b2481 HEAD | grep -c '^+.*(added)'   # 163
```
