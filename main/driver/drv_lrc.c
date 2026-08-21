/**
 * @file drv_lrc.c
 * @brief LRC 歌词文件解析器实现
 */
#include "drv_lrc.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "LRC";

void lrc_parser_init(lrc_parser_t *lrc)
{
    if (!lrc) return;
    memset(lrc, 0, sizeof(lrc_parser_t));
    lrc->line_count = 0;
    lrc->loaded = false;
}

/* 解析时间标签 [mm:ss.xx] 返回毫秒，失败返回 -1 */
static int parse_time_tag(const char *str)
{
    if (!str || *str != '[') return -1;
    const char *p = str + 1;
    if (!isdigit((unsigned char)*p)) return -1;

    int minutes = 0, seconds = 0, centis = 0;
    /* 解析分钟 */
    while (isdigit((unsigned char)*p)) {
        minutes = minutes * 10 + (*p - '0');
        p++;
    }
    if (*p != ':') return -1;
    p++;
    /* 解析秒 */
    if (!isdigit((unsigned char)*p)) return -1;
    while (isdigit((unsigned char)*p)) {
        seconds = seconds * 10 + (*p - '0');
        p++;
    }
    /* 可选的小数部分（.xx 或 :xx） */
    if (*p == '.' || *p == ':') {
        p++;
        if (isdigit((unsigned char)*p)) {
            centis = (*p - '0') * 10;
            p++;
            if (isdigit((unsigned char)*p)) {
                centis += (*p - '0');
                p++;
            }
        }
    }
    if (*p != ']') return -1;

    return minutes * 60000 + seconds * 1000 + centis * 10;
}

/* 添加一行歌词（按时间排序插入） */
static void lrc_add_line(lrc_parser_t *lrc, uint32_t time_ms, const char *text)
{
    if (!lrc || !text) return;
    if (lrc->line_count >= LRC_MAX_LINES) return;

    /* 去掉文本首尾空白 */
    while (*text == ' ' || *text == '\t') text++;
    size_t len = strlen(text);
    while (len > 0 && (text[len-1] == ' ' || text[len-1] == '\t' ||
                       text[len-1] == '\r' || text[len-1] == '\n')) {
        len--;
    }
    if (len == 0) return;

    /* 插入排序（按时间） */
    int i = lrc->line_count - 1;
    while (i >= 0 && lrc->lines[i].time_ms > time_ms) {
        lrc->lines[i + 1] = lrc->lines[i];
        i--;
    }
    i++;
    lrc->lines[i].time_ms = time_ms;
    size_t copy_len = len < LRC_LINE_MAX_LEN - 1 ? len : LRC_LINE_MAX_LEN - 1;
    memcpy(lrc->lines[i].text, text, copy_len);
    lrc->lines[i].text[copy_len] = '\0';
    lrc->line_count++;
}

bool lrc_parser_load(lrc_parser_t *lrc, const char *filepath_base)
{
    if (!lrc || !filepath_base) return false;

    lrc_parser_init(lrc);

    /* 尝试 .lrc 文件 */
    char path[256];
    /* 手动拼接避免 -Wformat-truncation（filepath_base 最长 255 字节 + ".lrc" 会截断） */
    size_t base_len = strlen(filepath_base);
    size_t copy_len = base_len;
    if (copy_len > sizeof(path) - 6) copy_len = sizeof(path) - 6;  /* 预留 ".lrc\0" */
    memcpy(path, filepath_base, copy_len);
    memcpy(path + copy_len, ".lrc", 5);
    path[copy_len + 4] = '\0';

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGI(TAG, "No LRC file: %s", path);
        return false;
    }

    ESP_LOGI(TAG, "Loading LRC: %s", path);

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* 去掉换行 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        /* 解析元数据标签 [ti:xxx] [ar:xxx] [al:xxx] [by:xxx] */
        if (line[0] == '[' && (strncmp(line, "[ti:", 4) == 0 ||
                               strncmp(line, "[ar:", 4) == 0 ||
                               strncmp(line, "[al:", 4) == 0 ||
                               strncmp(line, "[by:", 4) == 0)) {
            const char *colon = strchr(line, ':');
            if (colon) {
                const char *val = colon + 1;
                const char *end = strchr(val, ']');
                if (end) {
                    char buf[LRC_LINE_MAX_LEN];
                    size_t vlen = (size_t)(end - val);
                    if (vlen > LRC_LINE_MAX_LEN - 1) vlen = LRC_LINE_MAX_LEN - 1;
                    memcpy(buf, val, vlen);
                    buf[vlen] = '\0';
                    if (strncmp(line, "[ti:", 4) == 0) {
                        strncpy(lrc->title, buf, LRC_LINE_MAX_LEN - 1);
                    } else if (strncmp(line, "[ar:", 4) == 0) {
                        strncpy(lrc->artist, buf, LRC_LINE_MAX_LEN - 1);
                    } else if (strncmp(line, "[al:", 4) == 0) {
                        strncpy(lrc->album, buf, LRC_LINE_MAX_LEN - 1);
                    }
                }
            }
            continue;
        }

        /* 解析时间标签（可能多个） */
        const char *p = line;
        int first_time = -1;
        char text_start[256] = "";
        bool has_time = false;

        while (*p == '[') {
            int t = parse_time_tag(p);
            if (t < 0) break;
            has_time = true;
            if (first_time < 0) first_time = t;
            /* 跳到下一个标签或文本 */
            const char *close = strchr(p, ']');
            if (!close) break;
            p = close + 1;
        }

        if (has_time) {
            /* 剩余部分为歌词文本 */
            strncpy(text_start, p, sizeof(text_start) - 1);
            text_start[sizeof(text_start) - 1] = '\0';
            lrc_add_line(lrc, (uint32_t)first_time, text_start);

            /* 如果有多个时间标签，为每个标签都添加一行（相同文本） */
            /* 简化：只处理第一个标签，后续标签不重复添加 */
        }
    }
    fclose(f);

    if (lrc->line_count > 0) {
        lrc->loaded = true;
        ESP_LOGI(TAG, "LRC loaded: %d lines (title=%s)", lrc->line_count, lrc->title);
        return true;
    }

    ESP_LOGW(TAG, "LRC file has no valid lines: %s", path);
    return false;
}

int lrc_parser_get_line_at(lrc_parser_t *lrc, uint32_t time_ms)
{
    if (!lrc || !lrc->loaded || lrc->line_count <= 0) return -1;

    int result = 0;
    for (int i = 0; i < lrc->line_count; i++) {
        if (lrc->lines[i].time_ms <= time_ms) {
            result = i;
        } else {
            break;
        }
    }
    return result;
}

const lrc_line_t* lrc_parser_get_line(lrc_parser_t *lrc, int index)
{
    if (!lrc || index < 0 || index >= lrc->line_count) return NULL;
    return &lrc->lines[index];
}

int lrc_parser_get_count(lrc_parser_t *lrc)
{
    return lrc ? lrc->line_count : 0;
}

bool lrc_parser_is_loaded(lrc_parser_t *lrc)
{
    return lrc ? lrc->loaded : false;
}

void lrc_parser_clear(lrc_parser_t *lrc)
{
    if (!lrc) return;
    memset(lrc, 0, sizeof(lrc_parser_t));
}