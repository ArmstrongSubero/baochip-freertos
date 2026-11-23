/*
 * rtos_sync: semaphores, mutexes, notifications, event groups and buffers
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Binary and counting semaphores, task notifications, an event group bit
 * and a stream buffer are each fed from a timer interrupt. Priority
 * inheritance, a recursive mutex, waiting for all bits and message
 * buffers are checked from tasks.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "event_groups.h"
#include "stream_buffer.h"
#include "message_buffer.h"

static SemaphoreHandle_t bin, counting, mtx, rmtx;
static EventGroupHandle_t events;
static StreamBufferHandle_t stream;
static MessageBufferHandle_t messages;
static TaskHandle_t taker_h, runner_h;

typedef enum {
    IDLE,
    BINARY,
    COUNTING,
    NOTIFY,
    BITS,
    STREAM
} Mode;

static volatile Mode mode = IDLE;
static volatile uint32_t isr_count, isr_refused, taken;
static volatile bool armed;

static void isr(void)
{
    BaseType_t woken = pdFALSE;

    switch (mode) {
    case BINARY:
        if (armed && isr_count < 200) {
            isr_count++;
            xSemaphoreGiveFromISR(bin, &woken);
        }
        break;
    case COUNTING:
        if (isr_count == 0) {
            for (int i = 0; i < 11; i++) {
                if (xSemaphoreGiveFromISR(counting, &woken) == pdTRUE) {
                    isr_count++;
                } else {
                    isr_refused++;
                }
            }
        }
        break;
    case NOTIFY:
        if (isr_count < 100) {
            isr_count++;
            vTaskNotifyGiveFromISR(taker_h, &woken);
        }
        break;
    case BITS:
        if (isr_count == 0) {
            isr_count++;
            xEventGroupSetBitsFromISR(events, 0x10, &woken);
        }
        break;
    case STREAM:
        if (isr_count < 64) {
            uint8_t b = (uint8_t)isr_count;
            if (xStreamBufferSendFromISR(stream, &b, 1, &woken) == 1) {
                isr_count++;
            }
        }
        break;
    default:
        break;
    }
    portYIELD_FROM_ISR(woken);
}

static void taker(void *arg)
{
    (void)arg;

    for (;;) {
        if (mode == BINARY && xSemaphoreTake(bin, 50) == pdTRUE) {
            taken++;
        } else if (mode == NOTIFY) {
            taken += ulTaskNotifyTake(pdTRUE, 50);
        } else {
            vTaskDelay(1);
        }
    }
}

static volatile bool low_has_mutex, low_done;
static volatile UBaseType_t low_prio_while_held;
static volatile uint32_t medium_runs, medium_runs_while_held;
static TaskHandle_t low_h;

static void low(void *arg)
{
    (void)arg;

    xSemaphoreTake(mtx, portMAX_DELAY);
    low_has_mutex = true;
    uint32_t until = millis() + 30;
    while ((int32_t)(millis() - until) < 0) {
    }
    low_prio_while_held = uxTaskPriorityGet(NULL);
    medium_runs_while_held = medium_runs;
    xSemaphoreGive(mtx);
    low_done = true;
    vTaskSuspend(NULL);
}

static void medium(void *arg)
{
    (void)arg;

    for (;;) {
        medium_runs++;
    }
}

static volatile bool other_got_rmtx;

static void other(void *arg)
{
    (void)arg;

    other_got_rmtx = xSemaphoreTakeRecursive(rmtx, 0) == pdTRUE;
    if (other_got_rmtx) {
        xSemaphoreGiveRecursive(rmtx);
    }
    vTaskSuspend(NULL);
}

static volatile EventBits_t woke_with;
static volatile TickType_t woke_at;

static void waiter_all(void *arg)
{
    (void)arg;

    woke_with = xEventGroupWaitBits(events, 0x03, pdTRUE, pdTRUE, 500);
    woke_at = xTaskGetTickCount();
    vTaskSuspend(NULL);
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_sync: start\r\n");

    bin = xSemaphoreCreateBinary();
    counting = xSemaphoreCreateCounting(10, 0);
    mtx = xSemaphoreCreateMutex();
    rmtx = xSemaphoreCreateRecursiveMutex();
    events = xEventGroupCreate();
    stream = xStreamBufferCreate(32, 1);
    messages = xMessageBufferCreate(128);
    xTaskCreate(taker, "taker", 384, NULL, 4, &taker_h);
    test_timer_start(1000, isr);

    mode = BINARY;
    isr_count = 0;
    taken = 0;
    vTaskDelay(5);
    armed = true;
    vTaskDelay(300);
    mode = IDLE;
    vTaskDelay(60);
    check("binary semaphore: 200 gives from an interrupt, 200 takes",
          isr_count == 200 && taken == 200);

    isr_count = 0;
    isr_refused = 0;
    mode = COUNTING;
    vTaskDelay(5);
    mode = IDLE;
    int n = 0;
    while (xSemaphoreTake(counting, 0) == pdTRUE) {
        n++;
    }
    check("counting semaphore of 10: 10 gives from an interrupt, the 11th refused",
          isr_count == 10 && isr_refused == 1);
    check("counting semaphore: exactly 10 takes", n == 10);

    mode = NOTIFY;
    isr_count = 0;
    taken = 0;
    vTaskDelay(200);
    mode = IDLE;
    vTaskDelay(60);
    check("task notification: 100 gives from an interrupt, 100 counted",
          isr_count == 100 && taken == 100);

    TaskHandle_t medium_h;
    xTaskCreate(low, "low", 384, NULL, 1, &low_h);
    vTaskDelay(3);
    check("low priority task holds the mutex", low_has_mutex);

    xTaskCreate(medium, "medium", 256, NULL, 2, &medium_h);
    TickType_t t0 = xTaskGetTickCount();
    bool got = xSemaphoreTake(mtx, 1000) == pdTRUE;
    TickType_t waited = xTaskGetTickCount() - t0;
    if (got) {
        xSemaphoreGive(mtx);
    }
    check("high priority task gets the mutex once low gives it", got && waited < 100);
    check("low inherited priority 3 while it held the mutex", low_prio_while_held == 3);
    check("the medium task did not run while low held the mutex", medium_runs_while_held == 0);

    vTaskDelete(medium_h);
    vTaskDelete(low_h);

    bool rec = true;
    for (int i = 0; i < 3; i++) {
        rec &= xSemaphoreTakeRecursive(rmtx, 0) == pdTRUE;
    }
    xSemaphoreGiveRecursive(rmtx);
    xSemaphoreGiveRecursive(rmtx);
    xTaskCreate(other, "other", 256, NULL, 4, NULL);
    bool blocked = !other_got_rmtx;
    xSemaphoreGiveRecursive(rmtx);
    xTaskCreate(other, "other2", 256, NULL, 4, NULL);
    check("recursive mutex taken 3 times stays held until the 3rd give",
          rec && blocked && other_got_rmtx);

    xTaskCreate(waiter_all, "waiter", 256, NULL, 4, NULL);
    TickType_t start = xTaskGetTickCount();
    xEventGroupSetBits(events, 0x01);
    vTaskDelay(10);
    bool not_yet = woke_at == 0;
    xEventGroupSetBits(events, 0x02);
    vTaskDelay(2);
    check("wait for all bits wakes only when both are set, and clears them",
          not_yet && (woke_with & 0x03) == 0x03 && woke_at - start >= 10 &&
              (xEventGroupGetBits(events) & 0x03) == 0);

    EventBits_t any = 0;
    mode = BITS;
    isr_count = 0;
    any = xEventGroupWaitBits(events, 0x10 | 0x20, pdTRUE, pdFALSE, 200);
    mode = IDLE;
    check("wait for any bit wakes on a bit set from an interrupt", (any & 0x10) != 0);

    mode = STREAM;
    isr_count = 0;
    uint8_t buf[64];
    size_t total = 0;
    while (total < 64) {
        size_t got_n = xStreamBufferReceive(stream, buf + total, sizeof(buf) - total, 200);
        if (got_n == 0) {
            break;
        }
        total += got_n;
    }
    mode = IDLE;
    bool seq = total == 64;
    for (size_t i = 0; i < total; i++) {
        seq &= buf[i] == (uint8_t)i;
    }
    check("stream buffer: 64 bytes from an interrupt arrive whole and in order", seq);

    bool msgs = true;
    for (int len = 1; len <= 20; len++) {
        uint8_t out[20], in[24];
        for (int i = 0; i < len; i++) {
            out[i] = (uint8_t)(len * 7 + i);
        }
        msgs &= xMessageBufferSend(messages, out, len, 0) == (size_t)len;
        size_t r = xMessageBufferReceive(messages, in, sizeof(in), 0);
        msgs &= r == (size_t)len;
        for (int i = 0; i < len && msgs; i++) {
            msgs &= in[i] == out[i];
        }
    }
    check("message buffer: 20 messages of 1 to 20 bytes keep their lengths", msgs);

    test_timer_stop();

    result();
    vTaskSuspend(NULL);
}

int main(void)
{
    bao_init();
    xTaskCreate(runner, "runner", 512, NULL, 3, &runner_h);
    vTaskStartScheduler();

    for (;;) {
    }
}
