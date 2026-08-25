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
#include <string.h>
#include <strings.h>
#include <dirent.h>

static const char *TAG = "FONT";

/* 字体文件路径 - 支持多种大小写变体
 * 优先使用子集化字体（~260KB，适合 ESP32 资源受限环境），
 * 回退到完整版字体（8.3MB）。子集化字体由 CI 自动构建放入 retro-core 分区。 */
#define FONT_PATH_SDCARD_1   "/sdcard/Fonts/NotoSansSC-Regular.subset.otf"
#define FONT_PATH_SDCARD_2   "/sdcard/fonts/NotoSansSC-Regular.subset.otf"
#define FONT_PATH_SDCARD_3   "/sdcard/Fonts/NotoSansSC-Regular.otf"
#define FONT_PATH_SDCARD_4   "/sdcard/fonts/NotoSansSC-Regular.otf"
#define FONT_PATH_SDCARD_5   "/sdcard/Fonts/notosanssc-regular.otf"
#define FONT_PATH_FLASH_1    "/flash/Fonts/NotoSansSC-Regular.otf"
#define FONT_PATH_FLASH_2    "/flash/fonts/NotoSansSC-Regular.otf"

/* 最大缓存字形数 */
#define FONT_CACHE_GLYPH_CNT 256

/* FreeType 字体句柄（按尺寸缓存） */
static lv_font_t *s_font_14 = NULL;
static lv_font_t *s_font_16 = NULL;
static lv_font_t *s_font_20 = NULL;
static lv_font_t *s_font_24 = NULL;
static bool s_initialized = false;

/* 尝试打开文件，返回路径或NULL */
static const char* try_open_font(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        ESP_LOGI(TAG, "Font file found: %s", path);
        return path;
    }
    ESP_LOGD(TAG, "Font file not found: %s", path);
    return NULL;
}

/* 查找字体文件 - 尝试多个路径 */
static const char* find_font_file(void)
{
    const char *path;
    
    /* 优先从 SD 卡加载（尝试多种大小写） */
    if ((path = try_open_font(FONT_PATH_SDCARD_1)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_SDCARD_2)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_SDCARD_3)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_SDCARD_4)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_SDCARD_5)) != NULL) return path;
    
    /* 回退到 retro-core 分区 */
    if ((path = try_open_font(FONT_PATH_FLASH_1)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_FLASH_2)) != NULL) return path;
    
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
        ESP_LOGE(TAG, "Font file not found! Tried paths:");
        ESP_LOGE(TAG, "  SD: %s, %s, %s", FONT_PATH_SDCARD_1, FONT_PATH_SDCARD_2, FONT_PATH_SDCARD_3);
        ESP_LOGE(TAG, "  Flash: %s, %s", FONT_PATH_FLASH_1, FONT_PATH_FLASH_2);
        ESP_LOGE(TAG, "Please put font file at: /sdcard/Fonts/NotoSansSC-Regular.otf");
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

/* ========== 字体文件自动扫描 ========== */

/* 扫描指定目录中的字体文件 */
static int scan_font_dir(const char *dir_path, char paths[][128], int max_paths, int count)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGI(TAG, "Font dir not found: %s", dir_path);
        return count;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_paths) {
        if (entry->d_name[0] == '.') continue;

        /* 检查扩展名 */
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".ttf") != 0 && strcasecmp(ext, ".otf") != 0) continue;

        /* 拼接完整路径 - 手动拼接避免 -Werror=format-truncation 警告
         * （d_name 最大 255 字节 + dir_path 可能导致 snprintf 理论截断） */
        size_t dlen = strlen(dir_path);
        if (dlen >= 127) dlen = 126;  /* 预留 '/' + NUL */
        memcpy(paths[count], dir_path, dlen);
        paths[count][dlen] = '/';
        strncpy(paths[count] + dlen + 1, entry->d_name, 127 - dlen - 1);
        paths[count][127] = '\0';
        ESP_LOGI(TAG, "Found font: %s", paths[count]);
        count++;
    }
    closedir(dir);
    return count;
}

int lv_freetype_font_scan(char paths[][128], int max_paths)
{
    if (!paths || max_paths <= 0) return 0;
    int count = 0;

    /* 优先扫描 SD 卡 Fonts 目录 */
    count = scan_font_dir("/sdcard/Fonts", paths, max_paths, count);
    if (count < max_paths) {
        count = scan_font_dir("/sdcard/fonts", paths, max_paths, count);
    }
    /* 回退到 retro-core 分区 */
    if (count < max_paths) {
        count = scan_font_dir("/flash/Fonts", paths, max_paths, count);
    }
    if (count < max_paths) {
        count = scan_font_dir("/flash/fonts", paths, max_paths, count);
    }

    ESP_LOGI(TAG, "Font scan complete: %d font file(s) found", count);
    return count;
}

/* ========== 从指定路径加载字体 ========== */

lv_result_t lv_freetype_font_load_path(const char *path)
{
    if (!path) return LV_RESULT_INVALID;

    /* 验证文件存在 */
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Font file not found: %s", path);
        return LV_RESULT_INVALID;
    }
    fclose(f);

    /* 若引擎未初始化，先初始化 */
    if (!s_initialized) {
        lv_result_t res = lv_freetype_init(FONT_CACHE_GLYPH_CNT);
        if (res != LV_RESULT_OK) {
            ESP_LOGE(TAG, "lv_freetype_init failed");
            return LV_RESULT_INVALID;
        }
        s_initialized = true;
    }

    /* 先创建新字体（全部成功后再原子替换，失败时保留旧字体，避免系统字体瘫痪） */
    lv_font_t *new_14 = create_freetype_font(path, 14);
    if (!new_14) {
        ESP_LOGE(TAG, "Failed to create 14px FreeType font from %s", path);
        return LV_RESULT_INVALID;
    }
    lv_font_t *new_16 = create_freetype_font(path, 16);
    lv_font_t *new_20 = create_freetype_font(path, 20);
    lv_font_t *new_24 = create_freetype_font(path, 24);
    if (!new_16) new_16 = new_14;
    if (!new_20) new_20 = new_16;
    if (!new_24) new_24 = new_20;

    /*
     * 原子替换全局字体指针。
     *
     * 注意：不立即 lv_freetype_font_delete 旧字体对象！
     * 因为已创建的界面（桌面、设置页等）的 label 仍引用旧字体指针，
     * 删除会导致悬空指针/崩溃。旧字体对象保留（少量内存泄漏可接受，
     * 本设备有 8MB PSRAM；用户切换字体频率低），由调用方随后触发
     * 全 UI 重建（ui_stack_back_home），新页面将使用新字体。
     */
    s_font_14 = new_14;
    s_font_16 = new_16;
    s_font_20 = new_20;
    s_font_24 = new_24;

    ESP_LOGI(TAG, "Font reloaded from: %s (%dpx/%dpx/%dpx/%dpx)", path, 14, 16, 20, 24);
    return LV_RESULT_OK;
}