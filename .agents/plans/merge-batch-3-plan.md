# 合并批次 3 小计划：硬件资源层对齐

- 日期：2026-10-21
- 前置：批次 1（`a2299b95`）、批次 2（`e716c823`）已完成并真机验证
- 状态：**代码完成，待真机验证**（板子 USB 串口中途消失，等待重新连接）

## 执行记录（2026-10-21）

| 项 | 结果 |
|---|---|
| SCD41 | 已改为 `lua_driver_i2c_ref_bus` + `cap_lua_hw_claim`，Lua API 不变 |
| IMU / 磁力计 / SCI | 已采用上游实现（板载 device / 外接 chip 两种构造） |
| QMI8658 | backend 已迁到新接口 + 原生 `i2c_master_transfer`，Kconfig/CMake/注册完成 |
| 兼容层 | **未添加**（按最新原则：应用适配系统） |
| 构建 | `esp32_S3_test_board` 与 `lichuang_dev_board` 均通过 |
| 待办 | `compass` app 改到新构造函数与校准语义；`hwmap`/传感器读数真机验证 |

**阻塞**：设备 USB 串口 `/dev/cu.usbserial-2120` 在批次中途消失，无法继续烧录与实测。恢复连接后继续。

## 1. 目标

把传感器从"各自拿 board 外设句柄"迁到上游的 **资源租约 + 受管总线** 模型，同时保持现有 Lua app 可用。

| 模块 | 现状 | 目标 |
|---|---|---|
| IMU（BMI270） | 我们的 backend 表 + `i2c_bus_*` | 上游 `imu.new()` + `lua_driver_i2c_ref_bus` + claim |
| IMU（QMI8658） | 我们的 backend | 保留 backend，改原生 `i2c_master_*` 接口 |
| 磁力计（BMM150） | 我们的实现 | 上游实现 + claim |
| SCD41 | `esp_board_periph_ref_handle` | `lua_driver_i2c_ref_bus` + `cap_lua_hw_claim` |
| board_manager Lua | 保留旧 API（暂缓） | 本批不动，第 4 批切纯查询 |

## 2. 应用适配原则（2026-10-21 修正）

**不做系统侧兼容层。** 旧 app 适配新系统，而不是系统迁就旧 app：

- 取消 `imu.new()` / `magnetometer.new()` 的旧调用兼容层；
- 采用上游构造函数语义（`{ device = ... }` / `{ chip = ..., bus = ... }`）；
- SD 卡上的 `compass` 等 app 在批次 3 内一次性改到新 API；
- 后续 `app_layout` / `dev_workflow` 文档同步新写法。

## 3. 具体步骤

1. **SCD41**
   - `esp_board_periph_ref_handle` → `lua_driver_i2c_ref_bus(L, -1, &port, &owner)`；
   - 打开前 `cap_lua_hw_claim(L, &lease, "lua/scd41", keys, count)`，关闭时 release + unref；
   - 保持 Lua API 与 `scd41_sensor` 名字不变。
2. **IMU**
   - 采用上游 `lua_module_imu`；
   - 把我们的 `qmi8658` backend 迁到上游 `lua_imu_backend_t`（原生 i2c_master）；
   - 在 Kconfig/CMake/注册表加 QMI8658；
   - 不改构造函数语义，旧 app 随批次适配。
3. **磁力计 / SCI**
   - 采用上游实现；
   - 校准持久化按上游语义（不自动保存），由 app 自己 `calibration_get/set`；
   - `compass` app 随批次改到新构造函数与校准语义。
4. **依赖与注册**
   - `app_claw/idf_component.yml` 增加 `lua_driver_i2c`；
   - 恢复 `app_lua_modules.c` 中传感器注册（IMU/磁力计/SCI/环境）。

## 4. 验收

| 项 | 通过条件 |
|---|---|
| 构建 | S3 测试板 + 立创板配置均通过 |
| hwmap | `i2c_bus:0` 被传感器以 lease 形式占用，owner 可读 |
| compass app | 旧 `imu.new()` / `magnetometer.new()` 仍能打开传感器并读数 |
| co2_monitor | SCD41 读数正常 |
| 冲突语义 | 同一传感器二次打开返回明确错误，而不是静默覆盖 |
| 回归 | 显示/桌面不受影响；WiFi/配置保留 |

## 5. 风险

| 风险 | 应对 |
|---|---|
| 上游构造函数严格，旧 app 打不开 | 直接改 app（本批内完成），不加系统兼容层 |
| 磁力计校准不再自动持久化 | 实测 compass 是否依赖；必要时把持久化作为独立层加回 |
| QMI8658 后端总线类型改写引入回归 | 立创板只做构建验证，S3 板无 QMI 不影响 |
| claim 粒度导致共享 I2C 冲突 | 用 `hwmap` 检查 claim 粒度；必要时改共享策略 |

## 6. 非目标

- 触摸 handle、display service（批次 4）。
- board_manager Lua API 收缩（批次 4）。
- 上游补丁提交（禁止）。
