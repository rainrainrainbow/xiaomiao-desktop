/**
 * @file app_sys_api.c
 * @brief 应用系统 API 门面层实现
 *
 * 本层只做"转发 + 统一"，不含业务逻辑。所有底层调用集中在此，
 * 新增 App 时无需再逐个 include 各底层驱动头文件。
 */

#include "app_sys_api.h"
#include "app_manager.h"
#include "esp_log.h"
#include "system/sys_nvs.h"
#include "driver/drv_battery.h"
#include "driver/drv_backlight.h"
#include "driver/drv_led_visualizer.h"
#include "ion/battery.h"
#include "ion/sdcard.h"
#include "lang/lang.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_mac.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

static const char *TAG = "APP_SYS_API";

/* ============================================================
 * 一、系统状态查询
 * ============================================================ */

uint8_t app_sys_battery_percent(void)
{
    return ion_battery_get_percentage();
}

uint16_t app_sys_battery_mv(void)
{
    return ion_battery_read_voltage_mv();
}

bool app_sys_sdcard_ready(void)
{
    return ion_sdcard_is_mounted();
}

bool app_sys_sdcard_try_mount(void)
{
    if (ion_sdcard_is_mounted()) return true;
    return ion_sdcard_init("/sdcard");
}

uint32_t app_sys_heap_internal_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

uint32_t app_sys_psram_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

const char *app_sys_version(void)
{
    return XIAOMIAO_VERSION;
}

/* ============================================================
 * 二、用户配置读写
 * ============================================================ */

int app_sys_cfg_get_int(app_cfg_key_t key, int default_val)
{
    ui_state_t *st = ui_state_get();
    if (!st) return default_val;

    switch (key) {
    case APP_CFG_BRIGHTNESS:   return st->brightness;
    case APP_CFG_VOLUME:       return st->volume;
    case APP_CFG_SOUND_ON:     return st->sound_on ? 1 : 0;
    case APP_CFG_THEME:        return (int)st->theme;
    case APP_CFG_WIFI_ON:      return st->wifi_on ? 1 : 0;
    case APP_CFG_LAYOUT:       return st->layout;
    case APP_CFG_FONT_SIZE:    return st->font_size;
    case APP_CFG_SLEEP_TIMEOUT:return st->sleep_timeout;
    case APP_CFG_LANGUAGE:     return sys_nvs_load_language();
    case APP_CFG_FONT_SOURCE:  return sys_nvs_load_font_source();
    case APP_CFG_AUDIO_OUT:    return sys_nvs_load_audio_output();
    case APP_CFG_MUSIC_EQ:     return sys_nvs_load_music_eq();
    case APP_CFG_MUSIC_LRC:    return sys_nvs_load_music_lrc();
    case APP_CFG_MUSIC_MODE:   return sys_nvs_load_music_mode();
    case APP_CFG_LED_VIS_MODE: return led_visualizer_get_mode();
    case APP_CFG_LED_VIS_COLOR:return led_visualizer_get_color();
    default:                   return default_val;
    }
}

void app_sys_cfg_set_int(app_cfg_key_t key, int value)
{
    ui_state_t *st = ui_state_get();

    switch (key) {
    case APP_CFG_BRIGHTNESS:
        if (st) { st->brightness = value; drv_backlight_set_brightness(value); }
        break;
    case APP_CFG_VOLUME:
        if (st) st->volume = value;
        sys_nvs_save_volume(value);
        break;
    case APP_CFG_SOUND_ON:
        if (st) st->sound_on = (value != 0);
        break;
    case APP_CFG_THEME:
        if (st) { st->theme = (theme_type_t)value; ui_theme_set(st->theme); }
        break;
    case APP_CFG_WIFI_ON:
        if (st) st->wifi_on = (value != 0);
        break;
    case APP_CFG_LAYOUT:
        if (st) st->layout = value;
        break;
    case APP_CFG_FONT_SIZE:
        if (st) st->font_size = value;
        break;
    case APP_CFG_SLEEP_TIMEOUT:
        if (st) st->sleep_timeout = value;
        break;
    case APP_CFG_LANGUAGE:     sys_nvs_save_language(value); break;
    case APP_CFG_FONT_SOURCE:  sys_nvs_save_font_source(value); break;
    case APP_CFG_AUDIO_OUT:    sys_nvs_save_audio_output(value); break;
    case APP_CFG_MUSIC_EQ:     sys_nvs_save_music_eq(value); break;
    case APP_CFG_MUSIC_LRC:    sys_nvs_save_music_lrc(value); break;
    case APP_CFG_MUSIC_MODE:   sys_nvs_save_music_mode(value); break;
    case APP_CFG_LED_VIS_MODE: led_visualizer_set_mode(value); break;
    case APP_CFG_LED_VIS_COLOR:led_visualizer_set_color(value); break;
    default: break;
    }
}

void app_sys_cfg_save_all(void)
{
    ui_state_t *st = ui_state_get();
    if (!st) return;
    /* 核心 7 项批量持久化（与各设置子页现有行为一致） */
    sys_nvs_save_settings(st->brightness, st->volume, st->sound_on,
                          (int)st->theme, st->wifi_on, st->layout, st->font_size);
}

/* ============================================================
 * 三、应用导航
 * ============================================================ */

void app_sys_nav_push(page_type_t type, const page_callbacks_t *cb, const char *title)
{
    if (!cb) return;
    ui_stack_push(type, cb, NULL);
    if (title) ui_statusbar_set_title(title);
}

void app_sys_nav_back(void)
{
    if (ui_stack_depth() > 1) ui_stack_pop();
}

void app_sys_nav_home(void)
{
    ui_stack_back_home();
}

void app_sys_set_title(const char *title)
{
    ui_statusbar_set_title(title);
}

/* ============================================================
 * 四、通用 UI 组件
 * ============================================================ */

lv_obj_t *app_sys_page_create(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    if (!scr) {
        ESP_LOGE(TAG, "failed to create screen");
        return NULL;
    }
    lv_obj_set_size(scr, LCD_H_RES, LCD_V_RES);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    const theme_colors_t *colors = ui_theme_colors();
    if (colors) {
        lv_obj_set_style_bg_color(scr, lv_color_hex(colors->bg), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    }

    ui_statusbar_create(scr);
    if (title) ui_statusbar_set_title(title);
    return scr;
}

lv_obj_t *app_sys_list_create(lv_obj_t *parent)
{
    if (!parent) return NULL;
    lv_obj_t *list = lv_obj_create(parent);
    if (!list) return NULL;

    lv_obj_set_width(list, LCD_H_RES - 8);
    lv_obj_set_height(list, LCD_V_RES - ui_content_y() - DOCK_H - 4);
    lv_obj_set_x(list, 4);
    lv_obj_set_y(list, ui_content_y());
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_style_pad_row(list, 1, 0);

    const theme_colors_t *colors = ui_theme_colors();
    if (colors) {
        lv_obj_set_style_bg_color(list, lv_color_hex(colors->bg), 0);
        lv_obj_set_style_border_color(list, lv_color_hex(colors->border), 0);
    }
    return list;
}

lv_obj_t *app_sys_list_add(lv_obj_t *list, const char *text)
{
    if (!list) return NULL;
    lv_obj_t *label = lv_label_create(list);
    if (!label) return NULL;
    lv_label_set_text(label, text ? text : "");
    return label;
}

void app_sys_list_highlight(lv_obj_t **rows, int count, int sel)
{
    if (!rows) return;
    const theme_colors_t *colors = ui_theme_colors();
    for (int i = 0; i < count; i++) {
        if (!rows[i]) continue;
        if (colors) {
            lv_obj_set_style_bg_color(rows[i],
                lv_color_hex(i == sel ? colors->sel_bg : colors->bg), 0);
            lv_obj_set_style_bg_opa(rows[i],
                i == sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

/* ============================================================
 * 五、文件/目录辅助
 * ============================================================ */

bool app_sys_fs_is_dir(const char *path)
{
    if (!path) return false;
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

bool app_sys_fs_is_file(const char *path)
{
    if (!path) return false;
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

bool app_sys_fs_has_ext(const char *name, const char *ext)
{
    if (!name || !ext) return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    /* 大小写不敏感比较 */
    while (*dot && *ext) {
        char a = *dot, b = *ext;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
        dot++; ext++;
    }
    return *dot == '\0' && *ext == '\0';
}

/* ============================================================
 * 六、存储信息
 *
 * 容量查询统一采用 POSIX statvfs()，而非 FATFS 的 f_getfree()：
 *   1. /flash、/sdcard 均通过 esp_vfs_fat 注册到 VFS，statvfs 自动处理
 *      挂载与卷对象，无需持有 FATFS 的 FATFS* 句柄、也无需关心 mount 状态；
 *   2. f_getfree() 需要 f_mount() 后拿到的卷对象并自行处理挂载/并发，
 *      门面层不适合持有这些底层状态；
 *   3. 未挂载时 statvfs 直接失败返回 0，符合"失败不崩溃"的要求。
 * 注意：ion_sdcard_get_free_space() 当前是简化实现（返回的是总容量），
 *       故剩余空间不用它，一律走 statvfs 的 f_bavail。
 * ============================================================ */

/** 取 path 所在文件系统的容量(KB)，total=true 取总量，false 取可用量 */
static uint32_t storage_space_kb(const char *path, bool total)
{
    struct statvfs st;
    if (!path || statvfs(path, &st) != 0) {
        ESP_LOGW(TAG, "statvfs failed: %s", path ? path : "(null)");
        return 0;
    }
    /* f_frsize 是每个分配单元的字节数，个别实现可能为 0，回退 f_bsize */
    uint64_t unit = st.f_frsize ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
    uint64_t blocks = total ? (uint64_t)st.f_blocks : (uint64_t)st.f_bavail;
    return (uint32_t)((blocks * unit) / 1024);
}

uint32_t app_sys_flash_total_kb(void)
{
    return storage_space_kb("/flash", true);
}

uint32_t app_sys_flash_free_kb(void)
{
    return storage_space_kb("/flash", false);
}

uint32_t app_sys_sdcard_total_kb(void)
{
    if (!ion_sdcard_is_mounted()) return 0;
    return storage_space_kb("/sdcard", true);
}

uint32_t app_sys_sdcard_free_kb(void)
{
    if (!ion_sdcard_is_mounted()) return 0;
    return storage_space_kb("/sdcard", false);
}

bool app_sys_storage_has_space(const char *path, uint32_t need_bytes)
{
    if (!path || path[0] == '\0') return false;

    struct statvfs st;
    if (statvfs(path, &st) != 0) {
        /* path 常是"待创建的目标文件"，此时 statvfs 可能失败；
         * 按前缀回退到所属存储根目录再查一次。 */
        const char *root = (strncmp(path, "/sdcard", 7) == 0) ? "/sdcard" : "/flash";
        if (statvfs(root, &st) != 0) {
            ESP_LOGE(TAG, "statvfs failed: %s (fallback %s)", path, root);
            return false;
        }
    }
    uint64_t unit = st.f_frsize ? (uint64_t)st.f_frsize : (uint64_t)st.f_bsize;
    uint64_t avail = (uint64_t)st.f_bavail * unit;
    return avail >= (uint64_t)need_bytes;
}

/* ============================================================
 * 七、网络信息
 * ============================================================ */

void app_sys_net_mac(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 18) return;

    uint8_t mac[6] = {0};
    esp_err_t err = ESP_FAIL;

    /* 优先取 STA 接口当前 MAC（可能被软件改写过） */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) err = esp_netif_get_mac(netif, mac);
    /* 未创建 netif / WiFi 未初始化时回退到 efuse 出厂 MAC */
    if (err != ESP_OK) err = esp_read_mac(mac, ESP_MAC_WIFI_STA);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get MAC failed: %s", esp_err_to_name(err));
        snprintf(buf, buf_size, "00:00:00:00:00:00");
        return;
    }
    snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void app_sys_net_ip(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
            esp_ip4addr_ntoa(&info.ip, buf, (int)buf_size);
            return;
        }
    }
    strncpy(buf, "0.0.0.0", buf_size - 1);
    buf[buf_size - 1] = '\0';
}

int8_t app_sys_net_rssi(void)
{
    /* 注意：项目统一使用 esp_wifi_sta_get_ap_info()（非 esp_wifi_get_ap_info），
     * 与 app_settings_wifi.c 保持一致。 */
    wifi_ap_record_t ap_info;
    memset(&ap_info, 0, sizeof(ap_info));
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && ap_info.ssid[0] != '\0') {
        return ap_info.rssi;
    }
    return 0;
}

/* ============================================================
 * 八、文件系统写操作封装
 * ============================================================ */

/** POSIX errno -> 统一错误码 */
static app_fs_result_t fs_errno_result(int err)
{
    switch (err) {
    case ENOENT: return APP_FS_ERR_NOTFOUND;
    case EACCES:
    case EPERM:  return APP_FS_ERR_NOACCESS;
    case ENOSPC: return APP_FS_ERR_NOSPACE;
    case EEXIST: return APP_FS_ERR_EXIST;
    default:     return APP_FS_ERR_FAIL;
    }
}

app_fs_result_t app_sys_fs_mkdir(const char *path)
{
    if (!path || path[0] == '\0') return APP_FS_ERR_FAIL;

    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? APP_FS_OK : APP_FS_ERR_EXIST;
    }

    /* 逐级创建父目录；256B 位于内部 RAM 栈，非 PSRAM 栈 */
    char tmp[256];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        ESP_LOGE(TAG, "mkdir path too long: %u", (unsigned)len);
        return APP_FS_ERR_FAIL;
    }
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (stat(tmp, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) return APP_FS_ERR_EXIST;
        } else if (errno == ENOENT) {
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
                return fs_errno_result(errno);
            }
        }
        *p = '/';
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return fs_errno_result(errno);
    }
    return APP_FS_OK;
}

app_fs_result_t app_sys_fs_remove(const char *path)
{
    if (!path || path[0] == '\0') return APP_FS_ERR_NOTFOUND;
    if (remove(path) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "remove(%s) failed: errno=%d", path, err);
        return fs_errno_result(err);
    }
    return APP_FS_OK;
}

app_fs_result_t app_sys_fs_rename(const char *src, const char *dst)
{
    if (!src || !dst || src[0] == '\0' || dst[0] == '\0') return APP_FS_ERR_FAIL;
    if (rename(src, dst) != 0) {
        int err = errno;
        ESP_LOGE(TAG, "rename(%s -> %s) failed: errno=%d", src, dst, err);
        return fs_errno_result(err);
    }
    return APP_FS_OK;
}

int app_sys_fs_copy(const char *src, const char *dst, bool (*cb)(int copied, int total))
{
    if (!src || !dst) return -1;

    /* 4KB 分块缓冲：优先 PSRAM，失败回退内部 RAM。
     * 缓冲走堆分配而非栈，避免 PSRAM 栈/cache_disabled 崩溃风险。 */
    const size_t chunk = 4096;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "copy: PSRAM alloc failed, fallback to internal RAM");
        buf = (uint8_t *)heap_caps_malloc(chunk, MALLOC_CAP_INTERNAL);
    }
    if (!buf) {
        ESP_LOGE(TAG, "copy: no memory for 4KB buffer");
        return -1;
    }

    int total = 0;
    int64_t size = app_sys_fs_size(src);
    if (size > 0 && size <= INT_MAX) total = (int)size;

    FILE *fin = fopen(src, "rb");
    if (!fin) {
        ESP_LOGE(TAG, "copy: open src failed: %s", src);
        heap_caps_free(buf);
        return -1;
    }
    FILE *fout = fopen(dst, "wb");
    if (!fout) {
        ESP_LOGE(TAG, "copy: open dst failed: %s", dst);
        fclose(fin);
        heap_caps_free(buf);
        return -1;
    }

    int copied = 0;
    bool aborted = false;
    while (1) {
        size_t n = fread(buf, 1, chunk, fin);
        if (n > 0) {
            if (fwrite(buf, 1, n, fout) != n) {
                ESP_LOGE(TAG, "copy: write failed (disk full?): %s", dst);
                copied = -1;
                break;
            }
            copied += (int)n;
            if (cb && !cb(copied, total)) {
                aborted = true;
                break;
            }
        }
        if (n < chunk) {
            if (ferror(fin)) {
                ESP_LOGE(TAG, "copy: read failed: %s", src);
                copied = -1;
            }
            break;
        }
    }

    fclose(fout);
    fclose(fin);
    heap_caps_free(buf);

    if (aborted) {
        ESP_LOGI(TAG, "copy aborted by callback: %s -> %s", src, dst);
        remove(dst);   /* 清理未完成的目标文件，避免留下半截数据 */
        return -1;
    }
    if (copied >= 0) {
        ESP_LOGI(TAG, "copy done: %d bytes, %s -> %s", copied, src, dst);
    }
    return copied;
}

int64_t app_sys_fs_size(const char *path)
{
    if (!path || path[0] == '\0') return -1;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (int64_t)st.st_size;
}

/* ============================================================
 * 九、设备信息
 * ============================================================ */

const char *app_sys_chip_model(void)
{
    /* 注意：API 只能取到"芯片型号"，WROVER/WROOM 属于模组封装，无法从
     * esp_chip_info 获得；本函数返回芯片级型号字符串。 */
    esp_chip_info_t info;
    esp_chip_info(&info);
    switch (info.model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C2: return "ESP32-C2";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    default:           return "ESP32";
    }
}

const char *app_sys_sdk_version(void)
{
    return esp_get_idf_version();
}

void app_sys_chip_id(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 17) return;

    /* ESP-IDF 未提供跨芯片统一的 8 字节唯一 ID 读取 API；
     * 这里用出厂烧录的 6 字节 MAC 作为唯一标识，高 2 字节补 0，
     * 输出 16 个 hex 字符（+结尾 NUL 共 17 字节）。 */
    uint8_t id[8] = {0};
    esp_err_t err = esp_read_mac(&id[2], ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get chip id failed: %s", esp_err_to_name(err));
        strncpy(buf, "0000000000000000", buf_size - 1);
        buf[buf_size - 1] = '\0';
        return;
    }
    snprintf(buf, buf_size, "%02X%02X%02X%02X%02X%02X%02X%02X",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}
