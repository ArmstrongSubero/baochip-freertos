/*
 * rtos_fromisr: the FromISR calls the other tests do not use
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Each is made from a timer interrupt: queue receive, peek and overwrite,
 * semaphore take, timer start and reset, stream buffer receive, and a
 * nested critical section inside the handler.
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
#include "stream_buffer.h"
#include <string.h>

static QueueHandle_t q, box;
static SemaphoreHandle_t sem;
static TimerHandle_t one_shot;
static StreamBufferHandle_t stream;

static volatile int stage;
static volatile bool stage_done;

static volatile uint32_t got[4];
static volatile bool empty_after, waiting_was_3, full_was, empty_was;
static volatile bool peek_ok, overwrite_ok;
static volatile bool take1, take2;
static volatile uint32_t fired, fired_tick;
static volatile uint32_t stream_n;
static volatile char stream_buf[8];
static volatile uint32_t mie_inside, mie_after, mie_before, tick_inside;

static uint32_t mstatus(void)
{
    uint32_t v;

    __asm volatile("csrr %0, mstatus" : "=r"(v));
    return v;
}

static void timer_cb(TimerHandle_t timer)
{
    (void)timer;

    fired++;
    fired_tick = xTaskGetTickCount();
}

static void isr(void)
{
    if (stage_done) {
        return;
    }
    BaseType_t woken = pdFALSE;
    uint32_t v = 0;

    switch (stage) {
    case 1:
        waiting_was_3 = uxQueueMessagesWaitingFromISR(q) == 3;
        full_was = xQueueIsQueueFullFromISR(q) == pdTRUE;
        for (int i = 0; i < 3; i++) {
            if (xQueueReceiveFromISR(q, &v, &woken) == pdPASS) {
                got[i] = v;
            }
        }
        empty_after = xQueueReceiveFromISR(q, &v, &woken) == pdFAIL;
        empty_was = xQueueIsQueueEmptyFromISR(q) == pdTRUE;
        break;
    case 2:
        v = 42;
        xQueueOverwriteFromISR(box, &v, &woken);
        v = 43;
        xQueueOverwriteFromISR(box, &v, &woken);
        v = 0;
        peek_ok = xQueuePeekFromISR(box, &v) == pdPASS && v == 43;
        overwrite_ok = uxQueueMessagesWaitingFromISR(box) == 1;
        break;
    case 3:
        take1 = xSemaphoreTakeFromISR(sem, &woken) == pdTRUE;
        take2 = xSemaphoreTakeFromISR(sem, &woken) == pdTRUE;
        break;
    case 4:
        xTimerStartFromISR(one_shot, &woken);
        break;
    case 5:
        xTimerResetFromISR(one_shot, &woken);
        break;
    case 6:
        stream_n =
            xStreamBufferReceiveFromISR(stream, (void *)stream_buf, sizeof(stream_buf), &woken);
        break;
    case 7: {
        mie_before = mstatus() & 8u;
        UBaseType_t outer = taskENTER_CRITICAL_FROM_ISR();
        UBaseType_t inner = taskENTER_CRITICAL_FROM_ISR();
        mie_inside = mstatus() & 8u;
        tick_inside = xTaskGetTickCountFromISR();
        taskEXIT_CRITICAL_FROM_ISR(inner);
        taskEXIT_CRITICAL_FROM_ISR(outer);
        mie_after = mstatus() & 8u;
        break;
    }
    default:
        return;
    }
    stage_done = true;
    portYIELD_FROM_ISR(woken);
}

static void run_stage(int s)
{
    stage_done = false;
    stage = s;
    for (int i = 0; i < 50 && !stage_done; i++) {
        vTaskDelay(1);
    }
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_fromisr: start\r\n");

    q = xQueueCreate(3, sizeof(uint32_t));
    box = xQueueCreate(1, sizeof(uint32_t));
    sem = xSemaphoreCreateBinary();
    one_shot = xTimerCreate("once", 20, pdFALSE, NULL, timer_cb);
    stream = xStreamBufferCreate(16, 1);
    stage_done = true;
    test_timer_start(1000, isr);

    for (uint32_t i = 1; i <= 3; i++) {
        xQueueSend(q, &i, 0);
    }
    run_stage(1);
    check("uxQueueMessagesWaitingFromISR and xQueueIsQueueFullFromISR see 3 of 3",
          waiting_was_3 && full_was);
    check("xQueueReceiveFromISR takes 1, 2, 3 in order", got[0] == 1 && got[1] == 2 && got[2] == 3);
    check("xQueueReceiveFromISR on an empty queue fails, xQueueIsQueueEmptyFromISR agrees",
          empty_after && empty_was);

    run_stage(2);
    check("xQueueOverwriteFromISR keeps one, xQueuePeekFromISR reads the newest",
          peek_ok && overwrite_ok);

    xSemaphoreGive(sem);
    run_stage(3);
    check("xSemaphoreTakeFromISR takes a given semaphore once, then fails", take1 && !take2);

    fired = 0;
    TickType_t t0 = xTaskGetTickCount();
    run_stage(4);
    vTaskDelay(30);
    uint32_t start_delay = fired_tick - t0;
    check("xTimerStartFromISR starts a 20 tick timer that fires once on time",
          fired == 1 && start_delay >= 19 && start_delay <= 22);

    fired = 0;
    t0 = xTaskGetTickCount();
    run_stage(4);
    vTaskDelay(10);
    run_stage(5);
    vTaskDelay(40);
    uint32_t reset_delay = fired_tick - t0;
    check("xTimerResetFromISR at 10 ticks pushes the fire out to about 30",
          fired == 1 && reset_delay >= 28 && reset_delay <= 34);

    mini_printf("     timer fired after %u ticks, after a reset %u ticks\r\n",
                (uint32_t)start_delay, (uint32_t)reset_delay);

    xStreamBufferSend(stream, "rovari", 6, 0);
    run_stage(6);
    check("xStreamBufferReceiveFromISR reads what a task sent",
          stream_n == 6 && memcmp((const void *)stream_buf, "rovari", 6) == 0);

    run_stage(7);
    check("taskENTER_CRITICAL_FROM_ISR nests, masks interrupts, and restores them",
          mie_inside == 0 && mie_after == mie_before && tick_inside != 0);

    test_timer_stop();

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
