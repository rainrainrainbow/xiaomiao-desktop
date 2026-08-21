# 小喵掌机桌面系统 v65 — Epsilon 架构重构

## 📋 概述

本版本全面参考 [NumWorks Epsilon](https://github.com/numworks/epsilon) 项目的分层架构设计，将小喵掌机桌面系统重构为清晰的分层结构。

## 🏗️ 架构层次

```
┌─────────────────────────────────────────┐
│           Apps (应用程序层)              │
│  Calculator, Settings, Python, etc.     │
├─────────────────────────────────────────┤
│        Poincaré (脚本引擎层)             │
│  MicroPython Runtime, Custom Modules    │
├─────────────────────────────────────────┤
│         Escher (GUI 工具包层)            │
│  Widgets, Layouts, Themes               │
├─────────────────────────────────────────┤
│       Kandinsky (图形引擎层)             │
│  Canvas, Fonts, Drawing Primitives      │
├─────────────────────────────────────────┤
│          Ion (硬件抽象层)                │
│  Display, Buttons, Battery, SD Card     │
└─────────────────────────────────────────┘
```

### 1. Ion - 硬件抽象层 (HAL)

**职责**：抽象所有硬件操作，提供统一的硬件接口。

**核心模块**：
- `ion/display.h` - 显示驱动接口（ST7735 TFT）
- `ion/button.h` - 按键驱动接口（6键手柄）
- `ion/battery.h` - 电池管理接口（ADC 检测）
- `ion/sdcard.h` - SD 卡管理接口（SPI 挂载）

**设计原则**：
- 所有硬件相关代码集中在此层
- 上层代码不直接访问 ESP-IDF API
- 便于移植到其他硬件平台

### 2. Kandinsky - 图形引擎层

**职责**：提供高级绘图功能，基于 Ion 层的显示驱动。

**核心模块**：
- `kandinsky/canvas.h` - 画布操作（像素、直线、矩形、圆）
- `kandinsky/font.h` - 字体渲染（CJK 中文字库、FontAwesome）

**设计原则**：
- 不依赖 LVGL，使用原生绘图原语
- 支持双缓冲和批量刷新
- 字体引擎独立于 GUI 框架

### 3. Escher - GUI 工具包层

**职责**：提供 UI 组件和布局管理，基于 Kandinsky 图形引擎。

**核心模块**：
- `escher/widget.h` - 基础控件（Label, Button, Container）
- `escher/layout.h` - 布局管理（Vertical, Horizontal, Grid）
- `escher/theme.h` - 主题系统（Light/Dark/Metro）

**设计原则**：
- 轻量级控件系统，避免过度抽象
- 支持 Metro UI 风格（Windows Phone）
- 主题切换无需重新编译

### 4. Poincaré - 脚本引擎层

**职责**：提供脚本执行能力，当前基于 MicroPython。

**核心模块**：
- `poincare/runtime.h` - 运行时初始化和管理
- `poincare/modules/` - 自定义模块（未来扩展）

**设计原则**：
- 与 GUI 层解耦，可独立测试
- 支持动态加载 Python 脚本
- 异常捕获和错误报告

### 5. Apps - 应用程序层

**职责**：实现具体应用逻辑，使用下层提供的接口。

**示例应用**：
- `apps/calculator/` - 计算器
- `apps/settings/` - 设置
- `apps/python/` - Python 脚本执行器

**设计原则**：
- 每个应用独立目录
- 通过 Escher 构建 UI
- 通过 Poincaré 执行脚本（如需要）

## 🔄 迁移计划

### 阶段 1：Ion 层实现（当前）
- [x] 创建 Ion 层接口头文件
- [ ] 实现 `ion/display.c`（ST7735 驱动）
- [ ] 实现 `ion/button.c`（6键手柄驱动）
- [ ] 实现 `ion/battery.c`（ADC 电池检测）
- [ ] 实现 `ion/sdcard.c`（SPI SD 卡挂载）

### 阶段 2：Kandinsky 层实现
- [ ] 实现 `kandinsky/canvas.c`（绘图原语）
- [ ] 实现 `kandinsky/font.c`（字体渲染）

### 阶段 3：Escher 层实现
- [ ] 实现 `escher/widget.c`（控件系统）
- [ ] 实现 `escher/layout.c`（布局管理）
- [ ] 实现 `escher/theme.c`（主题系统）

### 阶段 4：Poincaré 层集成
- [ ] 实现 `poincare/runtime.c`（MicroPython 运行时）
- [ ] 迁移现有 `app_micropython.c` 到新架构

### 阶段 5：应用迁移
- [ ] 迁移桌面应用到新架构
- [ ] 迁移设置应用到新架构
- [ ] 创建示例应用验证架构

## 📊 内存管理策略

参考 NumWorks Epsilon 的经验：

| 资源类型 | 分配方式 | 说明 |
|---------|---------|------|
| 栈内存 | 静态分配 | main 任务栈 16KB（内部 DRAM） |
| UI 任务栈 | 静态分配 | UI 初始化任务 64KB（PSRAM） |
| MicroPython GC 堆 | 静态分配 | 64KB（PSRAM） |
| 帧缓冲区 | 静态分配 | 160×128×2 = 40KB（PSRAM） |
| 控件对象 | 静态池分配 | 预分配固定数量控件，避免 malloc |

**关键原则**：
- **避免 malloc/free**：使用静态分配或对象池
- **PSRAM 优先**：大对象（帧缓冲、GC 堆）放在 PSRAM
- **DRAM 保留**：关键代码和栈放在内部 DRAM

## 🎯 下一步行动

1. **等待 CI 构建完成**：检查 v64 的链接器修复是否成功
2. **开始实现 Ion 层**：从 `ion/display.c` 开始，逐步实现硬件驱动
3. **编写单元测试**：为每层接口编写测试用例
4. **渐进式迁移**：先在新架构下实现一个简单应用（如 Hello World），验证架构可行性

## 📚 参考资料

- [NumWorks Epsilon Documentation](https://github.com/numworks/epsilon/tree/master/epsilon/docs)
- [NumWorks Epsilon Architecture](https://www.numworks.com/resources/engineering/software/)
- [ESP-IDF v5.3 Documentation](https://docs.espressif.com/projects/esp-idf/en/v5.3/)
- [LVGL 9.5 Documentation](https://docs.lvgl.io/9.5/)