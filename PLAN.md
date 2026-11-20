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

### 第六阶段：深度审查加固（进行中）
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

## 硬件规格
- MCU: ESP32-WROVER-B (4MB Flash + 8MB PSRAM)
- 显示: ST7735 160×128 TFT (横屏 RGB565)
- 按键: 6键手柄 (UP/DOWN/LEFT/RIGHT/A/B)
- 存储: MicroSD 卡槽 (SPI模式)
- 音频: 蜂鸣器 PWM
- LED: GD32 协处理器 (I2C)
- 电池: 锂电池 + ADC 检测 (GPIO34)
- 背光: GPIO0 PWM

## 分区表
| 分区 | 类型 | 偏移 | 大小 |
|------|------|------|------|
| nvs | data | 0xA000 | 20KB |
| otadata | data | 0xF000 | 8KB |
| phy_init | data | 0x11000 | 4KB |
| factory | app | 0x12000 | 568KB |
| launcher (ota_0) | app | 0xA0000 | 2.17MB |
| retro_core (ota_1) | app | 0x2C0000 | 1.25MB |