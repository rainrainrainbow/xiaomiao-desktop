# XiaoMiao Desktop OS — 应用开发指南

## 概览

小喵桌面操作系统支持**两种应用类型**：

| 类型 | 语言 | 存储位置 | 性能 | 开发难度 |
|------|------|---------|------|---------|
| 内置 C 应用 | C (LVGL) | 编译进固件 | 🔥 高 | 高 |
| MicroPython .app | Python | SD 卡 `/sdcard/apps/` | 中等 | 低 |

---

## 1. 内置 C 应用

适用于性能敏感或系统级功能（如：贪吃蛇游戏、计算器、音乐播放器）。

### 1.1 创建步骤

1. 在 `main/app/` 下创建 `my_app.c` 和 `my_app.h`
2. 实现 `page_callbacks_t` 回调结构体（init / activate / deactivate / destroy / on_key）
3. 在 `app_builtin.c` 中添加应用条目和回调查找
4. 在 `main/CMakeLists.txt` 中添加源文件

### 1.2 完整示例

```c
// main/app/hello_c.c
#include "lvgl.h"
#include "ui_framework.h"
#include "app_builtin.h"
#include "lang/lang.h"
#include <stdio.h>
#include <string.h>

// 按键回调状态
static int counter = 0;
static lv_obj_t *counter_label = NULL;

// 页面初始化（创建 UI）
static void hello_init(void *data)
{
    (void)data;
    lv_obj_t *main = lv_obj_create(lv_scr_act());
    lv_obj_set_size(main, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(main, lv_color_hex(0xF6D34A), 0);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_style_border_width(main, 0, 0);

    lv_obj_t *title = lv_label_create(main);
    lv_label_set_text(title, "Hello C App");
    lv_obj_set_style_text_color(title, lv_color_hex(0x1B1713), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    counter_label = lv_label_create(main);
    lv_label_set_text(counter_label, "Count: 0");
    lv_obj_set_style_text_color(counter_label, lv_color_hex(0x5C4220), 0);
    lv_obj_align(counter_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *hint = lv_label_create(main);
    lv_label_set_text(hint, "↑↓: Count | B: Back");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x1B1713), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

static void hello_activate(void) {}
static void hello_deactivate(void) {}

// 页面销毁（清理 LVGL 对象）
static void hello_destroy(void)
{
    counter = 0;
    counter_label = NULL;
}

// 按键事件处理
static bool hello_on_key(int key)
{
    if (key == KEY_UP) {
        counter++;
        if (counter_label) {
            lv_label_set_text_fmt(counter_label, "Count: %d", counter);
        }
        return true;
    } else if (key == KEY_DOWN) {
        counter--;
        if (counter_label) {
            lv_label_set_text_fmt(counter_label, "Count: %d", counter);
        }
        return true;
    } else if (key == KEY_B) {
        // B 键退出
        if (ui_stack_depth() > 1) ui_stack_pop();
        return true;
    }
    return false;
}

// 导出回调结构体
static const page_callbacks_t g_hello_callbacks = {
    .init = hello_init,
    .activate = hello_activate,
    .deactivate = hello_deactivate,
    .destroy = hello_destroy,
    .on_key = hello_on_key,
};
```

### 1.3 注册到应用启动器

在 `main/app/app_builtin.c` 中：

**a) 在应用定义数组 `s_builtin_app_defs[]` 中添加：**
```c
{
    .name = "hello_c",
    .icon_text = LV_SYMBOL_WIFI,  // 使用 LVGL 内置符号
    .icon_color = 0x3B82F6,
    .type = APP_TYPE_BUILTIN,
    .launch_cb = NULL,
},
```

**b) 在 `app_builtin_get_callbacks()` 函数中添加：**
```c
// 在文件顶部 #include 你的头文件
#include "app/hello_c.h"

// 在 app_builtin_get_callbacks 中添加：
if (strcmp(app_name, "hello_c") == 0) return &g_hello_callbacks;
```

### 1.4 可用 API

| 函数 | 说明 |
|------|------|
| `ui_stack_push(PAGE_APP_PLACEHOLDER, &cbs, NULL)` | 推入新页面到栈顶（应用入口） |
| `ui_stack_pop()` | 返回上一页（B 键退出应用） |
| `ui_stack_back_home()` | 返回桌面（清除所有页面栈） |
| `ui_stack_depth()` | 获取页面栈深度 |
| `ui_stash_set(&stash)` / `ui_stash_pop()` | 页面间参数传递 |
| `ui_statusbar_set_title(title)` | 设置状态栏左上角文字 |
| `ui_statusbar_update_time()` | 更新状态栏时间 |
| `ui_statusbar_update_battery()` | 更新状态栏电池 |
| `ui_theme_set(theme)` | 设置主题 (THEME_DARK / THEME_LIGHT) |
| `ui_theme_colors()` | 获取当前主题颜色结构体 |
| `bg_manager_on_launch(name)` | 注册应用到后台管理 |
| `bg_manager_suspend_current()` | 暂停当前前台应用 |
| `bg_manager_kill(name)` | 关闭指定后台应用 |
| `bg_manager_is_running(name)` | 检查应用是否在运行 |
| `lang_get(STR_xxx)` | 获取本地化字符串 |
| `sys_nvs_save_*(...)` | 保存设置到 NVS 持久存储 |

### 1.5 颜色常量

```c
#define UI_YELLOW   0xF6D34A  // 主背景色
#define UI_BLACK    0x1B1713  // 深色文字
#define UI_BROWN    0x5C4220  // 标题栏/按钮焦点
#define UI_RED      0xE64B3C  // 警告/错误
#define UI_CREAM    0xFFF3B0  // 深色背景上的文字
#define UI_GREEN    0x2DD466  // 成功/进度条
```

> **注意**：以上颜色常量定义在 `main/ui/ui_framework.h` 中，可在 C 应用中直接引用。

---

## 2. MicroPython .app 应用

适用于快速开发、脚本化或用户自定义应用。应用存储在 SD 卡上，系统启动时自动扫描加载。

### 2.1 目录结构

```
/sdcard/apps/
└── myapp.app/              # 目录名 = 应用 ID，必须 .app 后缀
    ├── manifest.json       # 应用元数据（必需）
    ├── main.py             # 主入口脚本（必需）
    ├── icon.png            # 应用图标 48×48（可选，暂未实现）
    └── lib/                # Python 依赖库（可选）
        └── helpers.py
```

### 2.2 manifest.json 格式

```json
{
    "id": "myapp",
    "name": "我的应用",
    "version": "1.0.0",
    "author": "开发者名",
    "icon_emoji": "🎮",
    "description": "应用简介"
}
```

| 字段 | 必需 | 说明 |
|------|------|------|
| `id` | ✅ | 应用唯一标识符（字母数字，无空格） |
| `name` | ✅ | 显示名称 |
| `version` | ✅ | 语义化版本号 |
| `author` | ❌ | 作者名 |
| `icon_emoji` | ❌ | Emoji 图标（桌面显示用） |
| `description` | ❌ | 简短描述 |

### 2.3 main.py 规范

```python
"""
myapp — 应用说明
"""
import xiaomiao

# 1. 初始化帧缓冲（必须在绘制前调用）
xiaomiao.init()

# 2. 绘制 UI
xiaomiao.fill(0xF6D34A)          # 清屏为黄色背景
xiaomiao.rect(10, 10, 140, 2, 0x5C4220, True)  # 矩形分隔线
xiaomiao.rect_fill(10, 20, 30, 30, 0x3B82F6)    # 填充矩形
xiaomiao.pixel(80, 64, 0xE64B3C)                 # 画点
xiaomiao.line(10, 80, 150, 80, 0x1B1713)         # 画线

# 3. 上屏显示
xiaomiao.show()

# 4. 主循环（必须，否则应用立即退出）
while True:
    key = xiaomiao.get_key()  # 非阻塞读取按键（-1=无按键）
    if key == xiaomiao.KEY_A:
        # 处理 A 键
        pass
    elif key == xiaomiao.KEY_B:
        # B 键退出应用
        break
    xiaomiao.sleep_ms(50)     # 任务让步，避免忙等
```

**重要规则：**
- 初始化顺序：先 `xiaomiao.init()` 分配帧缓冲，绘制后调用 `xiaomiao.show()` 上屏
- 主循环**必须**存在，否则应用立即退出
- 使用 `break` 退出主循环以关闭应用
- 使用 `xiaomiao.sleep_ms(50)` 让步 CPU，避免忙等
- 避免长时间阻塞（>1秒），否则系统会认为应用无响应

### 2.4 MicroPython API 参考

#### `xiaomiao` 模块

| 函数 | 参数 | 说明 |
|------|------|------|
| `init()` | — | 初始化帧缓冲（幂等，必须先调用） |
| `fill(color)` | 24-bit RGB (0xRRGGBB) | 清屏为指定颜色 |
| `pixel(x, y, color)` | x, y, 0xRRGGBB | 画一个像素点 |
| `rect(x, y, w, h, color, fill)` | x, y, w, h, 0xRRGGBB, bool | 矩形（fill=True 实心） |
| `rect_fill(x, y, w, h, color)` | x, y, w, h, 0xRRGGBB | 便捷填充矩形 |
| `line(x0, y0, x1, y1, color)` | x0, y0, x1, y1, 0xRRGGBB | Bresenham 直线 |
| `show()` | — | 将帧缓冲上屏显示 |
| `get_key()` | — | 非阻塞读按键：返回按键索引或 -1 |
| `millis()` | — | 毫秒时间戳（从系统启动） |
| `sleep_ms(ms)` | int | 毫秒级休眠（任务让步） |
| `width()` | — | 屏幕宽度（160px） |
| `height()` | — | 屏幕高度（128px） |

**按键常量：**

| 常量 | 值 | 说明 |
|------|:---:|------|
| `xiaomiao.KEY_UP` | 0 | 上键 |
| `xiaomiao.KEY_DOWN` | 1 | 下键 |
| `xiaomiao.KEY_LEFT` | 2 | 左键 |
| `xiaomiao.KEY_RIGHT` | 3 | 右键 |
| `xiaomiao.KEY_A` | 4 | A 键（确认） |
| `xiaomiao.KEY_B` | 5 | B 键（返回/退出） |

---

## 3. 应用打包与部署

### 3.1 本地测试

```bash
# 1. 创建应用目录
mkdir -p /sdcard/apps/myapp.app/
# 2. 复制文件
cp manifest.json /sdcard/apps/myapp.app/
cp main.py /sdcard/apps/myapp.app/
# 3. 重启设备（或等待系统自动扫描）
# 系统启动时自动扫描 /sdcard/apps/ 目录
```

### 3.2 打包分发

MicroPython .app 应用不需要编译，直接复制目录即可分发。

### 3.3 CI/CD 验证

在 `.github/workflows/build.yml` 中，`validate-apps` job 会自动验证所有 `.app` 包：

```bash
# 检查 manifest.json 格式
# 检查 main.py Python 语法
# 检查 icon.png 是否存在（可选）
```

---

## 4. 最佳实践

### 4.1 屏幕适配

- 屏幕分辨率：**160×128 像素**（横屏）
- 状态栏 12px 高，标题栏约 14px 高
- 内容区可用高度：128 - 12 - 14 = **102px**（实际以 `ui_content_y()` 返回值为准）
- 内容区可用宽度：**160px**
- 字体：Montserrat 10px（默认），12px（标题），14px（大号），CJK 字体通过 FreeType 从 SD 卡动态加载（`/sdcard/Fonts/NotoSansSC-Regular.otf`）

### 4.2 按键操作

所有交互必须通过 6 键完成：

- **↑↓←→**：导航/选择
- **A**：确认/进入
- **B**：返回/退出

### 4.3 内存管理

| 项目 | 限制 |
|------|------|
| C 应用 | 注意 LVGL 对象生命周期，及时清理（destroy 回调） |
| Python 应用 | 帧缓冲 40KB（PSRAM），GC 堆 256KB，复杂应用注意内存使用 |
| PSRAM | 8MB 可用，优先使用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` |

### 4.4 线程安全

- C 应用中的 LVGL 操作必须在主任务（UI 线程）中执行
- 其他线程通过 `lv_async_call()` 调度 UI 操作
- Python 应用运行在独立任务中，通过帧缓冲（显式 `show()`）上屏，不直接操作 LVGL 对象

### 4.5 应用退出

- **Python 应用**：`break` 退出主循环，或 B 键触发 `KeyboardInterrupt` 自动退出
- **C 应用**：`ui_stack_pop()` 返回上一页（B 键处理中调用）

### 4.6 调试技巧

```bash
# 查看串口日志
idf.py -p /dev/ttyUSB0 monitor
# 关注关键日志标签
# - "mpy_engine": MicroPython 引擎状态
# - "APP_BUILTIN": 应用启动信息
# - "drv_button": 按键事件
# - "SYS_NVS": NVS 存储操作
# - "MAIN": 系统初始化
```

---

## 5. 故障排查

| 问题 | 可能原因 | 解决方法 |
|------|---------|---------|
| 应用未出现在桌面 | 应用包扫描失败 | 检查路径 `/sdcard/apps/xxx.app/` 正确性 |
| manifest.json 解析错误 | JSON 格式错误 | 用 `python3 -c "import json; json.load(open('manifest.json'))"` 验证 |
| main.py 执行错误 | Python 语法错误 | 检查串口日志中的 MicroPython 回溯信息 |
| 应用卡死 | 主循环阻塞 | 确保 `xiaomiao.sleep_ms(50)` 在循环中 |
| 屏幕无显示 | 未调用 `show()` | 确保在绘制后调用 `xiaomiao.show()` |
| 帧缓冲未初始化 | 未调用 `init()` | 确保在绘制前调用 `xiaomiao.init()` |
| 内存不足 | 对象未释放 | 检查循环中是否有对象泄漏 |
| SD 卡未识别 | 格式/接线问题 | 参考 `hardware-test-guide.md` |

---

## 6. 示例应用

### 6.1 Hello World（已有）

位置：`examples/apps/hello.app/`

演示了基础 API 使用：`init()`、`fill()`、`rect()`、`get_key()`、`sleep_ms()`。

### 6.2 建议练习项目

1. **计时器应用**：使用 `xiaomiao.sleep_ms()` 做倒计时提醒
2. **画板应用**：使用 `↑↓←→` 移动光标，A 键画点
3. **计步器**：记录按键次数并显示历史