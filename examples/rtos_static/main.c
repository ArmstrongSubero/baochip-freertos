/*
 * rtos_static: static allocation
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every kind of object is made in memory the program owns, used, and the
 * heap is checked to be exactly where it started. Built with static
 * allocation on (rtos_config.h).
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
#include "event_groups.h"
#include "stream_buffer.h"
#include "message_buffer.h"

static StaticTask_t worker_tcb;
static StackType_t worker_stack[256];
static StaticQueue_t q_struct;
static uint8_t q_storage[4 * sizeof(uint32_t)];
static StaticSemaphore_t bin_struct, mtx_struct, cnt_struct;
static StaticTimer_t tmr_struct;
static StaticEventGroup_t eg_struct;
static StaticStreamBuffer_t sb_struct, mb_struct;
static uint8_t sb_storage[33], mb_storage[65];

static volatile uint32_t worker_runs, timer_runs;
static QueueHandle_t q;

static void worker(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        if (xQueueReceive(q, &v, portMAX_DELAY) == pdPASS) {
            worker_runs += v;
        }
    }
}

static void tick_cb(TimerHandle_t timer)
{
    (void)timer;

    timer_runs++;
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_static: start\r\n");

    size_t heap0 = xPortGetFreeHeapSize();

    q = xQueueCreateStatic(4, sizeof(uint32_t), q_storage, &q_struct);
    TaskHandle_t w = xTaskCreateStatic(worker, "worker", 256, NULL, 3, worker_stack, &worker_tcb);
    SemaphoreHandle_t bin = xSemaphoreCreateBinaryStatic(&bin_struct);
    SemaphoreHandle_t mtx = xSemaphoreCreateMutexStatic(&mtx_struct);
    SemaphoreHandle_t cnt = xSemaphoreCreateCountingStatic(5, 2, &cnt_struct);
    TimerHandle_t tmr = xTimerCreateStatic("tick10", 10, pdTRUE, NULL, tick_cb, &tmr_struct);
    EventGroupHandle_t eg = xEventGroupCreateStatic(&eg_struct);
    StreamBufferHandle_t sb = xStreamBufferCreateStatic(32, 1, sb_storage, &sb_struct);
    MessageBufferHandle_t mb = xMessageBufferCreateStatic(64, mb_storage, &mb_struct);

    check("every static create returned a handle",
          q && w && bin && mtx && cnt && tmr && eg && sb && mb);
    check("the task and queue live in the program's own memory",
          (void *)w == (void *)&worker_tcb && (void *)q == (void *)&q_struct);
    check("making all nine took nothing from the heap", xPortGetFreeHeapSize() == heap0);

    for (uint32_t i = 1; i <= 10; i++) {
        xQueueSend(q, &i, portMAX_DELAY);
    }
    vTaskDelay(2);
    check("the static task took 10 values through the static queue", worker_runs == 55);

    xSemaphoreGive(bin);
    bool b = xSemaphoreTake(bin, 0) == pdTRUE && xSemaphoreTake(bin, 0) == pdFALSE;
    bool m = xSemaphoreTake(mtx, 0) == pdTRUE &&
             xSemaphoreGetMutexHolder(mtx) == xTaskGetCurrentTaskHandle();
    xSemaphoreGive(mtx);
    bool c = uxSemaphoreGetCount(cnt) == 2 && xSemaphoreTake(cnt, 0) && xSemaphoreTake(cnt, 0) &&
             !xSemaphoreTake(cnt, 0);
    check("binary, mutex and counting semaphores work from static memory", b && m && c);

    xTimerStart(tmr, 0);
    vTaskDelay(105);
    xTimerStop(tmr, 0);
    check("a static 10 tick timer ran 10 times in 105 ticks", timer_runs == 10);

    xEventGroupSetBits(eg, 0x3);
    check("a static event group holds its bits", (xEventGroupGetBits(eg) & 0x3) == 0x3);

    char in[16] = {0};
    xStreamBufferSend(sb, "abcdef", 6, 0);
    size_t sn = xStreamBufferReceive(sb, in, sizeof(in), 0);
    xMessageBufferSend(mb, "hello", 5, 0);
    xMessageBufferSend(mb, "world!", 6, 0);
    size_t m1 = xMessageBufferReceive(mb, in, sizeof(in), 0);
    size_t m2 = xMessageBufferReceive(mb, in, sizeof(in), 0);
    check("static stream and message buffers keep bytes and message edges",
          sn == 6 && m1 == 5 && m2 == 6);

    vTaskDelete(w);
    vTaskDelay(5);
    check("deleting a static task gives nothing back to the heap", xPortGetFreeHeapSize() == heap0);

    worker_runs = 0;
    w = xTaskCreateStatic(worker, "worker2", 256, NULL, 3, worker_stack, &worker_tcb);
    uint32_t one = 1;
    xQueueSend(q, &one, 0);
    vTaskDelay(2);
    check("its memory can make a new task straight away", w && worker_runs == 1);

    mini_printf("     free heap %u, the same before and after every static object\r\n",
                (uint32_t)heap0);

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
