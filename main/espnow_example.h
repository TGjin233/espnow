/* ESPNOW 示例 - 精简版

   演示 ESPNOW 基本的发送与接收功能。
   准备两台设备：一台发送，一台接收。
*/

#ifndef ESPNOW_EXAMPLE_H
#define ESPNOW_EXAMPLE_H

/* WiFi 模式：站点或软AP，通过 menuconfig 配置 */
#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_AP
#endif

#define ESPNOW_QUEUE_SIZE           6

/* 判断 MAC 地址是否为广播地址 (FF:FF:FF:FF:FF:FF) */
#define IS_BROADCAST_ADDR(addr) (memcmp(addr, s_broadcast_mac, ESP_NOW_ETH_ALEN) == 0)

/* 从回调函数投递到应用任务的事件 ID */
typedef enum {
    EXAMPLE_ESPNOW_SEND_CB,
    EXAMPLE_ESPNOW_RECV_CB,
} example_espnow_event_id_t;

/* 发送回调携带的数据 */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
} example_espnow_event_send_cb_t;

/* 接收回调携带的数据 */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t *data;
    int data_len;
} example_espnow_event_recv_cb_t;

/* 联合体：存放发送或接收回调信息 */
typedef union {
    example_espnow_event_send_cb_t send_cb;
    example_espnow_event_recv_cb_t recv_cb;
} example_espnow_event_info_t;

/* 投递到队列的事件结构体 */
typedef struct {
    example_espnow_event_id_t id;
    example_espnow_event_info_t info;
} example_espnow_event_t;

/* ESPNOW 数据包头 - 每个数据包前都会附加此头部 */
typedef struct {
    uint8_t type;       // 0=广播, 1=单播
    uint8_t state;      // 0=初始, 1=已收到对端广播
    uint16_t seq_num;   // 序列号
    uint16_t crc;       // CRC16 完整性校验
    uint32_t magic;     // 魔数，用于决定哪端发送单播
    uint8_t payload[0]; // 柔性数组，实际载荷数据
} __attribute__((packed)) example_espnow_data_t;

/* 发送行为控制参数 */
typedef struct {
    bool unicast;                       // 是否正在发送单播
    bool broadcast;                     // 是否正在发送广播
    uint8_t state;                      // 对端发现状态
    uint32_t magic;                     // 随机魔数，用于角色协商
    uint16_t count;                     // 剩余单播发送次数
    uint16_t delay;                     // 发送间隔 (ms)
    int len;                            // 数据包总长度 (头部+载荷)
    uint8_t *buffer;                    // 数据包缓冲区指针
    uint8_t dest_mac[ESP_NOW_ETH_ALEN]; // 目标 MAC 地址
} example_espnow_send_param_t;

#endif
