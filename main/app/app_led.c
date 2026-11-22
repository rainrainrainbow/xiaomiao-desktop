/**
 * @file app_led.c
 * @brief 灯效应用 — 控制 NeoPixel RGB LED 灯带效果
 *
 * 支持效果：
 * - 关闭 / 单色常亮（可调色） / 呼吸灯（可调色） / 彩虹渐变
 * - 流光溢彩 / 闪烁 / 音乐律动（随播放的 PCM 幅度跳动）
 *
 * 规格对齐（feature/spec-alignment）新增控制参数，均只在本文件内实现：
 * - 速度 1-10 档：缩放效果步进延时，档 5 为原速
 * - 方向 正向/反向：作用于流光(offset±1)与彩虹(色相±step)
 * - 循环次数 无限/1/3/5/10：动态效果重复到设定次数后自动停止并回到关闭
 * - 音乐律动灵敏度 1-10：UI 可设并持久化；底层律动服务当前为自适应增益，
 *   未消费该值（需配合修改 drv_led_visualizer.c 才能真正生效）
 * - 方案保存/加载：效果+颜色+亮度+速度+方向+循环+灵敏度 打包为单个 NVS blob
 * - 逐颗设色：UP/DOWN 选灯珠、LEFT/RIGHT 选颜色、A 应用 set_pixel、B 返回
 *
 * 持久化说明：不改动 app_cfg_key_t 公共枚举（本任务仅允许改本文件），
 * 参照 app_settings_datetime.c / app_settings_sleep.c 的做法，
 * 直接用 nvs_* + NVS_NAMESPACE 存本页独立 key。
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "ui_framework.h"
#include "driver/drv_led_strip.h"
#include "driver/drv_led_visualizer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "fonts/lv_freetype_font.h"
#include "lang/lang.h"
#include "system/sys_nvs.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "APP_LED";

/* ========== 效果定义 ========== */
typedef enum {
    LED_EFFECT_OFF = 0,        // 关闭
    LED_EFFECT_STATIC,         // 单色常亮
    LED_EFFECT_BREATH,         // 呼吸灯
    LED_EFFECT_RAINBOW,        // 彩虹渐变
    LED_EFFECT_FLOW,           // 流光溢彩
    LED_EFFECT_BLINK,          // 闪烁
    LED_EFFECT_MUSIC,          // 音乐律动（随播放的 PCM 幅度跳动）
    LED_EFFECT_COUNT
} led_effect_t;

/* 效果名 → lang string ID 映射 */
static const str_id_t s_effect_name_ids[LED_EFFECT_COUNT] = {
    STR_LED_EFFECT_OFF,
    STR_LED_EFFECT_STATIC,
    STR_LED_EFFECT_BREATH,
    STR_LED_EFFECT_RAINBOW,
    STR_LED_EFFECT_FLOW,
    STR_LED_EFFECT_BLINK,
    STR_LED_EFFECT_MUSIC,
};

/* 预设颜色 */
typedef struct {
    str_id_t name_id;    /* 语言ID，渲染时通过 lang_get() 获取 */
    led_rgb_t color;
} color_preset_t;

static const color_preset_t s_color_presets[] = {
    {STR_LED_COLOR_RED,    {.r=255, .g=0,   .b=0}},
    {STR_LED_COLOR_ORANGE, {.r=255, .g=128, .b=0}},
    {STR_LED_COLOR_YELLOW, {.r=255, .g=255, .b=0}},
    {STR_LED_COLOR_GREEN,  {.r=0,   .g=255, .b=0}},
    {STR_LED_COLOR_CYAN,   {.r=0,   .g=255, .b=255}},
    {STR_LED_COLOR_BLUE,   {.r=0,   .g=0,   .b=255}},
    {STR_LED_COLOR_PURPLE, {.r=128, .g=0,   .b=255}},
    {STR_LED_COLOR_PINK,   {.r=255, .g=0,   .b=128}},
    {STR_LED_COLOR_WHITE,  {.r=255, .g=255, .b=255}},
};
#define COLOR_PRESET_COUNT (sizeof(s_color_presets) / sizeof(s_color_presets[0]))

/* ========== 全局状态 ========== */
static lv_obj_t *s_led_obj = NULL;       // 列表容器
static lv_obj_t *s_info_label = NULL;    // 信息提示标签
static int s_sel = 0;                    // 选中索引
static int s_scroll = 0;                 // 滚动偏移
static int s_row_h = 16;                 // 行高

/* 当前LED状态 */
static led_effect_t s_current_effect = LED_EFFECT_OFF;
static int s_current_color = 0;          // 选中颜色预设索引
static uint8_t s_current_brightness = 128; // 当前亮度

/* 规格对齐：新增控制参数（默认值即“未设置”时的回退值） */
static int s_speed = 5;          /* 速度档 1-10，5 为原速 */
static int s_direction = 0;      /* 方向 0=正向 1=反向 */
static int s_loop_idx = 0;       /* 循环档位索引，0=无限 */
static int s_sensitivity = 5;    /* 音乐律动灵敏度 1-10（底层自动增益暂未消费） */
static int s_loop_count = 0;     /* 效果任务运行时循环计数 */
static volatile bool s_loop_finished = false; /* 本次效果是否因达到循环次数而结束 */

/* 定时开关：到期自动熄灯并回到“关闭”效果 */
static int s_timer_idx = 0;                   /* 定时档位索引 0..4，0=关闭 */
static int64_t s_timer_deadline_us = 0;       /* 到期绝对时间（esp_timer 微秒），0=未计时 */
static volatile bool s_timer_expired = false; /* 效果任务检测到定时到期 */

/* 逐颗设色子模式 */
static bool s_pixel_mode = false;
static int s_pixel_index = 0;    /* 当前灯珠编号 0..LED_STRIP_COUNT-1 */
static int s_pixel_color = 0;    /* 当前待应用的颜色预设索引 */
/* 每颗灯已应用的颜色：static 缓冲，避免任务/UI 栈上大数组 */
static led_rgb_t s_pixel_colors[LED_STRIP_COUNT];
static lv_obj_t *s_tip_label = NULL; /* 底部功耗提示标签 */

/* 效果任务相关 */
static volatile bool s_effect_running = false;
static volatile bool s_effect_stop = false;

/* 效果延时常量（毫秒） */
#define LED_DELAY_STATIC      50   /* 常亮刷新间隔 */
#define LED_DELAY_BREATH      30   /* 呼吸灯步进 */
#define LED_DELAY_RAINBOW     50   /* 彩虹渐变间隔 */
#define LED_DELAY_FLOW        30   /* 流光步进间隔 */
#define LED_DELAY_BLINK_ON    50   /* 闪烁亮持续时间 */
#define LED_DELAY_BLINK_OFF   50   /* 闪烁灭持续时间 */

/* ========== 规格对齐：新增参数的持久化定义 ==========
 * 不改动 app_cfg_key_t 枚举，用独立 NVS key（key 长度 ≤15 字符）。 */
#define LED_NVS_KEY_SPEED     "led_speed"   /* 速度 1-10 */
#define LED_NVS_KEY_DIR       "led_dir"     /* 方向 0=正向 1=反向 */
#define LED_NVS_KEY_LOOP      "led_loop"    /* 循环档位索引 0..4 */
#define LED_NVS_KEY_SENS      "led_sens"    /* 灵敏度 1-10 */
#define LED_NVS_KEY_PRESET    "led_preset"  /* 方案 blob */
#define LED_NVS_KEY_TIMER     "led_timer"   /* 定时开关档位 0..4 */

/* 底部提示区高度：信息栏 + 功耗提示两行 */
#define LED_FOOTER_H          26

/* 循环次数档位：索引 0=无限，其余为具体次数 */
static const int s_loop_options[] = {0, 1, 3, 5, 10};
#define LOOP_OPTION_COUNT (sizeof(s_loop_options) / sizeof(s_loop_options[0]))

/* 定时开关档位（分钟），0=关闭 */
static const int s_timer_options_min[] = {0, 1, 5, 15, 30};
#define TIMER_OPTION_COUNT (sizeof(s_timer_options_min) / sizeof(s_timer_options_min[0]))

/* 设置项名称（列表末尾，紧跟颜色列表之后） */
static const str_id_t s_setting_name_ids[] = {
    STR_LED_SPEED,
    STR_LED_DIRECTION,
    STR_LED_LOOP,
    STR_LED_SENSITIVITY,
    STR_LED_SCHEDULE,
    STR_LED_SAVE,
    STR_LED_LOAD,
    STR_LED_PIXEL,
};
#define SETTING_COUNT (sizeof(s_setting_name_ids) / sizeof(s_setting_name_ids[0]))

/* 行索引分区：效果 + 分隔线 + 颜色 + 分隔线 + 设置项 */
#define ROW_EFFECT_END      (LED_EFFECT_COUNT - 1)
#define ROW_SEP1            (LED_EFFECT_COUNT)
#define ROW_COLOR_START     (LED_EFFECT_COUNT + 1)
#define ROW_COLOR_END       (ROW_COLOR_START + (int)COLOR_PRESET_COUNT - 1)
#define ROW_SEP2            (ROW_COLOR_END + 1)
#define ROW_SETTING_START   (ROW_SEP2 + 1)
#define ROW_SPEED           (ROW_SETTING_START + 0)
#define ROW_DIRECTION       (ROW_SETTING_START + 1)
#define ROW_LOOP            (ROW_SETTING_START + 2)
#define ROW_SENSITIVITY     (ROW_SETTING_START + 3)
#define ROW_TIMER           (ROW_SETTING_START + 4)
#define ROW_SAVE            (ROW_SETTING_START + 5)
#define ROW_LOAD            (ROW_SETTING_START + 6)
#define ROW_PIXEL           (ROW_SETTING_START + 7)
#define TOTAL_ROWS          (ROW_SETTING_START + (int)SETTING_COUNT)

/* 方案数据结构：8 字节整体作为单个 NVS blob 存储 */
typedef struct {
    uint8_t effect;       /* led_effect_t */
    uint8_t color;        /* 颜色预设索引 0-8 */
    uint8_t brightness;   /* 0-255 */
    uint8_t speed;        /* 1-10 */
    uint8_t direction;    /* 0=正向 1=反向 */
    uint8_t loop;         /* 循环档位索引 0..4 */
    uint8_t sensitivity;  /* 1-10（底层暂未消费，随方案一并保存） */
    uint8_t reserved;
} led_preset_t;


/* ========== 颜色辅助函数 ========== */

/** HSV 转 RGB */
static led_rgb_t hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v)
{
    led_rgb_t rgb = {0, 0, 0};
    uint8_t region, remainder, p, q, t;

    if (s == 0) {
        rgb.r = rgb.g = rgb.b = v;
        return rgb;
    }

    region = h / 43;
    remainder = (h - region * 43) * 6;

    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
        case 0: rgb.r = v; rgb.g = t; rgb.b = p; break;
        case 1: rgb.r = q; rgb.g = v; rgb.b = p; break;
        case 2: rgb.r = p; rgb.g = v; rgb.b = t; break;
        case 3: rgb.r = p; rgb.g = q; rgb.b = v; break;
        case 4: rgb.r = t; rgb.g = p; rgb.b = v; break;
        default: rgb.r = v; rgb.g = p; rgb.b = q; break;
    }
    return rgb;
}

/* ========== 参数存储 / 效果调度辅助函数 ========== */

static int led_clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/** 保存整数参数到本页独立 NVS key */
static void led_nvs_save_int(const char *key, int value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, (int32_t)value);
        nvs_commit(h);
        nvs_close(h);
    }
}

/** 读取整数参数；键不存在或读取失败时返回默认值 */
static int led_nvs_load_int(const char *key, int def)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_i32(h, key, &v);
        nvs_close(h);
        if (err == ESP_OK) return (int)v;
    }
    return def;
}

/** 速度档 → 延时缩放：档 5 保持原延时，档位越高延时越短 */
static uint32_t led_scale_delay(uint32_t base_ms)
{
    int num = 11 - led_clamp_int(s_speed, 1, 10);  /* 10..1；档5=6 */
    uint32_t d = (uint32_t)((base_ms * num + 3) / 6);
    if (d < 1) d = 1;
    return d;
}

/** 可及时响应停止请求的分段延时：总时长 ms，单段 ≤50ms */
static void led_delay_responsive(uint32_t ms)
{
    while (ms > 0 && !s_effect_stop) {
        if (led_timer_check_expired()) {
            s_effect_stop = true;   /* 定时到期：请求效果任务立即收尾 */
            break;
        }
        uint32_t slice = (ms > 50) ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
}

static bool led_timer_check_expired(void);

/** 定时开关到期检查；到期置 s_timer_expired 并返回 true（档位保留供下次使用） */
static bool led_timer_check_expired(void)
{
    if (s_timer_idx <= 0 || s_timer_deadline_us == 0 || s_timer_expired) {
        return false;
    }
    if (esp_timer_get_time() >= s_timer_deadline_us) {
        s_timer_expired = true;
        s_timer_deadline_us = 0;   /* 下次 start_effect 重新计时 */
        ESP_LOGI(TAG, "LED timer expired (%d min)", s_timer_options_min[s_timer_idx]);
        return true;
    }
    return false;
}

/** 动态效果完成一个完整周期时调用；达到设定次数则请求停止 */
static void led_loop_tick(void)
{
    int limit = s_loop_options[led_clamp_int(s_loop_idx, 0, (int)LOOP_OPTION_COUNT - 1)];
    if (limit <= 0) return;              /* 无限循环 */
    if (++s_loop_count >= limit) {
        s_effect_stop = true;            /* 由任务收尾：清灯、回到关闭、退出 */
        s_loop_finished = true;
    }
}

/* ========== 效果任务函数 ========== */

/** 应用当前选中效果（在任务中运行） */
static void led_effect_task(void *arg)
{
    (void)arg;
    /* 运行/停止标志统一由 start_effect() 在创建任务前设置。任务本身绝不清除
       s_effect_stop，否则会覆盖 destroy()/start_effect() 发出的停止请求，
       导致任务无法退出、deinit 时仍在操作 RMT。 */
    /* 边界检查：确保颜色索引有效 */
    if (s_current_color < 0 || s_current_color >= (int)COLOR_PRESET_COUNT) {
        s_current_color = 0;
    }
    led_rgb_t base_color = s_color_presets[s_current_color].color;

    switch (s_current_effect) {
    case LED_EFFECT_OFF:
        drv_led_strip_clear();
        break;

    case LED_EFFECT_STATIC:
        drv_led_strip_set_all(base_color);
        drv_led_strip_refresh();
        /* 常亮：保持当前颜色直到收到停止请求（循环内刷新以响应亮度调节）。
           常亮无“动画周期”概念，循环次数对其不生效。 */
        while (!s_effect_stop) {
            led_delay_responsive(led_scale_delay(LED_DELAY_STATIC));
            if (s_effect_stop) break;
            drv_led_strip_set_all(base_color);
            drv_led_strip_refresh();
        }
        break;

    case LED_EFFECT_BREATH:
        while (!s_effect_stop) {
            /* phase 走完 0..256 即一次完整呼吸 */
            for (int phase = 0; phase < 256 && !s_effect_stop; phase += 4) {
                float rad = (float)phase * 3.14159f / 128.0f;
                uint8_t breath = (uint8_t)(128 + 127 * sinf(rad));
                led_rgb_t scaled = {
                    .r = base_color.r * breath / 255,
                    .g = base_color.g * breath / 255,
                    .b = base_color.b * breath / 255,
                };
                drv_led_strip_set_all(scaled);
                drv_led_strip_refresh();
                led_delay_responsive(led_scale_delay(LED_DELAY_BREATH));
            }
            if (s_effect_stop) break;
            led_loop_tick();   /* 一次呼吸完成 */
        }
        break;

    case LED_EFFECT_RAINBOW: {
        uint16_t hue = 0;
        int hue_step = (s_direction == 0) ? 4 : -4;   /* 方向：色相推进方向 */
        while (!s_effect_stop) {
            /* hue 走完一圈（256/4=64 步）即一次循环 */
            for (int k = 0; k < 64 && !s_effect_stop; k++) {
                for (int i = 0; i < LED_STRIP_COUNT; i++) {
                    led_rgb_t shifted = hsv_to_rgb((hue + i * 60) % 256, 255, 255);
                    drv_led_strip_set_pixel(i, shifted);
                }
                drv_led_strip_refresh();
                hue = (uint16_t)((hue + hue_step + 256) % 256);
                led_delay_responsive(led_scale_delay(LED_DELAY_RAINBOW));
            }
            if (s_effect_stop) break;
            led_loop_tick();   /* 色相走完一圈 */
        }
        break;
    }

    case LED_EFFECT_FLOW: {
        int offset = 0;
        int step = (s_direction == 0) ? 1 : -1;       /* 方向：offset+1 / offset-1 */
        while (!s_effect_stop) {
            /* offset 走完整圈（LED_STRIP_COUNT 步）即一次循环 */
            for (int pass = 0; pass < LED_STRIP_COUNT && !s_effect_stop; pass++) {
                drv_led_strip_clear();
                for (int i = 0; i < 2; i++) {
                    int idx = (offset + i * step + LED_STRIP_COUNT) % LED_STRIP_COUNT;
                    drv_led_strip_set_pixel(idx, base_color);
                }
                drv_led_strip_refresh();
                offset = (offset + step + LED_STRIP_COUNT) % LED_STRIP_COUNT;
                /* 原 150ms（5×30ms）拆成 ≤50ms 小段，及时响应停止请求 */
                led_delay_responsive(led_scale_delay(LED_DELAY_FLOW * 5));
            }
            if (s_effect_stop) break;
            led_loop_tick();   /* 流光走完整圈 */
        }
        break;
    }
    case LED_EFFECT_BLINK:
        while (!s_effect_stop) {
            drv_led_strip_set_all(base_color);
            drv_led_strip_refresh();
            led_delay_responsive(led_scale_delay(LED_DELAY_BLINK_ON * 10));  /* 原 500ms */
            if (s_effect_stop) break;
            drv_led_strip_clear();
            led_delay_responsive(led_scale_delay(LED_DELAY_BLINK_OFF * 10)); /* 原 500ms */
            if (s_effect_stop) break;
            led_loop_tick();   /* 亮+灭 = 一次循环 */
        }
        break;

    default:
        break;
    }

    drv_led_strip_clear();
    /* 达到循环次数后回到“关闭”，使状态与已熄灭的实际灯效一致 */
    if (s_loop_finished) s_current_effect = LED_EFFECT_OFF;
    /* 定时开关到期：强制回到“关闭”，与已熄灭的灯一致 */
    if (s_timer_expired) s_current_effect = LED_EFFECT_OFF;
    s_effect_running = false;
    vTaskDelete(NULL);
}

/** 启动当前效果 */
static void start_effect(void)
{
    /* 已有任务在运行：先请求停止并等待其真正退出，避免新旧任务并发操作 RMT。
       各效果的内层循环都检查 s_effect_stop 且延时被拆成 <=50ms 的小段，
       因此 200ms 足以让旧任务退出。 */
    if (s_effect_running) {
        s_effect_stop = true;
        for (int i = 0; i < 20 && s_effect_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_effect_running) {
            /* 旧任务仍未退出：放弃本次重启，绝不带病创建新任务，
               否则会出现两个任务同时写 LED/RMT 的严重并发问题。 */
            ESP_LOGW(TAG, "old effect task still running, skip restart");
            return;
        }
    }

    /* 初始化 LED（如果未初始化） */
    drv_led_strip_init();
    drv_led_strip_set_brightness(s_current_brightness);

    /* 定时开关：有档位时重新武装计时（到期后档位保留，下次启动重新计时） */
    if (s_timer_idx > 0) {
        s_timer_expired = false;
        if (s_timer_deadline_us == 0) {
            int tmin = s_timer_options_min[led_clamp_int(s_timer_idx, 1, (int)TIMER_OPTION_COUNT - 1)];
            s_timer_deadline_us = esp_timer_get_time() + (int64_t)tmin * 60LL * 1000000LL;
            ESP_LOGI(TAG, "LED timer armed: %d min", tmin);
        }
    }

    /* "音乐律动"效果：不启动本地效果任务，而是交给常驻后台律动服务接管。
     * 把当前选中颜色同步给服务，并设置 busy=false 让服务可以写灯。 */
    if (s_current_effect == LED_EFFECT_MUSIC) {
        led_visualizer_set_color(s_current_color);
        /* 若服务当前处于关闭态，启用单色律动；否则保留用户已选的律动模式 */
        if (led_visualizer_get_mode() == LED_VIS_MODE_OFF) {
            led_visualizer_set_mode(LED_VIS_MODE_SINGLE);
        }
        led_visualizer_set_busy(false);
        led_visualizer_set_sensitivity(s_sensitivity);   /* 把当前灵敏度同步给底层律动 */
        /* 灵敏度已存 NVS 并在 UI 可见，底层律动服务经 set_sensitivity 消费该值影响律动幅度。 */
        ESP_LOGI(TAG, "Music visualizer effect selected (mode=%d color=%d sens=%d)",
                 led_visualizer_get_mode(), s_current_color, s_sensitivity);
        return;   /* 无本地任务，s_effect_running 保持 false */
    }

    /* 手动效果：通知后台律动服务让位，避免两处并发写同一 RMT */
    led_visualizer_set_busy(true);

    /* 先置位运行标志再创建任务，封堵"任务已创建但尚未真正运行"的竞态窗口：
       否则用户快速连续切换时，新任务还没开始跑、s_effect_running 仍为 false，
       下一次 start_effect() 不会等待，从而创建出多个并发任务。 */
    s_effect_stop = false;
    s_loop_count = 0;
    s_loop_finished = false;
    s_effect_running = true;

    /* 启动新效果任务；创建失败时一次性设置当前颜色（安全兜底） */
    BaseType_t rt = xTaskCreate(led_effect_task, "led_effect", 3072, NULL, 5, NULL);
    if (rt != pdPASS) {
        s_effect_running = false;
        led_visualizer_set_busy(false);   /* 任务没起来，归还灯带给律动服务 */
        ESP_LOGE(TAG, "xTaskCreate(led_effect) failed! setting color once");
        /* 边界检查：确保颜色索引有效 */
        if (s_current_color < 0 || s_current_color >= (int)COLOR_PRESET_COUNT) {
            s_current_color = 0;
        }
        led_rgb_t c = s_color_presets[s_current_color].color;
        if (s_current_effect != LED_EFFECT_OFF) {
            drv_led_strip_set_all(c);
            drv_led_strip_refresh();
        } else {
            drv_led_strip_clear();
        }
    }
}

/* ========== UI 相关 ========== */

/** 获取当前页面行数（扣除底部信息栏与功耗提示占用高度） */
static int get_visible_rows(void)
{
    int vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H - LED_FOOTER_H) / s_row_h;
    if (vis_rows < 1) vis_rows = 1;
    return vis_rows;
}

/** 逐颗设色子模式的行渲染（s_led_obj 已由调用者清空） */
static void led_rebuild_pixel(int font_px, const theme_colors_t *colors)
{
    for (int i = 0; i < LED_STRIP_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(s_led_obj);
        if (!row) continue;
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_pixel_index) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }

        /* 该颗灯已应用的颜色小方块 */
        lv_obj_t *box = lv_obj_create(row);
        if (box) {
            lv_obj_remove_style_all(box);
            lv_obj_set_size(box, font_px - 2, font_px - 2);
            lv_obj_set_pos(box, 2, 1);
            lv_obj_set_style_bg_color(box, lv_color_hex(
                (s_pixel_colors[i].r << 16) |
                (s_pixel_colors[i].g << 8) |
                s_pixel_colors[i].b), 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(box, 2, 0);
        }

        /* 编号 + 当前待应用的颜色名 */
        lv_obj_t *lbl = lv_label_create(row);
        if (lbl) {
            char t[48];
            snprintf(t, sizeof(t), "LED %d  %s", i,
                     lang_get(s_color_presets[s_pixel_color].name_id));
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, font_px + 4, 0);
            lv_label_set_text(lbl, t);
        }
    }

    if (s_info_label) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s  %s:%s  A:%s  B:%s",
                 lang_get(STR_LED_PIXEL),
                 lang_get(STR_LED_COLOR_SEP),
                 lang_get(s_color_presets[s_pixel_color].name_id),
                 lang_get(STR_COMMON_CONFIRM),
                 lang_get(STR_BACK));
        lv_label_set_text(s_info_label, buf);
    }
}

/** 重新构建可见行 */
static void led_rebuild_visible(void)
{
    if (!s_led_obj) return;

    lv_obj_clean(s_led_obj);

    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_row_h = font_px + 2;
    if (s_row_h < 16) s_row_h = 16;

    /* 逐颗设色子模式：单独渲染 3 颗灯 */
    if (s_pixel_mode) {
        led_rebuild_pixel(font_px, colors);
        return;
    }

    int vis_rows = get_visible_rows();
    int total_rows = TOTAL_ROWS;

    /* 确保选中项可见 */
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + vis_rows) s_scroll = s_sel - vis_rows + 1;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > total_rows - vis_rows) s_scroll = total_rows - vis_rows;
    if (s_scroll < 0) s_scroll = 0;

    int row_idx = 0;

    /* ---- 效果列表 ---- */
    for (int i = 0; i < LED_EFFECT_COUNT; i++) {
        if (row_idx < s_scroll || row_idx >= s_scroll + vis_rows) {
            row_idx++;
            continue;
        }
        int vis_i = row_idx - s_scroll;
        lv_obj_t *row = lv_obj_create(s_led_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(effect_row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            row_idx++;
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, vis_i * s_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        if (i == s_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(effect_lbl) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            row_idx++;
            continue;
        }
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 50);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 2, 0);
        lv_label_set_text(lbl, lang_get(s_effect_name_ids[i]));
        /* 当前效果标记 */
        if (s_current_effect == i) {
            lv_obj_t *mark = lv_label_create(row);
            if (mark) {
                lv_obj_set_style_text_font(mark, lv_font_cn_get(font_px), 0);
                lv_obj_set_style_text_color(mark, lv_color_hex(0x6FA891), 0);
                lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -2, 0);
                lv_label_set_text(mark, LV_SYMBOL_OK);
            }
        }
        row_idx++;
    }

    /* ---- 分隔线 ---- */
    if (row_idx >= s_scroll && row_idx < s_scroll + vis_rows) {
        lv_obj_t *sep = lv_label_create(s_led_obj);
        if (sep) {
            lv_obj_set_style_text_font(sep, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(sep, lv_color_hex(colors->text_dim), 0);
            lv_obj_set_pos(sep, 2, (row_idx - s_scroll) * s_row_h);
            lv_label_set_text(sep, lang_get(STR_LED_COLOR_SEP));
        }
    }
    row_idx++;

    /* ---- 颜色列表 ---- */
    for (int i = 0; i < (int)COLOR_PRESET_COUNT; i++) {
        if (row_idx < s_scroll || row_idx >= s_scroll + vis_rows) {
            row_idx++;
            continue;
        }
        int vis_i = row_idx - s_scroll;
        lv_obj_t *row = lv_obj_create(s_led_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(color_row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            row_idx++;
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, vis_i * s_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (i == s_sel - ROW_COLOR_START) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
        /* 颜色小方块 */
        lv_obj_t *color_box = lv_obj_create(row);
        if (color_box) {
            lv_obj_remove_style_all(color_box);
            lv_obj_set_size(color_box, font_px - 2, font_px - 2);
            lv_obj_set_pos(color_box, 2, 1);
            lv_obj_set_style_bg_color(color_box, lv_color_hex(
                (s_color_presets[i].color.r << 16) |
                (s_color_presets[i].color.g << 8) |
                s_color_presets[i].color.b), 0);
            lv_obj_set_style_bg_opa(color_box, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(color_box, 2, 0);
        }

        /* 颜色名（通过 lang_get 国际化） */
        lv_obj_t *lbl = lv_label_create(row);
        if (!lbl) {
            ESP_LOGE(TAG, "lv_label_create(color_lbl) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            row_idx++;
            continue;
        }
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
        lv_obj_set_x(lbl, font_px + 4);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, font_px + 4, 0);
        lv_label_set_text(lbl, lang_get(s_color_presets[i].name_id));
        if (s_current_color == i) {
            lv_obj_t *mark = lv_label_create(row);
            if (mark) {
                lv_obj_set_style_text_font(mark, lv_font_cn_get(font_px), 0);
                lv_obj_set_style_text_color(mark, lv_color_hex(0x6FA891), 0);
                lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -2, 0);
                lv_label_set_text(mark, LV_SYMBOL_OK);
            }
        }
        row_idx++;
    }

    /* ---- 第二分隔线：设置区 ---- */
    if (row_idx >= s_scroll && row_idx < s_scroll + vis_rows) {
        lv_obj_t *sep = lv_label_create(s_led_obj);
        if (sep) {
            lv_obj_set_style_text_font(sep, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(sep, lv_color_hex(colors->text_dim), 0);
            lv_obj_set_pos(sep, 2, (row_idx - s_scroll) * s_row_h);
            lv_label_set_text(sep, lang_get(STR_SETTINGS));
        }
    }
    row_idx++;

    /* ---- 新增设置项列表 ---- */
    for (int i = 0; i < (int)SETTING_COUNT; i++) {
        int row = ROW_SETTING_START + i;
        if (row < s_scroll || row >= s_scroll + vis_rows) continue;
        int vis_i = row - s_scroll;
        lv_obj_t *r = lv_obj_create(s_led_obj);
        if (!r) continue;
        lv_obj_remove_style_all(r);
        lv_obj_set_pos(r, 0, vis_i * s_row_h);
        lv_obj_set_size(r, LCD_H_RES, s_row_h);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        if (s_sel == row) {
            lv_obj_set_style_bg_color(r, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        }

        lv_obj_t *lbl = lv_label_create(r);
        if (lbl) {
            lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_width(lbl, LCD_H_RES - 50);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 2, 0);
            lv_label_set_text(lbl, lang_get(s_setting_name_ids[i]));
        }

        /* 右侧显示当前值：速度/方向/循环/灵敏度有值，保存/加载/逐颗设色无值 */
        char valbuf[16];
        const char *val = NULL;
        switch (i) {
        case 0:  /* 速度 */
            snprintf(valbuf, sizeof(valbuf), "%d/10", s_speed);
            val = valbuf;
            break;
        case 1:  /* 方向 */
            val = (s_direction == 0) ? ">" : "<";
            break;
        case 2: {  /* 循环：0=无限 */
            int n = s_loop_options[led_clamp_int(s_loop_idx, 0, (int)LOOP_OPTION_COUNT - 1)];
            if (n == 0) {
                val = "INF";
            } else {
                snprintf(valbuf, sizeof(valbuf), "%d", n);
                val = valbuf;
            }
            break;
        }
        case 3:  /* 灵敏度 */
            snprintf(valbuf, sizeof(valbuf), "%d/10", s_sensitivity);
            val = valbuf;
            break;
        case 4: {  /* 定时开关：0=关闭，其余显示分钟数 */
            int tmin = s_timer_options_min[led_clamp_int(s_timer_idx, 0, (int)TIMER_OPTION_COUNT - 1)];
            if (tmin == 0) {
                val = lang_get(STR_LED_VIS_OFF);
            } else {
                snprintf(valbuf, sizeof(valbuf), "%dmin", tmin);
                val = valbuf;
            }
            break;
        }
        default:
            break;
        }
        if (val) {
            lv_obj_t *vlbl = lv_label_create(r);
            if (vlbl) {
                lv_obj_set_style_text_font(vlbl, lv_font_cn_get(font_px), 0);
                lv_obj_set_style_text_color(vlbl, lv_color_hex(colors->text_dim), 0);
                lv_obj_align(vlbl, LV_ALIGN_RIGHT_MID, -2, 0);
                lv_label_set_text(vlbl, val);
            }
        }
    }

    /* 更新信息标签：亮度 + 关键参数 + 操作提示（功耗提示见独立 tip 标签） */
    if (s_info_label) {
        char buf[96];
        char loop_s[8];
        int loop_n = s_loop_options[led_clamp_int(s_loop_idx, 0, (int)LOOP_OPTION_COUNT - 1)];
        if (loop_n == 0) snprintf(loop_s, sizeof(loop_s), "INF");
        else snprintf(loop_s, sizeof(loop_s), "%d", loop_n);
        snprintf(buf, sizeof(buf), "%s:%d%% %s:%d %s:%s %s:%d  A:%s B:%s",
                 lang_get(STR_LED_BRIGHTNESS), s_current_brightness * 100 / 255,
                 lang_get(STR_LED_SPEED), s_speed,
                 lang_get(STR_LED_LOOP), loop_s,
                 lang_get(STR_LED_SENSITIVITY), s_sensitivity,
                 lang_get(STR_MUSIC_PLAY),   /* 复用"播放"→"应用" */
                 lang_get(STR_BACK));
        lv_label_set_text(s_info_label, buf);
    }
}

/* ========== 页面生命周期回调 ========== */

static void led_init(void *data)
{
    (void)data;
    ESP_LOGI(TAG, "LED effect app init");

    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_APP_LED));

    s_row_h = ui_state_get()->font_size + 2;
    if (s_row_h < 16) s_row_h = 16;

    /* 列表容器 */
    s_led_obj = lv_obj_create(scr);
    if (!s_led_obj) {
        ESP_LOGE(TAG, "lv_obj_create(led_obj) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_led_obj);
    lv_obj_set_pos(s_led_obj, 0, ui_content_y());
    lv_obj_set_size(s_led_obj, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H - LED_FOOTER_H);
    lv_obj_clear_flag(s_led_obj, LV_OBJ_FLAG_SCROLLABLE);
    /* 底部信息栏 */
    s_info_label = lv_label_create(scr);
    if (!s_info_label) {
        ESP_LOGE(TAG, "lv_label_create(info_label) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    } else {
        lv_obj_set_style_text_font(s_info_label, lv_font_cn_get(ui_state_get()->font_size), 0);
        lv_obj_set_style_text_color(s_info_label, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_width(s_info_label, LCD_H_RES - 4);
        lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_pos(s_info_label, 2, LCD_V_RES - DOCK_H - 12);
    }

    /* 底部功耗提示：信息栏上方一行 */
    s_tip_label = lv_label_create(scr);
    if (!s_tip_label) {
        ESP_LOGE(TAG, "lv_label_create(tip_label) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    } else {
        /* 固定小字号，避免大字体时与下方信息栏/导航栏重叠 */
        lv_obj_set_style_text_font(s_tip_label, lv_font_cn_get(14), 0);
        lv_obj_set_style_text_color(s_tip_label, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_width(s_tip_label, LCD_H_RES - 4);
        lv_label_set_long_mode(s_tip_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_pos(s_tip_label, 2, LCD_V_RES - DOCK_H - LED_FOOTER_H);
        lv_label_set_text(s_tip_label, lang_get(STR_LED_POWER_TIP));
    }

    /* 底部导航栏 */
    ui_dock_create(scr, 1, 0);

    /* 初始化 LED 驱动 */
    drv_led_strip_init();
    s_current_brightness = drv_led_strip_get_brightness();

    /* 从 NVS 恢复规格对齐参数（含边界校验，防止脏数据导致越界） */
    s_speed = led_clamp_int(led_nvs_load_int(LED_NVS_KEY_SPEED, 5), 1, 10);
    s_direction = led_clamp_int(led_nvs_load_int(LED_NVS_KEY_DIR, 0), 0, 1);
    s_loop_idx = led_clamp_int(led_nvs_load_int(LED_NVS_KEY_LOOP, 0), 0, (int)LOOP_OPTION_COUNT - 1);
    s_sensitivity = led_clamp_int(led_nvs_load_int(LED_NVS_KEY_SENS, 5), 1, 10);
    s_timer_idx = led_clamp_int(led_nvs_load_int(LED_NVS_KEY_TIMER, 0), 0, (int)TIMER_OPTION_COUNT - 1);
    /* 启动时把灵敏度同步给底层律动服务（visualizer 独立 NVS 也存一份，
     * 二者同源值保持一致，避免律动幅度与用户设定偏离）。 */
    led_visualizer_set_sensitivity(s_sensitivity);

    /* 复位逐颗设色子模式 */
    s_pixel_mode = false;
    s_pixel_index = 0;
    s_pixel_color = led_clamp_int(s_current_color, 0, (int)COLOR_PRESET_COUNT - 1);
    for (int i = 0; i < LED_STRIP_COUNT; i++) {
        s_pixel_colors[i] = (led_rgb_t){0, 0, 0};
    }

    s_sel = (int)s_current_effect;
    s_scroll = 0;
    led_rebuild_visible();
}

static void led_destroy(void)
{
    ESP_LOGI(TAG, "LED app destroy");

    /* 停止效果任务并等待其真正退出，避免 deinit 释放 RMT 后仍有任务在刷新。
       各效果内层循环均检查停止标志且延时被拆成 <=50ms 小段，200ms 足够。 */
    s_effect_stop = true;
    if (s_effect_running) {
        for (int i = 0; i < 20 && s_effect_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_effect_running) {
            ESP_LOGW(TAG, "effect task did not exit before deinit");
        }
    }

    /* 彻底熄灭并释放 LED（RMT 通道） */
    drv_led_strip_deinit();

    /* 复位标志，为下次进入 app 做准备 */
    s_effect_running = false;
    s_effect_stop = false;
    s_loop_finished = false;
    s_pixel_mode = false;

    /* 归还灯带给后台律动服务：退出 LED app 后若设置了律动模式则自动恢复 */
    led_visualizer_set_busy(false);

    s_led_obj = NULL;
    s_info_label = NULL;
    s_tip_label = NULL;
}

static void led_activate(void)
{
    ESP_LOGI(TAG, "LED app activate");
    led_rebuild_visible();
}

/* ========== 方案保存/加载、逐颗设色 ========== */

/** 保存当前参数组合到 NVS（单 slot blob） */
static void led_preset_save(void)
{
    led_preset_t p = {
        .effect      = (uint8_t)s_current_effect,
        .color       = (uint8_t)led_clamp_int(s_current_color, 0, (int)COLOR_PRESET_COUNT - 1),
        .brightness  = s_current_brightness,
        .speed       = (uint8_t)s_speed,
        .direction   = (uint8_t)s_direction,
        .loop        = (uint8_t)s_loop_idx,
        .sensitivity = (uint8_t)s_sensitivity,
        .reserved    = 0,
    };
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t err = nvs_set_blob(h, LED_NVS_KEY_PRESET, &p, sizeof(p));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "save preset %s", (err == ESP_OK) ? "ok" : "failed");
    }
}

/** 加载方案 blob：逐字段校验后恢复并立即生效 */
static void led_preset_load(void)
{
    led_preset_t p;
    size_t len = sizeof(p);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no preset saved yet");
        return;
    }
    esp_err_t err = nvs_get_blob(h, LED_NVS_KEY_PRESET, &p, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(p)) {
        ESP_LOGW(TAG, "load preset failed (err=%d len=%u)", (int)err, (unsigned)len);
        return;
    }

    if (p.effect < (uint8_t)LED_EFFECT_COUNT) s_current_effect = (led_effect_t)p.effect;
    if (p.color < (uint8_t)COLOR_PRESET_COUNT) s_current_color = p.color;
    s_current_brightness = p.brightness ? p.brightness : 128;
    s_speed = led_clamp_int(p.speed, 1, 10);
    s_direction = led_clamp_int(p.direction, 0, 1);
    s_loop_idx = led_clamp_int(p.loop, 0, (int)LOOP_OPTION_COUNT - 1);
    s_sensitivity = led_clamp_int(p.sensitivity, 1, 10);

    /* 同步单项 NVS，保证下次进入页面时显示一致 */
    led_nvs_save_int(LED_NVS_KEY_SPEED, s_speed);
    led_nvs_save_int(LED_NVS_KEY_DIR, s_direction);
    led_nvs_save_int(LED_NVS_KEY_LOOP, s_loop_idx);
    led_nvs_save_int(LED_NVS_KEY_SENS, s_sensitivity);

    drv_led_strip_set_brightness(s_current_brightness);
    led_visualizer_set_sensitivity(s_sensitivity);   /* 方案加载同步底层律动灵敏度 */
    start_effect();
    ESP_LOGI(TAG, "preset loaded (effect=%d color=%d)", s_current_effect, s_current_color);
}

/** 进入逐颗设色：停掉手动效果任务并占用灯带，避免被并发刷新覆盖 */
static void led_pixel_enter(void)
{
    if (s_effect_running) {
        s_effect_stop = true;
        for (int i = 0; i < 20 && s_effect_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_effect_running) {
            ESP_LOGW(TAG, "effect task still running, pixel mode may flicker");
        }
    }
    led_visualizer_set_busy(true);   /* 占用灯带，律动服务让位 */
    s_pixel_mode = true;
    s_pixel_index = 0;
    s_pixel_color = led_clamp_int(s_current_color, 0, (int)COLOR_PRESET_COUNT - 1);
    for (int i = 0; i < LED_STRIP_COUNT; i++) {
        s_pixel_colors[i] = (led_rgb_t){0, 0, 0};
    }
    drv_led_strip_clear();           /* 清屏表示进入独立设色模式 */
    ESP_LOGI(TAG, "enter pixel mode (count=%d)", LED_STRIP_COUNT);
}

static bool led_on_key(int key)
{
    /* ---- 逐颗设色子模式：独立按键映射 ---- */
    if (s_pixel_mode) {
        switch (key) {
        case KEY_UP:
            if (s_pixel_index > 0) s_pixel_index--;
            led_rebuild_visible();
            return true;

        case KEY_DOWN:
            if (s_pixel_index < LED_STRIP_COUNT - 1) s_pixel_index++;
            led_rebuild_visible();
            return true;

        case KEY_LEFT:
            s_pixel_color = (s_pixel_color - 1 + (int)COLOR_PRESET_COUNT) % (int)COLOR_PRESET_COUNT;
            led_rebuild_visible();
            return true;

        case KEY_RIGHT:
            s_pixel_color = (s_pixel_color + 1) % (int)COLOR_PRESET_COUNT;
            led_rebuild_visible();
            return true;

        case KEY_A:
            /* 应用：逐颗写入底层驱动 */
            drv_led_strip_set_pixel(s_pixel_index, s_color_presets[s_pixel_color].color);
            drv_led_strip_refresh();
            s_pixel_colors[s_pixel_index] = s_color_presets[s_pixel_color].color;
            led_rebuild_visible();
            return true;

        case KEY_B:
            /* 退出子模式：保持已设置的逐颗颜色，律动服务继续让位；
               之后按 A 选任意效果即可切换回常规灯效。 */
            s_pixel_mode = false;
            s_current_effect = LED_EFFECT_OFF;
            led_rebuild_visible();
            return true;

        default:
            return true;
        }
    }

    /* ---- 主列表 ---- */
    switch (key) {
        case KEY_UP:
            if (s_sel > 0) {
                s_sel--;
                led_rebuild_visible();
            }
            return true;

        case KEY_DOWN:
            if (s_sel < TOTAL_ROWS - 1) {
                s_sel++;
                led_rebuild_visible();
            }
            return true;

        case KEY_LEFT:
            /* 设置行调整参数，其余行仍为亮度减 */
            if (s_sel == ROW_SPEED) {
                s_speed = led_clamp_int(s_speed - 1, 1, 10);
                led_nvs_save_int(LED_NVS_KEY_SPEED, s_speed);
            } else if (s_sel == ROW_DIRECTION) {
                s_direction = (s_direction == 0) ? 1 : 0;
                led_nvs_save_int(LED_NVS_KEY_DIR, s_direction);
            } else if (s_sel == ROW_LOOP) {
                s_loop_idx = (s_loop_idx - 1 + (int)LOOP_OPTION_COUNT) % (int)LOOP_OPTION_COUNT;
                led_nvs_save_int(LED_NVS_KEY_LOOP, s_loop_idx);
            } else if (s_sel == ROW_SENSITIVITY) {
                s_sensitivity = led_clamp_int(s_sensitivity - 1, 1, 10);
                led_nvs_save_int(LED_NVS_KEY_SENS, s_sensitivity);
                led_visualizer_set_sensitivity(s_sensitivity);   /* 同步底层律动增益 */
            } else if (s_sel == ROW_TIMER) {
                s_timer_idx = (s_timer_idx - 1 + (int)TIMER_OPTION_COUNT) % (int)TIMER_OPTION_COUNT;
                led_nvs_save_int(LED_NVS_KEY_TIMER, s_timer_idx);
                s_timer_deadline_us = 0;   /* 档位变更后下次启动重新计时 */
                s_timer_expired = false;
            } else if (s_current_brightness > 20) {
                s_current_brightness -= 20;
                drv_led_strip_set_brightness(s_current_brightness);
            }
            led_rebuild_visible();
            return true;

        case KEY_RIGHT:
            /* 设置行调整参数，其余行仍为亮度加 */
            if (s_sel == ROW_SPEED) {
                s_speed = led_clamp_int(s_speed + 1, 1, 10);
                led_nvs_save_int(LED_NVS_KEY_SPEED, s_speed);
            } else if (s_sel == ROW_DIRECTION) {
                s_direction = (s_direction == 0) ? 1 : 0;
                led_nvs_save_int(LED_NVS_KEY_DIR, s_direction);
            } else if (s_sel == ROW_LOOP) {
                s_loop_idx = (s_loop_idx + 1) % (int)LOOP_OPTION_COUNT;
                led_nvs_save_int(LED_NVS_KEY_LOOP, s_loop_idx);
            } else if (s_sel == ROW_SENSITIVITY) {
                s_sensitivity = led_clamp_int(s_sensitivity + 1, 1, 10);
                led_nvs_save_int(LED_NVS_KEY_SENS, s_sensitivity);
                led_visualizer_set_sensitivity(s_sensitivity);   /* 同步底层律动增益 */
            } else if (s_sel == ROW_TIMER) {
                s_timer_idx = (s_timer_idx + 1) % (int)TIMER_OPTION_COUNT;
                led_nvs_save_int(LED_NVS_KEY_TIMER, s_timer_idx);
                s_timer_deadline_us = 0;   /* 档位变更后下次启动重新计时 */
                s_timer_expired = false;
            } else if (s_current_brightness < 240) {
                s_current_brightness += 20;
                drv_led_strip_set_brightness(s_current_brightness);
            }
            led_rebuild_visible();
            return true;

        case KEY_A: {
            if (s_sel <= ROW_EFFECT_END) {
                /* 选择效果 */
                s_current_effect = (led_effect_t)s_sel;
                start_effect();
            } else if (s_sel >= ROW_COLOR_START && s_sel <= ROW_COLOR_END) {
                /* 选择颜色 */
                s_current_color = s_sel - ROW_COLOR_START;
                if (s_current_effect != LED_EFFECT_OFF) {
                    start_effect();   /* 当前有效果运行则重新应用 */
                }
            } else if (s_sel == ROW_SAVE) {
                led_preset_save();
            } else if (s_sel == ROW_LOAD) {
                led_preset_load();
            } else if (s_sel == ROW_PIXEL) {
                led_pixel_enter();
            }
            /* 速度/方向/循环/灵敏度行由左右键调整，A 无需额外操作 */
            led_rebuild_visible();
            return true;
        }

        case KEY_B:
            if (ui_stack_depth() > 1) ui_stack_pop();
            return true;

        default:
            return true;
    }
}

/* ========== 页面回调定义 ========== */
const page_callbacks_t g_led_callbacks = {
    .init = led_init,
    .activate = led_activate,
    .destroy = led_destroy,
    .on_key = led_on_key,
};
