# 小喵掌机桌面系统

> Windows Phone Metro UI 风格的全功能桌面系统，基于 ESP32 + LVGL 9.5 构建。

## 功能特性

### 桌面（Metro UI）
- 动态磁贴：时间、天气、系统状态实时更新
- 磁贴交互：点击打开、长按进入编辑模式（拖拽/缩放）
- 多页面磁贴：横向滑动切换页面

### 应用抽屉
- 所有应用以网格形式展示
- 支持搜索过滤
- 支持按名称/使用频率排序

### 内置应用
| 应用 | 功能 |
|------|------|
| 时钟 | 闹钟/秒表/计时器/世界时钟 |
| 天气 | 多城市天气（需联网获取） |
| 设置 | 网络/WiFi、亮度、声音、语言、关于 |
| 音乐 | MP3 播放器（SD 卡） |
| 相册 | 图片浏览（JPG/PNG） |
| 文件 | 文件管理器（SD 卡） |
| 游戏中心 | ROM 列表管理 |
| 计算器 | 基础科学计算器 |
| 日历 | 月视图日历 |
| 记事本 | 纯文本编辑器 |

### 系统功能
- 多任务管理：任务切换卡片
- 通知中心：下拉查看通知
- 快速设置：WiFi、蓝牙、亮度、声音
- 系统导航：磁贴返回键、物理按键支持
- 状态栏：电池、WiFi、蓝牙、时间、信号

## 构建

本仓库配置了 GitHub Actions 自动编译。

推送代码到 `main` 分支后，Actions 会自动开始编译，完成后在 Actions 页面下载 Artifacts 中的 `.bin` 文件。

## 硬件
- 主控：ESP32-WROVER-B（4MB Flash / 8MB PSRAM）
- 协处理器：GD32F350G8
- 屏幕：ST7735 160×128 TFT
- 存储：MicroSD 卡
- 输入：6 键手柄 + 陀螺仪

## 相关项目
- [jsfaint/xiaomiao-firmware](https://github.com/jsfaint/xiaomiao-firmware) - 基础固件模板
- [ZyoungInc/xueersi-idf](https://github.com/ZyoungInc/xueersi-idf) - 硬件逆向成果
