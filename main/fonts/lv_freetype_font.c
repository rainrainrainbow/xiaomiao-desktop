/**
 * @file lv_freetype_font.c
 * @brief FreeType 字体管理实现 - 从SD卡加载TrueType/OpenType字体
 *
 * 使用 LVGL 内置的 FreeType 字体引擎（lv_freetype），
 * 从 SD 卡加载 NotoSansSC-Regular.otf 字体文件，
 * 创建多个大小的字体对象供全局使用。
 *
 * 依赖：
 *   - LVGL v9.5+（内置 lv_freetype 支持，需启用 LV_USE_FREETYPE）
 *   - ESP-IDF 组件：espressif/freetype
 *   - SD 卡文件系统（字体文件存储在 /sdcard/fonts/）
 */

#include "lv_freetype_font.h"
#include "esp_log.h"
#include <sys/stat.h>

static const char *TAG = "LV_FREETYPE_FONT";

/* FreeType 字体文件路径（SD卡） */
#define FREETYPE_FONT_PATH "/sdcard/fonts/NotoSansSC-Regular.otf"

/* FreeType 字体对象缓存 */
#define FREETYPE_FONT_COUNT 4
static lv_font_t *s_freetype_fonts[FREETYPE_FONT_COUNT] = {NULL};
static bool s_freetype_initialized = false;

/* 字体大小映射表 */
static const lv_freetype_font_size_t s_font_sizes[FREETYPE_FONT_COUNT] = {
    LV_FREETYPE_FONT_SIZE_14,
    LV_FREETYPE_FONT_SIZE_16,
    LV_FREETYPE_FONT_SIZE_20,
    LV_FREETYPE_FONT_SIZE_24,
};

/**
 * @brief 检查字体文件是否存在
 */
static bool check_font_file_exists(void)
{
    struct stat st;
    if (stat(FREETYPE_FONT_PATH, &st) == 0 && S_ISREG(st.st_mode)) {
        ESP_LOGI(TAG, "Font file found: %s (%d bytes)", FREETYPE_FONT_PATH, (int)st.st_size);
        return true;
    }
    ESP_LOGW(TAG, "Font file not found: %s", FREETYPE_FONT_PATH);
    ESP_LOGW(TAG, "Please copy NotoSansSC-Regular.otf to /sdcard/fonts/");
    return false;
}

lv_result_t lv_freetype_font_init(void)
{
    if (s_freetype_initialized) {
        ESP_LOGW(TAG, "FreeType font already initialized");
        return LV_RESULT_OK;
    }

    /* 检查字体文件是否存在 */
    if (!check_font_file_exists()) {
        ESP_LOGE(TAG, "FreeType font init failed: font file not found");
        return LV_RESULT_INVALID;
    }

    /* 初始化 LVGL FreeType 引擎 */
    lv_result_t res = lv_freetype_init(0);  /* 0 = 使用默认缓存大小 */
    if (res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "lv_freetype_init failed");
        return LV_RESULT_INVALID;
    }

    /* 创建各大小字体对象 */
    for (int i = 0; i < FREETYPE_FONT_COUNT; i++) {
        s_freetype_fonts[i] = lv_freetype_font_create(
            FREETYPE_FONT_PATH,
            LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
            (uint32_t)s_font_sizes[i],
            LV_FREETYPE_FONT_STYLE_NORMAL
        );

        if (s_freetype_fonts[i] == NULL) {
            ESP_LOGE(TAG, "Failed to create FreeType font size=%d", s_font_sizes[i]);
            /* 继续尝试其他大小 */
        } else {
            ESP_LOGI(TAG, "FreeType font size=%d created successfully", s_font_sizes[i]);
        }
    }

    /* 检查是否至少有一个字体创建成功 */
    bool any_ok = false;
    for (int i = 0; i < FREETYPE_FONT_COUNT; i++) {
        if (s_freetype_fonts[i] != NULL) {
            any_ok = true;
            break;
        }
    }

    if (!any_ok) {
        ESP_LOGE(TAG, "FreeType font init failed: no font created");
        lv_freetype_uninit();
        return LV_RESULT_INVALID;
    }

    s_freetype_initialized = true;
    ESP_LOGI(TAG, "FreeType font engine initialized successfully");
    return LV_RESULT_OK;
}

void lv_freetype_font_deinit(void)
{
    if (!s_freetype_initialized) return;

    /* 删除所有字体对象 */
    for (int i = 0; i < FREETYPE_FONT_COUNT; i++) {
        if (s_freetype_fonts[i] != NULL) {
            lv_freetype_font_delete(s_freetype_fonts[i]);
            s_freetype_fonts[i] = NULL;
        }
    }

    /* 反初始化 FreeType 引擎 */
    lv_freetype_uninit();

    s_freetype_initialized = false;
    ESP_LOGI(TAG, "FreeType font engine deinitialized");
}

const lv_font_t* lv_freetype_font_get(lv_freetype_font_size_t size)
{
    if (!s_freetype_initialized) return NULL;

    for (int i = 0; i < FREETYPE_FONT_COUNT; i++) {
        if (s_font_sizes[i] == size) {
            return s_freetype_fonts[i];
        }
    }
    return NULL;
}

bool lv_freetype_font_is_ready(void)
{
    return s_freetype_initialized;
}

/**
 * @brief 获取中文显示字体（14px）
 * 
 * 优先返回 FreeType 字体（完整中文支持），
 * 如果 FreeType 未就绪则回退到内置自定义字体。
 */
const lv_font_t* lv_font_cn_14(void)
{
    if (s_freetype_initialized) {
        const lv_font_t *ft = lv_freetype_font_get(LV_FREETYPE_FONT_SIZE_14);
        if (ft != NULL) return ft;
    }
    /* 回退到内置自定义字体 */
    LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
    return &lv_font_xiaomiao_cn_14;
}

/**
 * @brief 获取中文显示字体（16px）
 * 
 * 优先返回 FreeType 字体（完整中文支持），
 * 如果 FreeType 未就绪则回退到内置自定义字体。
 */
const lv_font_t* lv_font_cn_16(void)
{
    if (s_freetype_initialized) {
        const lv_font_t *ft = lv_freetype_font_get(LV_FREETYPE_FONT_SIZE_16);
        if (ft != NULL) return ft;
    }
    /* 回退到内置自定义字体 */
    LV_FONT_DECLARE(lv_font_xiaomiao_cn_14);
    return &lv_font_xiaomiao_cn_14;
}