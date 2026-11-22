/**
 * @file app_store.c
 * @brief 应用商店 — 浏览、查看详情、安装/升级 MicroPython 应用
 *
 * 架构说明：独立应用文件，通过 app_builtin.h 暴露 g_store_callbacks。
 * 扫描 /sdcard 与 /flash/apps 下的 *.app 目录，解析 app.json 元数据，
 * 支持详情页、安装进度、空间检查、版本比较与安装校验。
 */
#include "app_builtin.h"
#include "app_manager.h"
#include "app_sys_api.h"
#include "ui_framework.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "fonts/lv_freetype_font.h"
#include "lang/lang.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "APP_STORE";

/* ========== 常量 ========== */
#define STORE_MAX_APPS       20    // 最大显示应用数
#define STORE_PATH_LEN       256   // 路径缓冲区大小（d_name 最大 255 字节 + 前缀）
#define STORE_SCAN_DIR       "/sdcard"       // SD 卡扫描根（/*.app）
#define STORE_FLASH_DIR      "/flash/apps"   // 内置存储扫描根（/*.app）
#define STORE_APPS_DIR       "/sdcard/apps"  // 已安装应用目录（与 app_manager 一致）
#define STORE_JSON_MAX       2048            // app.json 读取上限，用完即释放
#define STORE_COPY_MARGIN    (4 * 1024)      // 安装时额外预留 4KB，避免 FAT 元数据耗尽
#define STORE_DETAIL_LINES   10              // 详情页文本行数（含状态/提示）

/* ========== 应用来源 ========== */
typedef enum {
    STORE_SOURCE_SD = 0,      // 内存卡 /sdcard
    STORE_SOURCE_INTERNAL,    // 内置存储 /flash/apps
} store_source_t;

/* ========== 应用条目 ==========
 * 新增字段均为定长数组，整个数组仍是 static（约 20 * 0.6KB），不动态分配。 */
typedef struct {
    char name[32];                  // 目录名（去掉 .app），作为安装路径标识
    char display_name[32];          // app.json 中的 name，缺省同 name
    char path[STORE_PATH_LEN];      // 源目录完整路径
    char version[16];               // 版本号（缺省 "1.0"）
    char author[24];                // 开发者（缺省 "-"）
    char changelog[64];             // 更新说明（缺省 "-"）
    char permissions[32];           // 权限描述（缺省 "-"）
    uint32_t size_kb;               // 源目录累计大小（KB）
    store_source_t source;          // 来源
    bool has_app_json;              // 源目录含 app.json
    bool has_main_py;               // 源目录含 main.py
    bool installed;                 // 已安装在 STORE_APPS_DIR
    bool upgradable;                // 已安装且扫描到更高版本
    bool install_allowed;           // 签名/安装许可校验结果
} store_app_t;

/* ========== 全局状态 ========== */
static lv_obj_t *s_store_obj = NULL;       // 列表/详情容器
static lv_obj_t *s_info_label = NULL;      // 底部信息提示标签
static store_app_t s_apps[STORE_MAX_APPS]; // 扫描到的应用列表
static int s_app_count = 0;                // 应用数量
static int s_sel = 0;                      // 选中索引
static int s_scroll = 0;                   // 列表滚动偏移
static int s_row_h = 16;                   // 行高（根据字体自适应）
static bool s_in_detail = false;           // 当前是否处于详情页
static int s_detail_scroll = 0;            // 详情滚动偏移
static lv_obj_t *s_progress_bar = NULL;    // 安装进度条（详情页内，重建时置 NULL）
static lv_obj_t *s_progress_label = NULL;  // 百分比文本
static int s_copy_done_bytes = 0;          // 已完成文件字节数（跨多个文件累计）
static int s_copy_total_bytes = 0;         // 待复制总字节数
static char s_msg[64] = {0};               // 详情页瞬时提示（成功/失败/校验等）

/* ========== 版本比较 ========== */
/** 去 v 前缀，按 '.' 分段逐段数字比较；返回 >0 表示 a 更新，<0 表示 b 更新。
 *  非数字段（如 -beta）按字符序兜底，与 app_ota.c 的实现保持一致。 */
static int store_version_compare(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    while (*a == 'v' || *a == 'V') a++;
    while (*b == 'v' || *b == 'V') b++;

    while (*a && *b) {
        char *ae = NULL;
        char *be = NULL;
        unsigned long av = strtoul(a, &ae, 10);
        unsigned long bv = strtoul(b, &be, 10);
        if (av != bv) return (av > bv) ? 1 : -1;
        if (ae == a || be == b) {
            if (*a != *b) return (*a > *b) ? 1 : -1;
            a++;
            b++;
            continue;
        }
        if (*ae == '.') ae++;
        if (*be == '.') be++;
        a = ae;
        b = be;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

/* ========== app.json 解析（cJSON，用完即释放） ========== */

/** 读取 app.json 到堆缓冲（优先 PSRAM，失败回退内部 RAM）；调用方负责 heap_caps_free */
static char *store_read_json_file(const char *path)
{
    if (!path || !path[0]) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    char *buf = heap_caps_malloc(STORE_JSON_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) buf = heap_caps_malloc(STORE_JSON_MAX, MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, STORE_JSON_MAX - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/** 取字符串字段，缺失或空串时回落默认值 */
static void store_json_str(cJSON *root, const char *key, char *dst, size_t sz, const char *def)
{
    if (def && def[0]) {
        strncpy(dst, def, sz - 1);
        dst[sz - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
    cJSON *it = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) {
        strncpy(dst, it->valuestring, sz - 1);
        dst[sz - 1] = '\0';
    }
}

/** 权限字段：支持字符串或字符串数组，缺省 "-" */
static void store_json_permissions(cJSON *root, char *dst, size_t sz)
{
    strcpy(dst, "-");
    cJSON *it = cJSON_GetObjectItem(root, "permissions");
    if (!it) it = cJSON_GetObjectItem(root, "permission");
    if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) {
        strncpy(dst, it->valuestring, sz - 1);
        dst[sz - 1] = '\0';
        return;
    }
    if (!cJSON_IsArray(it)) return;

    size_t used = 0;
    dst[0] = '\0';
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, it) {
        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) continue;
        if (used > 0 && used + 1 < sz) dst[used++] = ',';
        size_t len = strlen(item->valuestring);
        if (len > sz - 1 - used) len = sz - 1 - used;
        memcpy(dst + used, item->valuestring, len);
        used += len;
        dst[used] = '\0';
        if (used + 1 >= sz) break;
    }
    if (used == 0) strcpy(dst, "-");
}

/** 本地签名校验：与 app_micropython.c 的 FNV-1a 方案一致
 *  data = name|icon|color|version|author，signature 为 "%08X" */
static bool store_verify_signature(cJSON *root)
{
    cJSON *sig = cJSON_GetObjectItem(root, "signature");
    if (!cJSON_IsString(sig) || !sig->valuestring || !sig->valuestring[0]) return false;

    char name[64], icon[16], color[16], ver[16], author[64];
    store_json_str(root, "name",    name,  sizeof(name),  "");
    store_json_str(root, "icon",    icon,  sizeof(icon),  "");
    store_json_str(root, "color",   color, sizeof(color), "");
    store_json_str(root, "version", ver,   sizeof(ver),   "");
    store_json_str(root, "author",  author,sizeof(author),"");

    char data[256];
    snprintf(data, sizeof(data), "%s|%s|%s|%s|%s", name, icon, color, ver, author);

    uint32_t hash = 0x811C9DC5;
    for (const char *p = data; *p; ++p) {
        hash ^= (unsigned char)*p;
        hash *= 0x01000193;
    }
    char expect[16];
    snprintf(expect, sizeof(expect), "%08X", (unsigned int)hash);
    bool ok = (strcasecmp(sig->valuestring, expect) == 0);
    if (!ok) ESP_LOGW(TAG, "Signature mismatch: expect=%s got=%s", expect, sig->valuestring);
    return ok;
}

/** 解析 app.json 元数据填充条目；字段缺失时保留默认值。
 *  签名策略：app.json 显式声明 signature 时强制校验并走 app_check_install_permission；
 *  未声明 signature 的遗留应用保持兼容（记录告警但仍允许安装）。 */
static void store_parse_meta(const char *json_path, store_app_t *app)
{
    // 默认值
    strncpy(app->display_name, app->name, sizeof(app->display_name) - 1);
    app->display_name[sizeof(app->display_name) - 1] = '\0';
    strncpy(app->version, "1.0", sizeof(app->version) - 1);
    app->version[sizeof(app->version) - 1] = '\0';
    strcpy(app->author, "-");
    strcpy(app->changelog, "-");
    strcpy(app->permissions, "-");
    app->install_allowed = true;

    char *buf = store_read_json_file(json_path);
    if (!buf) return;                       // 无 app.json：全部使用默认值
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "Invalid app.json: %s", json_path);
        heap_caps_free(buf);
        return;
    }

    store_json_str(root, "name",    app->display_name, sizeof(app->display_name), app->name);
    store_json_str(root, "version", app->version,      sizeof(app->version),      "1.0");
    store_json_str(root, "author",  app->author,       sizeof(app->author),       "-");
    store_json_str(root, "changelog", app->changelog,  sizeof(app->changelog),    "");
    if (app->changelog[0] == '\0') {
        store_json_str(root, "description", app->changelog, sizeof(app->changelog), "-");
    }
    store_json_permissions(root, app->permissions, sizeof(app->permissions));

    // 签名/安装许可：通过 app_manager 的公开接口做最终裁决
    cJSON *sig = cJSON_GetObjectItem(root, "signature");
    if (cJSON_IsString(sig) && sig->valuestring && sig->valuestring[0]) {
        bool valid = store_verify_signature(root);
        cJSON *id_it = cJSON_GetObjectItem(root, "id");
        const char *id = cJSON_IsString(id_it) ? id_it->valuestring : NULL;
        app->install_allowed =
            (app_check_install_permission(id, valid ? sig->valuestring : NULL) == APP_INSTALL_OK);
        if (!app->install_allowed) {
            ESP_LOGW(TAG, "Install blocked by permission: %s", app->name);
        }
    } else {
        ESP_LOGW(TAG, "No signature in app.json (%s), allow for compatibility", app->name);
    }

    cJSON_Delete(root);                     // 立即释放，仅把需要的字段拷入静态数组
    heap_caps_free(buf);
}

/** 读取已安装版本的 app.json，仅取 version；失败回落 "0.0" */
static void store_read_installed_version(const char *app_name, char *ver, size_t sz)
{
    strncpy(ver, "0.0", sz - 1);
    ver[sz - 1] = '\0';
    char path[STORE_PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/%s/app.json", STORE_APPS_DIR, app_name);

    char *buf = store_read_json_file(path);
    if (!buf) return;
    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *it = cJSON_GetObjectItem(root, "version");
        if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) {
            strncpy(ver, it->valuestring, sz - 1);
            ver[sz - 1] = '\0';
        }
        cJSON_Delete(root);
    }
    heap_caps_free(buf);
}

/** 统计应用目录内所有普通文件的累计大小（KB）。目录为平铺结构，忽略子目录。 */
static uint32_t store_calc_dir_size_kb(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    uint64_t bytes = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[STORE_PATH_LEN + 16];
        int n = snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(p)) continue;
        if (!app_sys_fs_is_file(p)) continue;
        int64_t s = app_sys_fs_size(p);
        if (s > 0) bytes += (uint64_t)s;
    }
    closedir(d);
    return (uint32_t)(bytes / 1024);
}

/* ========== 扫描来源 ========== */

/** 检查应用是否已安装（在 STORE_APPS_DIR 下） */
static bool is_app_installed(const char *app_name)
{
    char path[STORE_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", STORE_APPS_DIR, app_name);
    return app_sys_fs_is_dir(path);
}

/** 扫描单个目录中的 *.app 应用，填充 s_apps */
static void scan_one_dir(const char *base, store_source_t source)
{
    if (!base || !base[0]) return;
    if (source == STORE_SOURCE_SD && !app_sys_sdcard_ready()) {
        ESP_LOGW(TAG, "SD card not ready, skip %s", base);
        return;
    }
    if (!app_sys_fs_is_dir(base)) return;

    DIR *dir = opendir(base);
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open %s", base);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_app_count < STORE_MAX_APPS) {
        if (entry->d_name[0] == '.') continue;

        // 必须是 *.app 目录
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".app") != 0) continue;

        // d_name 最长 255，显式检测 snprintf 截断，避免误判
        char full_path[STORE_PATH_LEN];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", base, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(full_path)) {
            ESP_LOGW(TAG, "Skip overlong entry: %.20s...", entry->d_name);
            continue;
        }
        if (!app_sys_fs_is_dir(full_path)) continue;

        size_t name_len = dot - entry->d_name;
        if (name_len >= sizeof(s_apps[0].name)) name_len = sizeof(s_apps[0].name) - 1;

        // 去重：同名应用只保留先扫描到的（SD 优先）
        bool dup = false;
        for (int i = 0; i < s_app_count; i++) {
            if (strlen(s_apps[i].name) == name_len &&
                strncmp(s_apps[i].name, entry->d_name, name_len) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        store_app_t *app = &s_apps[s_app_count];
        memset(app, 0, sizeof(*app));
        memcpy(app->name, entry->d_name, name_len);
        app->name[name_len] = '\0';
        strncpy(app->path, full_path, sizeof(app->path) - 1);
        app->path[sizeof(app->path) - 1] = '\0';
        app->source = source;

        char json_path[STORE_PATH_LEN + 16];
        snprintf(json_path, sizeof(json_path), "%s/app.json", full_path);
        app->has_app_json = app_sys_fs_is_file(json_path);
        char py_path[STORE_PATH_LEN + 16];
        snprintf(py_path, sizeof(py_path), "%s/main.py", full_path);
        app->has_main_py = app_sys_fs_is_file(py_path);

        store_parse_meta(json_path, app);          // 无 json 时填默认值
        app->size_kb = store_calc_dir_size_kb(full_path);
        app->installed = is_app_installed(app->name);

        // 版本比较：已安装且源版本更高 → 可升级
        if (app->installed) {
            char installed_ver[16];
            store_read_installed_version(app->name, installed_ver, sizeof(installed_ver));
            app->upgradable = (store_version_compare(app->version, installed_ver) > 0);
        }

        s_app_count++;
        ESP_LOGI(TAG, "Found app: %s v%s src=%s %luKB json=%d py=%d inst=%d upg=%d allow=%d",
                 app->name, app->version,
                 (source == STORE_SOURCE_SD) ? "SD" : "FLASH",
                 (unsigned long)app->size_kb, app->has_app_json, app->has_main_py,
                 app->installed, app->upgradable, app->install_allowed);
    }
    closedir(dir);
}

/** 扫描 SD 卡与内置存储中的应用 */
static void scan_sdcard_apps(void)
{
    s_app_count = 0;
    scan_one_dir(STORE_SCAN_DIR, STORE_SOURCE_SD);
    scan_one_dir(STORE_FLASH_DIR, STORE_SOURCE_INTERNAL);
    ESP_LOGI(TAG, "Scanned %d apps (SD + internal)", s_app_count);
}

/* ========== 安装 / 卸载应用 ========== */

/** 删除已安装应用目录（平铺文件逐个 unlink 后 rmdir，不用 system） */
static bool store_remove_installed(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "opendir failed on uninstall: %s", dir);
        return false;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[STORE_PATH_LEN];
        int n = snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(p)) {
            ESP_LOGW(TAG, "Skip overlong entry during uninstall: %.20s...", e->d_name);
            continue;
        }
        if (app_sys_fs_is_file(p) && unlink(p) != 0) {
            ESP_LOGW(TAG, "unlink failed: %s", p);
        }
    }
    closedir(d);
    if (rmdir(dir) != 0) {
        ESP_LOGE(TAG, "rmdir failed: %s (possibly non-empty)", dir);
        return false;
    }
    return true;
}

/** 复制进度回调：跨文件累计百分比，强制刷新让同步复制期间也能看到进度 */
static bool store_copy_progress_cb(int copied, int total)
{
    (void)total;
    int pct = 0;
    if (s_copy_total_bytes > 0 && copied >= 0) {
        int done = s_copy_done_bytes + copied;
        pct = done * 100 / s_copy_total_bytes;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
    }
    if (s_progress_bar) lv_bar_set_value(s_progress_bar, pct, LV_ANIM_OFF);
    if (s_progress_label) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(s_progress_label, buf);
    }
    // 复制发生在按键事件内、阻塞 UI 线程，需强制刷新一次使进度可见
    if (s_store_obj) lv_obj_invalidate(s_store_obj);
    lv_refr_now(NULL);
    return true;   // 返回 false 会中止复制，这里始终继续
}

/** 安装/升级选中的应用：校验 → 空间检查 → 进度复制 → 刷新桌面 */
static void install_selected_app(void)
{
    if (s_sel < 0 || s_sel >= s_app_count) return;
    store_app_t *app = &s_apps[s_sel];

    // 已安装且无新版本：无需重复安装
    if (app->installed && !app->upgradable) {
        snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_STORE_INSTALLED));
        return;
    }

    // 1) 完整性校验：app.json 与 main.py 必须都存在
    if (!app->has_app_json || !app->has_main_py) {
        ESP_LOGW(TAG, "Verify failed for %s: json=%d py=%d",
                 app->name, app->has_app_json, app->has_main_py);
        snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_STORE_VERIFYFAIL));
        return;
    }

    // 2) 签名/安装许可校验
    if (!app->install_allowed) {
        ESP_LOGW(TAG, "Install not allowed for %s", app->name);
        snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_STORE_VERIFYFAIL));
        return;
    }

    // 目标统一在 /sdcard/apps（app_manager 目前只扫描该目录）
    if (!app_sys_sdcard_ready()) {
        snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_COMMON_FAILED));
        return;
    }

    char dest_dir[STORE_PATH_LEN];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", STORE_APPS_DIR, app->name);

    // 3) 空间检查：需要空间 = 源目录累计大小 + 余量
    uint32_t need_bytes = app->size_kb * 1024 + STORE_COPY_MARGIN;
    if (!app_sys_storage_has_space(dest_dir, need_bytes)) {
        ESP_LOGW(TAG, "No space for %s: need=%u bytes", app->name, (unsigned)need_bytes);
        snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_FILE_NOSPACE));
        return;
    }

    // 升级：先清掉旧版本，避免残留文件
    if (app->installed && !store_remove_installed(dest_dir)) {
        snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_COMMON_FAILED));
        return;
    }
    if (app_sys_fs_mkdir(dest_dir) != APP_FS_OK) {
        ESP_LOGE(TAG, "mkdir failed: %s", dest_dir);
        snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_COMMON_FAILED));
        return;
    }

    char json_src[STORE_PATH_LEN + 16], json_dst[STORE_PATH_LEN + 16];
    char py_src[STORE_PATH_LEN + 16],   py_dst[STORE_PATH_LEN + 16];
    snprintf(json_src, sizeof(json_src), "%s/app.json", app->path);
    snprintf(json_dst, sizeof(json_dst), "%s/app.json", dest_dir);
    snprintf(py_src, sizeof(py_src), "%s/main.py", app->path);
    snprintf(py_dst, sizeof(py_dst), "%s/main.py", dest_dir);

    // 4) 计算总字节，供跨文件进度条使用
    s_copy_total_bytes = 0;
    int64_t js = app_sys_fs_size(json_src);
    if (js > 0) s_copy_total_bytes += (int)js;
    int64_t ps = app_sys_fs_size(py_src);
    if (ps > 0) s_copy_total_bytes += (int)ps;
    s_copy_done_bytes = 0;
    snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_COMMON_LOADING));

    // 5) 用 app_sys_fs_copy 的回调驱动进度
    bool ok = true;
    int r = app_sys_fs_copy(json_src, json_dst, store_copy_progress_cb);
    if (r < 0) ok = false; else s_copy_done_bytes += r;
    r = app_sys_fs_copy(py_src, py_dst, store_copy_progress_cb);
    if (r < 0) ok = false; else s_copy_done_bytes += r;

    if (!ok) {
        ESP_LOGE(TAG, "Install failed for %s", app->name);
        store_remove_installed(dest_dir);         // 清理半成品
        app->installed = false;
        app->upgradable = false;
        snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_COMMON_FAILED));
        return;
    }

    // 6) 重新扫描桌面应用列表
    app_manager_scan_sdcard();
    app->installed = true;
    app->upgradable = false;
    ESP_LOGI(TAG, "App %s installed/upgraded successfully", app->name);
    snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_COMMON_SUCCESS));
}

/** 卸载选中的应用（详情页 KEY_LEFT，保留原有能力） */
static void uninstall_selected_app(void)
{
    if (s_sel < 0 || s_sel >= s_app_count) return;
    store_app_t *app = &s_apps[s_sel];
    if (!app->installed) {
        snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_STORE_NOT_INSTALLED));
        return;
    }

    char dest_dir[STORE_PATH_LEN];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", STORE_APPS_DIR, app->name);
    if (!store_remove_installed(dest_dir)) {
        snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_COMMON_FAILED));
        return;
    }
    app_manager_scan_sdcard();
    app->installed = false;
    app->upgradable = false;
    snprintf(s_msg, sizeof(s_msg), "%s", lang_get(STR_COMMON_SUCCESS));
}

/* ========== UI 渲染 ========== */

/** 详情页第 i 行文本 */
static void store_detail_line(store_app_t *app, int i, char *buf, size_t sz)
{
    switch (i) {
    case 0:
        snprintf(buf, sz, "%s: %s", lang_get(STR_FILE_NAME), app->display_name); break;
    case 1:
        snprintf(buf, sz, "%s: %s", lang_get(STR_STORE_VERSION), app->version); break;
    case 2:
        snprintf(buf, sz, "%s: %s", lang_get(STR_STORE_AUTHOR), app->author); break;
    case 3:
        snprintf(buf, sz, "%s: %lu KB", lang_get(STR_FILE_SIZE), (unsigned long)app->size_kb); break;
    case 4:
        snprintf(buf, sz, "%s: %s", lang_get(STR_STORE_SOURCE),
                 (app->source == STORE_SOURCE_SD) ? lang_get(STR_STORE_SD)
                                                  : lang_get(STR_STORE_INTERNAL)); break;
    case 5:
        snprintf(buf, sz, "%s: %s", lang_get(STR_STORE_PERM), app->permissions); break;
    case 6:
        snprintf(buf, sz, "%s: %s", lang_get(STR_STORE_CHANGELOG), app->changelog); break;
    case 7:
        snprintf(buf, sz, "%s", app->upgradable ? lang_get(STR_STORE_UPGRADE)
                              : (app->installed ? lang_get(STR_STORE_INSTALLED)
                                                : lang_get(STR_STORE_NOT_INSTALLED))); break;
    case 8:
        snprintf(buf, sz, "%s", s_msg); break;
    case 9:
    default:
        // 操作提示无对应 lang 词条，按 app_ota.c 的做法做双语字面量
        snprintf(buf, sz, "%s", (lang_get_current() == LANG_EN)
                 ? "A=Install/Upgrade L=Uninstall B=Back"
                 : "A=安装/升级 L=卸载 B=返回");
        break;
    }
}

/** 渲染详情页：字段按行显示，UP/DOWN 滚动，底部固定进度条与百分比 */
static void store_render_detail(void)
{
    if (!s_store_obj) return;
    lv_obj_clean(s_store_obj);
    s_progress_bar = NULL;
    s_progress_label = NULL;

    const theme_colors_t *colors = ui_theme_colors();
    int font_px = ui_state_get()->font_size;
    if (font_px < 12) font_px = 12;
    if (font_px > 16) font_px = 16;          // 详情字段较多，适当收紧字号
    int line_h = font_px + 3;
    int content_h = LCD_V_RES - ui_content_y() - DOCK_H - 12;
    int vis_lines = content_h / line_h - 1;  // 预留 1 行给进度条
    if (vis_lines < 1) vis_lines = 1;

    if (s_detail_scroll < 0) s_detail_scroll = 0;
    if (s_detail_scroll > STORE_DETAIL_LINES - vis_lines) s_detail_scroll = STORE_DETAIL_LINES - vis_lines;
    if (s_detail_scroll < 0) s_detail_scroll = 0;

    store_app_t *app = (s_sel >= 0 && s_sel < s_app_count) ? &s_apps[s_sel] : NULL;
    char buf[128];
    for (int i = 0; i < vis_lines && (i + s_detail_scroll) < STORE_DETAIL_LINES; i++) {
        int idx = i + s_detail_scroll;
        if (app) store_detail_line(app, idx, buf, sizeof(buf)); else buf[0] = '\0';

        lv_obj_t *lbl = lv_label_create(s_store_obj);
        if (!lbl) continue;
        lv_obj_set_style_text_font(lbl, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(lbl,
            lv_color_hex((idx == 7 || idx == 8) ? colors->sel_border : colors->text), 0);
        lv_obj_set_width(lbl, LCD_H_RES - 4);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_pos(lbl, 2, i * line_h);
        lv_label_set_text(lbl, buf);
    }

    // 固定进度条（护眼主题色，不使用高饱和色）
    s_progress_bar = lv_bar_create(s_store_obj);
    if (s_progress_bar) {
        lv_obj_remove_style_all(s_progress_bar);
        lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(colors->border), 0);
        lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_progress_bar, 2, 0);
        lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(colors->text), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_progress_bar, 2, LV_PART_INDICATOR);
        lv_obj_set_size(s_progress_bar, LCD_H_RES - 46, 8);
        lv_obj_set_pos(s_progress_bar, 4, content_h - 10);
        lv_bar_set_range(s_progress_bar, 0, 100);
        lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    }
    s_progress_label = lv_label_create(s_store_obj);
    if (s_progress_label) {
        lv_obj_set_style_text_font(s_progress_label, lv_font_cn_get(font_px), 0);
        lv_obj_set_style_text_color(s_progress_label, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_width(s_progress_label, 38);
        lv_obj_set_pos(s_progress_label, LCD_H_RES - 42, content_h - 13);
        lv_label_set_text(s_progress_label, "0%");
    }
}

/** 重新构建列表可见行 */
static void store_rebuild_visible(void)
{
    if (s_in_detail) {
        store_render_detail();
        return;
    }
    if (!s_store_obj) return;

    lv_obj_clean(s_store_obj);
    s_progress_bar = NULL;
    s_progress_label = NULL;

    const theme_colors_t *colors = ui_theme_colors();
    ui_state_t *st = ui_state_get();
    int font_px = st->font_size;
    if (font_px < 14) font_px = 14;
    if (font_px > 24) font_px = 24;
    s_row_h = font_px + 2;
    if (s_row_h < 16) s_row_h = 16;

    int vis_rows = (LCD_V_RES - ui_content_y() - DOCK_H) / s_row_h;
    if (vis_rows < 1) vis_rows = 1;

    // 确保选中项可见
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + vis_rows) s_scroll = s_sel - vis_rows + 1;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > s_app_count - vis_rows) s_scroll = s_app_count - vis_rows;
    if (s_scroll < 0) s_scroll = 0;

    for (int i = 0; i < vis_rows && (i + s_scroll) < s_app_count; i++) {
        int idx = i + s_scroll;
        store_app_t *app = &s_apps[idx];

        lv_obj_t *row = lv_obj_create(s_store_obj);
        if (!row) {
            ESP_LOGE(TAG, "lv_obj_create(row) failed! mem free=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            continue;
        }
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, 0, i * s_row_h);
        lv_obj_set_size(row, LCD_H_RES, s_row_h);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // 选中高亮
        if (idx == s_sel) {
            lv_obj_set_style_bg_color(row, lv_color_hex(colors->sel_bg), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }

        // 应用展示名（app.json name，缺省为目录名）
        lv_obj_t *name_lbl = lv_label_create(row);
        if (name_lbl) {
            lv_obj_set_style_text_font(name_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(name_lbl, lv_color_hex(colors->text), 0);
            lv_obj_set_width(name_lbl, LCD_H_RES - 88);
            lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
            lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 2, 0);
            lv_label_set_text(name_lbl, app->display_name);
        }

        // 版本（小字，便于识别可升级项）
        lv_obj_t *ver_lbl = lv_label_create(row);
        if (ver_lbl) {
            lv_obj_set_style_text_font(ver_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(ver_lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_align(ver_lbl, LV_ALIGN_RIGHT_MID, -52, 0);
            lv_label_set_text(ver_lbl, app->version);
        }

        // 状态：可升级 / 已安装 / 未安装
        lv_obj_t *status_lbl = lv_label_create(row);
        if (status_lbl) {
            lv_obj_set_style_text_font(status_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(status_lbl,
                lv_color_hex(app->upgradable ? colors->sel_border
                             : (app->installed ? 0x6FA891 : colors->text_dim)), 0);
            lv_obj_align(status_lbl, LV_ALIGN_RIGHT_MID, -2, 0);
            lv_label_set_text(status_lbl, app->upgradable ? lang_get(STR_STORE_UPGRADE)
                              : (app->installed ? lang_get(STR_STORE_INSTALLED)
                                                : lang_get(STR_STORE_NOT_INSTALLED)));
        }
    }

    // 无应用提示：区分"未插卡"与"卡内无应用"
    if (s_app_count == 0) {
        lv_obj_t *empty_lbl = lv_label_create(s_store_obj);
        if (empty_lbl) {
            lv_obj_set_style_text_font(empty_lbl, lv_font_cn_get(font_px), 0);
            lv_obj_set_style_text_color(empty_lbl, lv_color_hex(colors->text_dim), 0);
            lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 0);
            // 语言包未含"未插卡"词条且不可修改 lang/*，此处用字面量
            const char *msg = app_sys_sdcard_ready()
                                ? lang_get(STR_STORE_EMPTY)
                                : "未检测到SD卡\n请插入SD卡后重试";
            lv_label_set_text(empty_lbl, msg);
        }
    }
}

/** 更新底部信息标签（详情页有独立的提示行，此处留空） */
static void update_info_label(void)
{
    if (!s_info_label) return;
    if (s_in_detail) {
        lv_label_set_text(s_info_label, "");
        return;
    }
    const theme_colors_t *colors = ui_theme_colors();
    if (s_app_count > 0 && s_sel >= 0 && s_sel < s_app_count) {
        store_app_t *app = &s_apps[s_sel];
        char buf[96];
        snprintf(buf, sizeof(buf), "A=%s v%s | %s%s",
                 app->upgradable ? lang_get(STR_STORE_UPGRADE) : lang_get(STR_STORE_DETAIL),
                 app->version,
                 app->has_app_json ? "✓json" : "✗json",
                 app->has_main_py ? " ✓py" : " ✗py");
        lv_obj_set_style_text_color(s_info_label, lv_color_hex(colors->text_dim), 0);
        lv_label_set_text(s_info_label, buf);
    } else {
        lv_label_set_text(s_info_label, "");
    }
}

/* ========== 页面生命周期回调 ========== */

static void store_init(void *data)
{
    (void)data;
    ESP_LOGI(TAG, "Store init");

    s_in_detail = false;
    s_detail_scroll = 0;
    s_msg[0] = '\0';

    lv_obj_t *scr = lv_screen_active();
    const theme_colors_t *colors = ui_theme_colors();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    ui_statusbar_create(scr);
    ui_statusbar_set_title(lang_get(STR_APP_STORE));

    // 计算行高
    s_row_h = ui_state_get()->font_size + 2;
    if (s_row_h < 16) s_row_h = 16;

    // 列表/详情容器
    s_store_obj = lv_obj_create(scr);
    if (!s_store_obj) {
        ESP_LOGE(TAG, "lv_obj_create(store_obj) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    lv_obj_remove_style_all(s_store_obj);
    lv_obj_set_pos(s_store_obj, 0, ui_content_y());
    lv_obj_set_size(s_store_obj, LCD_H_RES, LCD_V_RES - ui_content_y() - DOCK_H - 12);
    lv_obj_clear_flag(s_store_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 底部信息栏
    s_info_label = lv_label_create(scr);
    if (!s_info_label) {
        ESP_LOGE(TAG, "lv_label_create(info_label) failed! mem free=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    } else {
        lv_obj_set_style_text_font(s_info_label, lv_font_cn_get(ui_state_get()->font_size), 0);
        lv_obj_set_style_text_color(s_info_label, lv_color_hex(colors->text_dim), 0);
        lv_obj_set_width(s_info_label, LCD_H_RES - 4);
        lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_pos(s_info_label, 2, LCD_V_RES - DOCK_H - 12);
    }

    // 底部导航栏
    ui_dock_create(scr, 1, 0);

    // 扫描应用
    s_sel = 0;
    s_scroll = 0;
    scan_sdcard_apps();
    store_rebuild_visible();
    update_info_label();
}

static void store_destroy(void)
{
    ESP_LOGI(TAG, "Store destroy");
    s_store_obj = NULL;
    s_info_label = NULL;
    s_progress_bar = NULL;
    s_progress_label = NULL;
}

static void store_activate(void)
{
    ESP_LOGI(TAG, "Store activate");
    // 重新扫描（可能有新安装的应用），并回到列表视图
    s_in_detail = false;
    s_detail_scroll = 0;
    s_msg[0] = '\0';
    ui_statusbar_set_title(lang_get(STR_APP_STORE));
    scan_sdcard_apps();
    store_rebuild_visible();
    update_info_label();
}

static bool store_on_key(int key)
{
    // ========== 详情页按键 ==========
    if (s_in_detail) {
        switch (key) {
        case KEY_UP:
            if (s_detail_scroll > 0) {
                s_detail_scroll--;
                store_render_detail();
            }
            return true;
        case KEY_DOWN:
            if (s_detail_scroll < STORE_DETAIL_LINES - 1) {
                s_detail_scroll++;
                store_render_detail();
            }
            return true;
        case KEY_A:                         // 安装 / 升级
            install_selected_app();
            store_render_detail();
            update_info_label();
            return true;
        case KEY_LEFT:                      // 卸载（保留原能力）
            uninstall_selected_app();
            store_render_detail();
            return true;
        case KEY_B:                         // 返回列表
            s_in_detail = false;
            s_detail_scroll = 0;
            s_msg[0] = '\0';
            ui_statusbar_set_title(lang_get(STR_APP_STORE));
            store_rebuild_visible();
            update_info_label();
            return true;
        default:
            return true;
        }
    }

    // ========== 列表页按键 ==========
    switch (key) {
        case KEY_UP:
            if (s_sel > 0) {
                s_sel--;
                store_rebuild_visible();
                update_info_label();
            }
            return true;
        case KEY_DOWN:
            if (s_sel < s_app_count - 1) {
                s_sel++;
                store_rebuild_visible();
                update_info_label();
            }
            return true;
        case KEY_A:                         // 进入详情页（不再直接安装）
            if (s_app_count > 0 && s_sel >= 0 && s_sel < s_app_count) {
                s_in_detail = true;
                s_detail_scroll = 0;
                s_msg[0] = '\0';
                ui_statusbar_set_title(lang_get(STR_STORE_DETAIL));
                store_rebuild_visible();
                update_info_label();
            }
            return true;
        case KEY_B:
            if (ui_stack_depth() > 1) ui_stack_pop();
            return true;
        default:
            return true;
    }
}

/* ========== 页面回调定义（全局可见） ========== */
const page_callbacks_t g_store_callbacks = {
    .init = store_init,
    .activate = store_activate,
    .destroy = store_destroy,
    .on_key = store_on_key,
};
