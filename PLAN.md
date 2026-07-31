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
- [ ] 实际硬件烧录测试（需硬件）
- [ ] 按键响应验证（需硬件）
- [ ] SD卡应用加载测试（需硬件）
- [ ] 电池管理验证（需硬件）
- [ ] OTA 更新测试（需硬件）

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