/*
 * rtos_qset: queue sets
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * One task blocks on two queues and a semaphore at once while a timer
 * interrupt feeds all three. Built with queue sets on (rtos_config.h).
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

static QueueHandle_t qa, qb, qc;
static SemaphoreHandle_t sem;
static QueueSetHandle_t set;
static volatile uint32_t isr_sent;
static volatile uint32_t from_a, from_b, from_sem, bad, woke_ticks_worst;
static volatile TickType_t sent_tick;

static void isr_send(void)
{
    if (isr_sent >= 200) {
        return;
    }
    BaseType_t woken = pdFALSE;
    uint32_t v = isr_sent;
    sent_tick = xTaskGetTickCountFromISR();
    if (isr_sent % 4 == 3) {
        xSemaphoreGiveFromISR(sem, &woken);
    } else {
        xQueueSendFromISR((isr_sent % 4 == 0) ? qa : qb, &v, &woken);
    }
    isr_sent++;
    portYIELD_FROM_ISR(woken);
}

static void reader(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        QueueSetMemberHandle_t m = xQueueSelectFromSet(set, portMAX_DELAY);
        TickType_t lag = xTaskGetTickCount() - sent_tick;
        if (lag > woke_ticks_worst) {
            woke_ticks_worst = lag;
        }
        if (m == qa) {
            if (xQueueReceive(qa, &v, 0) == pdPASS) {
                from_a++;
            } else {
                bad++;
            }
        } else if (m == qb) {
            if (xQueueReceive(qb, &v, 0) == pdPASS) {
                from_b++;
            } else {
                bad++;
            }
        } else if (m == sem) {
            if (xSemaphoreTake(sem, 0) == pdTRUE) {
                from_sem++;
            } else {
                bad++;
            }
        } else {
            bad++;
        }
    }
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_qset: start\r\n");

    qa = xQueueCreate(4, sizeof(uint32_t));
    qb = xQueueCreate(4, sizeof(uint32_t));
    qc = xQueueCreate(2, sizeof(uint32_t));
    sem = xSemaphoreCreateBinary();
    set = xQueueCreateSet(4 + 4 + 1);
    check("the queue set was made", set != NULL);
    check("two queues and a semaphore join the set", xQueueAddToSet(qa, set) == pdPASS &&
                                                         xQueueAddToSet(qb, set) == pdPASS &&
                                                         xQueueAddToSet(sem, set) == pdPASS);

    uint32_t v = 7;
    xQueueSend(qc, &v, 0);
    check("a queue that already holds items cannot join", xQueueAddToSet(qc, set) == pdFAIL);

    xQueueReceive(qc, &v, 0);
    check("once empty it can join and leave again",
          xQueueAddToSet(qc, set) == pdPASS && xQueueRemoveFromSet(qc, set) == pdPASS);

    TickType_t t0 = xTaskGetTickCount();
    QueueSetMemberHandle_t m = xQueueSelectFromSet(set, 10);
    TickType_t waited = xTaskGetTickCount() - t0;
    check("select with nothing ready times out after 10 ticks",
          m == NULL && waited >= 10 && waited <= 12);

    v = 1;
    xQueueSend(qb, &v, 0);
    xSemaphoreGive(sem);
    v = 2;
    xQueueSend(qa, &v, 0);
    QueueSetMemberHandle_t m1 = xQueueSelectFromSet(set, 0);
    QueueSetMemberHandle_t m2 = xQueueSelectFromSet(set, 0);
    QueueSetMemberHandle_t m3 = xQueueSelectFromSet(set, 0);
    check("select hands members back in the order they became ready",
          m1 == qb && m2 == sem && m3 == qa);

    xQueueReceive(qb, &v, 0);
    xSemaphoreTake(sem, 0);
    xQueueReceive(qa, &v, 0);
    check("nothing is left in the set after reading them", xQueueSelectFromSet(set, 0) == NULL);

    xTaskCreate(reader, "reader", 384, NULL, 3, NULL);
    vTaskDelay(2);
    test_timer_start(1000, isr_send);
    vTaskDelay(300);
    test_timer_stop();
    vTaskDelay(5);
    check("a task blocked on the set got all 200 from an interrupt, sorted by member",
          from_a == 50 && from_b == 100 && from_sem == 50 && bad == 0);

    mini_printf(
        "     from the interrupt: queue a %u, queue b %u, semaphore %u, slowest wake %u ticks\r\n",
        (uint32_t)from_a, (uint32_t)from_b, (uint32_t)from_sem, (uint32_t)woke_ticks_worst);
    check("[hw] each one was handled within the tick it was sent", woke_ticks_worst <= 1);

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
