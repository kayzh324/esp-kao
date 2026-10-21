# 版本记录

本仓库同时维护**两条版本线**，任何一次发布或上游合并都必须同时更新它们：

1. **上游基线台账** —— 我们的代码二开自 `espressif/esp-claw` 的哪个提交，之后又合入过哪些上游更新；
2. **ESP KAO 版本台账** —— 分支自己的版本号、tag 与内容。

---

## 1. 远端约定

| remote | 地址 | 用途 |
|---|---|---|
| `origin` | `git@github.com:kayzh324/esp-kao.git` | 本项目（ESP KAO） |
| `upstream` | `git@github.com:espressif/esp-claw.git` | 官方上游，只读拉取 |

日常开发推 `origin`；同步上游只 `git fetch upstream`，不直接向 `upstream` 推送。

---

## 2. 上游基线台账

| ESP KAO 版本 | 上游仓库 / 分支 | 上游提交 | 上游日期 | 分叉点（含板级提交） | 说明 |
|---|---|---|---|---|---|
| 1.0.0 | `espressif/esp-claw` `master` | `fb7b2481` | 2026-08-20 | `4e5dd5a2`（2026-10-14） | 分叉后未合入任何上游更新 |
| 1.1.0 | `espressif/esp-claw` `master` | `345073de` | 2026-10-19 | `6b7bcc2d`（2026-10-20） | 合入 134 个上游提交：display service / system_ui / app_registry / 硬件资源租约 / 消息投递与会话 API；保留 coding 优化 |

### 2.1 基线明细（1.0.0）

- **上游代码基线**：`fb7b2481`，`Merge branch 'fix/ci-build-jtag-p4x' into 'master'`（2026-08-20）。
- **最近上游 tag**：`v0.1.0` = `142bf3f3`（2026-06-12）。
  基线相对该 tag 的描述：`v0.1.0-104-gfb7b2481`。
- **本地板级/驱动提交**（位于分叉点之前，属于 1.0.0 的一部分）：
  - `681d7370` 合入上游 `fb7b2481`
  - `5efc5676` 修复 SD 卡容量读取截断
  - `3901d6eb` 修复 SCD41 Lua module
  - `327d7e92` 立创派开发板主体适配（麦克风未适配）
  - `dd348d73` display module 优化
  - `f52b7387` 立创实战派第二次适配
  - `4e5dd5a2` s3_test_board 硬件信息
- **二开分支起点**：`4e5dd5a2`（2026-10-14），其上为 `feat/agent-rework` 的 55 个改造提交。

### 2.2 尚未合入的上游更新

| 观察到的时间 | 上游分支头 | 距共同祖先 | 状态 |
|---|---|---|---|
| 2026-10-20 | `345073de`（2026-10-19） | 上游领先 134 个提交 | **已合入 2026-10-21**（分支 `merge/upstream-345073de`） |

> 这 134 个提交里可能包含与 [`ESP_KAO.md`](ESP_KAO.md) §2 所列问题重叠的修复。
> 合并时必须逐项复核：上游已解决的，采用上游实现并删掉我们的对应改动；上游未解决的，保留我们的实现。

---

## 3. ESP KAO 版本台账

| 版本 | tag | 日期 | 分支 | 主要变化 |
|---|---|---|---|---|
| 1.0.0 | `kao-v1.0.0` | 2026-10-20 | `feat/agent-rework` | 首个版本。M1 + M2 agent harness 改造：工具层（`edit_file` / `grep_files` / 分页读取）、上下文投影与分级逐出、序列化降峰与诊断、委派准入与存活检测、等待语义、网关重试、性能档位、硬件事实生成 |
| 1.1.0 | `kao-v1.1.0` | 2026-10-21 | `feat/agent-rework` | 合并上游 `345073de`（134 提交）。核心运行时对齐（`post_message` + receipt、`llm_authorizer`、历史读 API + `history_lock`、技能注册表、存储 space provider）；显示/启动器切到上游 display service + system_ui + app_registry；硬件层改用 `claw_hw_registry` 租约；保留 tier/窗口、投影、序列化闸门、工具层、子 agent 治理、`wait_ms`、传输重试、opencode-go。新增 USB HID 键鼠输入桥（触摸优先、自动识别、无输入提示）；恢复 Web 子 agent 工作灯、当前轮数 `R{n}` 与 Round brief；OpenCode Go 补真实 User-Agent 并确认 `x-opencode-session`；修复记忆 UTF-8 截断/`invalid_label`、`delete_file` 目录提示、异步 job `runtime_s`。S3 板触控回归通过；P4 板显示/SD/音频/WiFi/HID/自检工具链验收通过 |
| 1.1.1 | `kao-v1.1.1` | 2026-10-21 | `feat/agent-rework` | 修复旧固件遗留的空/损坏 `sessions/chat_map/*.json` 导致 `/api/webim/sessions` 返回 500 `storage_error`、Web 前端显示“暂时无法连接设备”的问题。`claw_session_mgr_visit()` 现在跳过单个非法 map 并继续构建 catalog。S3 烧录验证通过；P4 未重烧（同一源码，后续接板即可） |

### 3.1 1.0.0 内容锚点

- 功能基线：M2 功能提交 `22616fa4`（M1 = `b1897259`）
- 文档基线：M2 文档 `b369ad72` + 本文件与 `ESP_KAO.md`
- 完整设计：`.agents/plans/agent-harness-milestone.md`、`.agents/plans/agent-harness-milestone-m2.md`

### 3.2 版本号规则

- 格式 `X.Y.Z`，tag 前缀固定为 `kao-v`，避免与上游 `v*` 冲突。
- **major**：架构代际变化（与上一代不兼容的 harness / 基座重构）。
- **minor**：新增一个里程碑级能力（如一次新的优化批次、一次上游大版本合并）。
- **patch**：缺陷修复、档位校准、文档修正。
- 任何版本都必须在上表同时登记**上游基线**（§2）和**自身版本**（§3）。

---

## 4. 合并上游的操作流程

1. **拉取**：`git fetch upstream`
2. **预告**：在 §2.2 表里新增一行，记录准备合入的上游提交与状态"合并中"。
3. **合并**：在主分支上 `git merge upstream/master`（或指定提交）。
   保留完整上游历史，不使用 squash，不重写已发布提交。
4. **解决冲突**：
   - 项目代码中的我们的改动都带有 `(added)` 注释，用它定位；
   - 上游已实现同等能力的，采用上游实现并删除我们的对应代码；
   - 上游尚未实现的，保留我们的实现，并确认 `(added)` 注释仍在；
   - 板级目录（`application/edge_agent/boards/`）的本地适配优先保留。
5. **验证**：
   - `. $IDF_PATH/export.sh`（本项目仅支持 **ESP-IDF 5.5.5**）；
   - `cd application/edge_agent && idf.py bmgr -c ./boards -b <board> && idf.py build`；
   - 设备侧至少跑一次真实任务，确认没有 OOM、轮次没有退化。
6. **记账**：
   - 在 §2 表中把该版本的"上游提交"改为合并后的上游头，§2.2 状态改为"已合入 <日期>"；
   - 在 §3 表中新增一个 **minor** 版本，写明合并的上游范围与冲突处理摘要；
   - 打 tag：`kao-vX.Y.Z`；
   - 提交信息按 `merge upstream <short-sha>: <摘要>` 书写。
7. **推送**：`git push origin <branch> --follow-tags`。

---

## 5. 记录规范

- 所有日期写绝对日期（`YYYY-MM-DD`），不写"上周""昨天"。
- 上游提交与本地提交都写完整 40 位哈希的短形式（至少 8 位），并保证在仓库中可解析。
- 版本号只增不减；已发布 tag 不移动、不删除。
- 两条台账必须成对出现：任何 ESP KAO 版本都能回答"它基于哪个上游提交"，任何上游合并都能回答"它进入了哪个 ESP KAO 版本"。
