/**
 * @file lang.h
 * @brief 多语言支持框架
 *
 * 支持中英文切换，所有UI字符串通过 lang_get() 获取。
 * 新增语言只需添加 lang_zh.h / lang_en.h 翻译表。
 */
#ifndef LANG_H
#define LANG_H

#include <string.h>
#include <stdbool.h>

/* ========== 语言ID ========== */
typedef enum {
    LANG_ZH = 0,  // 中文
    LANG_EN,      // 英文
    LANG_MAX
} lang_id_t;

/* ========== 字符串ID ========== */
typedef enum {
    STR_WIFI = 0,
    STR_WIFI_ON,
    STR_WIFI_OFF,
    STR_WIFI_SCANNING,
    STR_WIFI_CONNECTING,
    STR_WIFI_CONNECTED,
    STR_WIFI_CLOSED,
    STR_WIFI_SCANNED,
    STR_BRIGHTNESS,
    STR_VOLUME,
    STR_THEME,
    STR_THEME_DARK,
    STR_THEME_LIGHT,
    STR_LAYOUT,
    STR_LAYOUT_3COL,
    STR_LAYOUT_2COL,
    STR_FONT,
    STR_SOUND,
    STR_SOUND_ON,
    STR_SOUND_OFF,
    STR_SLEEP_TIMEOUT,
    STR_DATE_TIME,
    STR_APP_MANAGER,
    STR_ABOUT,
    STR_RESET_DEFAULT,
    STR_SAVE_EXIT,
    STR_RETURN_LOADER,
    STR_SETTINGS,
    STR_DESKTOP,
    STR_RECENTS,
    STR_RECENTS_EMPTY,
    STR_CURRENT,
    STR_BACKGROUND,
    STR_BACK,
    STR_SYSTEM,
    STR_VERSION,
    STR_BUILD,
    STR_CHIP,
    STR_SCREEN,
    STR_PYTHON,
    STR_FONT_ENGINE,
    STR_BATTERY,
    STR_MEMORY,
    STR_FIRMWARE,
    STR_CPU_FREQ,
    STR_PSRAM,
    STR_FLASH,
    STR_UPTIME,
    STR_SDK_VERSION,
    STR_LVGL_VERSION,
    STR_RETRO_CORE,
    STR_LANGUAGE,
    STR_FONT_SOURCE,
    STR_FONT_SOURCE_FREETYPE,
    STR_FONT_SOURCE_BUILTIN,
    // 应用名
    STR_APP_SETTINGS,
    STR_APP_MUSIC,
    STR_APP_STORE,
    STR_APP_FILES,
    STR_APP_MID,
    STR_APP_PYTHON,
    STR_APP_SHELL,           // Shell 命令终端
    STR_APP_APPS,
    STR_APP_LED,
    // 应用类型标签
    STR_APP_TYPE_BUILTIN,
    STR_APP_TYPE_PYTHON,
    // 设置项名称（二级页面标题）
    STR_AUDIO_OUTPUT,
    // 布局设置
    STR_LAYOUT_3COL_DESC,
    STR_LAYOUT_2COL_DESC,
    STR_LAYOUT_HINT,
    STR_CURRENT_VALUE,
    // 字体设置
    STR_FONT_SIZE_SMALL,
    STR_FONT_SIZE_MEDIUM,
    STR_FONT_SIZE_LARGE,
    STR_FONT_SIZE_XLARGE,
    STR_FONT_PREVIEW,
    // 屏幕超时
    STR_SLEEP_NEVER,
    STR_SLEEP_30S,
    STR_SLEEP_60S,
    STR_SLEEP_2M,
    STR_SLEEP_5M,
    // 音频设置
    STR_AUDIO_MODE,
    STR_AUDIO_MODE_AUTO,
    STR_AUDIO_MODE_MANUAL,
    STR_AUDIO_UNAVAILABLE,
    STR_AUDIO_HINT,
    // 蓝牙设置
    STR_BLUETOOTH,
    STR_BT_STATUS,
    STR_BT_ON,
    STR_BT_OFF,
    STR_BT_CONNECTED,
    STR_BT_IDLE,
    STR_BT_PAIR_MODE,
    STR_BT_PEER,
    STR_BT_HINT,
    STR_BT_PAIR_OK,
    STR_BT_PAIR_FAIL,
    STR_BT_REINIT,
    // 亮度/音量提示
    STR_BRIGHTNESS_HINT,
    STR_VOLUME_HINT,
    // 音乐提示
    STR_MUSIC_PLAY_HINT,
    STR_MUSIC_HINT,
    // 音乐播放器三页面
    STR_MUSIC_LIST,          // 播放列表
    STR_MUSIC_SETTINGS,      // 音乐设置
    STR_MUSIC_SPECTRUM,      // 频谱
    STR_MUSIC_LYRICS,        // 歌词
    STR_MUSIC_LRC_OFF,       // 歌词:关
    STR_MUSIC_LRC_ON,        // 歌词:开
    STR_MUSIC_LOOP_MODE,     // 循环模式
    STR_MUSIC_LOOP_SINGLE,   // 单曲循环
    STR_MUSIC_LOOP_LIST,     // 列表循环
    STR_MUSIC_LOOP_RANDOM,   // 随机播放
    STR_MUSIC_PREV,          // 上一首
    STR_MUSIC_NEXT,          // 下一首
    STR_MUSIC_PLAY,          // 播放
    STR_MUSIC_PAUSE,         // 暂停
    STR_MUSIC_STOP,          // 停止
    STR_MUSIC_PLAYING,       // 正在播放
    STR_MUSIC_NO_FILE,       // 无音频文件
    STR_MUSIC_SPECTRUM_OFF,  // 频谱:关
    STR_MUSIC_SPECTRUM_ON,   // 频谱:开
    STR_MUSIC_SETTINGS_HINT, // 设置页操作提示
    STR_MUSIC_PLAY_HINT2,    // 播放页操作提示
    // 文件管理器
    STR_FILE_EMPTY_DIR,
    STR_FILE_UNSUPPORTED,
    STR_FILE_MID_REMOVED,
    STR_FILE_AUDIO_NA,
    // 商店
    STR_STORE_INSTALLED,
    STR_STORE_NOT_INSTALLED,
    STR_STORE_EMPTY,
    STR_STORE_INSTALL,
    STR_STORE_UNINSTALL,
    // MicroPython
    STR_MP_LOADING,
    STR_MP_OK,
    STR_MP_FAIL,
    STR_MP_NO_ENTRY,
    STR_MP_DONE,
    STR_MP_EXEC_FAIL,
    STR_MP_NO_SD_CARD,
    // 安装状态
    STR_INSTALL_OK,
    STR_INSTALL_BLOCKED,
    STR_INSTALL_UNTRUSTED,
    STR_INSTALL_UNKNOWN,
    // 日期时间
    STR_DATETIME_SYNC,
    STR_DATETIME_SYNCING,
    STR_DATETIME_SYNC_OK,
    STR_DATETIME_SYNC_FAIL,
    STR_DATETIME_SYNC_HINT,
    // WiFi
    STR_WIFI_MODE_STA,
    STR_WIFI_MODE_AP,
    STR_WIFI_AP_RUNNING,
    STR_WIFI_CONNECTED_PREFIX,
    STR_WIFI_SCANNED_PREFIX,
    STR_WIFI_NAME,
    STR_WIFI_PASSWORD,
    STR_WIFI_CHANNEL,
    STR_WIFI_SCAN_HINT,
    STR_IRAM,
    STR_LANGUAGE_ZH,
    STR_LANGUAGE_EN,
    // OTA
    STR_OTA_IDLE,
    STR_OTA_CHECKING,
    STR_OTA_DOWNLOADING,
    STR_OTA_READY,
    STR_OTA_ERROR,
    STR_OTA_LATEST_VERSION,
    STR_OTA_REBOOT_HINT,
    STR_OTA_RETRY_HINT,
    // 灯效
    STR_LED_EFFECT_OFF,
    STR_LED_EFFECT_STATIC,
    STR_LED_EFFECT_BREATH,
    STR_LED_EFFECT_RAINBOW,
    STR_LED_EFFECT_FLOW,
    STR_LED_EFFECT_BLINK,
    STR_LED_COLOR_RED,
    STR_LED_COLOR_ORANGE,
    STR_LED_COLOR_YELLOW,
    STR_LED_COLOR_GREEN,
    STR_LED_COLOR_CYAN,
    STR_LED_COLOR_BLUE,
    STR_LED_COLOR_PURPLE,
    STR_LED_COLOR_PINK,
    STR_LED_COLOR_WHITE,
    STR_LED_COLOR_SEP,
    STR_LED_BRIGHTNESS,
    // LED 音乐律动
    STR_LED_VIS_TITLE,       // LED音乐律动
    STR_LED_VIS_OFF,         // 关闭
    STR_LED_VIS_SINGLE,      // 单色律动
    STR_LED_VIS_RAINBOW,     // 彩虹律动
    STR_LED_VIS_PULSE,       // 节拍脉冲
    STR_LED_EFFECT_MUSIC,    // 音乐律动（LED app 效果项）
    // Shell 命令终端
    STR_SHELL_WELCOME,       // Shell 欢迎信息
    STR_SHELL_INPUT,         // 输入提示
    STR_SHELL_HELP,          // 帮助
    STR_SHELL_UNKNOWN,       // 未知命令
    STR_SHELL_SCROLL,        // 滚动提示
    STR_SHELL_AVAIL,         // 可用命令标题
    STR_SHELL_CODE,          // 代码（py 命令参数提示）
    STR_SHELL_TITLE,         // 键盘标题
    // 设置-关于系统：状态/字体引擎/内存信息
    STR_STATUS_READY,        // Python 就绪
    STR_STATUS_NOT_READY,    // Python 未就绪
    STR_FONT_ENGINE_FREETYPE,// 字体引擎: FreeType
    STR_FONT_ENGINE_BUILTIN, // 字体引擎: 内置
    STR_MEM_MIN_FREE,        // 内存最低空闲
    STR_MEM_MAX_BLOCK,       // 内存最大连续块
    // 恢复模式（Recovery）
    STR_RECOVERY,            // 恢复模式
    STR_REC_FACTORY_RESET,   // 恢复出厂设置
    STR_REC_CLEAR_WIFI,      // 清除WiFi凭据
    STR_REC_OTA_RESTART,     // 重新开始OTA升级
    STR_REC_SD_FLASH,        // 从SD卡刷机
    STR_REC_SYS_INFO,        // 查看系统信息
    STR_REC_REBOOT_MAIN,     // 重启到主系统
    STR_REC_HINT,            // 操作提示
    STR_REC_CONFIRM,         // 二次确认提示
    STR_REC_CANCELED,        // 已取消
    STR_REC_FACTORY_DONE,    // 设置已清空
    STR_REC_FAIL,            // 操作失败
    STR_REC_WIFI_DONE,       // WiFi凭据已清除
    STR_REC_SD_MOUNTED,      // SD卡已挂载
    STR_REC_SD_UNMOUNTED,    // SD卡未挂载
    STR_REC_SD_NOFW,         // 未发现固件
    STR_REC_SD_FOUND,        // 发现固件
    STR_REC_OTA_HINT,        // OTA升级提示
    STR_REC_REBOOTING,       // 正在重启
    STR_REC_INFO_BOOT,       // 启动分区
    STR_REC_INFO_PARTS,      // 分区数量
    STR_REC_INFO_MEM,        // 可用内存
    // ===== 需求文档对齐新增（批次A契约）=====
    STR_BG_TASKS,  // 后台任务
    STR_BG_SERVICES,  // 后台服务
    STR_BG_NOTIFY,  // 通知中心
    STR_BG_AUTOSTART,  // 自启动
    STR_BG_EMPTY,  // 暂无后台任务
    STR_BG_PAUSE,  // 暂停
    STR_BG_RESUME,  // 继续
    STR_BG_CANCEL,  // 取消
    STR_BG_RETRY,  // 重试
    STR_BG_PROGRESS,  // 进度
    STR_NOTIFY_CLEAR,  // 清空通知
    STR_NOTIFY_READALL,  // 全部已读
    STR_NOTIFY_EMPTY,  // 暂无通知
    STR_FILE_NEW,  // 新建
    STR_FILE_RENAME,  // 重命名
    STR_FILE_DELETE,  // 删除
    STR_FILE_COPY,  // 复制
    STR_FILE_CUT,  // 剪切
    STR_FILE_PASTE,  // 粘贴
    STR_FILE_MOVE,  // 移动
    STR_FILE_PROPERTIES,  // 属性
    STR_FILE_SEARCH,  // 搜索
    STR_FILE_SORT,  // 排序
    STR_FILE_RECYCLE,  // 回收站
    STR_FILE_RESTORE,  // 还原
    STR_FILE_PURGE,  // 彻底删除
    STR_FILE_EMPTY_RECYCLE,  // 清空回收站
    STR_FILE_SIZE,  // 大小
    STR_FILE_MODIFIED,  // 修改时间
    STR_FILE_TYPE,  // 类型
    STR_FILE_PATH,  // 路径
    STR_FILE_FREE,  // 剩余空间
    STR_FILE_NOSPACE,  // 空间不足
    STR_FILE_DELETED,  // 已删除
    STR_FILE_COPIED,  // 已复制
    STR_FILE_MOVED,  // 已移动
    STR_FILE_CONFIRM_DEL,  // 再按A确认删除
    STR_FILE_NAME,  // 名称
    STR_FILE_TIME,  // 时间
    STR_MUSIC_BACKGROUND,  // 后台播放
    STR_MUSIC_SEEK,  // 拖动进度
    STR_MUSIC_QUEUE,  // 播放队列
    STR_MUSIC_ORDER,  // 顺序播放
    STR_MUSIC_NO_LRC,  // 暂无歌词
    STR_MUSIC_EQ_POP,  // 流行
    STR_MUSIC_EQ_ROCK,  // 摇滚
    STR_MUSIC_EQ_CLASSICAL,  // 古典
    STR_MUSIC_EQ_BASS,  // 重低音
    STR_MUSIC_EQ_OFF,  // 关闭音效
    STR_MUSIC_EQ,  // 音效
    STR_MUSIC_ONLINE,  // 在线音乐
    STR_MUSIC_CACHE,  // 离线缓存
    STR_MUSIC_LOADING,  // 加载中
    STR_MUSIC_NETERR,  // 网络异常
    STR_SHELL_HISTORY,  // 历史命令
    STR_SHELL_WEBREPL,  // WebREPL
    STR_SHELL_CONNECT,  // 连接
    STR_SHELL_DISCONNECT,  // 断开
    STR_SHELL_PASSWORD,  // 密码
    STR_SHELL_REMOTE,  // 远程访问
    STR_SHELL_CURSOR,  // 光标
    STR_OTA_CURRENT,  // 当前版本
    STR_OTA_LATEST,  // 最新版本
    STR_OTA_CHANGELOG,  // 更新日志
    STR_OTA_SAFEMODE,  // 安全升级
    STR_OTA_VERIFYING,  // 校验中
    STR_OTA_BACKUP,  // 备份配置
    STR_OTA_ROLLBACK,  // 已回滚
    STR_ABOUT_DETAIL,  // 详细信息
    STR_ABOUT_SUMMARY,  // 设备信息
    STR_ABOUT_SERIAL,  // 序列号
    STR_ABOUT_HWVER,  // 硬件版本
    STR_ABOUT_STORAGE,  // 存储信息
    STR_ABOUT_NETWORK,  // 网络信息
    STR_ABOUT_MAC,  // MAC地址
    STR_ABOUT_IPV4,  // IPv4地址
    STR_ABOUT_CPU,  // CPU占用
    STR_ABOUT_TEMP,  // 温度
    STR_ABOUT_LICENSE,  // 开源许可
    STR_ABOUT_EULA,  // 用户协议
    STR_ABOUT_SUPPORT,  // 支持与反馈
    STR_ABOUT_CHECKUPDATE,  // 检查更新
    STR_ABOUT_SD_TOTAL,  // 内存卡容量
    STR_ABOUT_FLASH_TOTAL,  // 内部存储
    STR_LED_SPEED,  // 速度
    STR_LED_DIRECTION,  // 方向
    STR_LED_LOOP,  // 循环次数
    STR_LED_SENSITIVITY,  // 灵敏度
    STR_LED_SCHEDULE,  // 定时开关
    STR_LED_SAVE,  // 保存方案
    STR_LED_LOAD,  // 加载方案
    STR_LED_PIXEL,  // 逐颗设色
    STR_LED_POWER_TIP,  // 请注意功耗
    STR_SET_FORGET,  // 忘记网络
    STR_SET_IP,  // IP地址
    STR_SET_AUTO_BRIGHT,  // 自动亮度
    STR_SET_TIMER_OFF,  // 定时关机
    STR_SET_TIME_MANUAL,  // 手动设置时间
    STR_SET_TIMEZONE,  // 时区
    STR_SET_TIME_12H,  // 12小时制
    STR_SET_MEDIA_VOL,  // 媒体音量
    STR_SET_TIP_VOL,  // 提示音音量
    STR_SET_TESTTONE,  // 测试音
    STR_SET_FACTORY_SCOPE,  // 将清除所有用户数据
    STR_SET_APP_FREEZE,  // 冻结应用
    STR_SET_BT_MASTER,  // 蓝牙主模式
    STR_SET_BT_SOURCE,  // 音频发送
    STR_STORE_DETAIL,  // 应用详情
    STR_STORE_VERSION,  // 版本
    STR_STORE_AUTHOR,  // 开发者
    STR_STORE_SOURCE,  // 来源
    STR_STORE_PERM,  // 权限
    STR_STORE_CHANGELOG,  // 更新说明
    STR_STORE_UPGRADE,  // 升级
    STR_STORE_VERIFYFAIL,  // 校验失败
    STR_STORE_SD,  // 内存卡
    STR_STORE_INTERNAL,  // 内置存储
    STR_STORE_ONLINE,  // 在线仓库
    STR_COMMON_LOADING,  // 加载中...
    STR_COMMON_SUCCESS,  // 成功
    STR_COMMON_FAILED,  // 失败
    STR_COMMON_RETRY,  // 重试
    STR_COMMON_BACK,  // 返回
    STR_COMMON_SAVE,  // 保存
    STR_COMMON_CANCEL,  // 取消
    STR_COMMON_CONFIRM,  // 确认
    STR_COMMON_NOWIFI,  // 无网络连接
    STR_COMMON_NOPERM,  // 无权限
    STR_COMMON_OFFLINE,  // 离线
    // 总数
    STR_COUNT
} str_id_t;

/**
 * 设置当前语言
 */
void lang_set(lang_id_t lang);

/**
 * 获取当前语言ID
 */
lang_id_t lang_get_current(void);

/**
 * 获取字符串翻译
 * @param id 字符串ID
 * @return 翻译后的字符串（UTF-8）
 */
const char* lang_get(str_id_t id);

#endif /* LANG_H */