# 合并批次 1 小计划：合并机制与核心运行时

- 日期：2026-10-20
- 状态：**已确认**（2026-10-20）
- 上游：`upstream/master` @ `345073de`
- 基线：`feat/agent-rework` @ `6b7bcc2d`

## 决策记录

| 决策 | 结论 |
|---|---|
| 合并机制 | **方案 A**：一次 `merge` + 分阶段验收（保留完整上游历史） |
| 显示/启动器 | **暂缓到第 4 批**，第 1–3 批整体保留本地版本 |
| 上游补丁 | 全量合并并通过测试前，不提交任何补丁建议 |

## 0. 本阶段的硬约束

1. **在全量合并完成并通过测试前，不向上游提交任何补丁建议**（issue / PR / patch 都不提）。
2. 每一批开工前先写小计划（本文即第 1 批）。
3. 遇到重要技术路线分叉，先向项目负责人确认。
4. 需要目视确认（屏幕显示效果、指示灯、接线）或我无法完成的硬件操作（按键、拔插、测量），向项目负责人求助。

## 1. 环境确认（已完成）

| 项 | 结果 |
|---|---|
| 串口 | `/dev/cu.usbserial-2120` |
| 芯片 | ESP32-S3 (QFN56) rev v0.2，8 MB PSRAM，MAC `84:c7:bb:70:5b:58` |
| ESP-IDF | `~/.espressif/v5.5.5/esp-idf`，`ESP-IDF v5.5.5` |
| 仓库状态 | `feat/agent-rework` 干净（仅两份未跟踪的分析文档） |
| 当前构建配置 | **`sdkconfig` / `build` 仍是 `esp32p4`**（上次烧的是 P4 板），必须先清理再重选 S3 板 |

> 注 1：ESP-IDF 由 **EIM** 安装，入口是 `~/.espressif/tools/activate_idf_v5.5.5.sh`。后续一律用它激活，不再手工 `export.sh` 或覆盖 `IDF_PYTHON_ENV_PATH`。
>
> 注 2：**构建前必须 `idf.py fullclean` 并删掉旧的 `sdkconfig`**，否则 board manager 仍会沿用 P4 的 target 与板级配置。切换板子后重新生成配置，不能复用 P4 的产物。

## 2. 本批目标

把上游的**核心运行时架构**接进来，同时保住我们的 coding 优化：

| 领域 | 采纳上游 | 保留我们 |
|---|---|---|
| 消息投递 | `post_message` + receipt | reaper、报告落盘、`BLOCKED`、准入 |
| 授权 | `llm_authorizer` | recon/verify 策略（改为独立 authorizer） |
| 记忆 | history 读 API + `history_lock` | 投影、pinned core |
| 技能 | 注册表重构、元数据扫描、`cap_skill_mgr` | DATA 优先顺序 |
| 存储 | space provider、`remove_tree` | >2 GiB 数字返回修复 |
| 启动 | 启动期不压缩/不持久化、event router 恢复 | — |
| 配置 | `APP_CLAW_MAX_TOOL_ITERATIONS` | tier 默认值 |

**验收**：能构建、能开机、能跑一次真实 agent 任务与一次子 agent 任务。

## 3. 技术路线分叉（需确认）

### 问题

"按批次合并"在 git 层面**不可行**：

- 上游 134 个提交按功能交错（merge commit 34 个），同一文件被多个提交反复修改；
- 冲突集中在同几个核心文件（`claw_core_agent_loop.c`、`claw_memory_session.c`、`claw_agent_mgr.c`、`app_claw.c`），无法按批次切开；
- 核心 API 互相依赖：`post_message` 改了 ingress，`history_lock` 改了 memory，`authorizer` 改了 cap，单独摘一个都编译不过。

### 三个方案

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A（推荐）一次 merge + 分阶段验收** | 在 `merge/upstream-345073de` 上 merge 一次，冲突按"先核心、后外围"的顺序解决；每完成一个领域就 build + 真机验证，形成分阶段提交 | 与上游历史一致；不会漏改动；可分阶段回滚提交 | 第一次 merge 冲突量大（25 文件 / 44 块） |
| B 按文件挑选上游改动 | `git checkout upstream/master -- <path>` 逐块拼装，不产生 merge commit | 看似可控 | 极易漏依赖；无上游历史；后续合并更痛苦 |
| C 全量 merge 后一次适配 | 一次解决所有冲突（含显示/启动器/传感器），再统一测试 | 一步到位 | 风险集中，出问题无法定位领域 |

**建议 A。** 批次 1 与批次 2 会合并成同一次 merge 的"核心部分"，因为它们改的是同一批文件；批次 3（硬件）、批次 4（显示/启动器）作为后续独立提交与新分支。

### 需要确认的点

1. 是否接受 A（一次 merge + 分阶段验收），而不是严格"每批独立 merge"？
2. 是否接受在第 1–3 批期间，**显示/启动器子系统整体保留本地版本**，直到第 4 批一次性切换？（这是唯一能让前几批可开机、可测试的办法；代价是分叉面在这期间仍然存在。）

## 4. 暂缓迁移清单（候选，按构建结果最终确定）

原则：**只有"不暂缓就无法构建或无法开机"时才暂缓**；每一项都要登记，第 4 批必须清零。

| 子系统 | 暂缓理由 | 切换批次 |
|---|---|---|
| `lua_module_display`（含 `display_hal.c`） | Lua API 换代，引用面 700+ 处 | 4 |
| `lua_module_lcd_touch` | 上游删除，触摸并入 display service | 4 |
| `system_ui` + `display_service` | 与上面两项同属一套会话/触摸 API；先换会导致旧 display 模块编译不过 | 4 |
| `lua_module_board_manager` | 上游收缩为纯查询，我们的 Lua 脚本仍用旧接口 | 4 |
| `app_registry` + `cap_app_mgr` | 依赖 launcher/应用包格式，单独引入会暴露半可用工具 | 4 |
| `lua_module_imu` 的 QMI8658 backend | 上游 IMU backend 表换了总线类型与符号名 | 3 |
| `lua_module_scd41_sensor` | 需迁到 `lua_driver_i2c` + `cap_lua_hw_claim` | 3 |

> 上游传感器模块（IMU / 磁力计 / 环境 / SCI）本体可以随 merge 进来；暂缓的只是"我们自己的实现"与依赖它们的注册项。

## 5. 执行步骤

1. **建分支**：`git switch -c merge/upstream-345073de`
2. **合并**：`git merge upstream/master`（保留完整上游历史，不 squash）
3. **冲突解决顺序**（先核心后外围）：
   1. `claw_core.h` / `claw_core_internal.h` / `claw_core_agent_loop.c`
   2. `claw_memory_session.c` / `claw_memory_utils.c`
   3. `claw_agent_mgr.c` / `claw_agent_mgr.h` 引用点
   4. `claw_cap.c` + 新 authorizer 文件
   5. `claw_skill.c` / `app_claw.c` / `app_lua_modules.c` / `Kconfig` / `idf_component.yml`
   6. LLM transport / openai_compatible / memory_utils（opencode-go）
   7. `generate_board_skill.py` / `cap_boards` CMake
   8. 文档与 README
4. **暂缓项落地**：按 §4 恢复本地版本，并同步 `app_lua_modules.c` 注册与 `idf_component.yml` 依赖
5. **构建**（必须从 S3 干净配置开始，不能复用 P4 产物）：
   ```bash
   . ~/.espressif/tools/activate_idf_v5.5.5.sh
   cd application/edge_agent
   idf.py fullclean          # 清掉 esp32p4 的 build/
   mv -f sdkconfig sdkconfig.p4.bak   # 丢弃 P4 的生成配置（不删，留档）
   idf.py bmgr -c ./boards -b esp32_S3_test_board
   idf.py build
   ```
   > 如果 `bmgr` 没有自动生成 S3 的 `sdkconfig`，改用 `idf.py set-target esp32s3` 后再执行 `bmgr`；两条路都要确认 `build/CMakeCache.txt` 里 `IDF_TARGET=esp32s3`。
6. **烧录与冒烟**：`idf.py flash monitor`
7. **分阶段提交**：核心运行时一个提交；每个暂缓项登记为一个 TODO 提交信息（不实现）

## 6. 冲突处理对照（本批相关）

| 文件 | 处理原则 |
|---|---|
| `claw_core.h` | 采用上游 `post_message`/receipt；保留 `publish_agent_notice`、`mark_blocked` |
| `claw_core_agent_loop.c` | 采用上游 ingress；保留我们的重试策略与 blocked 状态 |
| `claw_memory_session.c` | 采用上游 history lock/读 API；投影与 pinned core 保留在其写路径 |
| `claw_agent_mgr.c` | 采用上游会话 API 命名；保留 report/reaper/notify 字段与流程 |
| `claw_cap.c` | 采用 `ctx` 与 `POLICY_DENIED`；agent_type 拒用表迁出为 authorizer |
| `claw_skill.c` / `app_claw.c` | 采用上游 provider 定义；补 `REQUEST_START_ONLY`；保留 DATA 优先 |
| `app_lua_modules.c` | 采用上游模块表；暂缓项按 §4 加回注册 |
| `claw_memory_utils.c` | 保留 opencode-go 的 message format 映射 |
| `claw_llm_http_transport.c` / `openai_compatible.*` | 两侧合并：上游 body 清洗 + 我们的重试/opencode-go |
| `generate_board_skill.py` | 同时保留 authored YAML 芯片信息与上游 README 打包 |
| `Kconfig` | 接入上游迭代上限；默认值按 tier |
| `idf_component.yml` | 保留 `claw_tier`；暂缓项依赖按 §4 调整 |

## 7. 验收标准

| 项 | 通过条件 |
|---|---|
| 构建 | `idf.py build` 无错误、无新增 warning 阻断 |
| 开机 | 正常启动，无 panic / watchdog |
| 网络 | WiFi 连接正常 |
| 显示 | 保持本地实现，画面与合并前一致 |
| 触摸 | 保持本地实现，坐标正确 |
| 会话 | 一次多轮对话；投影按触发条件执行 |
| 子 agent | 派发成功、完成通知到达、报告落盘 |
| 容量 | 序列化不 OOM；窗口值与 tier 一致 |
| 存储 | SD 容量读数正确 |

## 8. 回滚

- 所有工作只在 `merge/upstream-345073de` 上进行，`feat/agent-rework` 保持不动；
- 任一步骤失败：`git merge --abort` 或 `git reset --hard` 到分支起点；
- 设备侧保留一份合并前的固件 bin，便于 A/B。

## 9. 风险

| 风险 | 应对 |
|---|---|
| 核心语义变化导致子 agent 通知丢失 | 先在桌面侧读代码确认 receipt 语义，再上板跑并发任务 |
| 暂缓 display 导致 `display_service` API 不兼容 | 构建时若冲突，扩大暂缓范围到整个显示子系统，记录并告知 |
| 上游传感器模块与我们的板级声明不匹配 | 先构建；运行失败则把对应模块加入暂缓清单，第 3 批处理 |
| 首次构建耗时长 / 组件下载 | 复用主仓库 `managed_components`，必要时离线 |

## 10. 不在本批

- display / launcher 切换（第 4 批）
- SCD41 / QMI8658 迁移（第 3 批）
- web 前端对齐（第 4–5 批）
- 任何上游补丁提交
