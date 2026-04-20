#include "led_blink.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led_blink";

void led_blink_init(void)
{
    gpio_reset_pin(LED_BLINK_GPIO);
    gpio_set_direction(LED_BLINK_GPIO, GPIO_MODE_OUTPUT);
    ESP_LOGI(TAG, "LED 初始化完成，GPIO%d", LED_BLINK_GPIO);
}

void led_blink_task(void *pvParameter)
{
    led_blink_init();
    
    bool led_state = false;
    
    while (1) {
        led_state = !led_state;
        gpio_set_level(LED_BLINK_GPIO, led_state ? 1 : 0);
        ESP_LOGI(TAG, "LED 状态: %s", led_state ? "ON" : "OFF");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
