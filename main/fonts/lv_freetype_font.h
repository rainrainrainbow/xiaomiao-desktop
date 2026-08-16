/**
 * @file lv_freetype_font.h
 * @brief FreeType 字体管理 - 从SD卡加载TrueType/OpenType字体
 *
 * 提供 FreeType 字体初始化和全局字体对象管理。
 * 字体文件从 SD 卡加载，支持完整 Unicode 字符集（含中文）。
 */

#ifndef LV_FREETYPE_FONT_H
#define LV_FREETYPE_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/**
 * @brief FreeType 字体大小枚举
 */
typedef enum {
    LV_FREETYPE_FONT_SIZE_14 = 14,  // 小号字体（14px，用于列表、标签）
    LV_FREETYPE_FONT_SIZE_16 = 16,  // 中号字体（16px，用于标题）
    LV_FREETYPE_FONT_SIZE_20 = 20,  // 大号字体（20px，用于大标题）
    LV_FREETYPE_FONT_SIZE_24 = 24,  // 特大号字体（24px）
} lv_freetype_font_size_t;

/**
 * @brief 初始化 FreeType 字体引擎
 * 
 * 在 LVGL 初始化后调用，从 SD 卡加载字体文件并创建字体对象。
 * 字体文件路径：/sdcard/fonts/NotoSansSC-Regular.otf
 * 
 * @return lv_result_t LV_RESULT_OK 成功，LV_RESULT_INVALID 失败
 */
lv_result_t lv_freetype_font_init(void);

/**
 * @brief 反初始化 FreeType 字体引擎
 * 
 * 释放所有 FreeType 字体对象和资源。
 */
void lv_freetype_font_deinit(void);

/**
 * @brief 获取指定大小的 FreeType 字体
 * 
 * @param size 字体大小（见 lv_freetype_font_size_t 枚举）
 * @return const lv_font_t* 字体指针，失败返回 NULL
 */
const lv_font_t* lv_freetype_font_get(lv_freetype_font_size_t size);

/**
 * @brief 获取中文显示字体（14px）
 * 
 * 优先返回 FreeType 字体（完整中文支持），
 * 如果 FreeType 未就绪则回退到内置自定义字体。
 * 这是所有中文UI组件的统一字体获取入口。
 * 
 * @return const lv_font_t* 字体指针（始终非NULL）
 */
const lv_font_t* lv_font_cn_14(void);

/**
 * @brief 获取中文显示字体（16px）
 * 
 * @return const lv_font_t* 字体指针（始终非NULL）
 */
const lv_font_t* lv_font_cn_16(void);

/**
 * @brief 根据指定大小获取中文显示字体
 * 
 * 优先返回 FreeType 字体（完整中文支持），
 * 如果 FreeType 未就绪则回退到内置自定义字体。
 * 支持大小：14、16、20、24，其他大小回退到14px。
 * 
 * @param size 字体大小（px），支持14/16/20/24
 * @return const lv_font_t* 字体指针（始终非NULL）
 */
const lv_font_t* lv_font_cn_get(int size);

/**
 * @brief 检查 FreeType 字体是否已就绪
 * 
 * @return true 字体已加载，false 未加载
 */
bool lv_freetype_font_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_FREETYPE_FONT_H */