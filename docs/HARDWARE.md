# 小喵教育掌机（XiaoMiao Desktop OS）硬件文档

> 适用范围：`rainrainrainbow/xiaomiao-desktop` 仓库 `main` 分支，固件自报版本 `v71`（`main/ui/ui_framework.h`）。
> 整理日期：2026-11-19。
>
> **重要说明**：仓库中**没有原理图、PCB、Gerber、正式 BOM**。本文档以源码中的引脚宏定义、初始化代码、测试指南为准整理；凡源码本身存在冲突的地方，均在第 8 节单列，请按第 8 节结论设计硬件，不要直接照抄旧文件。

---

## 1. 硬件平台概览

| 项目 | 参数 | 依据 |
|---|---|---|
| 主控 | ESP32-WROVER-B（双核 Xtensa LX6，240 MHz） | `README.md`、`sdkconfig.defaults` |
| Flash | 4 MB，QIO，80 MHz | `sdkconfig.defaults` |
| PSRAM | 8 MB，Quad，40 MHz（必须，代码/rodata/任务栈大量放 PSRAM） | `sdkconfig.defaults` |
| 显示 | ST7735，160×128 横屏（面板原生 128×160，经 MADCTL 旋转），RGB565 | `main/main.c`、`ion/display.h` |
| 触控 | 无，6 键手柄 | `main/driver/drv_button.h` |
| 存储 | MicroSD（SPI 模式，FAT32，≤32 GB） | `ion/sdcard.c`、`docs/hardware-test-guide.md` |
| 电池 | 3.7 V 锂电，ADC 分压检测 | `main/driver/drv_battery.h`、`docs/hardware-test-guide.md` |
| 音频输出 | I2S 数字功放（v71 为 NS4168），蓝牙 A2DP Sink 可选 | `main/driver/drv_audio_i2s.h`、`sdkconfig.defaults` |
| 音频输入 | LMD2718 PDM 数字麦克风 | `main/driver/drv_mic_lmd2718.h` |
| 指示 | 3 颗 WS2812B RGB（GPIO14，RMT 驱动） | `main/driver/drv_led_strip.h` |
| 蜂鸣器 | 历史外设，v71 已拆除、驱动未初始化 | `main/main.c:989` |
| 图形栈 | LVGL 9.5；MicroPython 1.26.x 运行时 | `README.md` |
| 版本标识 | `XIAOMIAO_VERSION "v71"` | `main/ui/ui_framework.h:34` |

---

## 2. 系统连接框图

```text
                        +----------------------------------+
                        |        ESP32-WROVER-B             |
                        |                                  |
  3.7V Li-Ion -- 分压 --| GPIO34 / ADC1_CH6  (电池 + A键共用)|
                        |                                  |
  UP  -- GPIO2          |                                  |
  DOWN-- GPIO13         |  6键输入（低有效，34/35需外置上拉）|
  LEFT-- GPIO27         |                                  |
  RIGHT-GPIO35          |                                  |
  B   -- GPIO12         |                                  |
  A   -- GPIO34(ADC)    |                                  |
                        |                                  |
                        | SPI2 40MHz  SCLK=18 MOSI=23 MISO=19
                        |   |-- LCD CS=5  DC=4   (ST7735)   |
                        |   `-- SD  CS=22       (MicroSD)   |
                        |       ↑ 二者由 ion_spi_mutex 互斥  |
                        |                                  |
                        | GPIO0  -- LEDC PWM -- LCD 背光     |
                        | GPIO14 -- RMT     -- WS2812B ×3    |
                        | GPIO25/32/33 -- I2S -- NS4168 功放 |
                        | GPIO21/15    -- I2S -- LMD2718 MIC |
                        | UART0 TXD1/RXD3 -- USB-UART 烧录日志|
                        +----------------------------------+
```

---

## 3. 引脚总表（现行固件）

> “现行固件”指 `main.c` 与 `main/driver/*` 中实际编译、实际调用的引脚定义。
> 输入/输出方向均以 ESP32 视角描述。

| 模块 | ESP32 GPIO | 连接对象 | 方向 / 有效电平 | 关键约束 | 代码依据 |
|---|---|---|---|---|---|
| LCD | GPIO18 | ST7735 SCL / SPI2 SCLK | 输出 | 与 SD 卡共用 SPI2 | `main/main.c:92` |
| LCD | GPIO23 | ST7735 SDA / SPI2 MOSI | 输出 | 与 SD 卡共用 SPI2 | `main/main.c:93` |
| LCD | GPIO19 | SPI2 MISO | 输入 | LCD 本身不使用，SD 卡使用 | `main/main.c:94` |
| LCD | GPIO5 | ST7735 CS | 输出，低有效 | 独立片选 | `main/main.c:95` |
| LCD | GPIO4 | ST7735 DC | 输出 | 命令/数据选择 | `main/main.c:96` |
| LCD RST | — | 未使用 | — | 固件只发软件复位命令 `0x01` | `main/main.c:100` |
| 背光 | GPIO0 | LCD 背光（经驱动电路） | PWM 输出，5 kHz / 13 bit | **启动模式引脚，上电必须为正确电平** | `main/driver/drv_backlight.h:18` |
| SD 卡 | GPIO22 | MicroSD CS | 输出，低有效 | 与 LCD 共用 SPI2 总线 | `ion/sdcard.c:27` |
| 按键 | GPIO2 | UP | 输入，低有效，内部上拉 | 启动相关引脚 | `main/driver/drv_button.h:15` |
| 按键 | GPIO13 | DOWN | 输入，低有效，内部上拉 | — | `main/driver/drv_button.h:16` |
| 按键 | GPIO27 | LEFT | 输入，低有效，内部上拉 | — | `main/driver/drv_button.h:17` |
| 按键 | GPIO35 | RIGHT | 输入，低有效 | **仅输入，无内部上拉，需外置上拉** | `main/driver/drv_button.h:18` |
| 按键 | GPIO34 | A（ADC 方式判定） | 模拟输入，低电压表示按下 | **仅输入，无内部上拉；与电池 ADC 共用** | `main/driver/drv_button.h:13` |
| 按键 | GPIO12 | B | 输入，低有效，内部上拉 | **启动 strapping 引脚，启动时不可按下** | `main/driver/drv_button.h:14` |
| 电池 | GPIO34 / ADC1_CH6 | 分压采样 | 模拟输入 | 3.3 kΩ / 10 kΩ 分压 + 0.30 V 二极管压降 | `main/driver/drv_battery.h:13-19` |
| RGB LED | GPIO14 | WS2812B ×3 数据脚（DIN） | 输出（RMT） | 当前 v71 唯一使用者 | `main/driver/drv_led_strip.c:28` |
| 蜂鸣器 | GPIO14 | 蜂鸣器 PWM | 输出（LEDC） | **v71 未焊接 / `drv_buzzer_init()` 被注释** | `main/driver/drv_backlight.h:21`、`main/main.c:989` |
| 音频 | GPIO25 | NS4168 BCLK | 输出（I2S0） | MCLK 未使用 | `main/driver/drv_audio_i2s.h:36` |
| 音频 | GPIO32 | NS4168 LRCLK / WS | 输出（I2S0） | — | `main/driver/drv_audio_i2s.h:37` |
| 音频 | GPIO33 | NS4168 SDA / DOUT | 输出（I2S0） | — | `main/driver/drv_audio_i2s.h:38` |
| 麦克风 | GPIO21 | LMD2718 DATA (SD) | 输入（I2S PDM） | 与旧版 LCD MOSI 定义冲突，见第 8 节 | `main/driver/drv_mic_lmd2718.h:24` |
| 麦克风 | GPIO15 | LMD2718 CLK (SCK) | 输出（I2S PDM） | 启动相关引脚，注意上电电平 | `main/driver/drv_mic_lmd2718.h:25` |
| 调试 | UART0（默认 TXD1 / RXD3） | USB-UART（CP2102 / CH340） | 双向，115200 | 仓库未显式重映射，按 ESP32 默认 UART0 处理 | `docs/hardware-test-guide.md:43`、`tests/test_hardware.py:43` |

---

## 4. 各模块硬件细节

### 4.1 显示 / 背光

- 控制器 ST7735，面板原生 128×160，固件通过 `MADCTL` 旋转为 **160×128 横屏**；颜色格式 RGB565。
- SPI2，模式 0，像素时钟 **40 MHz**，DMA 自动分配，单次最大传输 8 KB。
- 片选 GPIO5，DC GPIO4；**未使用硬件 RST**，初始化调用 `ST7735_SWRESET (0x01)` 软件复位。
- 背光 GPIO0 使用 LEDC：`LEDC_TIMER_0` / `LEDC_CHANNEL_0`，13 位分辨率，5 kHz；亮度 0–100% 映射到占空比 0–8191，默认值 75%，由 NVS 持久化。
- LVGL 刷新回调通过 `esp_lcd_panel_io_tx_param()` 发送 `CASET/RASET/RAMWR`。

### 4.2 MicroSD 卡

- SPI 模式，挂载点 `/sdcard`，FAT32，建议 ≤32 GB。
- 与 LCD 共享 SPI2：SCLK=18、MOSI=23、MISO=19，SD 独立 CS=22。
- 初始化时 LCD 已占用 SPI2，因此 SD 驱动**不再调用 `spi_bus_initialize()`**，只配置 `sdspi_device_config_t` 的 CS。
- 所有 SPI 操作由 `ion_spi_mutex` 保护，锁定超时 5000 ms。
- FATFS 配置：`format_if_mount_failed=false`、`max_files=5`、分配单元 16 KB；`sdkconfig.defaults` 开启长文件名（LFN_HEAP，最大 128）。
- 关键目录：`/sdcard/apps/*.app`（MicroPython 应用）、`/sdcard/Fonts/NotoSansSC-Regular.otf`（FreeType 中文字体）。

### 4.3 六键手柄

- 按键索引固定：`0=UP, 1=DOWN, 2=LEFT, 3=RIGHT, 4=A, 5=B`（MicroPython `KEY_*` 常量与此一致）。
- 低电平有效。`drv_button_init()` 给支持内部上拉的引脚开启上拉；**GPIO34/35 无内部上拉，必须外接上拉电阻**。
- A 键不读数字电平，而是复用电池 ADC：读取 ADC1_CH6 原始值，`raw < 500`（12 bit 满量程 4095，约 0.4 V）判定为按下。
- 现行驱动参数：去抖 **30 ms**，长按阈值 **500 ms**，5 ms 扫描周期，独立 FreeRTOS 任务（栈 2048）。
- 测试指南描述 UI 层行为：长按 360 ms 后开始重复，重复间隔 130 ms；这与驱动层 500 ms 长按事件阈值为不同层次，排错时需区分。

### 4.4 电池检测

- ADC1 / CH6（GPIO34），12 bit，衰减 `ADC_ATTEN_DB_12`，使用 `esp_adc` oneshot + 线拟合校准。
- 分压参数（现行 `drv_battery.h`）：`R1=3300 Ω`、`R2=10000 Ω`，换算 `Vbat = Vadc × 1.33 + 0.30 V`；0.30 V 为防反接二极管（K23 典型值）压降。
- 电量分段估算：≥4.10 V→100%；3.95–4.10 V→80–100%；3.80–3.95 V→50–80%；3.65–3.80 V→30–50%；3.45–3.65 V→10–30%；3.00–3.45 V→0–10%；<3.00 V→0%。
- `drv_battery_init()` 必须在 `drv_button_init()` 之后调用，否则会覆盖 GPIO34 配置（`main.c:983-984` 已按此顺序）。
- 充电状态：旧 HAL 层 `ion/battery.c` 用 >4.3 V 判定充电，但该层当前未被主程序调用；现行 `drv_battery` 不提供充电检测。

### 4.5 音频输出

- I2S0 标准 Philips 格式，16/24/32 bit，8–96 kHz，支持单声道/立体声。
- v71 硬件为 NS4168 数字功放：BCLK=GPIO25、LRCLK=GPIO32、DOUT=GPIO33；MCLK 不用。
- 驱动同时声明兼容 CS43131、ES7134LV/ES7148/ES7243、MAX98357、PCM5102/PCM5122、UDA1334ATS 等 I2S DAC。
- 输出抽象层按优先级自动选路：I2S(100) > 蓝牙 A2DP(80) > 蜂鸣器(10)。
- 蓝牙为经典 BR/EDR + A2DP Sink，BLE 关闭，仅允许 1 个 ACL 连接。

### 4.6 数字麦克风

- LMD2718，PDM 输出，经 I2S 接收：DATA=GPIO21，CLK=GPIO15。
- 固定 16 kHz / 16 bit / 单声道；提供 `drv_mic_init/deinit/start/stop/read/get_level`。

### 4.7 RGB 灯带

- 3 颗 WS2812B，DIN 接 GPIO14；官方 `espressif/led_strip` 组件 + RMT，颜色位序 GRB，RMT `mem_block_symbols` 配置为 128。
- 退出 LED 应用时调用 `drv_led_strip_deinit()` 释放 GPIO14。

### 4.8 蜂鸣器（历史外设）

- 定义：GPIO14，LEDC_TIMER_1 / LEDC_CHANNEL_1，10 bit，5 kHz，支持 MIDI 音符映射。
- v71 已拆除蜂鸣器，`main.c` 中 `drv_buzzer_init()` 被注释；原因：LEDC 会持续占用 GPIO14，导致 RMT 无法输出 WS2812B 时序。

### 4.9 供电与存储

- 3.7 V 锂电经 LDO 给 ESP32 与外设供电；Wi-Fi/蓝牙发射峰值电流较大，LDO 与走线需留足余量。
- Flash 4 MB QIO 80 MHz；PSRAM 8 MB Quad 40 MHz（开启 cache workaround，代码/rodata 可从 PSRAM 执行）。
- 分区表见第 7 节；`/flash` 挂载 wear-levelling FAT 分区 `retro-core`。

---

## 5. 启动流程与初始化顺序

`app_main()` 的关键顺序（顺序本身有硬件含义，不要随意调整）：

1. `sys_nvs_init()` — 读取持久化设置。
2. `esp_netif_init()` / `esp_event_loop_create_default()` — 网络基础。
3. `wifi_auto_connect()` / `wifi_debug_init()` — Wi-Fi 与调试服务器（TCP 3333）。
4. `drv_button_init()` — **必须先于电池**。
5. `drv_backlight_init()` — GPIO0 LEDC PWM。
6. `drv_battery_init()` — 复用 GPIO34 ADC，避免覆盖按键配置。
7. `drv_led_strip_init()` — GPIO14 RMT。
8. `drv_mic_init()` — GPIO21/15 I2S PDM。
9. `audio_output_init()` — 选择 I2S / A2DP / 蜂鸣器后端。
10. 创建按键任务（5 ms 扫描）。
11. LCD 初始化（SPI2 40 MHz + ST7735），随后挂载 `/flash`。
12. `ion_sdcard_init("/sdcard")` — 复用已初始化的 SPI2，扫描 `.app`。
13. FreeType 中文字体加载（默认 `/sdcard/Fonts/NotoSansSC-Regular.otf`，失败回退英文）。
14. 推入桌面页面。

---

## 6. 固件硬件抽象层次

| 层次 | 目录 | 当前状态 |
|---|---|---|
| 实际使用 | `main/main.c`（LCD）、`main/driver/*`（按键/背光/电池/LED/麦克风/音频） | 正式运行路径 |
| 实际使用 | `ion/sdcard.c`、`ion/spi_mutex.c` | SD 卡与 SPI 互斥 |
| 编译但未调用 | `ion/display.c`、`ion/button.c`、`ion/battery.c` | **引脚参数与现行固件冲突，不可作为接线依据**，见第 8 节 |
| Python 接口 | `poincare/mp_xiaomiao.c` | 暴露 `xiaomiao` 模块：`init/fill/pixel/rect/line/show/get_key/sleep_ms` 等；帧缓冲 160×128×2=40 KB（PSRAM，RGB565 SWAPPED） |

MicroPython 可用的硬件相关示例（来自 `README.md` / `docs/app-dev-guide.md`）：

```python
import xiaomiao
xiaomiao.init()
xiaomiao.fill(0xF6D34A)
xiaomiao.rect(10, 20, 30, 30, 0x3B82F6, True)
xiaomiao.show()
while True:
    key = xiaomiao.get_key()
    if key == xiaomiao.KEY_B:
        break
    xiaomiao.sleep_ms(50)
```

> `README.md` 里出现的 `xiaomiao.lcd.* / keypad.* / buzzer.* / battery.* / sd.* / lvgl.* / xm_time.*` 属于另一套早期 API 描述；`docs/app-dev-guide.md` 中的 `xiaomiao` 模块接口与当前代码一致，开发时以后者为准。

---

## 7. Flash 分区布局（`partitions.csv`）

| 分区 | 类型 | 子类型 | 偏移 | 大小 | 用途 |
|---|---|---|---|---|---|
| nvs | data | nvs | 0xA000 | 0x5000 | 设置持久化 |
| phy_init | data | phy | 0xF000 | 0x1000 | 射频校准 |
| factory | app | factory | 0x10000 | 0x8E000 | Loader（OTA 不覆盖） |
| otadata | data | ota | 0x9E000 | 0x2000 | 启动分区选择 |
| launcher | app | ota_0 | 0xA0000 | 0x360000 | 桌面系统固件（3.375 MB） |

> 备注：`retro-core` 原先独立占用 OTA 槽，现已合并进 `launcher`；`main.c` 中仍保留对名为 `retro-core` 的分区挂载尝试，找不到时仅告警、不影响启动。

---

## 8. 已知冲突与硬件设计风险（重点）

| # | 问题 | 影响 | 处理建议 |
|---|---|---|---|
| 1 | **GPIO34 同时接 A 键与电池分压**（`drv_button.h:13`、`drv_battery.h`、`docs/hardware-test-guide.md:29,32`） | 两个模拟/数字网络直接并联会互相拉低，A 键判定与电量读数都不准 | 硬件上用二极管/大电阻隔离，或把电池 ADC 改到空闲的 ADC1 通道（如 GPIO36/39，需结合 WROVER 引脚占用确认）；固件保持“按键先初始化、电池后初始化” |
| 2 | **GPIO14 蜂鸣器 vs WS2812B**（`drv_backlight.h:21` vs `drv_led_strip.h:6`） | LEDC 与 RMT 抢占同一引脚，LED 不亮 | v71 按 LED 设计；蜂鸣器不焊接，不要同时接。恢复蜂鸣器必须做互斥并重新评估引脚 |
| 3 | **GPIO0 用作背光 PWM**（`drv_backlight.h:12-18`） | GPIO0 是 ESP32 启动模式引脚，背光电路若在上电时把 GPIO0 拉低会进下载模式 | 背光驱动加栅极/电阻隔离，确保复位期间 GPIO0 为高；量产建议迁移到非启动关键引脚 |
| 4 | **GPIO12 用作 B 键**（`docs/hardware-test-guide.md:30,181`） | GPIO12 为启动 strapping（MTDI），启动时被按下/拉低会影响启动电压配置 | B 键必须外置上拉，且约定“上电时不要按 B” |
| 5 | GPIO2 / GPIO15 也被使用 | 两者均与 ESP32 启动 strapping 相关 | 上电时序上保证外围不强制拉低，必要时串电阻隔离 |
| 6 | `ion/display.c` 引脚与现行 LCD 不一致 | 旧代码定义 MOSI=21、DC=19、RST=23；现行 `main.c` 为 MOSI=23、DC=4、无 RST。照旧文件接线会黑屏 | 以第 3 节引脚表为准；`ion/display.c` 仅作历史参考 |
| 7 | `ion/battery.c` 分压参数不一致 | 旧层用 100 kΩ/100 kΩ（×2），现行 `drv_battery.h` 用 3.3 kΩ/10 kΩ（×1.33）+0.30 V | 以 `drv_battery.h` 为准；`ion/*` 未被主程序调用 |
| 8 | 缺少 LCD 硬件复位脚 | 依赖软件复位，某些 ST7735 上电状态不稳定会花屏 | 如板级预留 RST，可接至空闲 GPIO 并在固件中补复位时序；当前固件未使用 |
| 9 | 仓库无原理图 / PCB / 正式 BOM | 无法核对电源、电平转换、ESD、天线净空等 | 打板前需补齐硬件设计文件；第 9 节仅为按固件需求推导的参考清单 |
| 10 | PSRAM 为硬性依赖 | 无 PSRAM 或型号不对会导致 LVGL / 字体 / 任务栈分配失败（虽设置 `IGNORE_NOTFOUND=y` 不崩溃，但功能异常） | 必须选 WROVER 系列，PSRAM 40 MHz Quad 配置保持与 `sdkconfig.defaults` 一致 |
| 11 | LCD 与 SD 共享 40 MHz SPI2 | 走线过长或分支过长会导致 SD 初始化失败/花屏 | 短线、共地、必要时降速；片选各自独立；软件已用互斥锁避免并发 |
| 12 | 电池读数含二极管压降补偿 | 不同二极管压降差异会直接带来电量百分比偏差 | 用万用表实测校准 `BAT_DIODE_DROP`，必要时微调分压电阻 |

---

## 9. 参考物料清单（按固件需求推导，非正式 BOM）

| 类别 | 器件 | 关键参数 | 备注 |
|---|---|---|---|
| 主控 | ESP32-WROVER-B | 4 MB Flash / 8 MB PSRAM | 必须是 WROVER，不能用普通 WROOM |
| 显示 | ST7735 TFT | 160×128，SPI，RGB565 | 常见 1.8" 模块；确认 CS/DC 可改接 |
| 存储 | MicroSD 卡 | ≤32 GB，FAT32 | 固件 `max_files=5` |
| 输入 | 轻触按键 ×6 | UP/DOWN/LEFT/RIGHT/A/B | GPIO34/35 需外置上拉 |
| 电源 | 3.7 V 锂电 + LDO | 3.3 V，峰值电流留余量 | 支持 Wi-Fi/BT 发射峰值 |
| 电池检测 | 分压电阻 3.3 kΩ / 10 kΩ + 肖特基二极管 | 对应 ×1.33 + 0.30 V | 建议 K23 或 equivalent |
| 背光 | 背光驱动电路 | GPIO0 PWM 5 kHz | 注意启动电平隔离 |
| 音频 | NS4168 I2S 数字功放 + 喇叭 | BCLK/LRCLK/DOUT | 或兼容 I2S DAC |
| 麦克风 | LMD2718 | PDM，16 kHz | DATA/CLK 两线 |
| 指示 | WS2812B ×3 | GRB，5 V/3.3 V 电平注意 | 单线 DIN 接 GPIO14 |
| 调试 | USB-UART | CP2102 / CH340 | 烧录与日志 |

---

## 10. 硬件验证与排错

### 10.1 构建与烧录

```bash
# ESP-IDF v5.3.2
idf.py set-target esp32
idf.py reconfigure
idf.py build

# 直接烧录
idf.py -p /dev/ttyUSB0 flash monitor

# 或使用合并镜像
python3 -m esptool merge_bin \
  --flash_mode qio --flash_freq 80m --flash_size 4MB \
  --output xiaomiao-desktop-merged.bin \
  0x1000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x12000 build/xiaomiao-desktop.bin
esptool.py --chip esp32 -p /dev/ttyUSB0 -b 460800 write_flash 0x0 xiaomiao-desktop-merged.bin
```

### 10.2 测试用例

完整 11 项手工用例见 `docs/hardware-test-guide.md`：TC-01 开机、TC-02 桌面导航、TC-03 应用启动、TC-04 后台任务、TC-05 文件管理器、TC-06 设置、TC-07 按键长按、TC-08 SD 热插拔、TC-09 MicroPython 加载、TC-10 电池检测、TC-11 压力测试。

自动化串口测试：

```bash
python3 tests/test_hardware.py --port /dev/ttyUSB0 --all
python3 tests/test_hardware.py --dry-run   # 无硬件演练
```

关键日志：`XiaoMiao Desktop OS booting...`、`LCD initialized`、`SD card mounted`、`MicroPython engine initialized`。

### 10.3 常见故障

| 现象 | 检查点 |
|---|---|
| 屏幕不亮 | ST7735 接线（尤其 MOSI=23、DC=4、CS=5）；GPIO0 背光 PWM；`LCD initialized` 日志 |
| 按键无响应 | 低有效逻辑；GPIO34/35 外置上拉；GPIO12 上电未被按下；`BUTTON_ACTIVE_LEVEL=0` |
| SD 不识别 | FAT32 格式；CS=GPIO22；与 LCD 共享 SPI2；`SD card mount failed` 日志 |
| 烧录失败 | 按住 BOOT 上电；确认 GPIO12 未被拉低；降速到 115200 |
| LED 不亮 | 确认 GPIO14 未接蜂鸣器；`drv_buzzer_init()` 保持注释；RMT 位序 GRB |
| 电量跳变 | GPIO34 与 A 键网络冲突；分压电阻/二极管参数；ADC 校准 |

---

## 11. 文档来源索引

| 文件 | 提供信息 |
|---|---|
| `main/main.c` | LCD 引脚、SPI2 配置、初始化顺序、SD 挂载 |
| `main/driver/drv_button.h/.c` | 按键引脚、有效电平、ADC A 键、去抖/长按 |
| `main/driver/drv_backlight.h/.c` | 背光与蜂鸣器引脚、LEDC 参数 |
| `main/driver/drv_battery.h/.c` | 电池分压、ADC 通道、电量曲线 |
| `main/driver/drv_led_strip.h/.c` | WS2812B GPIO14、RMT 配置 |
| `main/driver/drv_audio_i2s.h` | I2S 功放引脚、兼容芯片 |
| `main/driver/drv_mic_lmd2718.h` | 麦克风 DATA/CLK |
| `ion/sdcard.c`、`ion/spi_mutex.h` | SD SPI2 复用与互斥 |
| `partitions.csv`、`sdkconfig.defaults` | Flash/PSRAM/分区/外设配置 |
| `docs/hardware-test-guide.md` | 接线表、11 项测试用例、排错 |
| `docs/app-dev-guide.md` | MicroPython 硬件 API、按键常量 |
| `tests/test_hardware.py` | 自动化串口测试 |
| `README.md` | 平台概述、功能表、早期 API 描述（部分已过时） |
