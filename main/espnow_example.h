/* ESPNOW 示例

   本示例代码属于公共领域（或根据您的选择采用 CC0 许可）

   除非适用法律要求或书面同意，本软件按"原样"分发，
   不提供任何明示或暗示的保证。
*/

#ifndef ESPNOW_EXAMPLE_H
#define ESPNOW_EXAMPLE_H

/* ==================== 设备角色配置 ==================== */
/* 只需要修改下面这个宏即可切换主机/从机模式 */
#define DEVICE_ROLE_MASTER  1
#define DEVICE_ROLE_SLAVE   0

#ifndef DEVICE_ROLE
#define DEVICE_ROLE DEVICE_ROLE_SLAVE    /* 默认设置为从机 */
#endif

/* 根据角色定义设备名称 */
#if DEVICE_ROLE == DEVICE_ROLE_MASTER
    #define DEVICE_NAME "MASTER"
    #define IS_MASTER 1
    #define IS_SLAVE  0
#else
    #define DEVICE_NAME "SLAVE"
    #define IS_MASTER 0
    #define IS_SLAVE  1
#endif
/* ==================== 配置结束 ==================== */

/* ESPNOW 可在 station 或 softap 模式下工作，通过 menuconfig 配置 */
#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_AP
#endif

#define ESPNOW_QUEUE_SIZE           6

#define IS_BROADCAST_ADDR(addr) (memcmp(addr, s_example_broadcast_mac, ESP_NOW_ETH_ALEN) == 0)

typedef enum {
    EXAMPLE_ESPNOW_SEND_CB,
    EXAMPLE_ESPNOW_RECV_CB,
} example_espnow_event_id_t;

/* 发送回调事件数据 */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];   //目标 MAC 地址
    esp_now_send_status_t status;         //发送状态
} example_espnow_event_send_cb_t;

/* 接收回调事件数据 */
typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];   //源 MAC 地址
    uint8_t *data;                        //数据指针
    int data_len;                         //数据长度
} example_espnow_event_recv_cb_t;

/* 事件信息联合体 */
typedef union {
    example_espnow_event_send_cb_t send_cb;   //发送回调信息
    example_espnow_event_recv_cb_t recv_cb;   //接收回调信息
} example_espnow_event_info_t;

/* 当 ESPNOW 发送或接收回调被调用时，将事件投递到 ESPNOW 任务 */
typedef struct {
    example_espnow_event_id_t id;
    example_espnow_event_info_t info;
} example_espnow_event_t;

enum {
    EXAMPLE_ESPNOW_DATA_BROADCAST,
    EXAMPLE_ESPNOW_DATA_UNICAST,
    EXAMPLE_ESPNOW_DATA_MAX,
};

/* 本示例中 ESPNOW 数据的用户定义字段 */
typedef struct {
    uint8_t type;                         //广播或单播 ESPNOW 数据类型
    uint8_t state;                        //状态标志（0=未配对，1=已配对）
    uint16_t seq_num;                     //ESPNOW 数据序列号
    uint16_t crc;                         //ESPNOW 数据的 CRC16 校验值
    uint8_t payload[0];                   //ESPNOW 数据的实际载荷
} __attribute__((packed)) example_espnow_data_t;

/* ESPNOW 发送参数 */
typedef struct {
    bool unicast;                         //发送单播 ESPNOW 数据
    bool broadcast;                       //发送广播 ESPNOW 数据
    uint8_t state;                        //状态标志（0=未配对，1=已配对）
    uint16_t count;                       //待发送的单播 ESPNOW 数据总数
    uint16_t delay;                       //发送两个 ESPNOW 数据之间的延迟，单位：毫秒
    int len;                              //待发送 ESPNOW 数据的长度，单位：字节
    uint8_t *buffer;                      //指向 ESPNOW 数据的缓冲区
    uint8_t dest_mac[ESP_NOW_ETH_ALEN];   //目标设备的 MAC 地址
} example_espnow_send_param_t;

/* 温湿度数据结构 */
typedef struct {
    int16_t temperature;                  //温度值（扩大100倍，例如 2550 表示 25.50°C）
    int16_t humidity;                     //湿度值（扩大100倍，例如 6000 表示 60.00%）
    uint32_t timestamp;                   //时间戳
} __attribute__((packed)) sensor_data_t;

#endif
