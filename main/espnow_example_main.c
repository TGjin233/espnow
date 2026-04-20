/* ESPNOW 示例 - 精简版

   演示两台 ESP 设备之间 ESPNOW 的基本收发。
   一台广播，另一台响应后切换为单播通信。
*/

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "espnow_example.h"

#define ESPNOW_MAXDELAY 512

/* 数据包类型标识（两端必须一致） */
#define EXAMPLE_ESPNOW_DATA_BROADCAST 0
#define EXAMPLE_ESPNOW_DATA_UNICAST   1

static const char *TAG = "espnow_example";

/* 用于从回调向应用任务传递事件的队列 */
static QueueHandle_t s_espnow_queue = NULL;

/* 广播 MAC 地址：FF:FF:FF:FF:FF:FF */
static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* 各类型序列号计数器（广播、单播） */
static uint16_t s_espnow_seq[2] = { 0, 0 };

static void example_espnow_deinit(example_espnow_send_param_t *send_param);

/* ─── WiFi 初始化 ───────────────────────────────────────────── */

static void example_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK(esp_wifi_set_protocol(ESPNOW_WIFI_IF,
                     WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));
#endif
}

/* ─── ESPNOW 回调（在 WiFi 任务中调用，必须快速返回） ────────── */

static void example_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (mac_addr == NULL) {
        ESP_LOGE(TAG, "发送回调：MAC 为空");
        return;
    }

    example_espnow_event_t evt = {
        .id = EXAMPLE_ESPNOW_SEND_CB,
    };
    evt.info.send_cb.status = status;
    memcpy(evt.info.send_cb.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);

    if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "发送队列已满");
    }
}

static void example_espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    uint8_t *mac_addr = recv_info->src_addr;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "接收回调：参数无效");
        return;
    }

    ESP_LOGD(TAG, "收到%s ESPNOW 数据",
             IS_BROADCAST_ADDR(recv_info->des_addr) ? "广播" : "单播");

    example_espnow_event_t evt = {
        .id = EXAMPLE_ESPNOW_RECV_CB,
    };
    memcpy(evt.info.recv_cb.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    evt.info.recv_cb.data = malloc(len);
    if (evt.info.recv_cb.data == NULL) {
        ESP_LOGE(TAG, "接收数据内存分配失败");
        return;
    }
    memcpy(evt.info.recv_cb.data, data, len);
    evt.info.recv_cb.data_len = len;

    if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "接收队列已满");
        free(evt.info.recv_cb.data);
    }
}

/* ─── 数据包辅助函数 ────────────────────────────────────────── */

/* 解析收到的 ESPNOW 数据，校验 CRC，返回数据包类型；出错返回 -1 */
static int example_espnow_data_parse(uint8_t *data, uint16_t data_len,
                                      uint8_t *state, uint16_t *seq, uint32_t *magic)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)data;

    if (data_len < sizeof(example_espnow_data_t)) {
        ESP_LOGE(TAG, "数据包过短：%d 字节", data_len);
        return -1;
    }

    *state = buf->state;
    *seq   = buf->seq_num;
    *magic = buf->magic;

    /* 校验 CRC：先将 CRC 字段置零，计算后再比较 */
    uint16_t crc = buf->crc;
    buf->crc = 0;
    uint16_t crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

    return (crc_cal == crc) ? buf->type : -1;
}

/* 填充 ESPNOW 数据包缓冲区（头部+随机载荷），并计算 CRC */
static void example_espnow_data_prepare(example_espnow_send_param_t *send_param)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)send_param->buffer;
    assert(send_param->len >= sizeof(example_espnow_data_t));

    buf->type    = IS_BROADCAST_ADDR(send_param->dest_mac) ? EXAMPLE_ESPNOW_DATA_BROADCAST
                                                           : EXAMPLE_ESPNOW_DATA_UNICAST;
    buf->state   = send_param->state;
    buf->seq_num = s_espnow_seq[buf->type]++;
    buf->crc     = 0;
    buf->magic   = send_param->magic;

    /* 用随机数据填充剩余字节 */
    esp_fill_random(buf->payload, send_param->len - sizeof(example_espnow_data_t));

    /* 计算并存入 CRC */
    buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

/* ─── ESPNOW 主任务 ──────────────────────────────────────────── */

static void example_espnow_task(void *pvParameter)
{
    example_espnow_event_t evt;
    uint8_t recv_state = 0;
    uint16_t recv_seq = 0;
    uint32_t recv_magic = 0;
    bool is_broadcast = false;

    example_espnow_send_param_t *send_param = (example_espnow_send_param_t *)pvParameter;

    /* 等待 WiFi 稳定后发送首次广播 */
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "开始发送广播");

    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
        ESP_LOGE(TAG, "首次发送失败");
        example_espnow_deinit(send_param);
        vTaskDelete(NULL);
    }

    /* 事件循环：从队列中处理发送/接收回调 */
    while (xQueueReceive(s_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {

        /* ── 发送确认 ── */
        case EXAMPLE_ESPNOW_SEND_CB: {
            example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
            is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);

            ESP_LOGD(TAG, "Send to " MACSTR " status: %d", MAC2STR(send_cb->mac_addr), send_cb->status);

            /* 已切换到单播模式时，跳过广播重发 */
            if (is_broadcast && !send_param->broadcast) {
                break;
            }

            /* 单播包计数递减；发完后退出 */
            if (!is_broadcast) {
                send_param->count--;
                if (send_param->count == 0) {
                    ESP_LOGI(TAG, "所有单播包已发完");
                    example_espnow_deinit(send_param);
                    vTaskDelete(NULL);
                }
            }

            /* 发送前延时 */
            if (send_param->delay > 0) {
                vTaskDelay(send_param->delay / portTICK_PERIOD_MS);
            }

            ESP_LOGI(TAG, "Send to " MACSTR, MAC2STR(send_cb->mac_addr));

            /* 准备并发送下一个数据包 */
            memcpy(send_param->dest_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
            example_espnow_data_prepare(send_param);

            if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
                ESP_LOGE(TAG, "发送失败");
                example_espnow_deinit(send_param);
                vTaskDelete(NULL);
            }
            break;
        }

        /* ── 接收数据 ── */
        case EXAMPLE_ESPNOW_RECV_CB: {
            example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
            int type = example_espnow_data_parse(recv_cb->data, recv_cb->data_len,
                                                  &recv_state, &recv_seq, &recv_magic);
            free(recv_cb->data);

            if (type == EXAMPLE_ESPNOW_DATA_BROADCAST) {
                ESP_LOGI(TAG, "Broadcast #%d from " MACSTR " len:%d",
                         recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                /* 将未知对端加入列表，以便后续加密单播 */
                if (!esp_now_is_peer_exist(recv_cb->mac_addr)) {
                    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
                    if (peer == NULL) {
                        ESP_LOGE(TAG, "对端信息内存分配失败");
                        example_espnow_deinit(send_param);
                        vTaskDelete(NULL);
                    }
                    memset(peer, 0, sizeof(*peer));
                    peer->channel = CONFIG_ESPNOW_CHANNEL;
                    peer->ifidx   = ESPNOW_WIFI_IF;
                    peer->encrypt = true;
                    memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
                    memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                    ESP_ERROR_CHECK(esp_now_add_peer(peer));
                    free(peer);
                }

                /* 标记已收到对端广播 */
                if (send_param->state == 0) {
                    send_param->state = 1;
                }

                /* 双方都已收到广播 → 通过魔数协商发送方。
                 * 魔数较大的一方成为单播发送方。 */
                if (recv_state == 1 && !send_param->unicast && send_param->magic >= recv_magic) {
                    ESP_LOGI(TAG, "切换为单播 → " MACSTR, MAC2STR(recv_cb->mac_addr));
                    memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                    example_espnow_data_prepare(send_param);
                    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
                        ESP_LOGE(TAG, "单播发送失败");
                        example_espnow_deinit(send_param);
                        vTaskDelete(NULL);
                    }
                    send_param->broadcast = false;
                    send_param->unicast   = true;
                }

            } else if (type == EXAMPLE_ESPNOW_DATA_UNICAST) {
                ESP_LOGI(TAG, "Unicast #%d from " MACSTR " len:%d",
                         recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);
                send_param->broadcast = false;  // 收到单播后停止广播

            } else {
                ESP_LOGI(TAG, "Bad packet from " MACSTR, MAC2STR(recv_cb->mac_addr));
            }
            break;
        }

        default:
            ESP_LOGE(TAG, "未知事件：%d", evt.id);
            break;
        }
    }
}

/* ─── ESPNOW 初始化 ─────────────────────────────────────────── */

static esp_err_t example_espnow_init(void)
{
    /* 创建事件队列 */
    s_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(example_espnow_event_t));
    if (s_espnow_queue == NULL) {
        ESP_LOGE(TAG, "创建队列失败");
        return ESP_FAIL;
    }

    /* 初始化 ESPNOW 并注册回调 */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(example_espnow_send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(example_espnow_recv_cb));

#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
    ESP_ERROR_CHECK(esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW));
    ESP_ERROR_CHECK(esp_wifi_connectionless_module_set_wake_interval(CONFIG_ESPNOW_WAKE_INTERVAL));
#endif

    /* 设置主密钥（PMK），用于加密通信 */
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK));

    /* 注册广播对端（不加密），以便能向 FF:FF:FF:FF:FF:FF 发送 */
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
        ESP_LOGE(TAG, "对端内存分配失败");
        goto fail;
    }
    memset(peer, 0, sizeof(*peer));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx   = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(peer));
    free(peer);

    /* 分配并初始化发送参数 */
    example_espnow_send_param_t *send_param = malloc(sizeof(example_espnow_send_param_t));
    if (send_param == NULL) {
        ESP_LOGE(TAG, "发送参数内存分配失败");
        goto fail;
    }
    memset(send_param, 0, sizeof(*send_param));
    send_param->unicast   = false;
    send_param->broadcast = true;
    send_param->state     = 0;
    send_param->magic     = esp_random();
    send_param->count     = CONFIG_ESPNOW_SEND_COUNT;
    send_param->delay     = CONFIG_ESPNOW_SEND_DELAY;
    send_param->len       = CONFIG_ESPNOW_SEND_LEN;
    send_param->buffer    = malloc(CONFIG_ESPNOW_SEND_LEN);
    if (send_param->buffer == NULL) {
        ESP_LOGE(TAG, "发送缓冲区内存分配失败");
        free(send_param);
        goto fail;
    }
    memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    example_espnow_data_prepare(send_param);

    /* 启动应用任务 */
    xTaskCreate(example_espnow_task, "espnow_task", 2048, send_param, 4, NULL);
    return ESP_OK;

fail:
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
    esp_now_deinit();
    return ESP_FAIL;
}

/* ─── 资源释放 ─────────────────────────────────────────────── */

static void example_espnow_deinit(example_espnow_send_param_t *send_param)
{
    free(send_param->buffer);
    free(send_param);
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
    esp_now_deinit();
}

/* ─── 程序入口 ─────────────────────────────────────────────── */

void app_main(void)
{
    /* 初始化 NVS（WiFi 依赖） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    example_wifi_init();
    example_espnow_init();
}
