/**
 * @file lv_freetype_font.h
 * @brief 中文字体管理 - 统一获取入口
 *
 * 使用 LVGL FreeType 引擎从 SD 卡加载 TTF/OTF 字体文件，
 * 支持多尺寸中文字体渲染。
 * 字体文件路径：/sdcard/Fonts/NotoSansSC-Regular.otf
 */

#ifndef LV_FREETYPE_FONT_H
#define LV_FREETYPE_FONT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/**
 * @brief 初始化 FreeType 字体引擎并加载字体
 * 从 /sdcard/Fonts/NotoSansSC-Regular.otf 加载中文字体
 * @return LV_RESULT_OK 成功，LV_RESULT_INVALID 失败
 */
lv_result_t lv_freetype_font_init(void);

/**
 * @brief 获取中文显示字体（14px）
 * @return const lv_font_t* 字体指针（失败时返回 NULL）
 */
const lv_font_t* lv_font_cn_14(void);

/**
 * @brief 获取中文显示字体（16px）
 * @return const lv_font_t* 字体指针（失败时返回 NULL）
 */
const lv_font_t* lv_font_cn_16(void);

/**
 * @brief 获取中文显示字体（20px）
 * @return const lv_font_t* 字体指针（失败时返回 NULL）
 */
const lv_font_t* lv_font_cn_20(void);

/**
 * @brief 根据指定大小获取中文显示字体
 * 支持大小：14、16、20、24，其他大小回退到14px。
 * @param size 字体大小（px）
 * @return const lv_font_t* 字体指针（失败时返回 NULL）
 */
const lv_font_t* lv_font_cn_get(int size);

/**
 * @brief 检查 FreeType 字体是否已就绪
 * @return true 已就绪，false 未就绪
 */
bool lv_freetype_font_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_FREETYPE_FONT_H */