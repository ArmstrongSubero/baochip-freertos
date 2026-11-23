/*
 * rtos_sched: suspending, resuming and deleting tasks
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Suspend and resume from a task and from an interrupt, a priority
 * change, ten tasks that delete themselves and give their memory back,
 * the scheduler suspended, nested critical sections and an aborted delay.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

static volatile uint32_t worker_count, isr_ticks;
static TaskHandle_t worker_h, sleeper_h;
static volatile bool resume_from_isr;

static void worker(void *arg)
{
    (void)arg;

    for (;;) {
        worker_count++;
        vTaskDelay(1);
    }
}

static void isr(void)
{
    isr_ticks++;
    if (resume_from_isr) {
        resume_from_isr = false;
        portYIELD_FROM_ISR(xTaskResumeFromISR(worker_h));
    }
}

static volatile bool raised_ran;

static void raised(void *arg)
{
    (void)arg;

    raised_ran = true;
    vTaskSuspend(NULL);
}

static void quitter(void *arg)
{
    (void)arg;

    vTaskDelete(NULL);
}

static volatile bool woke_early;
static volatile TickType_t slept;

static void sleeper(void *arg)
{
    (void)arg;
    TickType_t t0 = xTaskGetTickCount();

    vTaskDelay(1000);
    slept = xTaskGetTickCount() - t0;
    woke_early = true;
    vTaskSuspend(NULL);
}

static SemaphoreHandle_t gate;
static volatile bool gate_task_ran;

static void gate_task(void *arg)
{
    (void)arg;

    xSemaphoreTake(gate, portMAX_DELAY);
    gate_task_ran = true;
    vTaskSuspend(NULL);
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_sched: start\r\n");

    xTaskCreate(worker, "worker", 256, NULL, 2, &worker_h);
    test_timer_start(1000, isr);
    vTaskDelay(20);

    vTaskSuspend(worker_h);
    uint32_t c = worker_count;
    vTaskDelay(50);
    check("a suspended task does not run",
          worker_count == c && eTaskGetState(worker_h) == eSuspended);

    vTaskResume(worker_h);
    vTaskDelay(20);
    check("a resumed task runs again", worker_count > c + 10);

    vTaskSuspend(worker_h);
    c = worker_count;
    resume_from_isr = true;
    vTaskDelay(20);
    check("an interrupt can resume a task", !resume_from_isr && worker_count > c + 10);

    TaskHandle_t r;
    xTaskCreate(raised, "raised", 256, NULL, 1, &r);
    raised_ran = false;
    vTaskPrioritySet(r, 4);
    bool at_once = raised_ran;
    check("raising a task above the caller runs it at once", at_once && uxTaskPriorityGet(r) == 4);

    vTaskDelete(r);

    vTaskDelay(20);
    size_t before = xPortGetFreeHeapSize();
    for (int i = 0; i < 10; i++) {
        xTaskCreate(quitter, "quitter", 256, NULL, 1, NULL);
    }
    size_t during = xPortGetFreeHeapSize();
    vTaskDelay(50);
    size_t after = xPortGetFreeHeapSize();
    mini_printf("     free heap %u, with 10 tasks %u, after they deleted themselves %u\r\n",
                (uint32_t)before, (uint32_t)during, (uint32_t)after);
    check("10 tasks that delete themselves give all their memory back",
          during < before && after == before);

    gate = xSemaphoreCreateBinary();
    xTaskCreate(gate_task, "gate", 256, NULL, 4, NULL);
    TickType_t t_before = xTaskGetTickCount();
    vTaskSuspendAll();
    xSemaphoreGive(gate);
    bool held_back = !gate_task_ran;
    delay_ms(20);
    TickType_t frozen = xTaskGetTickCount();
    xTaskResumeAll();
    TickType_t t_after = xTaskGetTickCount();
    check("with the scheduler suspended, a woken higher priority task waits",
          held_back && gate_task_ran);
    check("the ticks missed while suspended are caught up afterwards",
          frozen - t_before <= 1 && t_after - t_before >= 15);

    uint32_t i0 = isr_ticks;
    taskENTER_CRITICAL();
    taskENTER_CRITICAL();
    uint32_t a = isr_ticks;
    for (volatile uint32_t k = 0; k < 300000; k++) {
    }
    bool still_a = isr_ticks == a;
    taskEXIT_CRITICAL();
    for (volatile uint32_t k = 0; k < 300000; k++) {
    }
    bool still_b = isr_ticks == a;
    taskEXIT_CRITICAL();
    vTaskDelay(5);
    check("interrupts stay off through nested critical sections until the last exit",
          still_a && still_b && isr_ticks > a && a >= i0);

    xTaskCreate(sleeper, "sleeper", 256, NULL, 3, &sleeper_h);
    vTaskDelay(10);
    check("aborting a 1000 tick delay wakes the task early", xTaskAbortDelay(sleeper_h) == pdPASS);

    vTaskDelay(2);
    check("the task saw a short sleep", woke_early && slept < 20);

    test_timer_stop();

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
