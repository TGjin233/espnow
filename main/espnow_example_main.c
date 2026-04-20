/* ESPNOW Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
   This example shows how to use ESPNOW.
   Prepare two device, one for sending ESPNOW data and another for receiving
   ESPNOW data.
*/
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "espnow_example.h"
#include "led_blink.h"

#define ESPNOW_MAXDELAY 512

static const char *TAG = "espnow_example";

static QueueHandle_t s_example_espnow_queue = NULL;

static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t s_example_espnow_seq[EXAMPLE_ESPNOW_DATA_MAX] = { 0, 0 };

static void example_espnow_deinit(example_espnow_send_param_t *send_param);

/* WiFi 应在 ESPNOW 使用前启动 */
static void example_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

/* ESPNOW 发送或接收回调函数在 WiFi 任务中调用。
 * 用户不应在此任务中执行耗时操作，应将必要数据投递到队列，
 * 由低优先级任务处理。 */
static void example_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    example_espnow_event_t evt;
    example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

    if (mac_addr == NULL) {
        ESP_LOGE(TAG, "发送回调参数错误");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "发送队列失败");
    }
}

static void example_espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    example_espnow_event_t evt;
    example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
    uint8_t * mac_addr = recv_info->src_addr;
    uint8_t * des_addr = recv_info->des_addr;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "接收回调参数错误");
        return;
    }

    if (IS_BROADCAST_ADDR(des_addr)) {
        ESP_LOGI(TAG, ">>> [接收回调] 收到广播数据, 长度=%d, 来自=" MACSTR "",
                 len, MAC2STR(mac_addr));
    } else {
        ESP_LOGI(TAG, ">>> [接收回调] 收到单播数据, 长度=%d, 来自=" MACSTR "",
                 len, MAC2STR(mac_addr));
    }

    evt.id = EXAMPLE_ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = malloc(len);
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG, "接收数据内存分配失败");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    
    example_espnow_data_t *buf = (example_espnow_data_t *)data;
    uint16_t payload_len = len - sizeof(example_espnow_data_t);
    
#if IS_SLAVE
    if (payload_len >= sizeof(sensor_data_t)) {
        sensor_data_t *sensor = (sensor_data_t *)buf->payload;
        ESP_LOGI(TAG, "========== 收到温湿度数据 ==========");
        ESP_LOGI(TAG, "  温度: %.2f°C", sensor->temperature / 100.0);
        ESP_LOGI(TAG, "  湿度: %.2f%%", sensor->humidity / 100.0);
        ESP_LOGI(TAG, "  发送时间戳: %" PRIu32 " ms", sensor->timestamp);
        ESP_LOGI(TAG, "====================================");
    } else if (payload_len > 0) {
        ESP_LOGI(TAG, "收到数据，长度: %d 字节（无温湿度数据）", payload_len);
    } else {
        ESP_LOGI(TAG, "收到数据（仅包含头部）");
    }
#elif IS_MASTER
    if (payload_len >= sizeof(sensor_data_t)) {
        ESP_LOGD(TAG, "收到来自其他主机的数据");
    }
#endif
    
    if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "接收队列失败");
        free(recv_cb->data);
    }
}

/* 解析接收到的 ESPNOW 数据 */
int example_espnow_data_parse(uint8_t *data, uint16_t data_len, uint8_t *state, uint16_t *seq)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)data;
    uint16_t crc, crc_cal = 0;

    if (data_len < sizeof(example_espnow_data_t)) {
        ESP_LOGE(TAG, "接收 ESPNOW 数据过短，长度：%d", data_len);
        return -1;
    }

    *state = buf->state;
    *seq = buf->seq_num;
    crc = buf->crc;
    buf->crc = 0;
    crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

    if (crc_cal == crc) {
        return buf->type;
    }

    return -1;
}

/* 准备要发送的 ESPNOW 数据 */
void example_espnow_data_prepare(example_espnow_send_param_t *send_param)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)send_param->buffer;

    assert(send_param->len >= sizeof(example_espnow_data_t));

    buf->type = IS_BROADCAST_ADDR(send_param->dest_mac) ? EXAMPLE_ESPNOW_DATA_BROADCAST : EXAMPLE_ESPNOW_DATA_UNICAST;
    buf->state = send_param->state;
    buf->seq_num = s_example_espnow_seq[buf->type]++;
    buf->crc = 0;
    
#if IS_MASTER
    sensor_data_t *sensor = (sensor_data_t *)buf->payload;
    sensor->temperature = (int16_t)(esp_random() % 4000 - 1000);
    sensor->humidity = (int16_t)(esp_random() % 10000);
    sensor->timestamp = esp_log_timestamp();
    ESP_LOGI(TAG, "准备发送 - 温度: %.2f°C, 湿度: %.2f%%", 
             sensor->temperature / 100.0, sensor->humidity / 100.0);
#endif
    
    buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

static void example_espnow_task(void *pvParameter)
{
    example_espnow_event_t evt;
    uint8_t recv_state = 0;
    uint16_t recv_seq = 0;
    bool is_broadcast = false;
    int ret;
    uint32_t unicast_count = 0;

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    
    example_espnow_send_param_t *send_param = (example_espnow_send_param_t *)pvParameter;
    
#if IS_MASTER
    ESP_LOGI(TAG, "开始发送广播数据，等待从机响应...");
    
    /* 主机开始发送广播 ESPNOW 数据 */
    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
        ESP_LOGE(TAG, "Send error");
        example_espnow_deinit(send_param);
        vTaskDelete(NULL);
    }
#elif IS_SLAVE
    ESP_LOGI(TAG, "从机模式，等待主机广播...");
    ESP_LOGI(TAG, "将从机放到主机的广播范围内即可自动连接");
#endif

    while (xQueueReceive(s_example_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {
            case EXAMPLE_ESPNOW_SEND_CB:
            {
#if IS_MASTER
                example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);

                if (send_cb->status == ESP_NOW_SEND_SUCCESS) {
                    ESP_LOGI(TAG, ">>> [发送回调] 发送成功到 " MACSTR "", MAC2STR(send_cb->mac_addr));
                } else {
                    ESP_LOGI(TAG, ">>> [发送回调] 发送失败到 " MACSTR "，状态：%d", MAC2STR(send_cb->mac_addr), send_cb->status);
                }

                if (is_broadcast && (send_param->broadcast == false)) {
                    ESP_LOGI(TAG, "已切换到单播模式，忽略广播回调");
                    break;
                }

                if (!is_broadcast) {
                    send_param->count--;
                    if (send_param->count == 0) {
                        ESP_LOGI(TAG, "发送完成");
                        example_espnow_deinit(send_param);
                        vTaskDelete(NULL);
                    }
                }

                /* 在发送下一个数据之前延迟一段时间 */
                if (send_param->delay > 0) {
                    vTaskDelay(send_param->delay/portTICK_PERIOD_MS);
                }

                ESP_LOGI(TAG, "发送数据到 "MACSTR"", MAC2STR(send_cb->mac_addr));

                /* 单播计数器递增 */
                if (!is_broadcast) {
                    unicast_count++;
                    ESP_LOGI(TAG, "★ [单播 #%" PRIu32 "] 发送到 " MACSTR "", 
                             unicast_count, MAC2STR(send_cb->mac_addr));
                }

                memcpy(send_param->dest_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
                example_espnow_data_prepare(send_param);

                /* 上一个数据发送完成后发送下一个数据 */
                if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
                    ESP_LOGE(TAG, "发送错误");
                    example_espnow_deinit(send_param);
                    vTaskDelete(NULL);
                }
#elif IS_SLAVE
                /* 从机不发送数据，只记录发送状态 */
                ESP_LOGD(TAG, "[从机] 忽略发送回调");
#endif
                break;
            }
            case EXAMPLE_ESPNOW_RECV_CB:
            {
                example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

                ret = example_espnow_data_parse(recv_cb->data, recv_cb->data_len, &recv_state, &recv_seq);
                free(recv_cb->data);
                
#if IS_MASTER
                /* 主机收到任何从机的消息（广播或单播），都尝试获取从机MAC */
                if (ret == EXAMPLE_ESPNOW_DATA_BROADCAST || ret == EXAMPLE_ESPNOW_DATA_UNICAST) {
                    
                    ESP_LOGI(TAG, "★★★ 收到数据包! 类型=%d, state=%d, 来自=" MACSTR "",
                             ret, recv_state, MAC2STR(recv_cb->mac_addr));

                    /* 检查是否是从机的响应（state=1表示从机已就绪） */
                    if (recv_state == 1) {
                        ESP_LOGI(TAG, "收到从机的响应，来自："MACSTR"", MAC2STR(recv_cb->mac_addr));
                    } else {
                        ESP_LOGI(TAG, "收到从机的广播，来自："MACSTR"", MAC2STR(recv_cb->mac_addr));
                    }

                    /* 添加从机到对等列表（使用与广播相同的配置，不加密） */
                    if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {
                        esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
                        if (peer == NULL) {
                            ESP_LOGE(TAG, "对端信息内存分配失败");
                            example_espnow_deinit(send_param);
                            vTaskDelete(NULL);
                        }
                        memset(peer, 0, sizeof(esp_now_peer_info_t));
                        peer->channel = CONFIG_ESPNOW_CHANNEL;
                        peer->ifidx = ESPNOW_WIFI_IF;
                        peer->encrypt = false;
                        memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                        ESP_ERROR_CHECK( esp_now_add_peer(peer) );
                        free(peer);
                        ESP_LOGI(TAG, "已将 MAC " MACSTR " 添加到对等列表(不加密)", MAC2STR(recv_cb->mac_addr));
                    }

                    /* 主机收到从机响应后，切换到单播模式 */
                    if (send_param->unicast == false) {
                        ESP_LOGI(TAG, "从机已就绪，开始向从机发送数据");
                        memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                        send_param->broadcast = false;
                        send_param->unicast = true;
                        
                        /* 立即发送第一个单播数据 */
                        example_espnow_data_prepare(send_param);
                        if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
                            ESP_LOGE(TAG, "发送错误");
                            example_espnow_deinit(send_param);
                            vTaskDelete(NULL);
                        }
                    }
                } else if (ret == -1) {
                    ESP_LOGW(TAG, "数据校验失败，忽略此包");
                }
                
                /* 主机收到从机的ACK（state=2） */
                if (recv_state == 2) {
                    ESP_LOGI(TAG, "收到从机的ACK");
                }
#elif IS_SLAVE
                if (ret == EXAMPLE_ESPNOW_DATA_BROADCAST) {
                    ESP_LOGI(TAG, "收到主机的广播，来自："MACSTR"", MAC2STR(recv_cb->mac_addr));

                    /* 添加主机到对等列表 */
                    if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {
                        esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
                        if (peer == NULL) {
                            ESP_LOGE(TAG, "对端信息内存分配失败");
                            example_espnow_deinit(send_param);
                            vTaskDelete(NULL);
                        }
                        memset(peer, 0, sizeof(esp_now_peer_info_t));
                        peer->channel = CONFIG_ESPNOW_CHANNEL;
                        peer->ifidx = ESPNOW_WIFI_IF;
                        peer->encrypt = false;
                        memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                        ESP_ERROR_CHECK( esp_now_add_peer(peer) );
                        free(peer);
                        
                        ESP_LOGI(TAG, "已添加主机到对等列表(不加密)，准备回复主机...");
                        ESP_LOGI(TAG, ">>> 发送回复到=" MACSTR "", MAC2STR(recv_cb->mac_addr));
                        
                        /* 回复主机，告知从机已就绪 */
                        send_param->state = 1;
                        example_espnow_data_prepare(send_param);
                        if (esp_now_send(recv_cb->mac_addr, send_param->buffer, send_param->len) != ESP_OK) {
                            ESP_LOGE(TAG, "回复主机失败");
                        } else {
                            ESP_LOGI(TAG, "已回复主机，从机就绪");
                        }
                    }
                } else if (ret == EXAMPLE_ESPNOW_DATA_UNICAST) {
                    ESP_LOGI(TAG, "收到主机的单播数据，来自："MACSTR"", MAC2STR(recv_cb->mac_addr));
                    
                    /* 回复ACK */
                    send_param->state = 2;  // 2 表示ACK
                    example_espnow_data_prepare(send_param);
                    if (esp_now_send(recv_cb->mac_addr, send_param->buffer, send_param->len) != ESP_OK) {
                        ESP_LOGW(TAG, "ACK回复失败");
                    }
                }
#endif
                break;
            }
            default:
                ESP_LOGE(TAG, "回调类型错误：%d", evt.id);
                break;
        }
    }
}

static esp_err_t example_espnow_init(void)
{
    example_espnow_send_param_t *send_param;

    s_example_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(example_espnow_event_t));
    if (s_example_espnow_queue == NULL) {
        ESP_LOGE(TAG, "创建队列失败");
        return ESP_FAIL;
    }

    /* 初始化 ESPNOW 并注册发送和接收回调函数 */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(example_espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(example_espnow_recv_cb) );
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
    ESP_ERROR_CHECK( esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW) );
    ESP_ERROR_CHECK( esp_wifi_connectionless_module_set_wake_interval(CONFIG_ESPNOW_WAKE_INTERVAL) );
#endif
    /* 设置主密钥 */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    /* 添加广播对端信息到对等列表 */
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
        ESP_LOGE(TAG, "对端信息内存分配失败");
        vQueueDelete(s_example_espnow_queue);
        s_example_espnow_queue = NULL;
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );
    free(peer);

    /* 初始化发送参数 */
    send_param = malloc(sizeof(example_espnow_send_param_t));
    if (send_param == NULL) {
        ESP_LOGE(TAG, "发送参数内存分配失败");
        vQueueDelete(s_example_espnow_queue);
        s_example_espnow_queue = NULL;
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(send_param, 0, sizeof(example_espnow_send_param_t));
    send_param->unicast = false;
    send_param->broadcast = true;
    send_param->state = 0;
    send_param->count = CONFIG_ESPNOW_SEND_COUNT;
    send_param->delay = CONFIG_ESPNOW_SEND_DELAY;
    send_param->len = CONFIG_ESPNOW_SEND_LEN;
    send_param->buffer = malloc(CONFIG_ESPNOW_SEND_LEN);
    if (send_param->buffer == NULL) {
        ESP_LOGE(TAG, "发送缓冲区内存分配失败");
        free(send_param);
        vQueueDelete(s_example_espnow_queue);
        s_example_espnow_queue = NULL;
        esp_now_deinit();
        return ESP_FAIL;
    }
    memcpy(send_param->dest_mac, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
    example_espnow_data_prepare(send_param);

    xTaskCreate(example_espnow_task, "example_espnow_task", 2048, send_param, 4, NULL);

    return ESP_OK;
}

static void example_espnow_deinit(example_espnow_send_param_t *send_param)
{
    free(send_param->buffer);
    free(send_param);
    vQueueDelete(s_example_espnow_queue);
    s_example_espnow_queue = NULL;
    esp_now_deinit();
}

void app_main(void)
{
    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    example_wifi_init();
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  设备角色: %s", DEVICE_NAME);
    ESP_LOGI(TAG, "========================================");
    
#if IS_MASTER
    ESP_LOGI(TAG, "  [主机模式] 初始化中...");
    example_espnow_init();
#elif IS_SLAVE
    ESP_LOGI(TAG, "  [从机模式] 初始化中...");
    example_espnow_init();
#endif

    xTaskCreate(led_blink_task, "led_blink_task", 2048, NULL, 5, NULL);
}
