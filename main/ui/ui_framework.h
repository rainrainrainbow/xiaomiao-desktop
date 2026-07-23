/**
 * @file ui_framework.h
 * @brief 小喵桌面 UI框架核心 - 页面栈管理、导航、主题系统
 * 
 * 架构设计：
 * - 页面栈模式：支持多级导航，B键返回上一级
 * - 页面生命周期：init → activate → deactivate → destroy
 * - 统一输入处理：on_key事件分发到当前页面
 * - 主题系统：颜色、字体、间距统一管理
 */

#ifndef UI_FRAMEWORK_H
#define UI_FRAMEWORK_H

#include "lvgl.h"
#include <stdbool.h>

/* ========== 屏幕尺寸定义 ========== */
#define LCD_H_RES           160
#define LCD_V_RES           128

/* ========== 页面类型 ========== */
typedef enum {
    PAGE_DESKTOP = 0,       // 桌面主页
    PAGE_SETTINGS,          // 设置页面
    PAGE_APP_PLACEHOLDER,   // 应用占位页
    PAGE_CUSTOM,            // 自定义应用页
    PAGE_MAX
} page_type_t;

/* ========== 页面生命周期回调 ========== */
typedef struct {
    /**
     * 页面初始化
     * @param data 传递给页面的数据（如应用信息）
     */
    void (*init)(void *data);
    
    /**
     * 页面激活（进入前台）
     */
    void (*activate)(void);
    
    /**
     * 页面失活（进入后台）
     */
    void (*deactivate)(void);
    
    /**
     * 页面销毁
     */
    void (*destroy)(void);
    
    /**
     * 按键事件处理
     * @param key 按键类型（UP/DOWN/LEFT/RIGHT/A/B）
     * @return true 表示已处理，false 表示未处理
     */
    bool (*on_key)(int key);
} page_callbacks_t;

/* ========== 页面栈管理 ========== */

/**
 * 初始化页面栈
 */
void ui_stack_init(void);

/**
 * 推入新页面到栈顶
 * @param type 页面类型
 * @param callbacks 页面回调函数集
 * @param data 传递给页面的数据
 */
void ui_stack_push(page_type_t type, const page_callbacks_t *callbacks, void *data);

/**
 * 弹出栈顶页面，返回上一级
 * @return true 表示成功弹出，false 表示栈为空
 */
bool ui_stack_pop(void);

/**
 * 获取当前页面类型
 * @return 当前页面类型
 */
page_type_t ui_stack_current(void);

/**
 * 获取当前页面的回调函数集
 * @return 回调函数集指针（栈空时返回NULL）
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
    lv_obj_t *time_label;       // 时间标签
    lv_obj_t *bat_label;        // 电池标签
    theme_type_t theme;         // 当前主题
    int brightness;             // 亮度 (10-100)
    bool sound_on;              // 声音开关
    bool wifi_on;               // WiFi开关
    int layout;                 // 布局模式 (0=4应用, 1=2应用)
} ui_state_t;

/**
 * 获取UI全局状态
 * @return UI状态指针
 */
ui_state_t* ui_state_get(void);

#endif /* UI_FRAMEWORK_H */
