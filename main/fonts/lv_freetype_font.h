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

/**
 * @brief 扫描 SD 卡 Fonts 目录下的字体文件
 * 扫描 /sdcard/Fonts/ 目录（以及 /flash/Fonts/ 备用路径），
 * 收集所有 .ttf/.otf 文件路径。
 * @param paths 输出数组，用于存放找到的字体文件完整路径（调用方提供）
 * @param max_paths paths 数组最大容量
 * @param path_len 每个路径缓冲区的最大长度
 * @return 找到的字体文件数量（0 表示未找到）
 */
int lv_freetype_font_scan(char paths[][128], int max_paths);

/**
 * @brief 卸载 FreeType 字体并释放资源
 * 关闭 FreeType 引擎，清空所有字体句柄。
 * 调用后 lv_freetype_font_is_ready() 返回 false，
 * 所有字体获取函数回退到内置 Montserrat 字体。
 */
void lv_freetype_font_deinit(void);

/**
 * @brief 从指定路径加载 FreeType 字体
 * 若字体引擎尚未初始化，则自动初始化；
 * 销毁旧字体（若已加载），从新路径加载各尺寸字体。
 * @param path 字体文件完整路径（如 /sdcard/Fonts/MyFont.ttf）
 * @return LV_RESULT_OK 成功，LV_RESULT_INVALID 失败
 */
lv_result_t lv_freetype_font_load_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* LV_FREETYPE_FONT_H */