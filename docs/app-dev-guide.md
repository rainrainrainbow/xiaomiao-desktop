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

1. 在 `main/apps/` 下创建 `my_app.c`
2. 实现 `lv_obj_t *my_app_create(lv_obj_t *parent)` 函数
3. 在 `app_launcher.c` 中添加应用条目
4. 在 `CMakeLists.txt` 中添加源文件

### 1.2 完整示例

```c
// main/apps/hello_c.c
#include "lvgl.h"
#include "xiaomiao_desktop.h"

// 按键回调状态
static int counter = 0;
static lv_obj_t *counter_label = NULL;

static void on_key_event(lv_event_t *e)
{
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP) {
        counter++;
    } else if (key == LV_KEY_DOWN) {
        counter--;
    } else if (key == LV_KEY_ESC) {
        // B 键退出
        nav_back();
        return;
    }
    if (counter_label) {
        lv_label_set_text_fmt(counter_label, "Count: %d", counter);
    }
}

lv_obj_t *hello_c_create(lv_obj_t *parent)
{
    // 创建主容器（全屏）
    lv_obj_t *main = lv_obj_create(parent);
    lv_obj_set_size(main, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(main, lv_color_hex(UI_YELLOW), 0);
    lv_obj_set_style_pad_all(main, 0, 0);
    lv_obj_set_style_border_width(main, 0, 0);

    // 标题
    lv_obj_t *title = lv_label_create(main);
    lv_label_set_text(title, "Hello C App");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_BLACK), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // 计数器
    counter_label = lv_label_create(main);
    lv_label_set_text(counter_label, "Count: 0");
    lv_obj_set_style_text_color(counter_label, lv_color_hex(UI_BROWN), 0);
    lv_obj_align(counter_label, LV_ALIGN_CENTER, 0, 0);

    // 操作提示
    lv_obj_t *hint = lv_label_create(main);
    lv_label_set_text(hint, "↑↓: Count | B: Back");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_BLACK), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    // 注册按键事件
    lv_obj_add_event(main, on_key_event, LV_EVENT_KEY, NULL);
    lv_group_add_obj(lv_group_get_default(), main);

    return main;
}
```

### 1.3 注册到应用启动器

在 `main/system/app_launcher.c` 中：

```c
// 在 app_entries 数组中添加：
{ "hello_c", "Hello C", hello_c_create, NULL },
```

### 1.4 可用 API

| 函数 | 说明 |
|------|------|
| `nav_to(PAGE_xxx)` | 导航到指定页面 (PAGE_DESKTOP, PAGE_SETTINGS, PAGE_FILES, PAGE_TASKS) |
| `nav_back()` | 返回上一页 |
| `ui_show_toast(msg)` | 显示 Toast 提示消息 |
| `task_manager_add_task(id, name, icon, create_fn)` | 注册任务到后台管理 |
| `task_manager_remove_task(id)` | 从后台移除任务 |
| `task_manager_lock_task(id)` | 锁定任务（禁止清理） |
| `task_manager_unlock_task(id)` | 解锁任务 |

### 1.5 颜色常量

```c
#define UI_YELLOW   0xF6D34A  // 主背景色
#define UI_BLACK    0x1B1713  // 深色文字
#define UI_BROWN    0x5C4220  // 标题栏/按钮焦点
#define UI_RED      0xE64B3C  // 警告/错误
#define UI_CREAM    0xFFF3B0  // 深色背景上的文字
#define UI_GREEN    0x2DD466  // 成功/进度条
```

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
import xiaomiao_lvgl as lvgl
import xiaomiao_xm_time as time

# 1. 清屏
lvgl.clear()

# 2. 绘制 UI
lvgl.label("我的应用", 10, 10)
lvgl.rect(10, 30, 140, 2, 0xF6D34A)

# 3. 主循环（必须，否则应用立即退出）
while True:
    key = xiaomiao.keypad.get(100)  # 等待按键 100ms
    if key == xiaomiao.keypad.A:
        # 处理 A 键
        ...
    elif key == xiaomiao.keypad.B:
        # B 键退出应用
        break
    time.msleep(50)
```

**重要规则：**
- 主循环**必须**存在，否则应用立即退出
- 使用 `break` 退出主循环以关闭应用
- 所有 LVGL 操作自动通过 `lv_async_call` 调度到 UI 线程，无需担心线程安全
- 避免长时间阻塞（>1秒），否则系统会认为应用无响应

### 2.4 MicroPython API 参考

#### 硬件模块 `xiaomiao`

| 模块 | 函数/属性 | 说明 |
|------|----------|------|
| `xiaomiao.keypad` | `.get(timeout_ms)` → int | 等待按键按下，返回键码 |
| | `.is_pressed(key)` → bool | 检测指定键是否按住 |
| | `.A`, `.B`, `.UP`, `.DOWN`, `.LEFT`, `.RIGHT` | 键码常量 |
| `xiaomiao.battery` | `.read()` → int | 读取电池电量 0-100 |
| `xiaomiao.lcd` | `.fill(color)` | 屏幕填充颜色 |
| | `.pixel(x, y, color)` | 画一个像素点 |
| `xiaomiao.buzzer` | `.tone(freq_hz, duration_ms)` | 播放指定频率的声音 |
| | `.off()` | 关闭蜂鸣器 |
| `xiaomiao.sd` | `.listdir(path)` → list | 列出目录内容 |
| | `.file_exists(path)` → bool | 检查文件是否存在 |
| `xiaomiao.xm_time` | `.msleep(ms)` | 毫秒级延时 |
| `xiaomiao.lvgl` | `.clear()` | 清空屏幕（移除所有子对象） |
| | `.label(text, x, y)` | 在 (x,y) 绘制文字 |
| | `.rect(x, y, w, h, color)` | 绘制实心矩形 |
| | `.circle(cx, cy, r, color)` | 绘制实心圆 |

#### xiaomiao_lvgl 模块

```python
import xiaomiao_lvgl as lvgl

lvgl.clear()                          # 清屏
lvgl.label("Hello", 10, 20)           # 文字
lvgl.rect(10, 30, 140, 2, 0xF6D34A)  # 矩形
lvgl.circle(80, 64, 30, 0x2DD466)     # 圆形
```

#### xiaomiao_xm_time 模块

```python
import xiaomiao_xm_time as time

time.msleep(100)  # 延时 100ms
```

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

- 屏幕分辨率：**160×128 像素**
- 标题栏 12px 高，状态栏 10px 高
- 内容区可用高度：128 - 12 - 10 = **106px**
- 内容区可用宽度：**160px**
- 字体：Montserrat 10px（默认），12px（标题），14px（大号）

### 4.2 按键操作

所有交互必须通过 6 键完成：
- **↑↓←→**：导航/选择
- **A**：确认/进入
- **B**：返回/退出

### 4.3 内存管理

| 项目 | 限制 |
|------|------|
| C 应用 | 注意 LVGL 对象生命周期，及时清理 |
| Python 应用 | GC 堆 256KB，复杂应用注意内存使用 |
| Python 堆 | 优先使用 PSRAM（8MB 可用） |

### 4.4 线程安全

- Python 应用中的 LVGL 操作自动通过 `lv_async_call` 调度到 UI 线程
- 无需手动加锁
- 不要在其他线程中直接调用 LVGL API

### 4.5 应用退出

- **Python 应用**：`break` 退出主循环
- **C 应用**：返回父对象指针（或调用 `nav_back()`）

### 4.6 调试技巧

```bash
# 查看串口日志
idf.py -p /dev/ttyUSB0 monitor

# 关注关键日志标签
# - "mpy_engine": MicroPython 引擎状态
# - "app_launcher": 应用启动信息
# - "keypad": 按键事件
# - "lvgl": LVGL 渲染
```

---

## 5. 故障排查

| 问题 | 可能原因 | 解决方法 |
|------|---------|---------|
| 应用未出现在桌面 | 应用包扫描失败 | 检查路径 `/sdcard/apps/xxx.app/` 正确性 |
| manifest.json 解析错误 | JSON 格式错误 | 用 `python3 -c "import json; json.load(open('manifest.json'))"` 验证 |
| main.py 执行错误 | Python 语法错误 | 检查串口日志中的 MicroPython 回溯信息 |
| 应用卡死 | 主循环阻塞 | 确保 `time.msleep(50)` 在循环中 |
| LVGL 操作无响应 | 跨线程调用 | 确保使用 `lv_async_call`（Python 层自动处理） |
| 内存不足 | 对象未释放 | 检查循环中是否有对象泄漏 |
| SD 卡未识别 | 格式/接线问题 | 参考 `hardware-test-guide.md` |

---

## 6. 示例应用

### 6.1 Hello World（已有）

位置：`examples/apps/hello.app/`

演示了基础 API 使用：清屏、绘制文字、矩形、按键检测、电池读取。

### 6.2 建议练习项目

1. **计时器应用**：使用 `xiaomiao.buzzer.tone()` 做倒计时提醒
2. **画板应用**：使用 `↑↓←→` 移动光标，A 键画点
3. **计步器**：记录按键次数并显示历史
4. **电子书阅读器**：从 SD 卡读取文本文件并分页显示