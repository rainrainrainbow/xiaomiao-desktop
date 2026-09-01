/**
 * @file wifi_debug.c
 * @brief WiFi 调试服务器 - 通过 TCP 端口输出 ESP_LOG
 */

#include "wifi_debug.h"
#include "sys_nvs.h"
#include "esp_log.h"
#include "esp_vfs_dev.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <string.h>
#include <sys/errno.h>

static const char *TAG = "WIFI_DBG";

#define WIFI_DEBUG_PORT 3333
#define WIFI_DEBUG_TASK_STACK_SIZE 4096
#define WIFI_DEBUG_TASK_PRIORITY 5

/* WiFi 连接事件组 */
static EventGroupHandle_t s_wifi_event_group = NULL;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static bool s_wifi_connected = false;

static int s_server_fd = -1;
static int s_client_fd = -1;
static TaskHandle_t s_server_task_handle = NULL;
static bool s_running = false;
static vprintf_like_t s_original_vprintf = NULL;

/* 默认 WiFi 凭据 */
#define DEFAULT_WIFI_SSID     "tiger8"
#define DEFAULT_WIFI_PASSWORD "12345678"
#define MAX_WIFI_RETRY        5

/* WiFi 事件处理 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started, connecting...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_WIFI_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "WiFi connection failed, retrying... (%d/%d)", 
                     s_retry_num, MAX_WIFI_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "WiFi connection failed after %d retries", MAX_WIFI_RETRY);
        }
        s_wifi_connected = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        s_wifi_connected = true;
    }
}

void wifi_auto_connect(void)
{
    char ssid[33] = {0};
    char password[65] = {0};
    
    /* 尝试从 NVS 加载保存的 WiFi 凭据 */
    bool loaded = sys_nvs_load_wifi_credentials(ssid, password);
    
    if (!loaded || strlen(ssid) == 0) {
        /* 使用默认凭据 */
        strncpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(password, DEFAULT_WIFI_PASSWORD, sizeof(password) - 1);
        ESP_LOGI(TAG, "Using default WiFi: %s", ssid);
        
        /* 保存默认凭据到 NVS */
        sys_nvs_save_wifi_credentials(ssid, password);
    } else {
        ESP_LOGI(TAG, "Using saved WiFi: %s", ssid);
    }
    
    /* 创建事件组 */
    s_wifi_event_group = xEventGroupCreate();
    
    /* 创建默认 STA 网络接口 */
    esp_netif_create_default_wifi_sta();
    
    /* WiFi 初始化配置 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "WiFi ready (init ret=%d)", (int)ret);
    
    /* 注册事件处理 */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                        &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                        &wifi_event_handler, NULL, &instance_got_ip);
    
    /* 配置 WiFi */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    
    /* 设置 WiFi 模式为 STA */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    
    ESP_LOGI(TAG, "WiFi auto-connect started, waiting for connection...");
    
    /* 等待连接结果 */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(10000));  /* 10秒超时 */
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "WiFi connection failed");
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout");
    }
}

bool wifi_debug_is_connected(void)
{
    return s_client_fd >= 0 && s_wifi_connected;
}

/**
 * 自定义 vprintf 处理器
 * 将输出同时发送到原始输出和 TCP 客户端
 */
static int wifi_debug_vprintf(const char *fmt, va_list args)
{
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    
    // 发送到原始输出（串口）
    if (s_original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        s_original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }
    
    // 发送到 TCP 客户端
    if (s_client_fd >= 0 && len > 0) {
        // 确保发送完整数据
        int sent = 0;
        while (sent < len) {
            int n = send(s_client_fd, buf + sent, len - sent, 0);
            if (n < 0) {
                // 发送失败，可能是客户端断开
                if (errno == EPIPE || errno == ECONNRESET) {
                    close(s_client_fd);
                    s_client_fd = -1;
                    ESP_LOGI(TAG, "Debug client disconnected");
                }
                break;
            }
            sent += n;
        }
    }
    
    return len;
}

/**
 * TCP 服务器任务
 * 监听端口 3333，接受客户端连接
 */
static void wifi_debug_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    // 创建 TCP socket
    s_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_server_fd < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    // 设置 socket 选项
    int opt = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // 绑定端口
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(WIFI_DEBUG_PORT);
    
    int err = bind(s_server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(s_server_fd);
        s_server_fd = -1;
        vTaskDelete(NULL);
        return;
    }
    
    // 监听
    err = listen(s_server_fd, 1);
    if (err < 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(s_server_fd);
        s_server_fd = -1;
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "WiFi debug server listening on port %d", WIFI_DEBUG_PORT);
    
    // 接受客户端连接
    while (s_running) {
        int client_fd = accept(s_server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            if (s_running) {
                ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            }
            continue;
        }
        
        // 关闭旧连接
        if (s_client_fd >= 0) {
            close(s_client_fd);
        }
        
        s_client_fd = client_fd;
        ESP_LOGI(TAG, "Debug client connected from %s:%d", 
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        // 发送欢迎消息
        const char *welcome = "\r\n=== XiaoMiao WiFi Debug Console ===\r\n";
        send(s_client_fd, welcome, strlen(welcome), 0);
    }
    
    // 清理
    if (s_client_fd >= 0) {
        close(s_client_fd);
        s_client_fd = -1;
    }
    if (s_server_fd >= 0) {
        close(s_server_fd);
        s_server_fd = -1;
    }
    
    vTaskDelete(NULL);
}

void wifi_debug_init(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "WiFi debug server already running");
        return;
    }
    
    s_running = true;
    
    // 重定向 vprintf
    s_original_vprintf = esp_log_set_vprintf(wifi_debug_vprintf);
    
    // 创建服务器任务
    xTaskCreate(wifi_debug_server_task, "wifi_debug", 
                WIFI_DEBUG_TASK_STACK_SIZE, NULL, 
                WIFI_DEBUG_TASK_PRIORITY, &s_server_task_handle);
    
    ESP_LOGI(TAG, "WiFi debug server started on port %d", WIFI_DEBUG_PORT);
}

void wifi_debug_deinit(void)
{
    if (!s_running) {
        return;
    }
    
    s_running = false;
    
    // 恢复原始 vprintf
    if (s_original_vprintf) {
        esp_log_set_vprintf(s_original_vprintf);
        s_original_vprintf = NULL;
    }
    
    // 关闭 socket
    if (s_client_fd >= 0) {
        close(s_client_fd);
        s_client_fd = -1;
    }
    if (s_server_fd >= 0) {
        close(s_server_fd);
        s_server_fd = -1;
    }
    
    // 删除任务
    if (s_server_task_handle) {
        vTaskDelete(s_server_task_handle);
        s_server_task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "WiFi debug server stopped");
}
