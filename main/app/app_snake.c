/**
 * @file app_snake.c
 * @brief 贪吃蛇游戏
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_snake_callbacks。
 * 屏幕尺寸 160x128，游戏区域 160x(128-26-8)=160x94px
 * 网格 8x8px，共 20x11 格
 */
#include "app_builtin.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "fonts/lv_freetype_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "APP_SNAKE";

/* ========== 游戏常量 ========== */
#define SNAKE_GRID_W       20   // 横向格子数
#define SNAKE_GRID_H       11   // 纵向格子数
#define SNAKE_CELL_SIZE    8    // 每个格子像素大小
#define SNAKE_MAX_LEN      200  // 蛇最大长度
#define SNAKE_INIT_LEN     3    // 初始长度
#define TICK_INIT_MS       300  // 初始 tick 间隔（ms）
#define TICK_MIN_MS        80   // 最快 tick 间隔
#define TICK_SPEEDUP       5    // 每吃一个食物加速的毫秒数

/* ========== 方向枚举 ========== */
typedef enum {
    DIR_UP = 0,
    DIR_DOWN,
    DIR_LEFT,
    DIR_RIGHT,
    DIR_NONE
} snake_dir_t;

/* ========== 游戏状态 ========== */
typedef enum {
    STATE_PLAYING = 0,
    STATE_PAUSED,
    STATE_GAMEOVER
} game_state_t;

/* ========== 蛇身坐标 ========== */
typedef struct {
    int x;
    int y;
} snake_pos_t;

/* ========== 游戏全局状态 ========== */
static snake_pos_t s_body[SNAKE_MAX_LEN];  // 蛇身坐标数组
static int s_len;                          // 当前长度
static snake_dir_t s_dir;                  // 当前移动方向
static snake_dir_t s_next_dir;             // 下一帧方向（缓冲，防止一帧内多次转向）
static game_state_t s_state;               // 游戏状态
static int s_score;                        // 分数（吃到的食物数）
static snake_pos_t s_food;                 // 食物位置
static lv_obj_t *s_game_obj = NULL;       // 游戏容器对象
static lv_obj_t *s_score_label = NULL;    // 分数标签
static lv_obj_t *s_status_label = NULL;   // 状态提示标签（暂停/游戏结束）
static lv_timer_t *s_timer = NULL;        // 游戏循环定时器
static int s_tick_interval;               // 当前 tick 间隔

/* ========== 颜色定义 ========== */
#define COLOR_BG        0x1B1713  // 深色背景
#define COLOR_GRID      0x2A2420  // 网格线
#define COLOR_SNAKE     0x22C55E  // 蛇身绿色
#define COLOR_SNAKE_HEAD 0x16A34A // 蛇头深绿
#define COLOR_FOOD      0xEF4444  // 食物红色
#define COLOR_TEXT      0xF6D34A  // 文字金色
#define COLOR_GAMEOVER  0xEF4444  // 游戏结束红色

/* ========== 辅助函数 ========== */

/** 获取游戏区域左上角Y坐标 */
static inline int game_area_y(void)
{
    return ui_content_y();
}

/** 获取游戏区域高度 */
static inline int game_area_h(void)
{
    return LCD_V_RES - game_area_y() - DOCK_H;
}

/** 随机生成食物位置（避开蛇身） */
static void spawn_food(void)
{
    bool valid;
    int attempts = 0;
    do {
        valid = true;
        s_food.x = rand() % SNAKE_GRID_W;
        s_food.y = rand() % SNAKE_GRID_H;
        // 检查是否与蛇身重叠
        for (int i = 0; i < s_len; i++) {
            if (s_body[i].x == s_food.x && s_body[i].y == s_food.y) {
                valid = false;
                break;
            }
        }
        attempts++;
    } while (!valid && attempts < 100);
    // 如果100次尝试都找不到空位，说明蛇几乎占满，不生成食物
}

/** 初始化蛇的位置 */
static void init_snake(void)
{
    s_len = SNAKE_INIT_LEN;
    // 蛇初始在中间偏左，水平向右
    int start_x = SNAKE_GRID_W / 4;
    int start_y = SNAKE_GRID_H / 2;
    for (int i = 0; i < s_len; i++) {
        s_body[i].x = start_x - i;
        s_body[i].y = start_y;
    }
    s_dir = DIR_RIGHT;
    s_next_dir = DIR_RIGHT;
}

/** 重置游戏状态 */
static void reset_game(void)
{
    s_score = 0;
    s_tick_interval = TICK_INIT_MS;
    s_state = STATE_PLAYING;
    init_snake();
    spawn_food();
}

/** 绘制单个格子 */
static void draw_cell(int gx, int gy, uint32_t color)
{
    if (!s_game_obj) return;
    lv_obj_t *cell = lv_obj_create(s_game_obj);
    lv_obj_remove_style_all(cell);
    lv_obj_set_style_bg_color(cell, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_pos(cell, gx * SNAKE_CELL_SIZE, gy * SNAKE_CELL_SIZE);
    lv_obj_set_size(cell, SNAKE_CELL_SIZE, SNAKE_CELL_SIZE);
}

/** 重新绘制整个游戏画面 */
static void render_all(void)
{
    if (!s_game_obj) return;
    
    // 清空游戏区域
    lv_obj_clean(s_game_obj);
    
    // 绘制网格背景
    lv_obj_t *bg = lv_obj_create(s_game_obj);
    lv_obj_remove_style_all(bg);
    lv_obj_set_style_bg_color(bg, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_set_size(bg, SNAKE_GRID_W * SNAKE_CELL_SIZE, SNAKE_GRID_H * SNAKE_CELL_SIZE);
    
    // 绘制网格线（浅色线条）
    for (int x = 1; x < SNAKE_GRID_W; x++) {
        lv_obj_t *line = lv_obj_create(bg);
        lv_obj_remove_style_all(line);
        lv_obj_set_style_bg_color(line, lv_color_hex(COLOR_GRID), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_pos(line, x * SNAKE_CELL_SIZE - 1, 0);
        lv_obj_set_size(line, 1, SNAKE_GRID_H * SNAKE_CELL_SIZE);
    }
    for (int y = 1; y < SNAKE_GRID_H; y++) {
        lv_obj_t *line = lv_obj_create(bg);
        lv_obj_remove_style_all(line);
        lv_obj_set_style_bg_color(line, lv_color_hex(COLOR_GRID), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_pos(line, 0, y * SNAKE_CELL_SIZE - 1);
        lv_obj_set_size(line, SNAKE_GRID_W * SNAKE_CELL_SIZE, 1);
    }
    
    // 绘制食物
    draw_cell(s_food.x, s_food.y, COLOR_FOOD);
    
    // 绘制蛇身
    for (int i = 0; i < s_len; i++) {
        uint32_t color = (i == 0) ? COLOR_SNAKE_HEAD : COLOR_SNAKE;
        draw_cell(s_body[i].x, s_body[i].y, color);
    }
}

/** 更新分数标签 */
static void update_score_label(void)
{
    if (s_score_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "分数: %d", s_score);
        lv_label_set_text(s_score_label, buf);
    }
}

/** 更新状态提示标签 */
static void update_status_label(void)
{
    if (!s_status_label) return;
    switch (s_state) {
        case STATE_PAUSED:
            lv_label_set_text(s_status_label, "暂停");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_TEXT), 0);
            lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case STATE_GAMEOVER:
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "游戏结束\n得分: %d", s_score);
                lv_label_set_text(s_status_label, buf);
                lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_GAMEOVER), 0);
                lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
            }
            break;
        default:
            lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

/* ========== 游戏逻辑 ========== */

/** 游戏主循环（定时器回调） */
static void snake_tick(lv_timer_t *timer)
{
    (void)timer;
    
    if (s_state != STATE_PLAYING) return;
    
    // 应用缓冲方向
    s_dir = s_next_dir;
    
    // 计算新蛇头位置
    int new_x = s_body[0].x;
    int new_y = s_body[0].y;
    switch (s_dir) {
        case DIR_UP:    new_y--; break;
        case DIR_DOWN:  new_y++; break;
        case DIR_LEFT:  new_x--; break;
        case DIR_RIGHT: new_x++; break;
        default: break;
    }
    
    // 碰撞检测：撞墙
    if (new_x < 0 || new_x >= SNAKE_GRID_W || new_y < 0 || new_y >= SNAKE_GRID_H) {
        s_state = STATE_GAMEOVER;
        update_status_label();
        return;
    }
    
    // 碰撞检测：撞自身（检查新蛇头是否与蛇身重叠，不包括尾部因为尾部会移走）
    bool self_hit = false;
    for (int i = 0; i < s_len - 1; i++) {
        if (s_body[i].x == new_x && s_body[i].y == new_y) {
            self_hit = true;
            break;
        }
    }
    if (self_hit) {
        s_state = STATE_GAMEOVER;
        update_status_label();
        return;
    }
    
    // 检查是否吃到食物
    bool ate = (new_x == s_food.x && new_y == s_food.y);
    
    // 移动蛇身：从尾部开始向前移动
    // 如果吃到食物，尾部不移除（长度+1）
    if (!ate) {
        // 尾部移走，整体前移
        for (int i = 0; i < s_len - 1; i++) {
            s_body[i] = s_body[i + 1];
        }
        s_body[s_len - 1].x = new_x;
        s_body[s_len - 1].y = new_y;
    } else {
        // 吃到食物，长度+1，蛇身整体前移，新头部在最前面
        // 先把所有蛇身往后移一位
        for (int i = s_len; i > 0; i--) {
            s_body[i] = s_body[i - 1];
        }
        s_body[0].x = new_x;
        s_body[0].y = new_y;
        s_len++;
        s_score++;
        
        // 加速
        s_tick_interval = TICK_INIT_MS - s_score * TICK_SPEEDUP;
        if (s_tick_interval < TICK_MIN_MS) s_tick_interval = TICK_MIN_MS;
        lv_timer_set_period(s_timer, s_tick_interval);
        
        // 生成新食物
        spawn_food();
        update_score_label();
    }
    
    // 重新绘制
    render_all();
}

/* ========== 页面生命周期回调 ========== */

static void snake_init(void *data)
{
    (void)data;
    ESP_LOGI(TAG, "Snake init");
    
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    
    // 状态栏
    ui_statusbar_create(scr);
    ui_statusbar_set_title("贪吃蛇");
    
    // 标题栏
    ui_titlebar_create(scr, ui_titlebar_y(), "贪吃蛇");
    
    // 分数标签（在标题栏下方）
    s_score_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_score_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_score_label, lv_font_cn_get(ui_state_get()->font_size), 0);
    lv_obj_set_pos(s_score_label, 2, ui_content_y() + 2);
    lv_label_set_text(s_score_label, "分数: 0");
    
    // 游戏区域容器
    int area_y = ui_content_y() + 14;  // 分数标签下方
    s_game_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_game_obj);
    lv_obj_set_pos(s_game_obj, 0, area_y);
    lv_obj_set_size(s_game_obj, SNAKE_GRID_W * SNAKE_CELL_SIZE, SNAKE_GRID_H * SNAKE_CELL_SIZE);
    lv_obj_clear_flag(s_game_obj, LV_OBJ_FLAG_SCROLLABLE);
    
    // 状态提示标签（居中覆盖显示）
    s_status_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status_label, lv_font_cn_get(ui_state_get()->font_size), 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    
    // 初始化游戏
    srand((unsigned int)lv_tick_get());  // 用 LVGL tick 作为随机种子
    reset_game();
    render_all();
    update_score_label();
    
    // 创建游戏循环定时器
    s_timer = lv_timer_create(snake_tick, s_tick_interval, NULL);
    lv_timer_set_repeat_count(s_timer, -1);  // 无限重复
}

static void snake_destroy(void)
{
    ESP_LOGI(TAG, "Snake destroy");
    // 删除定时器
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_game_obj = NULL;
    s_score_label = NULL;
    s_status_label = NULL;
}

static void snake_activate(void)
{
    ESP_LOGI(TAG, "Snake activate");
    // 恢复定时器
    if (s_timer && s_state == STATE_PLAYING) {
        lv_timer_resume(s_timer);
    }
}

static void snake_deactivate(void)
{
    ESP_LOGI(TAG, "Snake deactivate");
    // 暂停定时器
    if (s_timer) {
        lv_timer_pause(s_timer);
    }
}

static bool snake_on_key(int key)
{
    switch (key) {
        case KEY_UP:
            if (s_state == STATE_PLAYING && s_dir != DIR_DOWN)
                s_next_dir = DIR_UP;
            return true;
        case KEY_DOWN:
            if (s_state == STATE_PLAYING && s_dir != DIR_UP)
                s_next_dir = DIR_DOWN;
            return true;
        case KEY_LEFT:
            if (s_state == STATE_PLAYING && s_dir != DIR_RIGHT)
                s_next_dir = DIR_LEFT;
            return true;
        case KEY_RIGHT:
            if (s_state == STATE_PLAYING && s_dir != DIR_LEFT)
                s_next_dir = DIR_RIGHT;
            return true;
        case KEY_A:
            if (s_state == STATE_PLAYING) {
                // 暂停
                s_state = STATE_PAUSED;
                lv_timer_pause(s_timer);
                update_status_label();
            } else if (s_state == STATE_PAUSED) {
                // 继续
                s_state = STATE_PLAYING;
                lv_timer_resume(s_timer);
                update_status_label();
            } else if (s_state == STATE_GAMEOVER) {
                // 重新开始
                reset_game();
                lv_timer_set_period(s_timer, s_tick_interval);
                lv_timer_resume(s_timer);
                render_all();
                update_score_label();
                update_status_label();
            }
            return true;
        case KEY_B:
            if (s_state == STATE_PLAYING) {
                // 游戏中按B暂停
                s_state = STATE_PAUSED;
                lv_timer_pause(s_timer);
                update_status_label();
            } else {
                // 暂停/游戏结束状态按B退出
                if (ui_stack_depth() > 1) ui_stack_pop();
            }
            return true;
        default:
            return true;
    }
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_snake_callbacks = {
    .init = snake_init,
    .activate = snake_activate,
    .deactivate = snake_deactivate,
    .destroy = snake_destroy,
    .on_key = snake_on_key,
};