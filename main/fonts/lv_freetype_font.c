/**
 * @file lv_freetype_font.c
 * @brief 中文字体管理实现 - 使用 LVGL FreeType 引擎从 SD 卡加载字体
 *
 * 使用 LVGL 内置的 FreeType 字体引擎，从 SD 卡加载 TTF/OTF 字体文件，
 * 支持多尺寸中文渲染。不再依赖巨大的内置位图字体（lv_font_xiaomiao_cn_14 约 371KB）。
 *
 * 字体文件路径：/sdcard/Fonts/NotoSansSC-Regular.otf
 * 备选路径：/flash/Fonts/NotoSansSC-Regular.otf（retro-core 分区）
 */

#include "lv_freetype_font.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "FONT";

/* 字体文件路径 */
#define FONT_PATH_SDCARD   "/sdcard/Fonts/NotoSansSC-Regular.otf"
#define FONT_PATH_FLASH    "/flash/Fonts/NotoSansSC-Regular.otf"

/* 最大缓存字形数 */
#define FONT_CACHE_GLYPH_CNT 256

/* FreeType 字体句柄（按尺寸缓存） */
static lv_font_t *s_font_14 = NULL;
static lv_font_t *s_font_16 = NULL;
static lv_font_t *s_font_20 = NULL;
static lv_font_t *s_font_24 = NULL;
static bool s_initialized = false;

/* 尝试从多个路径加载字体文件 */
static const char* find_font_file(void)
{
    /* 优先从 SD 卡加载 */
    FILE *f = fopen(FONT_PATH_SDCARD, "rb");
    if (f) {
        fclose(f);
        return FONT_PATH_SDCARD;
    }
    /* 回退到 retro-core 分区 */
    f = fopen(FONT_PATH_FLASH, "rb");
    if (f) {
        fclose(f);
        return FONT_PATH_FLASH;
    }
    return NULL;
}

/* 创建指定尺寸的 FreeType 字体 */
static lv_font_t* create_freetype_font(const char *path, int size)
{
    lv_font_t *font = lv_freetype_font_create(
        path,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
        size,
        LV_FREETYPE_FONT_STYLE_NORMAL
    );
    if (font) {
        ESP_LOGI(TAG, "FreeType font %dpx created from %s", size, path);
    } else {
        ESP_LOGW(TAG, "Failed to create FreeType font %dpx from %s", size, path);
    }
    return font;
}

lv_result_t lv_freetype_font_init(void)
{
    if (s_initialized) {
        return LV_RESULT_OK;
    }

    /* 查找字体文件 */
    const char *font_path = find_font_file();
    if (!font_path) {
        ESP_LOGE(TAG, "Font file not found at %s or %s", FONT_PATH_SDCARD, FONT_PATH_FLASH);
        return LV_RESULT_INVALID;
    }

    /* 初始化 FreeType 引擎 */
    lv_result_t res = lv_freetype_init(FONT_CACHE_GLYPH_CNT);
    if (res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "lv_freetype_init failed");
        return LV_RESULT_INVALID;
    }

    /* 创建各尺寸字体 */
    s_font_14 = create_freetype_font(font_path, 14);
    s_font_16 = create_freetype_font(font_path, 16);
    s_font_20 = create_freetype_font(font_path, 20);
    s_font_24 = create_freetype_font(font_path, 24);

    /* 至少 14px 字体必须成功 */
    if (!s_font_14) {
        ESP_LOGE(TAG, "Failed to create 14px FreeType font - fallback will use Montserrat");
        lv_freetype_uninit();
        return LV_RESULT_INVALID;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "FreeType font engine initialized: %s (%dpx/%dpx/%dpx/%dpx)",
             font_path, 14, 16, 20, 24);
    return LV_RESULT_OK;
}

const lv_font_t* lv_font_cn_14(void)
{
    return s_font_14 ? s_font_14 : &lv_font_montserrat_14;
}

const lv_font_t* lv_font_cn_16(void)
{
    if (s_font_16) return s_font_16;
    return lv_font_cn_14();
}

const lv_font_t* lv_font_cn_20(void)
{
    if (s_font_20) return s_font_20;
    return lv_font_cn_16();
}

const lv_font_t* lv_font_cn_get(int size)
{
    switch (size) {
        case 14: return lv_font_cn_14();
        case 16: return lv_font_cn_16();
        case 20: return lv_font_cn_20();
        case 24: return s_font_24 ? s_font_24 : lv_font_cn_20();
        default: return lv_font_cn_14();
    }
}

bool lv_freetype_font_is_ready(void)
{
    return s_initialized && s_font_14 != NULL;
}