/*
 * rtos_delay: delay_ms and delay_us inside FreeRTOS tasks
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * One task waits with delay_ms while a lower priority task counts, which
 * shows the wait gives the processor away. Then delay_us, and delay_ms
 * inside a critical section, and the tick is checked again afterwards.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"

static volatile uint32_t low_count;

static void low(void *arg)
{
    (void)arg;

    for (;;) {
        low_count++;
    }
}

static void high(void *arg)
{
    (void)arg;
    bool good = true;

    for (int i = 0; i < 5; i++) {
        TickType_t t0 = xTaskGetTickCount();
        uint32_t c0 = low_count;
        delay_ms(100);
        TickType_t dt = xTaskGetTickCount() - t0;
        uint32_t dc = low_count - c0;
        mini_printf("delay_ms(100): %u ticks, low task ran %u loops\r\n", (uint32_t)dt,
                    (uint32_t)dc);
        if (dt < 100 || dt > 103 || dc == 0) {
            good = false;
        }
    }
    TickType_t t0 = xTaskGetTickCount();
    for (int i = 0; i < 1000; i++) {
        delay_us(100);
    }
    TickType_t dt = xTaskGetTickCount() - t0;
    mini_printf("1000 x delay_us(100): %u ticks\r\n", (uint32_t)dt);
    if (dt < 99 || dt > 110) {
        good = false;
    }

    taskENTER_CRITICAL();
    delay_ms(20);
    taskEXIT_CRITICAL();
    t0 = xTaskGetTickCount();
    vTaskDelay(50);
    dt = xTaskGetTickCount() - t0;
    mini_printf("after delay_ms in a critical section, vTaskDelay(50): %u ticks\r\n", (uint32_t)dt);
    if (dt != 50) {
        good = false;
    }

    check("delays in a task and in a critical section left the tick right", good);

    result();
    for (;;) {
        vTaskDelay(1000);
    }
}

int main(void)
{
    bao_init();
    mini_printf("delay: start\r\n");

    xTaskCreate(low, "low", 256, NULL, 1, NULL);
    xTaskCreate(high, "high", 384, NULL, 2, NULL);
    vTaskStartScheduler();

    for (;;) {
    }
}
