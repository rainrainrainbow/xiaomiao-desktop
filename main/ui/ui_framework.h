/**
 * @file ui_framework.h
 * @brief 小喵桌面 UI框架核心 - 页面栈管理、导航、主题系统
 * 
 * 架构设计（v59 重构，借鉴 X-TRACK）：
 * - 页面栈模式：支持多级导航，B键返回上一级
 * - 页面生命周期：IDLE → LOAD → WILL_APPEAR → DID_APPEAR → ACTIVITY 
 *                → WILL_DISAPPEAR → DID_DISAPPEAR → UNLOAD
 * - 统一输入处理：on_key事件分发到当前页面
 * - 主题系统：颜色、字体、间距统一管理
 * - 页面缓存：可选缓存机制，提升切换性能
 * - Stash数据传递：页面间参数传递
 */

#ifndef UI_FRAMEWORK_H
#define UI_FRAMEWORK_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

/* ========== 屏幕尺寸定义 ========== */
#define LCD_H_RES           160
#define LCD_V_RES           128
#define STATUS_H            12   // 状态栏高度（默认值，实际高度在ui_statusbar_create中根据字体自适应）
#define DOCK_H              8    // 底部导航栏高度

/* ========== 系统版本信息 ========== */
#define XIAOMIAO_VERSION "v71"
#define XIAOMIAO_BUILD "2026-08-26"

/* ========== 页面类型 ========== */
typedef enum {
    PAGE_DESKTOP = 0,       // 桌面主页
    PAGE_SETTINGS,          // 设置页面
    PAGE_APP_LIST,          // 应用列表（全部应用）
    PAGE_APP_PLACEHOLDER,   // 应用占位页
    PAGE_APP_RUNNING,       // 运行中App全屏
    PAGE_STORE,             // 应用商店
    PAGE_RECENTS,           // 最近任务
    PAGE_CUSTOM,            // 自定义应用页
    PAGE_MAX
} page_type_t;

/* ========== 页面生命周期状态（借鉴 X-TRACK） ========== */
typedef enum {
    PAGE_STATE_IDLE,            // 空闲
    PAGE_STATE_LOAD,            // 加载中
    PAGE_STATE_WILL_APPEAR,     // 即将显示
    PAGE_STATE_DID_APPEAR,      // 已显示
    PAGE_STATE_ACTIVITY,        // 活动中
    PAGE_STATE_WILL_DISAPPEAR,  // 即将隐藏
    PAGE_STATE_DID_DISAPPEAR,   // 已隐藏
    PAGE_STATE_UNLOAD,          // 卸载中
    PAGE_STATE_MAX
} page_state_t;

/* ========== Stash 数据传递（简化版） ========== */
#define PAGE_STASH_SIZE 64  // Stash 缓冲区大小（字节）

typedef struct {
    uint8_t data[PAGE_STASH_SIZE];
    uint32_t size;
    bool valid;
} page_stash_t;

/* ========== 页面生命周期回调（v59 重构） ========== */
typedef struct {
    /**
     * 页面加载开始（创建 LVGL 对象）
     * @param data 传递给页面的数据（如应用信息）
     */
    void (*on_load)(void *data);
    
    /**
     * 页面即将显示（动画开始前）
     */
    void (*on_will_appear)(void);
    
    /**
     * 页面已显示（动画结束后，可启动定时任务）
     */
    void (*on_did_appear)(void);
    
    /**
     * 页面即将隐藏（动画开始前，可暂停任务）
     */
    void (*on_will_disappear)(void);
    
    /**
     * 页面已隐藏（动画结束后）
     */
    void (*on_did_disappear)(void);
    
    /**
     * 页面卸载（销毁 LVGL 对象）
     */
    void (*on_unload)(void);
    
    /**
     * 按键事件处理
     * @param key 按键类型（UP/DOWN/LEFT/RIGHT/A/B）
     * @return true 表示已处理，false 表示未处理
     */
    bool (*on_key)(int key);
    
    /**
     * 页面缓存配置（可选）
     * @return true 启用缓存，false 不缓存
     */
    bool (*should_cache)(void);
} page_callbacks_v2_t;

/* 兼容旧版回调结构体 */
typedef struct {
    void (*init)(void *data);
    void (*activate)(void);
    void (*deactivate)(void);
    void (*destroy)(void);
    bool (*on_key)(int key);
} page_callbacks_t;

/* ========== 页面栈管理（v59 重构） ========== */

/**
 * 初始化页面栈
 */
void ui_stack_init(void);

/**
 * 推入新页面到栈顶
 * @param type 页面类型
 * @param callbacks 页面回调函数集（v2 版本）
 * @param data 传递给页面的数据
 */
void ui_stack_push_v2(page_type_t type, const page_callbacks_v2_t *callbacks, void *data);

/**
 * 推入新页面到栈顶（兼容旧版）
 * @deprecated 请使用 ui_stack_push_v2
 */
void ui_stack_push(page_type_t type, const page_callbacks_t *callbacks, void *data);

/**
 * 弹出栈顶页面，返回上一级
 * @return true 表示成功弹出，false 表示栈为空
 */
bool ui_stack_pop(void);

/**
 * 返回桌面主页（清除所有页面栈）
 */
void ui_stack_back_home(void);

/**
 * 获取当前页面类型
 * @return 当前页面类型
 */
page_type_t ui_stack_current(void);

/**
 * 获取当前页面的回调函数集
 * @return 回调函数集指针（栈空时返回NULL）
 */
const page_callbacks_v2_t* ui_stack_current_callbacks_v2(void);

/**
 * 获取当前页面的回调函数集（兼容旧版）
 * @deprecated 请使用 ui_stack_current_callbacks_v2
 */
const page_callbacks_t* ui_stack_current_callbacks(void);

/**
 * 获取页面栈深度
 * @return 栈中页面数量
 */
int ui_stack_depth(void);

/**
 * 清空页面栈，只保留桌面
 */
void ui_stack_clear(void);

/**
 * 设置 Stash 数据（用于页面间传递参数）
 * @param stash Stash 数据指针
 */
void ui_stash_set(const page_stash_t *stash);

/**
 * 获取并清除 Stash 数据
 * @return Stash 数据指针（调用后 stash 失效）
 */
page_stash_t* ui_stash_pop(void);

/* ========== 主题系统 ========== */

/**
 * 主题类型
 */
typedef enum {
    THEME_DARK = 0,
    THEME_LIGHT,
    THEME_MAX
} theme_type_t;

/**
 * 主题颜色定义
 */
typedef struct {
    uint32_t bg;            // 背景色
    uint32_t text;          // 主文本色
    uint32_t text_dim;      // 次要文本色
    uint32_t header_bg;     // 头部背景
    uint32_t border;        // 边框色
    uint32_t sel_bg;        // 选中背景
    uint32_t sel_border;    // 选中边框
} theme_colors_t;

/**
 * 设置当前主题
 * @param theme 主题类型
 */
void ui_theme_set(theme_type_t theme);

/**
 * 获取当前主题类型
 * @return 当前主题
 */
theme_type_t ui_theme_get(void);

/**
 * 获取当前主题颜色
 * @return 主题颜色结构体指针
 */
const theme_colors_t* ui_theme_colors(void);

/* ========== 通用UI组件 ========== */

/**
 * 创建状态栏
 * @param parent 父对象
 * @return 状态栏对象
 */
lv_obj_t* ui_statusbar_create(lv_obj_t *parent);

/**
 * 设置状态栏左上角文字（应用名或品牌名）
 * @param title 显示的文字（如"设置"、"贪吃蛇"），传 NULL 恢复为"XiaoMiaoOS"
 */
void ui_statusbar_set_title(const char *title);

/**
 * 更新状态栏时间
 */
void ui_statusbar_update_time(void);

/**
 * 更新状态栏电池
 */
void ui_statusbar_update_battery(void);

/**
 * 创建底部导航栏（页面指示器）
 * @param parent 父对象
 * @param total_pages 总页数
 * @param active_idx 当前页索引
 * @return 导航栏对象
 */
lv_obj_t* ui_dock_create(lv_obj_t *parent, int total_pages, int active_idx);

/**
 * 创建通用标题栏（模拟器 titlebar 风格）
 * @param parent 父对象
 * @param y Y坐标
 * @param text 标题文字
 * @return 标题栏对象
 */
lv_obj_t* ui_titlebar_create(lv_obj_t *parent, lv_coord_t y, const char *text);

/**
 * 获取标题栏应放置的Y坐标（状态栏高度，根据字体大小自适应）
 * @return 标题栏Y坐标
 */
lv_coord_t ui_titlebar_y(void);

/**
 * 获取内容区起始Y坐标（状态栏 + 标题栏高度，根据字体大小自适应）
 * @return 内容区起始Y坐标
 */
lv_coord_t ui_content_y(void);

/**
 * 设置桌面图标选中状态（棕色背景替代边框）
 * @param cell 图标容器对象
 * @param selected 是否选中
 */
void ui_desktop_cell_set_selected(lv_obj_t *cell, bool selected);

/* ========== 长按A键相关 ========== */
/* 长按阈值统一使用 drv_button.h 中的定义 (500ms) */
/* 此项目前为模拟器备用值，实际使用以 drv_button.h 为准 */
#include "driver/drv_button.h"  // 引入 LONG_PRESS_MS 定义

/* ========== 按键定义 ========== */
typedef enum {
    KEY_UP = 0,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_A,      // 确认/进入
    KEY_B,      // 返回/取消
    KEY_MAX
} key_type_t;

/* ========== 全局UI状态 ========== */

/**
 * UI全局状态
 */
typedef struct {
    lv_obj_t *statusbar;        // 状态栏
    lv_obj_t *brand_label;      // 状态栏左上角品牌/应用名标签
    lv_obj_t *time_label;       // 时间标签
    lv_obj_t *bat_label;        // 电池标签
    theme_type_t theme;         // 当前主题
    int brightness;             // 亮度 (10-100)
    int volume;                 // 音量 (0-100)
    bool sound_on;              // 声音开关
    bool wifi_on;               // WiFi开关
    int layout;                 // 布局模式 (0=3列, 1=2列)
    int font_size;              // 字体大小 (14/16/20/24)
    int sleep_timeout;          // 屏幕超时秒数 (0=永不, 30/60/120/300)
    int font_source;            // 字库来源 (0=FreeType/SD卡, 1=内置/英文)
} ui_state_t;

/**
 * 获取UI全局状态
 * @return UI状态指针
 */
ui_state_t* ui_state_get(void);

#endif /* UI_FRAMEWORK_H */
