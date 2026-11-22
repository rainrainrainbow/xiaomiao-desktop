/**
 * @file drv_led_visualizer.c
 * @brief LED 音乐律动服务实现
 *
 * 数据源：drv_audio_decoder 的 audio_decoder_get_live_peak()（每次 read 更新）。
 * 由于部分歌曲母带响度极低（peak 只有 8~27，上轮已确认是文件本身问题），
 * 采用"自适应增益"：跟踪近期峰值上限 auto_gain，把当前峰值归一化到 0~1，
 * 使低响度歌曲同样能驱动明显律动（否则这些歌灯几乎不动）。
 *
 * 包络跟随：attack 快（约 60ms）、release 慢（约 250ms），视觉更像"跳动"
 * 而非抖动。节拍脉冲模式另做峰值检测，检测到一个上升沿后触发一次泛白闪光。
 */
#include "drv_led_visualizer.h"
#include "drv_led_strip.h"
#include "drv_audio_decoder.h"
#include "system/sys_nvs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "LED_VIS";

/* 律动基色调色板（与 app_led.c 的 s_color_presets 顺序一致） */
static const led_rgb_t s_vis_palette[] = {
    {.r=255, .g=0,   .b=0},     /* 红 */
    {.r=255, .g=128, .b=0},     /* 橙 */
    {.r=255, .g=255, .b=0},     /* 黄 */
    {.r=0,   .g=255, .b=0},     /* 绿 */
    {.r=0,   .g=255, .b=255},   /* 青 */
    {.r=0,   .g=0,   .b=255},   /* 蓝 */
    {.r=128, .g=0,   .b=255},   /* 紫 */
    {.r=255, .g=0,   .b=128},   /* 粉 */
    {.r=255, .g=255, .b=255},   /* 白 */
};
#define VIS_PALETTE_COUNT (sizeof(s_vis_palette) / sizeof(s_vis_palette[0]))

/* ========== 内部状态 ========== */
static int  s_mode = LED_VIS_MODE_OFF;   /* 当前模式 */
static int  s_color = 4;                  /* 当前基色预设索引（默认青） */
static int  s_sens = 5;                   /* 律动灵敏度 1~10（默认 5） */
static volatile bool s_busy = false;      /* LED app 手动效果占用中 */
static bool s_started = false;
static TaskHandle_t s_task = NULL;

#define VIS_NVS_NAMESPACE "led_vis"
#define VIS_NVS_KEY_SENS  "sens"

/* ========== 颜色辅助 ========== */
static led_rgb_t hsv_to_rgb(uint16_t h, uint8_t sat, uint8_t v)
{
    led_rgb_t rgb = {0, 0, 0};
    if (sat == 0) { rgb.r = rgb.g = rgb.b = v; return rgb; }
    uint8_t region = h / 43;
    uint8_t remainder = (uint8_t)((h - region * 43) * 6);
    uint8_t p = (uint8_t)((uint32_t)v * (255 - sat) >> 8);
    uint8_t q = (uint8_t)((uint32_t)v * (255 - ((uint32_t)sat * remainder >> 8)) >> 8);
    uint8_t t = (uint8_t)((uint32_t)v * (255 - ((uint32_t)sat * (255 - remainder) >> 8)) >> 8);
    switch (region) {
        case 0: rgb.r = v; rgb.g = t; rgb.b = p; break;
        case 1: rgb.r = q; rgb.g = v; rgb.b = p; break;
        case 2: rgb.r = p; rgb.g = v; rgb.b = t; break;
        case 3: rgb.r = p; rgb.g = q; rgb.b = v; break;
        case 4: rgb.r = t; rgb.g = p; rgb.b = v; break;
        default:rgb.r = v; rgb.g = p; rgb.b = q; break;
    }
    return rgb;
}

/* 把两颜色按比例混合 */
static led_rgb_t blend(led_rgb_t a, led_rgb_t b, int a_weight)
{
    if (a_weight < 0) a_weight = 0;
    if (a_weight > 255) a_weight = 255;
    led_rgb_t o;
    o.r = (uint8_t)(((uint32_t)a.r * a_weight + (uint32_t)b.r * (255 - a_weight)) / 255);
    o.g = (uint8_t)(((uint32_t)a.g * a_weight + (uint32_t)b.g * (255 - a_weight)) / 255);
    o.b = (uint8_t)(((uint32_t)a.b * a_weight + (uint32_t)b.b * (255 - a_weight)) / 255);
    return o;
}

/* ========== 律动任务 ========== */
static void led_visualizer_task(void *arg)
{
    (void)arg;

    /* 自适应增益：近期峰值上限（缓慢衰减），用于归一化低响度歌曲 */
    float auto_gain = 3000.0f;     /* 初始参考峰值 */
    float env = 0.0f;              /* 平滑包络 0~1 */
    float env_prev = 0.0f;         /* 上一拍包络，用于上升沿检测 */
    int pulse = 0;                 /* 泛白脉冲剩余帧数 */
    uint16_t hue = 0;              /* 彩虹色相 */
    const TickType_t period = pdMS_TO_TICKS(30);   /* ~33Hz 刷新 */
    int idle_frames = 0;           /* 连续静音帧计数 */
    bool cleared = false;          /* OFF 模式是否已熄灭过 */

    while (1) {
        vTaskDelay(period);

        /* LED app 手动效果占用：完全让位，不触碰灯带 */
        if (s_busy) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 模式关闭：熄灭后低功耗等待 */
        if (s_mode == LED_VIS_MODE_OFF) {
            if (!cleared) {
                drv_led_strip_clear();
                drv_led_strip_refresh();
                cleared = true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        cleared = false;   /* 从 OFF 切回：允许下次 OFF 时重新熄灭 */

        uint16_t peak = audio_decoder_get_live_peak();

        /* 自适应增益：
         * - 峰值高于当前增益：快速跟上（攻击）
         * - 峰值低于增益：缓慢衰减（释放），避免静音段把增益拉太低 */
        if (peak > auto_gain) {
            auto_gain = auto_gain * 0.6f + (float)peak * 0.4f;
        } else {
            auto_gain *= 0.995f;
        }
        if (auto_gain < 800.0f) auto_gain = 800.0f;   /* 防止除零/过度放大噪声 */

        /* 灵敏度增益：1~10 映射到 0.6~1.8 倍率。
         * 灵敏度越高（10），倍率越大，同等音量下 level 越高、律动越明显；
         * 灵敏度 1 最克制。倍率同时放大归一化结果，可越界后由下方钳位。 */
        float sens_gain = 0.6f + (float)(s_sens - 1) * (1.2f / 9.0f);   /* 1→0.6, 10→1.8 */

        /* 归一化幅度 0~1（留 15% 头room，避免满幅一直顶格；叠加灵敏度增益） */
        float level = (float)peak / (auto_gain * 1.15f) * sens_gain;
        if (level > 1.0f) level = 1.0f;
        if (level < 0.0f) level = 0.0f;

        /* 包络跟随：上升快、下降慢 */
        if (level > env) {
            env += (level - env) * 0.5f;    /* attack ~60ms */
        } else {
            env += (level - env) * 0.12f;   /* release ~250ms */
        }

        /* 峰值上升沿检测（用于节拍脉冲）：包络明显上升且超过阈值 */
        if ((env - env_prev) > 0.06f && env > 0.35f) {
            pulse = 6;   /* ~180ms 泛白闪光 */
        }
        env_prev = env;

        /* 静音判定：峰值连续为 0 一段时间则渐灭、增益复位 */
        if (peak == 0) {
            idle_frames++;
            if (idle_frames > 20) {          /* 约 600ms 无数据 */
                env *= 0.85f;
                auto_gain = auto_gain * 0.98f;
            }
        } else {
            idle_frames = 0;
        }

        /* ---- 按模式渲染 ---- */
        int brightness = 12 + (int)(env * 243.0f);   /* 底光 12 + 律动 */
        if (brightness > 255) brightness = 255;
        if (idle_frames > 40) brightness = 0;        /* 长时间静音彻底熄灭 */

        led_rgb_t base = s_vis_palette[s_color % VIS_PALETTE_COUNT];

        switch (s_mode) {
        case LED_VIS_MODE_SINGLE: {
            /* 单色律动：3 颗同色，亮度随音量 */
            led_rgb_t c = {
                .r = (uint8_t)((uint32_t)base.r * brightness / 255),
                .g = (uint8_t)((uint32_t)base.g * brightness / 255),
                .b = (uint8_t)((uint32_t)base.b * brightness / 255),
            };
            drv_led_strip_set_all(c);
            break;
        }
        case LED_VIS_MODE_RAINBOW: {
            /* 彩虹律动：色相随节拍推进（音量越大转越快），亮度随音量 */
            hue = (uint16_t)(hue + 2 + (uint16_t)(env * 10.0f)) % 256;
            for (int i = 0; i < LED_STRIP_COUNT; i++) {
                uint16_t h = (uint16_t)(hue + i * 80) % 256;
                led_rgb_t c = hsv_to_rgb(h, 255, (uint8_t)brightness);
                drv_led_strip_set_pixel(i, c);
            }
            break;
        }
        case LED_VIS_MODE_PULSE: {
            /* 节拍脉冲：底光常亮，检测到节拍时整体泛白闪一下 */
            led_rgb_t dim = {
                .r = (uint8_t)((uint32_t)base.r * brightness / 255),
                .g = (uint8_t)((uint32_t)base.g * brightness / 255),
                .b = (uint8_t)((uint32_t)base.b * brightness / 255),
            };
            if (pulse > 0) {
                led_rgb_t white = {
                    .r = (uint8_t)(180 + brightness * 75 / 255),
                    .g = (uint8_t)(180 + brightness * 75 / 255),
                    .b = (uint8_t)(180 + brightness * 75 / 255),
                };
                dim = blend(dim, white, 200);
                pulse--;
            }
            drv_led_strip_set_all(dim);
            break;
        }
        default:
            break;
        }
        drv_led_strip_refresh();
    }
}

/* ========== 公共接口 ========== */
esp_err_t led_visualizer_init(void)
{
    if (s_started) return ESP_OK;

    s_mode = sys_nvs_load_led_vis_mode();
    s_color = sys_nvs_load_led_vis_color();
    if (s_color < 0 || s_color >= (int)VIS_PALETTE_COUNT) s_color = 4;

    /* 加载律动灵敏度（独立 NVS namespace，避免污染主 handle） */
    {
        nvs_handle_t h;
        if (nvs_open(VIS_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            int32_t sens = 5;
            if (nvs_get_i32(h, VIS_NVS_KEY_SENS, &sens) == ESP_OK) s_sens = (int)sens;
            nvs_close(h);
        }
        if (s_sens < 1) s_sens = 1;
        if (s_sens > 10) s_sens = 10;
    }

    /* 常驻持有一份 LED 引用：即便 LED app 退出 deinit 也不会释放 RMT */
    drv_led_strip_init();

    BaseType_t ret = xTaskCreate(led_visualizer_task, "led_vis", 3072, NULL, 4, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(led_vis) failed");
        drv_led_strip_deinit();
        return ESP_FAIL;
    }
    s_started = true;
    ESP_LOGI(TAG, "LED visualizer started (mode=%d color=%d)", s_mode, s_color);
    return ESP_OK;
}

void led_visualizer_set_mode(int mode)
{
    if (mode < 0 || mode >= LED_VIS_MODE_MAX) mode = LED_VIS_MODE_OFF;
    s_mode = mode;
    sys_nvs_save_led_vis_mode(mode);
    ESP_LOGI(TAG, "mode set to %d", mode);
}

int led_visualizer_get_mode(void)
{
    return s_mode;
}

void led_visualizer_set_color(int preset_index)
{
    if (preset_index < 0 || preset_index >= (int)VIS_PALETTE_COUNT) preset_index = 4;
    s_color = preset_index;
    sys_nvs_save_led_vis_color(preset_index);
    ESP_LOGI(TAG, "color set to %d", preset_index);
}

int led_visualizer_get_color(void)
{
    return s_color;
}

void led_visualizer_set_sensitivity(int level)
{
    if (level < 1) level = 1;
    if (level > 10) level = 10;
    s_sens = level;
    nvs_handle_t h;
    if (nvs_open(VIS_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, VIS_NVS_KEY_SENS, level);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "sensitivity set to %d", level);
}

int led_visualizer_get_sensitivity(void)
{
    return s_sens;
}

void led_visualizer_set_busy(bool busy)
{
    s_busy = busy;
}

bool led_visualizer_is_active(void)
{
    return s_started && s_mode != LED_VIS_MODE_OFF && !s_busy;
}
