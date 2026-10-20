# 合并批次 4 小计划：显示、启动器与应用模型

- 日期：2026-10-21
- 前置：批次 1（`a2299b95`）、批次 2（`e716c823`）、批次 3（`cf11de82`/`b2ecd232`）已完成
- 状态：**待开工**（需要目视验收；用户不在时只做准备）

## 1. 目标（一次性切换，不留旧路径）

| 层 | 现在 | 目标 |
|---|---|---|
| display | 我们的 `display.init(panel, io, w, h, piv, fmt)` + `display_hal.c` | 上游 `display.open()` + screen 对象 + display service |
| 触摸 | `lua_module_lcd_touch` + board_manager 句柄 | 并入 display service；板级返回 `dev_lcd_touch_handles_t` |
| 启动器 | 我们的 system_ui 列出技能（当前为空壳） | 上游 system_ui + `app_registry` + `launcher.json` |
| 应用清单 | `app.json` + 根 `main.lua` | `launcher.json` + `scripts/<id>.lua`（entry 可指向任意包内 .lua） |
| board_manager Lua | 旧 API（init_device/get_*_params） | 纯查询（get_board_info/get_camera_paths） |
| SD 应用 | 旧 display/board_manager API | 全部改到新 display API + launcher.json |

## 2. 代码改动范围

1. 恢复上游组件：
   - `components/common/display_service`、`components/common/system_ui`
   - `components/lua_modules/lua_module_display`、`lua_module_lcd_touch`（删除）
   - `lua_module_board_manager`（纯查询）
   - `app_registry`、`cap_app_mgr`、`fatfs_image/system/apps`
2. 冲突处理：
   - `lua_module_display.c`（5 处）：采用上游，单 owner 保护改到 screen 会话；
   - `display_hal.c`：接受删除；16 KB 分带问题在 `display_service` 的 SPI flush 路径复验；
   - `app_lua_modules.c`：删除 lcd_touch 注册；
   - `app_claw.c`：采用上游 app 启动回调；系统提示词改为 launcher.json 约定；
   - 板级 `setup_device.c`：XPT2046 返回首成员为 `dev_lcd_touch_handles_t` 的结构体。
3. 板级/系统 UI：
   - `system_ui_private.h` 主题宏按产品需要调整（可选）；
   - `app_registry` 目录与 SYSTEM 内置应用保持一致。
4. 文档与技能：
   - `app_layout` 改为 `launcher.json` 单清单；
   - `dev_workflow`、系统提示词、`AGENTS.md` 中的应用约定同步；
   - `lua_module_builder` 生成的 display 文档/测试随之更新。

## 3. SD 应用迁移（一次性）

需要改的应用（SD 卡 `/apps`）：

| 应用 | 依赖 |
|---|---|
| `desktop` | display 全屏/局部刷新、触摸、app 启动 |
| `compass` | display 三区域刷新、IMU/磁力计（已适配传感器） |
| `game_2048` | display 绘制与触摸 |
| `co2_monitor` | display 刷新、SCD41 |
| `pomodoro` | display 小区域刷新 |
| `touch_lab` | 触摸/显示调试 |
| `font_probe` | 字体/显示探针 |
| `i2c_monitor` / `usb_audio_test` | 以非显示逻辑为主，检查是否引用旧 API |
| `ip_info` | 无显示，补 `launcher.json` |

迁移方式：从设备下载 → 本地改 → 上传覆盖；每个应用补 `launcher.json`（`entry` 指向 `scripts/<id>.lua` 或保留 `main.lua`）。

## 4. 验收（必须目视）

| 项 | 通过条件 |
|---|---|
| 启动 | 开机进入上游 launcher，能看到系统应用 + 我们的应用 |
| desktop | 正常绘制、触摸、启动应用 |
| SPI 大刷新 | 全屏 `present()` 不卡死、不撕裂（重点复验 color-done 问题） |
| 触摸 | XPT2046 坐标正确；立创 FT5X06 正常 |
| 应用 | compass/2048/co2/pomodoro 正常打开与退出 |
| board_manager | 旧 API 调用被移除后无脚本报错 |
| 回归 | 批次 1–3 的能力（窗口、投影、子 agent、传感器）不受影响 |

## 5. 风险与回滚

| 风险 | 应对 |
|---|---|
| SPI 大传输卡死（我们实测过） | 先在 `display_service` flush 路径移植分带；上板全屏刷新 |
| 触摸句柄类型不匹配导致开机异常 | 先改板级结构体，再启用 display service 触摸 |
| 应用一次性迁移遗漏 | 迁移清单 + 启动器可见性检查 |
| 视觉问题无法远程确认 | 需要用户目视；不在时只做代码与构建，不烧录验收 |

回滚：第 4 批单独分支/提交；保留批次 3 的固件 bin。

## 6. 开工前需要确认

1. 是否接受"旧 display 路径完全删除"（不保留兼容层）？
2. 应用清单采用 `scripts/<id>.lua` 还是保留 `main.lua` 作为 entry？
3. 启动器首屏是否要放我们的 `desktop`，还是先用上游 launcher？

> 以上三点涉及产品形态；用户不在时暂不决定，先把代码迁移与构建准备好。
