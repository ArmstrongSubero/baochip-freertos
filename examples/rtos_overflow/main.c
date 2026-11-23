/*
 * rtos_overflow: the stack overflow report
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * A task recurses one level deeper each time round until it runs off its
 * 192 word stack. The kernel finds the damage at a context switch and the
 * port prints STACK OVERFLOW in task greedy.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"

static uint32_t deep(uint32_t n)
{
    volatile uint8_t pad[64];

    pad[0] = (uint8_t)(n);
    if (n == 0) {
        return pad[0];
    }
    uint32_t r = deep(n - 1) + pad[0];
    taskYIELD();
    return r;
}

static void greedy(void *arg)
{
    (void)arg;

    for (uint32_t n = 1;; n++) {
        deep(n);
        vTaskDelay(1);
    }
}

int main(void)
{
    bao_init();
    mini_printf("overflow: start\r\n");

    xTaskCreate(greedy, "greedy", 192, NULL, 1, NULL);
    vTaskStartScheduler();

    for (;;) {
    }
}
