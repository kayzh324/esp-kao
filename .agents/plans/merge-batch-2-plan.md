# 合并批次 2 小计划：运行时接缝收口

- 日期：2026-10-20
- 前置：批次 1（`a2299b95`）已合并、构建通过、真机启动正常
- 状态：待批次 1 验收（联网 + agent 实跑）后开工

## 1. 本批目标

把批次 1 留下的三个"兼容层"换成上游接缝，消除平行机制：

| # | 现状 | 目标 |
|---|---|---|
| 2.1 | `claw_core_submit` 兼容包装（= `start_run`） | 通知路径改用 `post_message` + receipt |
| 2.2 | `claw_cap.c` 内硬编码 `recon`/`verify` 拒用表 | 独立 `llm_authorizer` 策略实现 |
| 2.3 | 历史读 API 已并入，但投影分页语义未定义 | 明确"读 API 返回投影后视图"，写测试 |

## 2. 具体改动

### 2.1 通知走 post_message

- `claw_agent_mgr_deliver_to_parent()`：`claw_core_submit(...)` → `claw_core_post_message(..., &receipt)`。
- 保留 `(added)` 可靠投递：失败时仍写入 `pending_notify_text`，由 reaper 重试。
- `claw_core_submit` 保留给测试/兼容，调用点清零后标记 deprecated。
- 验收：子 agent 完成通知在父 agent 正在运行时进入其 inbox（`run_id` 与当前 run 一致），不再依赖中断。

### 2.2 agent_type 策略迁到 authorizer

- 新增 `cap_agent_mgr` 内独立文件：
  - `claw_cap_agent_policy_allow(group_id, descriptor, ctx, user_ctx)`
  - 逻辑即现有 `recon` / `verify` 拒用表（`recon` 只读；`verify` 不改文件）。
- 在 `app_capabilities.c` 注册：`claw_cap_set_llm_authorizer(claw_cap_agent_policy_allow, NULL)`。
- 删除 `claw_cap.c` 内的 `claw_cap_tool_denied_for_agent_type()` 与 `CLAW_CAP_AUTH_AGENT_TYPE_DENIED`。
- 验收：recon 看不到也调不到写工具；verify 不能改文件；root/`build` 不受限；`cap` 列表行为不变。

### 2.3 历史读 API + 投影

- 确认 `claw_memory_read_session_messages(before, limit)` 对"投影后视图"分页：
  投影摘要以 `session summary` 记录写在同一 history 文件里，读 API 天然包含它。
- 在 `claw_memory.h` 注释里写死语义；前端只读该 API。
- 验收：长会话投影后，`before/limit` 分页连续、无重复/丢记录；投影任务与读并发不崩。

## 3. 非目标

- 显示 / 启动器 / 传感器（批次 3、4）。
- `agent_snapshot` 重构（当前保留我们的实现，工作正常）。
- 上游补丁提交（禁止）。

## 4. 验收清单

| 项 | 通过条件 |
|---|---|
| 构建 | `idf.py build` 通过 |
| 通知 | 子 agent 完成时父 agent 收到 `<subagent_completed>`；失败可重投 |
| 授权 | recon/verify 边界与合并前一致 |
| 投影 | 长会话投影仍触发；历史分页正确 |
| 回归 | 首轮 token、窗口 W、工具迭代数与批次 1 持平 |

## 5. 回滚

- 在 `merge/upstream-345073de` 上单独提交；失败 `git revert <sha>`。
