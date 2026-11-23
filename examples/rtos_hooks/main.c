/*
 * rtos_hooks: the idle, tick and malloc failed hooks
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Counts calls to each hook: one tick hook call per tick, the idle hook
 * only while every task is blocked, and the malloc failed hook once for
 * each allocation that fails. Built with all three on (rtos_config.h).
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"

static volatile uint32_t idle_calls, tick_calls, malloc_fails;

void vApplicationIdleHook(void)
{
    idle_calls++;
}

void vApplicationTickHook(void)
{
    tick_calls++;
}

void vApplicationMallocFailedHook(void)
{
    malloc_fails++;
}

static void spinner(void *arg)
{
    (void)arg;

    for (;;) {
    }
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_hooks: start\r\n");

    uint32_t t0 = tick_calls;
    TickType_t k0 = xTaskGetTickCount();
    vTaskDelay(500);
    uint32_t ticks = tick_calls - t0;
    TickType_t kernel = xTaskGetTickCount() - k0;
    check("the tick hook ran once for every tick", ticks == kernel && ticks >= 500 && ticks <= 501);

    check("the idle hook ran while every task was blocked", idle_calls > 100);

    TaskHandle_t spin_h;
    xTaskCreate(spinner, "spinner", 256, NULL, 1, &spin_h);
    uint32_t i0 = idle_calls;
    vTaskDelay(100);
    uint32_t while_busy = idle_calls - i0;
    vTaskDelete(spin_h);
    vTaskDelay(5);
    i0 = idle_calls;
    vTaskDelay(20);
    uint32_t after = idle_calls - i0;
    check("the idle hook stops while a task above idle never blocks", while_busy == 0);
    check("and runs again once that task is gone", after > 0);

    size_t heap = xPortGetFreeHeapSize();
    void *p = pvPortMalloc(heap + 1024);
    check("an allocation bigger than the heap fails and calls the hook once",
          p == NULL && malloc_fails == 1);

    TaskHandle_t h = NULL;
    BaseType_t r = xTaskCreate(spinner, "huge", 60000, NULL, 1, &h);
    check("a task too big for the heap is refused and calls the hook again",
          r == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY && h == NULL && malloc_fails == 2);
    check("the heap is unchanged by the failures", xPortGetFreeHeapSize() == heap);

    mini_printf("     tick hook %u in 500 ticks, idle hook %u calls, malloc failed hook %u\r\n",
                (uint32_t)ticks, (uint32_t)idle_calls, (uint32_t)malloc_fails);

    result();
    vTaskSuspend(NULL);
}

int main(void)
{
    bao_init();
    xTaskCreate(runner, "runner", 512, NULL, 2, NULL);
    vTaskStartScheduler();

    for (;;) {
    }
}
