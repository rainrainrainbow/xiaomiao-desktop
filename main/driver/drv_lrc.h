/**
 * @file drv_lrc.h
 * @brief LRC 歌词文件解析器
 *
 * 解析标准的 .lrc 歌词文件格式：
 *   [00:12.34]歌词文本
 *   [01:23.45]第二句
 * 支持多时间标签：[00:10.00][00:20.00]同一行
 * 支持元数据标签：[ti:标题] [ar:艺术家] [al:专辑] [by:编辑者]
 */
#ifndef DRV_LRC_H
#define DRV_LRC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 每行歌词最大长度 */
#define LRC_LINE_MAX_LEN   96
/* 最大歌词行数 */
#define LRC_MAX_LINES      64

/* ========== 歌词行 ========== */
typedef struct {
    uint32_t time_ms;        /* 时间戳（毫秒） */
    char text[LRC_LINE_MAX_LEN]; /* 歌词文本 */
} lrc_line_t;

/* ========== 歌词解析器句柄 ========== */
typedef struct {
    lrc_line_t lines[LRC_MAX_LINES];  /* 歌词行数组 */
    int line_count;                   /* 实际歌词行数 */
    char title[LRC_LINE_MAX_LEN];     /* 标题元数据 */
    char artist[LRC_LINE_MAX_LEN];    /* 艺术家元数据 */
    char album[LRC_LINE_MAX_LEN];     /* 专辑元数据 */
    bool loaded;                      /* 是否已加载歌词 */
} lrc_parser_t;

/* ========== API ========== */

/**
 * @brief 初始化歌词解析器
 */
void lrc_parser_init(lrc_parser_t *lrc);

/**
 * @brief 从文件加载歌词
 * @param lrc 解析器句柄
 * @param filepath LRC文件路径（不含扩展名的base路径，如"/sdcard/music/song"）
 * @return true 成功加载（找到.lrc文件且解析出至少1行）
 */
bool lrc_parser_load(lrc_parser_t *lrc, const char *filepath_base);

/**
 * @brief 获取指定时间点的歌词索引
 * @param lrc 解析器句柄
 * @param time_ms 当前播放时间（毫秒）
 * @return 当前应显示的歌词行索引，-1表示无歌词
 */
int lrc_parser_get_line_at(lrc_parser_t *lrc, uint32_t time_ms);

/**
 * @brief 获取歌词行
 */
const lrc_line_t* lrc_parser_get_line(lrc_parser_t *lrc, int index);

/**
 * @brief 获取歌词行数
 */
int lrc_parser_get_count(lrc_parser_t *lrc);

/**
 * @brief 是否已加载歌词
 */
bool lrc_parser_is_loaded(lrc_parser_t *lrc);

/**
 * @brief 清除歌词数据
 */
void lrc_parser_clear(lrc_parser_t *lrc);

#ifdef __cplusplus
}
#endif

#endif /* DRV_LRC_H */
