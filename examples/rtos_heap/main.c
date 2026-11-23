/*
 * rtos_heap: heap_4 running out and coming back
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fills the heap until malloc returns NULL, checks that a queue and a
 * task then fail cleanly, frees everything, breaks the heap into pieces
 * and joins it again, and creates tasks until memory runs out. Built
 * with a 20 KB heap (rtos_config.h).
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

static void idle_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(1000);
    }
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_heap: start\r\n");

    vTaskDelay(10);
    size_t start = xPortGetFreeHeapSize();
    mini_printf("     free heap at start %u of %u\r\n", (uint32_t)start,
                (uint32_t)configTOTAL_HEAP_SIZE);

    static void *blocks[200];
    int n = 0;
    while (n < 200 && (blocks[n] = pvPortMalloc(64)) != NULL) {
        n++;
    }
    size_t left = xPortGetFreeHeapSize();
    check("64 byte blocks fill the heap until malloc returns null",
          n > 10 && n < 200 && left < 64 + 16);
    check("with the heap full, creating a queue returns null", xQueueCreate(16, 16) == NULL);

    TaskHandle_t h = NULL;
    check("with the heap full, creating a task fails cleanly",
          xTaskCreate(idle_task, "nope", 256, NULL, 1, &h) ==
                  errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY &&
              h == NULL);

    for (int i = 0; i < n; i++) {
        vPortFree(blocks[i]);
    }
    check("freeing every block gives the whole heap back", xPortGetFreeHeapSize() == start);
    check("the lowest ever free figure remembers the full heap",
          xPortGetMinimumEverFreeHeapSize() <= left);

    for (int i = 0; i < 10; i++) {
        blocks[i] = pvPortMalloc(512);
    }
    for (int i = 0; i < 10; i += 2) {
        vPortFree(blocks[i]);
        blocks[i] = NULL;
    }
    HeapStats_t stats;
    vPortGetHeapStats(&stats);
    mini_printf("     after freeing every other block: largest free %u, %u free blocks\r\n",
                (uint32_t)stats.xSizeOfLargestFreeBlockInBytes,
                (uint32_t)stats.xNumberOfFreeBlocks);
    check("freeing every other block leaves the heap in pieces", stats.xNumberOfFreeBlocks >= 5);

    for (int i = 1; i < 10; i += 2) {
        vPortFree(blocks[i]);
        blocks[i] = NULL;
    }
    vPortGetHeapStats(&stats);
    check("freeing the rest joins them back into one block",
          stats.xNumberOfFreeBlocks == 1 && xPortGetFreeHeapSize() == start);

    void *big = pvPortMalloc(start - 64);
    check("one block of nearly the whole heap can be allocated again", big != NULL);

    vPortFree(big);

    TaskHandle_t tasks[40];
    int made = 0;
    while (made < 40 && xTaskCreate(idle_task, "t", 128, NULL, 1, &tasks[made]) == pdPASS) {
        made++;
    }
    check("tasks can be created until memory runs out", made > 5 && made < 40);

    for (int i = 0; i < made; i++) {
        vTaskDelete(tasks[i]);
    }
    vTaskDelay(20);
    check("deleting them gives the memory back once the idle task runs",
          xPortGetFreeHeapSize() == start);

    result();
    vTaskSuspend(NULL);
}

int main(void)
{
    bao_init();
    xTaskCreate(runner, "runner", 512, NULL, 3, NULL);
    vTaskStartScheduler();

    for (;;) {
    }
}
