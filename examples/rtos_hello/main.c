/*
 * rtos_hello: first FreeRTOS program for the Baochip-1x
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two tasks run at different rates, one of them blinking the LED, and a
 * third checks that 1000 kernel ticks take 1000 ms on millis().
 *
 * Wiring:
 *   PB1 (header pin 29) through 1 k to LED anode, LED cathode to GND
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"

static volatile uint32_t fast_runs, slow_runs;

static void fast(void *arg)
{
    (void)arg;

    for (;;) {
        fast_runs++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void slow(void *arg)
{
    (void)arg;

    for (;;) {
        slow_runs++;
        gpio_toggle(GPIO_PORT_B, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_hello: start\r\n");

    uint32_t m0 = (uint32_t)millis();
    TickType_t t0 = xTaskGetTickCount();
    uint32_t f0 = fast_runs, s0 = slow_runs;
    vTaskDelay(1000);
    uint32_t ms = (uint32_t)millis() - m0;
    TickType_t ticks = xTaskGetTickCount() - t0;
    uint32_t f = fast_runs - f0, sl = slow_runs - s0;

    mini_printf("     1000 ticks took %u ms, fast ran %u, slow ran %u\r\n", ms, f, sl);
    check("1000 ticks of vTaskDelay last 1000 ms", ticks == 1000 && ms >= 999 && ms <= 1001);
    check("the 10 ms task ran 100 times", f >= 99 && f <= 101);
    check("the 100 ms task ran 10 times", sl >= 9 && sl <= 11);

    result();
    vTaskSuspend(NULL);
}

int main(void)
{
    bao_init();
    gpio_init(GPIO_PORT_B, 1);
    gpio_set_dir(GPIO_PORT_B, 1, true);

    xTaskCreate(fast, "fast", 256, NULL, 3, NULL);
    xTaskCreate(slow, "slow", 256, NULL, 2, NULL);
    xTaskCreate(runner, "runner", 512, NULL, 1, NULL);
    vTaskStartScheduler();

    for (;;) {
    }
}
