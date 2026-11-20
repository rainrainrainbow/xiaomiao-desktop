# XiaoMiao Desktop OS — 开发计划

## 项目概述
小喵教育掌机桌面操作系统，基于 ESP32-WROVER-B + ST7735 160x128 TFT + MicroSD + 6键手柄。

## 架构四层
1. **硬件层** — ST7735 LCD、6键手柄、SD卡、背光PWM、电池ADC
2. **系统层** — 任务管理器、应用启动器、页面导航、返回加载器
3. **UI层** — LVGL 9.5 桌面/设置/文件/后台管理/开机动画
4. **MicroPython层** — 解释器引擎、硬件模块注册、.app应用包

## 开发阶段

### ✅ 第一阶段：UI/UX 原型设计（已完成）
- [x] 分析参考仓库硬件规格
- [x] 创建 HTML 高保真原型（160x128 横屏）
- [x] 3×2 应用网格布局
- [x] 纯按键操作（↑↓←→ + A/B）
- [x] 后台任务管理（锁定/清理/全部清理）
- [x] 设置应用（WiFi、显示、关于）
- [x] 文件管理器

### ✅ 第二阶段：ESP-IDF 固件编码（已完成）
- [x] 项目结构：CMakeLists.txt、partitions.csv、sdkconfig
- [x] 硬件驱动：ST7735 LCD、6键扫描、SD卡
- [x] UI 系统：页面管理、标准布局、导航
- [x] 桌面：3×2 应用网格
- [x] 后台任务管理：锁定/解锁/清理
- [x] 设置应用：WiFi列表、显示设置、关于本机
- [x] 文件管理器：目录浏览、文件操作
- [x] 内置应用：贪吃蛇、计算器、音乐播放器（骨架）
- [x] 代码审核修复（6个问题）
- [x] MicroPython 运行时集成引擎
  - [x] 解释器生命周期（mp_init/mp_deinit）
  - [x] 脚本执行（mp_compile_and_execute + NLR异常处理）
  - [x] .app 应用包扫描与解析
  - [x] C↔Python 桥接（lv_async_call 线程安全）
  - [x] 硬件模块注册（9个子模块：lcd/keypad/buzzer/led/motor/battery/sd/lvgl/time）
  - [x] 代码审查修复（10个问题）

### ✅ 第三阶段：CI/CD 自动化编译（已完成）
- [x] GitHub Actions 工作流（build.yml）
- [x] 自动构建：ESP-IDF v5.3.2 + 组件管理器
- [x] 合并二进制生成（merged.bin）
- [x] 代码质量检查（命名规范、GPIO硬编码、include guard）
- [x] 自动 Release (tagged versions)
- [x] 示例 .app 应用包验证
- [x] .gitignore 配置
- [x] 示例 MicroPython 应用（hello.app）

### ✅ 第四阶段：测试与部署（文档/工具完成，硬件测试待进行）
- [x] 硬件测试指南（hardware-test-guide.md，11个测试用例 + 故障排除）
- [x] 自动化测试脚本（test_hardware.py，5个测试场景 + 串口通信）
- [x] OTA 更新指南（ota-guide.md，双分区 + GitHub Releases）
- [x] 应用开发指南（app-dev-guide.md，C/Python双语言 + API参考 + 故障排查）
- [x] README 项目文档完善
- [x] CI/CD 路径修复（validate-apps → examples/apps）
- [x] 自动化测试接口代码审查与修复（app_test.c/h）
  - [x] 修复请求 ID 空指针解引用崩溃
  - [x] 补齐缺失头文件（uart/stdlib/esp_system 等）
  - [x] 结果数组与全局状态加互斥锁保护（消除竞态）
  - [x] 输出缓冲区改为边界安全的追加函数
  - [x] Python 执行改为异步任务 + 解释器串行化
  - [x] 修复 cJSON 参数内存泄漏 / UART 缓冲区越界
  - [x] XTC:/XTR: 命令响应前缀协议落地并对齐文档
  - [x] 消除硬编码（屏幕尺寸/LED 数量常量化）
- [x] 全功能自动化测试脚本 test_full.py（覆盖全部 13 个命令 + 10 个测试项 + 启动日志 + 稳定性）
- [x] 实际硬件烧录测试（COM6，launcher 分区 0xa0000）
- [x] 按键响应验证（cmd5 SIMULATE_KEY：6 键 + 长按 + 非法键拒绝）
- [x] 电池管理验证（cmd9 GET_BATTERY：电压 4035mV / 91%）
- [x] 音频/显示/LED/WiFi/蓝牙/MicroPython 运行时验证（RUN_TEST + 命令接口）
- [ ] SD卡应用加载测试（硬件未插卡，HARDWARE WARN）
- [ ] OTA 更新测试（需服务器配置）

### 第五阶段：全测发现的固件 Bug 修复（已完成，CI 全绿 + 硬件复测通过）
1. **UART 命令任务栈溢出**：`test_uart` 栈 4096 → 12288（PLAY_TONE 深层调用链 cJSON+I2S+sinf 溢出）
2. **PLAY_TONE 同步阻塞**：audio_output_tone 移到独立异步任务 + audio_mutex 串行化
3. **EXEC_PYTHON 跨任务 TLS abort**：MicroPython 运行时绑定 ui_init_task，短生命周期任务执行后删除触发
   "TLSP deletion callback at index 0 overwritten" → 改为 UART 任务同步执行直接返回
4. **RUN_TEST 任务未删除自身**：test_run_single/test_run_all 正常 return 导致 IllegalInstruction
   → 拆出 impl，任务入口末尾 vTaskDelete(NULL)
- 最终全测：41 项 36 PASS / 0 FAIL / 3 WARN / 2 SKIP（WARN 均为 SD 卡未插 / 输出捕获，SKIP 为物理按键/重复项）
- 最终固件 commit：6d84962（CI run 44573986977 success）

### 第六阶段：深度审查加固（完成）
对未在全测覆盖的应用/驱动/桥接层做三路深度代码审查，修复与已修 bug 同类的隐患：
1. **OTA 栈溢出**：`response_buffer[4096]` 从 8192 字节任务的栈移到静态区，`ota_task` 栈 8192→12288（与 PLAY_TONE 栈溢出同类）
2. **OTA ERROR 死锁**：`ota_start` 允许从 ERROR 状态重试（之前只接受 IDLE，重试键无效）
3. **WiFi debug 越界读**：`vsnprintf` 返回长度未夹紧就用于 `send()`，会越界读 256 字节栈缓冲并泄露数据 → clamp 到实际长度
4. **EXEC_PYTHON 输出**：固件侧重置缓冲时同步清空首字节（防陈旧数据）；测试脚本改为从 UART 日志抓取 print 回显，消除最后一项 WARN（不改变 MicroPython print 走 console 的设计）
5. `push_via_api.py` 改为通用工具（支持命令行指定文件 / 自动检测差异），`.gitignore` 补充本地固件与测试产物

**已识别、暂不修改的复杂并发隐患**（无法真机验证，避免破坏已通过全测的稳定固件，留待后续专项处理）：
- `app_music.c` 快速切歌：新旧播放任务并发操作全局 decoder，旧任务可能 fclose 新任务的 FILE*（UAF）
- `app_led.c`：新效果任务清零共享 stop 标志导致旧任务不退出；destroy 未确认任务退出即 deinit RMT
- `poincare/runtime.c`：解释器全局重入（多个执行路径仅 EXEC_PYTHON 持锁）；`nlr_jump_fail` 声明 NORETURN 却返回
- `app_micropython.c`：放弃未退出的 Python 任务后重进应用，两任务并发执行解释器（TLS abort 同类）
- `drv_audio_output.c`：音频后端全局状态跨音乐/BTC/主循环无锁
- `drv_battery.c`：运行期 `ESP_ERROR_CHECK` 且句柄未判空，初始化失败会 abort

### 第七阶段：修复 WiFi 扫描卡死重启（用户实机反馈，完成）
**现象**：用户反馈"扫描 WiFi 时直接卡死重启"。

**测试盲区根因**：串口 `SIMULATE_KEY`（cmd5）注入的是 Python 专用按键队列（`s_py_btn_queue`），
主 UI 导航读的是 `drv_button` 的 `s_btn_queue`——两者不通，所以全测从来无法进入 WiFi 设置页；
而 `test_wifi`（item8）只检查 WiFi 栈状态、从不发起扫描。该崩溃路径一直在测试覆盖之外。

**崩溃根因（COM6 实机日志确认）**：
```
I APP_WIFI: DIAG: trigger scan (init=0)
W wifi:sta_scan: STA is connecting, scan are not allowed!
W APP_WIFI: WiFi scan start failed: ESP_ERR_WIFI_STATE
***ERROR*** A stack overflow in task sys_evt has been detected.
rst:0xc (SW_CPU_RESET),boot:0x13
```
- `wifi_scan_done_handler()` 运行在 system_event 任务（sys_evt），
  该任务栈仅 `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` 默认 2048 字节。
  原代码在栈上放 `wifi_ap_record_t ap_records[8]`（约 928 字节）+ `prev_ssid[33]`，
  叠加 `esp_wifi_scan_get_ap_records()` 的深层调用链 → 栈溢出 → 重启。

**修复（CI run 44663375041 success，固件已烧录 COM6 验证）**：
1. 扫描结果缓冲 `ap_records` 从栈移到 `static`（事件循环单线程，无重入）— 根因修复
2. `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE` 2048→4096 — 防御性余量
3. WiFi 事件处理器改用 `esp_event_handler_instance_*` + 幂等标志，消除
   "handler already registered, overwriting"（页面反复进出/诊断命令重复注册）
4. 扫描被 STA 连接拒绝时（ESP_ERR_WIFI_STATE）先 disconnect 再重试一次；
   失败时状态复位 IDLE，避免 UI 永久停在"正在扫描"
5. 新增 cmd13（DIAG_WIFI_SCAN）诊断命令 + 全测 crash-guard 检查项，永久覆盖扫描路径

**验证**：修复后连续 3 次扫描全部 `scan complete`，0 崩溃 / 0 重启 / 最终 PING 正常；
完整全测 42 项 0 FAIL（仅 SD 卡未插 2 WARN、物理按键/重复项 2 SKIP）。

## 第八阶段：用户反馈三问题修复（WiFi连不上 / 蓝牙无界面 / OTA失败重启）

实机 COM6 抓日志 + 代码审查定位三个独立根因：

### 1. WiFi 连接失败
**根因A**：UI（`app_settings_wifi.c`）里用户选网连接成功后**从未保存凭据到 NVS**，
只有 `wifi_debug.c` 用默认凭据（tiger8/12345678）保存。重启后 `wifi_auto_connect()`
又用 tiger8 连接 → 实机日志显示 `WiFi connection failed, retrying... (1/5)~(5/5)`。
用户每次设置的 WiFi 都留不住。
**根因B**：`wifi_debug.c` 与 `app_settings_wifi.c` 都注册 WIFI_EVENT 处理器，
前者在 STA_DISCONNECTED 时用旧凭据自动重连，覆盖用户在设置页正在建立的新连接。
**修复**：
- GOT_IP 成功时用 `sys_nvs_save_wifi_credentials()` 持久化用户选的 SSID/密码
- 新增 `wifi_debug_pause_autoreconnect(bool)`，用户主动连网期间暂停开机自动重连，
  连接失败超时/退出设置页时恢复，避免与设置页 handler 打架

### 2. 蓝牙没有界面
**根因**：蓝牙只有 A2DP Sink 音频后端（`drv_audio_bt_a2dp.c`），无独立 UI 页面，
仅在"音频输出"页作为一个设备行出现且未连接时显示"不可用"，用户无从配对。
**修复**：
- 新增 `app_settings_bt.c` 蓝牙设置页：初始化状态/连接状态/对端设备名/本机蓝牙名 +
  A 键重新进入可被发现模式（手机可搜索配对），500ms 定时器刷新
- 暴露 `bt_a2dp_is_initialized()` / `bt_a2dp_start_discoverable()` API
- 加入设置菜单第 19 项 + 中英文语言字符串

### 3. OTA 升级失败（重启）
**根因A**：旧实现先把固件下载到 `/sdcard/ota/` 再回读刷写，**强依赖 SD 卡**；
本设备 SD 卡可选且实机未插 → `fopen` 失败 → OTA 必然失败。
**根因B**：版本检查响应缓冲仅 4KB，GitHub latest release JSON 含 release notes
常超 4KB，截断致 `cJSON_Parse` 失败 → 假"检查失败"。
**根因C**：启动日志见 `ota data partition invalid`，otadata 分区当前无效。
**修复**：
- 改为**流式 OTA**：`esp_http_client_read()` 直接喂 `esp_ota_write()`，去 SD 卡依赖
- 四重校验：HTTP 状态 200 / Content-Length 合理 / 固件魔数 0xE9 / esp_ota_end 完整性
- 版本检查缓冲 4KB→8KB
- `esp_ota_set_boot_partition()` 会重建无效 otadata，顺带修复 "ota data invalid"
- READY 后按 A 直接重启生效（不再二次从 SD 卡刷写）

**提交**：本地 `3b086cb`，远程 `0b4864f2ed75`，CI run `44670460745`。

## 第九阶段：16MB 分区重构 + Recovery + 内置字库 + 5键键盘 + app 加固

用户确认硬件 Flash 实为 **16MB**（原按 4MB 配置），并要求：重规划分区表、补齐 loader/recovery
（类安卓恢复模式）、内建精简常用汉字库、完善各 app、构建适合 5 键的键盘。派 5 个并行 agent 分工完成。

### 1. 16MB 分区表重规划（partitions.csv / sdkconfig.defaults / build.yml / docs/ota-guide.md）
- 新布局：nvs(24KB) + phy_init(4KB) + factory(1MB, recovery) + otadata(8KB) +
  launcher=ota_0(4MB, **label 保持 launcher** 兼容 app_ota.c) + launcher_b=ota_1(4MB) +
  retro-core=FAT(5MB, label 保持兼容 main.c 挂载 /flash)。data 分区 0x1000 对齐、app 分区 0x10000 对齐，无重叠/越界。
- flash size 配置 4MB→16MB；CI merge_bin 改为 4 段：0x1000 bootloader / 0x8000 分区表 /
  0x110000 otadata / 0x120000 app（不再把 2.4MB app 写到 1MB factory 造成覆盖）。
- **启动 otadata**：CI 新增 Python 生成 `otadata_launcher.bin`（seq=1 指向 ota_0 + CRC32 校验，
  符合 esp_ota_select_entry_t 32B 布局），替换 ESP-IDF 默认全 FF 空分区——否则空 otadata
  会让 bootloader 回退到空 factory 而无法启动。发布到 firmware 分支为 `otadata.bin`。

### 2. Recovery 恢复模式（新增 app_recovery.c/.h + 菜单/语言/CMake 集成）
- 7 项菜单：恢复出厂 / 清 WiFi 凭据 / 重新开始 OTA / 从 SD 卡刷机(仅提示不盲刷) /
  查看系统信息(版本+分区+内存) / 重启到主系统 / 重启到 Loader(factory)。二次确认防误触。
- 从设置页第 20 项「恢复模式」进入（索引 19）。所有破坏性操作需 A 二次确认。
- **约束**：重启到 factory 仅显式点击触发，开机/崩溃等自动路径绝不调用 return_to_loader_setup
  （尊重用户此前"取消崩溃后回退 factory"的明确要求）。

### 3. 内建精简汉字库（lv_freetype_font.c/.h + sys_nvs.c）
- 默认字体源 0→1（内置），新设备无 SD 卡也有中文。内置 `lv_font_xiaomiao_cn_14`（约 371KB，
  常用汉字集）正式编入 CMake；FreeType 就绪时按 14/16/20/24 用大字体，否则一律回退内置 14px，
  **任何情况不返回 NULL**。解决"没 SD 卡就没中文/英文降级"。

### 4. 5 键友好虚拟键盘（ui_keyboard.c 重写）
- 默认 T9 九宫格大按键（3×3，方向键导航 + A 输入/1.2s 多输循环 + B 退格），QWERTY 可选(6×5)。
- 公共 API 六个函数签名与类型完全不变，调用方零改动。清理原 T9 死代码，全部对象判空。

### 5. 各 app 加固（app_music.c / app_filemgr.c）
- music_play_task 创建失败时复位播放状态；filemgr 路径 strcpy→strncpy+显式 \0。
- app_shell/store/led 审查确认已健壮，未改动。

**验证**：Gh Actions 编译通过 → 按新分区表烧录 COM6 → 全功能测试。待补 CI run id 与测试结果。

## 硬件规格
- MCU: ESP32-WROVER-B (**16MB Flash** + 8MB PSRAM)
- 显示: ST7735 160×128 TFT (横屏 RGB565)
- 按键: 6键手柄 (UP/DOWN/LEFT/RIGHT/A/B)，UI 针对 5 键(T9 键盘)优化
- 存储: MicroSD 卡槽 (SPI模式) + 板载 5MB FAT 分区(/flash)
- 音频: 蜂鸣器 PWM
- LED: GD32 协处理器 (I2C)
- 电池: 锂电池 + ADC 检测 (GPIO34)
- 背光: GPIO0 PWM

## 分区表（16MB）
| 分区 | 类型 | 偏移 | 大小 |
|------|------|------|------|
| nvs | data/nvs | 0x9000 | 24KB |
| phy_init | data/phy | 0xF000 | 4KB |
| factory | app/factory | 0x10000 | 1MB |
| otadata | data/ota | 0x110000 | 8KB |
| launcher (ota_0) | app/ota_0 | 0x120000 | 4MB |
| launcher_b (ota_1) | app/ota_1 | 0x520000 | 4MB |
| retro-core | data/fat | 0x920000 | 5MB |