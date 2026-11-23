/*
 * rtos_queue: queues
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fills a queue and checks the full and empty timeouts, peek, the order
 * items come out in, send to front and overwrite. Then a timer interrupt
 * sends 500 values to a queue and the time for the waiting task to wake
 * is measured while a lower priority task spins.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

static QueueHandle_t q, isrq, mailbox;
static TaskHandle_t consumer_h;
static volatile uint32_t isr_next, isr_sent, isr_full, sent_at_us;
static volatile uint32_t got, got_bad, worst_wake_us;
static volatile bool consumer_in_time = true;

static void isr_send(void)
{
    if (isr_next >= 500) {
        return;
    }
    uint32_t v = ++isr_next;
    BaseType_t woken = pdFALSE;
    sent_at_us = micros();
    if (xQueueSendFromISR(isrq, &v, &woken) == pdPASS) {
        isr_sent++;
    } else {
        isr_full++;
    }
    portYIELD_FROM_ISR(woken);
}

static void consumer(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        if (xQueueReceive(isrq, &v, portMAX_DELAY) == pdPASS) {
            uint32_t lag = micros() - sent_at_us;
            if (lag > worst_wake_us) {
                worst_wake_us = lag;
            }
            if (v != got + 1) {
                got_bad++;
            }
            got = v;
        }
    }
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
    uint32_t v = 0;

    mini_printf("rtos_queue: start\r\n");

    q = xQueueCreate(4, sizeof(uint32_t));
    bool all = true;
    for (uint32_t i = 1; i <= 4; i++) {
        all &= xQueueSend(q, &i, 0) == pdPASS;
    }
    check("4 sends fill a queue of 4", all);

    v = 5;
    check("a 5th send with no wait reports full", xQueueSend(q, &v, 0) == errQUEUE_FULL);

    TickType_t t0 = xTaskGetTickCount();
    BaseType_t r = xQueueSend(q, &v, 20);
    TickType_t waited = xTaskGetTickCount() - t0;
    check("a send to a full queue waits its 20 tick timeout, then fails",
          r == errQUEUE_FULL && waited >= 20 && waited <= 22);
    check("spaces 0, waiting 4", uxQueueSpacesAvailable(q) == 0 && uxQueueMessagesWaiting(q) == 4);

    check("peek returns the oldest without removing it",
          xQueuePeek(q, &v, 0) == pdPASS && v == 1 && uxQueueMessagesWaiting(q) == 4);

    bool order = true;
    for (uint32_t i = 1; i <= 4; i++) {
        order &= xQueueReceive(q, &v, 0) == pdPASS && v == i;
    }
    check("receives come out first in, first out", order);

    t0 = xTaskGetTickCount();
    r = xQueueReceive(q, &v, 15);
    waited = xTaskGetTickCount() - t0;
    check("a receive from an empty queue waits its 15 tick timeout, then fails",
          r == pdFALSE && waited >= 15 && waited <= 17);

    v = 10;
    xQueueSend(q, &v, 0);
    v = 20;
    xQueueSendToFront(q, &v, 0);
    uint32_t a = 0, b = 0;
    xQueueReceive(q, &a, 0);
    xQueueReceive(q, &b, 0);
    check("send to front jumps the line", a == 20 && b == 10);

    mailbox = xQueueCreate(1, sizeof(uint32_t));
    v = 1;
    xQueueOverwrite(mailbox, &v);
    v = 2;
    xQueueOverwrite(mailbox, &v);
    check("overwrite keeps only the newest value",
          xQueueReceive(mailbox, &v, 0) == pdPASS && v == 2);

    isrq = xQueueCreate(8, sizeof(uint32_t));
    xTaskCreate(consumer, "consumer", 384, NULL, 4, &consumer_h);
    xTaskCreate(spinner, "spinner", 256, NULL, 1, NULL);
    test_timer_start(1000, isr_send);
    vTaskDelay(700);
    test_timer_stop();
    vTaskDelay(10);
    check("an interrupt sent 500 values, none lost to a full queue",
          isr_sent == 500 && isr_full == 0);
    check("the task got all 500, in order", got == 500 && got_bad == 0);

    mini_printf("     slowest wake after the interrupt: %u us\r\n", (uint32_t)worst_wake_us);
    check("[hw] the woken task ran before the next tick, ahead of the busy task",
          worst_wake_us < 500);

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
