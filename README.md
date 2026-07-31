# XiaoMiao Desktop OS

**小喵教育掌机桌面操作系统** — 基于 ESP32-WROVER-B + ST7735 160×128 TFT + MicroSD + 6键手柄

一个完整的类 Android 桌面操作系统，支持 LVGL 9.5 图形界面、MicroPython 运行时、OTA 更新和 .app 应用包系统。

## 项目概览

```
xiaomiao-desktop/
├── main/                    # 主固件
│   ├── main.c              # 入口 + 硬件初始化
│   ├── xiaomiao_desktop.h  # 全局头文件（引脚/配色/布局）
│   ├── CMakeLists.txt      # 主组件构建配置
│   ├── idf_component.yml   # IDF 组件管理器依赖
│   ├── ui/                  # LVGL 界面（6个页面）
│   ├── input/               # 6键手柄驱动
│   ├── system/              # 任务管理器 + 应用启动器
│   ├── apps/                # 内置应用骨架
│   └── mpy/                 # MicroPython 运行时引擎
├── components/              # ESP-IDF 组件
│   └── return_to_loader/   # 返回加载器机制
├── .github/workflows/       # CI/CD 自动化流水线
├── examples/apps/           # 示例 Python 应用
├── docs/                    # 文档
└── tests/                   # 测试脚本
```

## 快速开始

### 1. 环境准备

```bash
# 安装 ESP-IDF v5.3.2
git clone -b v5.3.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32 && . ./export.sh

# 安装 IDF 组件管理器
pip install idf-component-manager
```

### 2. 构建固件

```bash
cd xiaomiao-desktop
idf.py set-target esp32
idf.py reconfigure    # 解析组件依赖
idf.py build          # 编译
```

### 3. 烧录

```bash
# 方式1：直接烧录
idf.py -p /dev/ttyUSB0 flash monitor

# 方式2：使用合并二进制（推荐）
python3 -m esptool merge_bin \
  --flash_mode qio --flash_freq 80m --flash_size 4MB \
  --output xiaomiao-desktop-merged.bin \
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x12000 build/xiaomiao-desktop.bin

esptool.py write_flash 0x0 xiaomiao-desktop-merged.bin
```

## 功能特性

| 功能 | 状态 | 说明 |
|------|------|------|
| 3×2 桌面网格 | ✅ | 6个应用图标，方向键导航 |
| 纯按键操作 | ✅ | 所有页面通过 ↑↓←→ A/B 操作 |
| 后台任务管理 | ✅ | 锁定/解锁/单个清理/全部清理 |
| 设置应用 | ✅ | WiFi列表、显示设置、关于本机 |
| 文件管理器 | ✅ | 目录浏览、文件操作 |
| 开机动画 | ✅ | 进度条 + 猫 logo |
| MicroPython 引擎 | ✅ | 9个硬件模块注册到 Python |
| .app 应用包 | ✅ | SD卡加载 Python 应用 |
| 贪吃蛇游戏 | ✅ | 内置 C 应用 |
| 计算器 | ✅ | 内置 C 应用 |
| 音乐播放器 | ✅ | 内置 C 应用骨架 |
| OTA 更新 | ✅ | 双分区 ota_0/ota_1 |
| CI/CD 自动构建 | ✅ | GitHub Actions 4 级流水线 |
| 自动 Release | ✅ | 打 tag 后自动发布 |

## 架构四层

```
┌──────────────────────────────────────────────┐
│  4. MicroPython 运行时层                      │
│  ┌──────────────────────────────────────────┐│
│  │ mpy_engine · 9个硬件模块 · .app 包管理器 ││
│  └──────────────────────────────────────────┘│
├──────────────────────────────────────────────┤
│  3. UI 层 (LVGL 9.5)                        │
│  ┌──────────────────────────────────────────┐│
│  │ 桌面 · 设置 · 文件 · 后台 · 开机动画     ││
│  └──────────────────────────────────────────┘│
├──────────────────────────────────────────────┤
│  2. 系统层                                    │
│  ┌──────────────────────────────────────────┐│
│  │ 任务管理器 · 应用启动器 · 导航 · 返回加载器││
│  └──────────────────────────────────────────┘│
├──────────────────────────────────────────────┤
│  1. 硬件层 (ESP32-WROVER-B)                  │
│  ┌──────────────────────────────────────────┐│
│  │ ST7735 LCD · 6键 · SD卡 · PWM · ADC      ││
│  └──────────────────────────────────────────┘│
└──────────────────────────────────────────────┘
```

## MicroPython API 参考

### 硬件模块 `import xiaomiao`

```python
import xiaomiao

# LCD 控制
xiaomiao.lcd.fill(0xF6D34A)          # 填充颜色
xiaomiao.lcd.pixel(10, 20, 0x0000)   # 画像素点

# 按键检测
key = xiaomiao.keypad.get(100)       # 等待按键(超时100ms)
is_down = xiaomiao.keypad.is_pressed(xiaomiao.keypad.A)

# 蜂鸣器
xiaomiao.buzzer.tone(1000, 200)      # 1000Hz 200ms
xiaomiao.buzzer.off()

# 电池
level = xiaomiao.battery.read()      # 0-100%

# SD 卡
files = xiaomiao.sd.listdir("/sdcard")
exists = xiaomiao.sd.file_exists("/sdcard/test.txt")

# LVGL 绘图
xiaomiao.lvgl.clear()
xiaomiao.lvgl.label("Hello!", 10, 20)
xiaomiao.lvgl.rect(10, 30, 140, 2, 0xF6D34A)

# 延时
xiaomiao.xm_time.msleep(100)
```

## SD 卡 .app 应用包结构

```
/sdcard/apps/
├── hello.app/
│   ├── manifest.json   # 应用元数据
│   ├── main.py         # 主入口脚本
│   ├── icon.png        # 图标（可选）
│   └── lib/            # 依赖库（可选）
└── snake.app/
    └── ...
```

## License

MIT