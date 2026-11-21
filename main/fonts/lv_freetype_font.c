/**
 * @file lv_freetype_font.c
 * @brief 中文字体管理实现 - 使用 LVGL FreeType 引擎从 SD 卡加载字体
 *
 * 使用 LVGL 内置的 FreeType 字体引擎，从 SD 卡加载 TTF/OTF 字体文件，
 * 支持多尺寸中文渲染。当 FreeType 未初始化/加载失败（如未插 SD 卡、内置模式）时，
 * 回退到已编入固件的内置精简中文字库 lv_font_xiaomiao_cn_14，确保无 SD 卡也有中文。
 *
 * 字体文件路径：/sdcard/Fonts/NotoSansSC-Regular.otf
 * 备选路径：/flash/Fonts/NotoSansSC-Regular.otf（retro-core 分区）
 */

#include "lv_freetype_font.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
/* espressif/freetype 组件：用于直接调用 FT_Init_FreeType/FT_Error_String
 * 探测错误码（LVGL 的 lv_freetype_init() 失败原因通过 LVGL log 输出，
 * 但本项目未注册 LVGL log callback，错误码完全不可见，只能绕开 LVGL 直接探测）。 */
#include "freetype/freetype.h"
#include "freetype/fterrors.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

static const char *TAG = "FONT";

/* 内置精简中文字库（由 lv_font_xiaomiao_cn_14.c 提供，已编入固件，不依赖 SD 卡）。
 * 符号定义在 LVGL_VERSION_MAJOR >= 8 分支下为 const lv_font_t（本项目 v9.5 符合）。
 * 内置库仅 14px 一种尺寸，size>14 时返回它并由 LVGL 按需缩放。 */
extern const lv_font_t lv_font_xiaomiao_cn_14;

/* 返回内置精简中文字库：任何情况下都可用，绝不返回 NULL，
 * 避免中文变方框或降级为纯英文（Montserrat）。 */
static const lv_font_t* lv_font_cn_builtin(void)
{
    return &lv_font_xiaomiao_cn_14;
}

/* 字体文件路径 - 支持多种大小写变体
 * 优先使用子集化字体（~260KB，适合 ESP32 资源受限环境），
 * 回退到完整版字体（8.3MB）。子集化字体由 CI 自动构建放入 retro-core 分区。
 *
 * 自动搜索策略：
 * 1. 先扫描 /sdcard/Fonts 和 /sdcard/fonts 目录，使用找到的第一个 .ttf/.otf
 * 2. 回退到 /flash/Fonts（retro-core 分区）
 * 3. 最后尝试硬编码路径（兼容旧版 SD 卡布局） */
#define FONT_PATH_SDCARD_1   "/sdcard/Fonts/NotoSansSC-Regular.subset.otf"
#define FONT_PATH_SDCARD_2   "/sdcard/fonts/NotoSansSC-Regular.subset.otf"
#define FONT_PATH_SDCARD_3   "/sdcard/Fonts/NotoSansSC-Regular.otf"
#define FONT_PATH_SDCARD_4   "/sdcard/fonts/NotoSansSC-Regular.otf"
#define FONT_PATH_SDCARD_5   "/sdcard/Fonts/notosanssc-regular.otf"
#define FONT_PATH_FLASH_1    "/flash/Fonts/NotoSansSC-Regular.otf"
#define FONT_PATH_FLASH_2    "/flash/fonts/NotoSansSC-Regular.otf"

/* 最大缓存字形数
 * 增大到 512 以减少 cache evict 频率，降低 LVGL v9.5 lv_cache_lru_rb_count
 * 在高频 glyph lookup/evict 场景下的 UAF 风险（Issue #8526 相关）。
 * 每个 glyph entry ~100 字节，512 个约 50KB PSRAM，可接受。 */
#define FONT_CACHE_GLYPH_CNT 512

/* FreeType 字体句柄（按尺寸缓存） */
static lv_font_t *s_font_14 = NULL;
static lv_font_t *s_font_16 = NULL;
static lv_font_t *s_font_20 = NULL;
static lv_font_t *s_font_24 = NULL;
static bool s_initialized = false;

/* ========== 字体代际回收（防止反复切换字体泄漏） ==========
 * load_path 每次成功都会新建 4 个尺寸的 lv_font_t 对象。旧对象不能直接释放：
 * 屏幕上已创建的 label 仍持有旧字体指针，调用方在 load_path 返回后才会
 * ui_stack_back_home() 重建 UI（重建后旧指针才失去引用）。但若始终不释放，
 * 反复切换字体就会无限累积对象 + FT_Face，造成内存溢出。
 *
 * 采用"保留当前代 + 上一代"的有界回收策略：
 *   - s_font_*  ：当前代，正在被屏幕引用，绝不释放；
 *   - s_prev_*  ：上一次切换时被替换下来的字体。最近一次切换已触发 UI 重建，
 *                 故它已无引用；在下一次成功切换时释放（此时为两代以前）。
 * 这样堆上最多同时存在 2 套字体，且不会 use-after-free。
 *
 * 配合 s_current_font_path 去重：回滚到同一路径变成 no-op，避免回滚路径
 * 误把仍被屏幕引用的字体替换进待回收队列（app 回滚后不重建 UI）。 */
static char s_current_font_path[128] = {0};
static lv_font_t *s_prev_14 = NULL;
static lv_font_t *s_prev_16 = NULL;
static lv_font_t *s_prev_20 = NULL;
static lv_font_t *s_prev_24 = NULL;

/* 记录当前生效的字体路径（供去重使用） */
static void font_set_current_path(const char *path)
{
    if (!path) {
        s_current_font_path[0] = '\0';
        return;
    }
    strncpy(s_current_font_path, path, sizeof(s_current_font_path) - 1);
    s_current_font_path[sizeof(s_current_font_path) - 1] = '\0';
}

/* 释放一套字体对象；自动跳过 NULL 与别名指针（create 失败回退会让多个尺寸
 * 指向同一对象），避免重复 lv_freetype_font_delete 造成 double-free。 */
static void free_font_set(lv_font_t *f14, lv_font_t *f16, lv_font_t *f20, lv_font_t *f24)
{
    lv_font_t *arr[4] = {f14, f16, f20, f24};
    for (int i = 0; i < 4; i++) {
        if (!arr[i]) continue;
        bool duplicate = false;
        for (int j = 0; j < i; j++) {
            if (arr[i] == arr[j]) { duplicate = true; break; }
        }
        if (!duplicate) lv_freetype_font_delete(arr[i]);
    }
}

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

/* 扫描目录中第一个可用字体，返回路径（静态缓冲）或NULL */
static const char* scan_first_font(const char *dir_path)
{
    static char s_buf[128];
    DIR *dir = opendir(dir_path);
    if (!dir) return NULL;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".ttf") != 0 && strcasecmp(ext, ".otf") != 0) continue;

        size_t dlen = strlen(dir_path);
        if (dlen >= 100) dlen = 100;
        memcpy(s_buf, dir_path, dlen);
        s_buf[dlen] = '/';
        strncpy(s_buf + dlen + 1, entry->d_name, 127 - dlen - 1);
        s_buf[127] = '\0';

        closedir(dir);
        ESP_LOGI(TAG, "Auto-detected font: %s", s_buf);
        return s_buf;
    }
    closedir(dir);
    return NULL;
}

/* 查找字体文件 - 自动扫描 + 硬编码路径回退 */
static const char* find_font_file(void)
{
    const char *path;

    /* 1. 自动扫描 SD 卡 Fonts 目录 */
    if ((path = scan_first_font("/sdcard/Fonts")) != NULL) return path;
    if ((path = scan_first_font("/sdcard/fonts")) != NULL) return path;

    /* 2. 自动扫描 flash 备份目录 */
    if ((path = scan_first_font("/flash/Fonts")) != NULL) return path;
    if ((path = scan_first_font("/flash/fonts")) != NULL) return path;

    /* 3. 回退到硬编码路径（兼容旧版 SD 卡布局） */
    if ((path = try_open_font(FONT_PATH_SDCARD_1)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_SDCARD_2)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_SDCARD_3)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_SDCARD_4)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_SDCARD_5)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_FLASH_1)) != NULL) return path;
    if ((path = try_open_font(FONT_PATH_FLASH_2)) != NULL) return path;

    return NULL;
}

/* 诊断：渲染测试字符，检查 pitch/stride 安全性
 * 用于检测 LVGL v9.5 freetype_image_create_cb 的 memcpy(pitch) 越界风险。
 * 若 pitch > stride，每行拷贝会越界写 (pitch - stride) 字节，破坏堆元数据。
 * 子集化字体（~305KB）可能与完整版（16MB）有不同的字形度量。 */
static void diagnose_glyph_pitch(const char *path, int size)
{
    FT_Library lib = NULL;
    FT_Face face = NULL;
    FT_Error err = FT_Init_FreeType(&lib);
    if (err) {
        ESP_LOGW(TAG, "FT_Init_FreeType failed for diagnosis: 0x%02X", (unsigned)err);
        return;
    }
    err = FT_New_Face(lib, path, 0, &face);
    if (err) {
        ESP_LOGW(TAG, "FT_New_Face failed for diagnosis: 0x%02X", (unsigned)err);
        FT_Done_FreeType(lib);
        return;
    }
    FT_Set_Pixel_Sizes(face, 0, size);
    
    /* 扫描常见中文字符，检查 pitch vs width */
    const unsigned long test_chars[] = {
        0x4E2D,  /* 中 */
        0x6587,  /* 文 */
        0x8BBE,  /* 设 */
        0x7F6E,  /* 置 */
        0x4EAE,  /* 亮 */
        0x5EA6,  /* 度 */
        0x97F3,  /* 音 */
        0x91CF,  /* 量 */
        0x0041,  /* A */
        0x0030,  /* 0 */
        0xFF0C,  /* ， */
        0x3002,  /* 。 */
    };
    int max_diff = 0;
    unsigned long worst_cp = 0;
    int worst_pitch = 0, worst_width = 0;
    
    for (int i = 0; i < sizeof(test_chars)/sizeof(test_chars[0]); i++) {
        FT_UInt gi = FT_Get_Char_Index(face, test_chars[i]);
        if (!gi) continue;
        err = FT_Load_Glyph(face, gi, FT_LOAD_RENDER);
        if (err) continue;
        FT_Bitmap *b = &face->glyph->bitmap;
        int diff = b->pitch - b->width;
        if (diff > max_diff) {
            max_diff = diff;
            worst_cp = test_chars[i];
            worst_pitch = b->pitch;
            worst_width = b->width;
        }
    }
    
    if (max_diff > 0) {
        ESP_LOGE(TAG, "GLYPH PITCH > WIDTH at %dpx! worst: U+%04lX pitch=%d width=%d diff=%d",
                 size, worst_cp, worst_pitch, worst_width, max_diff);
        ESP_LOGE(TAG, "This will cause heap corruption in LVGL v9.5 freetype_image_create_cb!");
    } else {
        ESP_LOGI(TAG, "Glyph pitch safety OK at %dpx (all pitch==width)", size);
    }
    
    FT_Done_Face(face);
    FT_Done_FreeType(lib);
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
        /* 诊断：检查 glyph pitch 安全性 */
        diagnose_glyph_pitch(path, size);
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

    /* 查找字体文件（自动扫描 + 硬编码回退） */
    const char *font_path = find_font_file();
    if (!font_path) {
        ESP_LOGE(TAG, "Font file not found! Tried paths:");
        ESP_LOGE(TAG, "  SD: %s, %s, %s", FONT_PATH_SDCARD_1, FONT_PATH_SDCARD_2, FONT_PATH_SDCARD_3);
        ESP_LOGE(TAG, "  Flash: %s, %s", FONT_PATH_FLASH_1, FONT_PATH_FLASH_2);
        ESP_LOGE(TAG, "Please put a .ttf/.otf font file at: /sdcard/Fonts/");
        return LV_RESULT_INVALID;
    }

    /* 初始化 FreeType 引擎
     * 注意：不重试！如果第一次失败，直接使用内置字体。
     * 重试可能导致某些内部状态不一致，引发后续崩溃。 */
    ESP_LOGI(TAG, "Attempting FreeType init (DRAM free=%lu, PSRAM free=%lu)",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* 先 uninit 清空 ft_ctx 残留：
     * LVGL v9.5 lv_freetype_init() 第一行检查 ft_ctx != NULL 就直接返回 INVALID
     * （LV_LOG_WARN "freetype already initialized"），不走 FT_Init_FreeType。
     * 真机诊断日志显示 FT_Init_FreeType direct probe SUCCEEDED 但 lv_freetype_init()
     * 仍失败 —— 证明 ft_ctx 在进入 init() 前已是垃圾值（LVGL 全局变量未 zero-init）。
     * 主动 uninit 让 ft_ctx=NULL，确保 init() 真正进入 FT_Init_FreeType 分支。
     * lv_freetype_uninit() 内部有 if(!ctx) 保护，未初始化时调用安全 no-op。 */
    lv_freetype_uninit();
    lv_result_t res = lv_freetype_init(FONT_CACHE_GLYPH_CNT);
    if (res != LV_RESULT_OK) {
        /* 打印最大空闲块（比 free 总量更能反映能否分配），并记录 LVGL 内部日志
         * 是否输出了 "already initialized" / "malloc failed" / "FT_Init_FreeType"。
         * 注意：lv_freetype_init() 在 FT_Init_FreeType 失败时不会释放 ft_ctx，
         * 导致后续调用永远走 "already initialized" WARN 分支。此处主动 uninit
         * 清空残留状态，让后续重试（如用户再次选择字体）真正重新初始化。 */
        lv_freetype_uninit();
        ESP_LOGE(TAG, "lv_freetype_init failed (DRAM free=%lu, DRAM max_block=%lu, PSRAM free=%lu, PSRAM max_block=%lu) - falling back to built-in Chinese font",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

        /* 绕开 LVGL，直接调用 esp-freetype 的 FT_Init_FreeType 探测确切错误码。
         * LVGL 内部用它初始化 library，返回的错误码即 lv_freetype_init() 失败的真实原因。
         * 注意：FT_CONFIG_OPTION_ERROR_STRINGS 未启用时 FT_Error_String() 返回 NULL，
         * 需判空避免打印 (null)。错误码本身（十六进制）恒可靠。 */
        FT_Library probe_lib = NULL;
        FT_Error fterr = FT_Init_FreeType(&probe_lib);
        const char *err_str = FT_Error_String(fterr);
        if (err_str) {
            ESP_LOGE(TAG, "FT_Init_FreeType direct probe: error=0x%02X (%s)",
                     (unsigned int)fterr, err_str);
        } else {
            ESP_LOGE(TAG, "FT_Init_FreeType direct probe: error=0x%02X", (unsigned int)fterr);
        }
        if (probe_lib) {
            ESP_LOGI(TAG, "FT_Init_FreeType direct probe SUCCEEDED (library allocated, but LVGL init failed)");
            FT_Done_FreeType(probe_lib);
        } else {
            ESP_LOGE(TAG, "FT_Init_FreeType direct probe FAILED to allocate library");
        }
        return LV_RESULT_INVALID;
    }

    /* 创建各尺寸字体 */
    s_font_14 = create_freetype_font(font_path, 14);
    s_font_16 = create_freetype_font(font_path, 16);
    s_font_20 = create_freetype_font(font_path, 20);
    s_font_24 = create_freetype_font(font_path, 24);

    /* 至少 14px 字体必须成功 */
    if (!s_font_14) {
        /* 真泄漏修复：14px 失败时 16/20/24px 可能已创建成功，
         * 它们尚未被任何 UI label 引用（此时处于字体引擎初始化阶段），
         * 可在 uninit 前安全释放，避免残留字体对象 + FT_Face 泄漏。 */
        free_font_set(NULL, s_font_16, s_font_20, s_font_24);
        s_font_16 = s_font_20 = s_font_24 = NULL;
        ESP_LOGE(TAG, "Failed to create 14px FreeType font - fallback will use built-in Chinese font");
        lv_freetype_uninit();
        return LV_RESULT_INVALID;
    }

    s_initialized = true;
    font_set_current_path(font_path);
    ESP_LOGI(TAG, "FreeType font engine initialized: %s (%dpx/%dpx/%dpx/%dpx)",
             font_path, 14, 16, 20, 24);
    return LV_RESULT_OK;
}

const lv_font_t* lv_font_cn_14(void)
{
    /* 优先使用已成功加载的 FreeType 14px 字体；
     * 未就绪（内置模式 / SD 卡缺失 / 加载失败）时回退到内置精简中文字库。
     * 不再返回纯英文字体 &lv_font_montserrat_14。 */
    return s_font_14 ? s_font_14 : lv_font_cn_builtin();
}

const lv_font_t* lv_font_cn_16(void)
{
    if (s_font_16) return s_font_16;
    return lv_font_cn_14();  /* 逐级回退，最终落到内置中文字库 */
}

const lv_font_t* lv_font_cn_20(void)
{
    if (s_font_20) return s_font_20;
    return lv_font_cn_16();  /* 逐级回退，最终落到内置中文字库 */
}

const lv_font_t* lv_font_cn_get(int size)
{
    /* 优先级：FreeType 引擎已就绪时使用对应尺寸的 FreeType 字体（保留大字体能力）；
     * 内置模式 / FreeType 未初始化 / 加载失败时统一返回内置精简中文字库。
     * 内置库只有 14px 一种尺寸：size<=14 直接返回，size>14 时也返回它
     * （由 LVGL 按需缩放/接受），任何情况下都不返回 NULL。 */
    if (lv_freetype_font_is_ready()) {
        switch (size) {
            case 14: return s_font_14 ? s_font_14 : lv_font_cn_builtin();
            case 16: return s_font_16 ? s_font_16 : lv_font_cn_14();
            case 20: return s_font_20 ? s_font_20 : lv_font_cn_16();
            case 24: return s_font_24 ? s_font_24 : lv_font_cn_20();
            default: return lv_font_cn_14();
        }
    }
    return lv_font_cn_builtin();
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

/* ========== 卸载字体 ========== */

void lv_freetype_font_deinit(void)
{
    if (!s_initialized) return;

    /* 释放上一代字体：它在最近一次成功切换后已随 UI 重建失去引用。
     * 必须在 lv_freetype_uninit() 之前释放（delete 依赖引擎仍存活）。
     * 当前代 s_font_* 仍被屏幕 label 引用，不在此释放，交由调用方重建 UI。 */
    free_font_set(s_prev_14, s_prev_16, s_prev_20, s_prev_24);
    s_prev_14 = s_prev_16 = s_prev_20 = s_prev_24 = NULL;

    /* 清空所有字体句柄，后续 lv_font_cn_*() 将回退到内置精简中文字库 */
    s_font_14 = NULL;
    s_font_16 = NULL;
    s_font_20 = NULL;
    s_font_24 = NULL;
    s_initialized = false;
    font_set_current_path(NULL);

    /* 关闭 FreeType 引擎（释放所有缓存字形） */
    lv_freetype_uninit();

    ESP_LOGI(TAG, "FreeType font engine deinitialized, UI will use built-in Chinese font");
}

/* ========== 从指定路径加载字体 ========== */

lv_result_t lv_freetype_font_load_path(const char *path)
{
    if (!path) return LV_RESULT_INVALID;

    /* 去重：已是当前生效字体则直接返回，避免重复创建字体对象造成泄漏。
     * 对"再次确认当前项"和"加载失败回滚到原路径"尤其重要：回滚路径不重建 UI，
     * 若重复 new 一套字体会把仍被 label 引用的旧字体推入回收队列，
     * 造成 use-after-free。 */
    if (s_initialized && s_font_14 && s_current_font_path[0] != '\0' &&
        strcmp(s_current_font_path, path) == 0) {
        ESP_LOGI(TAG, "Font already loaded: %s (skip)", path);
        return LV_RESULT_OK;
    }

    /* 验证文件存在 */
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Font file not found: %s", path);
        return LV_RESULT_INVALID;
    }
    fclose(f);

    /* 若引擎未初始化，先初始化
     * 注意：不重试！与 lv_freetype_font_init() 保持一致。
     * uninit + reinit 会导致 FreeType 内部状态不一致，后续 UI 操作崩溃
     * （曾真机复现：init failed, retrying → 重试成功 → 启动设置应用 LoadProhibited）。 */
    if (!s_initialized) {
        ESP_LOGI(TAG, "Attempting FreeType init (load_path) (DRAM free=%lu, PSRAM free=%lu)",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        /* 与 init() 一致：先 uninit 清 ft_ctx 残留，确保进入真正的 FT_Init_FreeType 分支 */
        lv_freetype_uninit();
        lv_result_t res = lv_freetype_init(FONT_CACHE_GLYPH_CNT);
        if (res != LV_RESULT_OK) {
            /* 与 init() 一致：失败时主动 uninit 清空 ft_ctx 残留，
             * 避免后续调用永久卡在 "already initialized" WARN 分支。 */
            lv_freetype_uninit();
            ESP_LOGE(TAG, "lv_freetype_init failed (load_path) (DRAM free=%lu, DRAM max_block=%lu, PSRAM free=%lu, PSRAM max_block=%lu) - falling back to built-in Chinese font",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            /* 直接探测 FT_Init_FreeType 错误码（与 init() 一致） */
            FT_Library probe_lib2 = NULL;
            FT_Error fterr2 = FT_Init_FreeType(&probe_lib2);
            const char *err_str2 = FT_Error_String(fterr2);
            if (err_str2) {
                ESP_LOGE(TAG, "FT_Init_FreeType direct probe (load_path): error=0x%02X (%s)",
                         (unsigned int)fterr2, err_str2);
            } else {
                ESP_LOGE(TAG, "FT_Init_FreeType direct probe (load_path): error=0x%02X",
                         (unsigned int)fterr2);
            }
            if (probe_lib2) FT_Done_FreeType(probe_lib2);
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

    /* 有界代际回收（真泄漏修复）：
     * 刚被替换下来的 s_font_* 不能直接释放——屏幕 label 仍引用它们，调用方
     * 随后会 ui_stack_back_home() 重建 UI。但 s_prev_*（上一代）在最近一次
     * 切换时已经历 UI 重建、不再被引用，此处可安全释放。由此把堆占用限制在
     * 当前 + 上一代 2 套字体，避免反复切换字体时无界累积。 */
    free_font_set(s_prev_14, s_prev_16, s_prev_20, s_prev_24);
    s_prev_14 = s_font_14;
    s_prev_16 = s_font_16;
    s_prev_20 = s_font_20;
    s_prev_24 = s_font_24;

    /* 原子替换全局字体指针（新字体全部创建成功后才切换） */
    s_font_14 = new_14;
    s_font_16 = new_16;
    s_font_20 = new_20;
    s_font_24 = new_24;
    font_set_current_path(path);

    ESP_LOGI(TAG, "Font reloaded from: %s (%dpx/%dpx/%dpx/%dpx)", path, 14, 16, 20, 24);
    return LV_RESULT_OK;
}