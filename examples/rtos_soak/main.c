/*
 * rtos_soak: every kind of object busy at once
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Queues, a mutex, an event group, a stream buffer, a software timer and
 * a timer interrupt all run together for SOAK_SECONDS, 60 unless the
 * build sets it. Progress is checked once a second and the totals at the
 * end.
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

#ifndef SOAK_SECONDS
#define SOAK_SECONDS 60
#endif

static QueueHandle_t q;
static SemaphoreHandle_t isr_sem, shared_mtx;
static EventGroupHandle_t ping;
static StreamBufferHandle_t stream;
static TaskHandle_t notified_h;

static volatile uint32_t isr_runs, sem_taken, notes_taken;
static volatile uint32_t produced, consumed, queue_bad;
static volatile uint32_t shared, added[3];
static volatile uint32_t pings, pongs, timer_runs;
static volatile uint32_t stream_sent, stream_got, stream_bad;
static volatile uint32_t float_loops, float_bad;
static volatile bool stop;

static void isr(void)
{
    isr_runs++;
    BaseType_t w1 = pdFALSE, w2 = pdFALSE;
    xSemaphoreGiveFromISR(isr_sem, &w1);
    vTaskNotifyGiveFromISR(notified_h, &w2);
    portYIELD_FROM_ISR(w1 || w2);
}

static void sem_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (xSemaphoreTake(isr_sem, portMAX_DELAY)) {
            sem_taken++;
        }
    }
}

static void note_task(void *arg)
{
    (void)arg;

    for (;;) {
        notes_taken += ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

static void producer(void *arg)
{
    (void)arg;

    for (uint32_t v = 1; !stop; v++) {
        if (xQueueSend(q, &v, 100) == pdPASS) {
            produced = v;
        }
        if ((v & 7) == 0) {
            vTaskDelay(1);
        }
    }
    vTaskSuspend(NULL);
}

static void consumer(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        if (xQueueReceive(q, &v, portMAX_DELAY) == pdPASS) {
            if (v != consumed + 1) {
                queue_bad++;
            }
            consumed = v;
        }
    }
}

static void adder(void *p)
{
    int id = (int)(intptr_t)p;

    while (!stop) {
        xSemaphoreTake(shared_mtx, portMAX_DELAY);
        uint32_t s = shared;
        taskYIELD();
        shared = s + 1;
        added[id]++;
        xSemaphoreGive(shared_mtx);
        vTaskDelay(1 + id);
    }
    vTaskSuspend(NULL);
}

static void pinger(void *arg)
{
    (void)arg;

    while (!stop) {
        xEventGroupSetBits(ping, 0x1);
        if (xEventGroupWaitBits(ping, 0x2, pdTRUE, pdTRUE, 100) & 0x2) {
            pings++;
        }
        vTaskDelay(1);
    }
    vTaskSuspend(NULL);
}

static void ponger(void *arg)
{
    (void)arg;

    for (;;) {
        if (xEventGroupWaitBits(ping, 0x1, pdTRUE, pdTRUE, portMAX_DELAY) & 0x1) {
            pongs++;
            xEventGroupSetBits(ping, 0x2);
        }
    }
}

static void stream_writer(void *arg)
{
    (void)arg;
    uint8_t chunk[13];
    uint8_t next = 0;

    while (!stop) {
        for (size_t i = 0; i < sizeof(chunk); i++) {
            chunk[i] = next++;
        }
        stream_sent += xStreamBufferSend(stream, chunk, sizeof(chunk), 50);
        vTaskDelay(2);
    }
    vTaskSuspend(NULL);
}

static void stream_reader(void *arg)
{
    (void)arg;
    uint8_t buf[20];
    uint8_t expect = 0;

    for (;;) {
        size_t n = xStreamBufferReceive(stream, buf, sizeof(buf), portMAX_DELAY);
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != expect++) {
                stream_bad++;
            }
        }
        stream_got += n;
    }
}

static float series(int n)
{
    float s = 0.0f;

    for (int i = 1; i <= n; i++) {
        s += 1.0f / ((float)i * (float)i);
    }
    return s;
}

static void float_task(void *arg)
{
    (void)arg;
    volatile float want = series(300);

    for (;;) {
        if (series(300) != want) {
            float_bad++;
        }
        float_loops++;
    }
}

static void timer_cb(TimerHandle_t timer)
{
    (void)timer;

    timer_runs++;
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_soak: start, %d seconds\r\n", SOAK_SECONDS);

    q = xQueueCreate(8, sizeof(uint32_t));
    isr_sem = xSemaphoreCreateCounting(100000, 0);
    shared_mtx = xSemaphoreCreateMutex();
    ping = xEventGroupCreate();
    stream = xStreamBufferCreate(64, 1);
    TaskHandle_t all[12];
    int n = 0;
    xTaskCreate(sem_task, "sem", 256, NULL, 5, &all[n++]);
    xTaskCreate(note_task, "note", 256, NULL, 5, &all[n++]);
    notified_h = all[n - 1];
    xTaskCreate(producer, "prod", 256, NULL, 2, &all[n++]);
    xTaskCreate(consumer, "cons", 256, NULL, 3, &all[n++]);
    for (int i = 0; i < 3; i++) {
        xTaskCreate(adder, "add", 256, (void *)(intptr_t)i, 2, &all[n++]);
    }
    xTaskCreate(pinger, "ping", 256, NULL, 2, &all[n++]);
    xTaskCreate(ponger, "pong", 256, NULL, 3, &all[n++]);
    xTaskCreate(stream_writer, "sw", 256, NULL, 2, &all[n++]);
    xTaskCreate(stream_reader, "sr", 256, NULL, 3, &all[n++]);
    xTaskCreate(float_task, "float", 256, NULL, 1, &all[n++]);
    TimerHandle_t t = xTimerCreate("t", 10, pdTRUE, NULL, timer_cb);
    xTimerStart(t, 0);
    test_timer_start(1000, isr);

    uint32_t last_consumed = 0, last_pongs = 0, last_float = 0;
    bool moving = true;
    for (int s = 1; s <= SOAK_SECONDS; s++) {
        vTaskDelay(1000);
        bool ok = consumed > last_consumed && pongs > last_pongs && float_loops > last_float &&
                  queue_bad == 0 && stream_bad == 0 && float_bad == 0;
        moving &= ok;
        last_consumed = consumed;
        last_pongs = pongs;
        last_float = float_loops;
        if (s % 10 == 0 || !ok) {
            mini_printf("     %d s %s: queue %u, pongs %u, isr %u, heap low %u\r\n", s,
                        ok ? "ok" : "STUCK", (uint32_t)consumed, (uint32_t)pongs,
                        (uint32_t)isr_runs, (uint32_t)xPortGetMinimumEverFreeHeapSize());
        }
    }
    stop = true;
    test_timer_stop();
    xTimerStop(t, 0);
    vTaskDelay(200);

    check("every task kept making progress every second", moving);
    check("queue: every value arrived once and in order", queue_bad == 0 && consumed == produced);
    check("interrupt: every give and notification was taken",
          sem_taken == isr_runs && notes_taken == isr_runs);
    check("mutex: the shared counter equals the sum of the adds",
          shared == added[0] + added[1] + added[2] && shared > 0);
    check("event group ping pong stayed in step",
          pings > 0 && (pongs == pings || pongs == pings + 1));
    check("stream buffer: every byte arrived in order",
          stream_bad == 0 && stream_got == stream_sent && stream_got > 0);
    check("software timer kept firing", timer_runs >= (uint32_t)SOAK_SECONDS * 99);
    check("float results never changed", float_bad == 0 && float_loops > 0);

    UBaseType_t worst = 10000;
    for (int i = 0; i < n; i++) {
        UBaseType_t hw = uxTaskGetStackHighWaterMark(all[i]);
        if (hw < worst) {
            worst = hw;
        }
    }
    mini_printf("     least stack left in any task: %u words\r\n", (uint32_t)worst);
    check("every task still has 32 words of stack spare", worst >= 32);

    result();
    vTaskSuspend(NULL);
}

int main(void)
{
    bao_init();
    xTaskCreate(runner, "runner", 512, NULL, 6, NULL);
    vTaskStartScheduler();

    for (;;) {
    }
}
