#ifndef LED_BLINK_H
#define LED_BLINK_H

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_BLINK_GPIO GPIO_NUM_2

void led_blink_init(void);
void led_blink_task(void *pvParameter);

#endif
