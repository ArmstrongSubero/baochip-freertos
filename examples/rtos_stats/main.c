/*
 * rtos_stats: run time stats and the task information calls
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * A task that spins 6 ms of every 10 has to show up as about 60 percent
 * of the run time, with idle taking the rest. The counter is the CPU
 * cycle counter in microseconds (rtos_config.h), zeroed just before the
 * scheduler starts.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"

uint32_t rtos_stats_micros(void)
{
    return micros();
}

static bool same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void busy(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t t0 = micros();
        while (micros() - t0 < 6000) {
        }
        vTaskDelay(4);
    }
}

static void sleeper(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(10000);
    }
}

static uint32_t start_us;
static TaskStatus_t status[12];

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_stats: start\r\n");

    TaskHandle_t busy_h, sleeper_h;
    xTaskCreate(busy, "busy", 256, NULL, 1, &busy_h);
    xTaskCreate(sleeper, "sleeper", 256, NULL, 1, &sleeper_h);

    vTaskDelay(1000);

    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t n = uxTaskGetSystemState(status, 12, &total);
    check("uxTaskGetSystemState sees every task", n == uxTaskGetNumberOfTasks() && n == 5);

    configRUN_TIME_COUNTER_TYPE sum = 0, busy_t = 0, idle_t = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        sum += status[i].ulRunTimeCounter;
        if (status[i].xHandle == busy_h) {
            busy_t = status[i].ulRunTimeCounter;
        }
        if (same(status[i].pcTaskName, "IDLE")) {
            idle_t = status[i].ulRunTimeCounter;
        }
    }
    uint32_t ran = total - start_us;
    uint32_t busy_pc = (uint32_t)((uint64_t)busy_t * 100 / (ran ? ran : 1));
    uint32_t idle_pc = (uint32_t)((uint64_t)idle_t * 100 / (ran ? ran : 1));
    uint32_t sum_pc = (uint32_t)((uint64_t)sum * 100 / (ran ? ran : 1));
    uint32_t idle_of_total = (uint32_t)((uint64_t)idle_t * 100 / (total ? total : 1));
    uint32_t kernel_idle = (uint32_t)ulTaskGetIdleRunTimePercent();
    mini_printf("     scheduler ran %u us: busy %u%%, idle %u%%, all tasks add up to %u%%\r\n",
                (uint32_t)ran, (uint32_t)busy_pc, (uint32_t)idle_pc, (uint32_t)sum_pc);
    check("the run time counter covered the one second", ran >= 1000000 && ran < 1050000);
    check("the busy task used about 60 percent, spinning 6 ms of every 10",
          busy_pc >= 52 && busy_pc <= 66);
    check("idle got most of the rest", idle_pc >= 30 && idle_pc <= 48);
    check("every task's time adds up to the whole", sum_pc >= 97 && sum_pc <= 101);
    check("ulTaskGetIdleRunTimePercent agrees",
          kernel_idle + 2 >= idle_of_total && kernel_idle <= idle_of_total + 2);

    TaskStatus_t info;
    vTaskGetInfo(sleeper_h, &info, pdTRUE, eInvalid);
    check("vTaskGetInfo names the task, its state and its stack",
          same(info.pcTaskName, "sleeper") && info.eCurrentState == eBlocked &&
              info.usStackHighWaterMark > 0 && info.usStackHighWaterMark < 256);
    check("eTaskGetState sees the runner running and the sleeper blocked",
          eTaskGetState(xTaskGetCurrentTaskHandle()) == eRunning &&
              eTaskGetState(sleeper_h) == eBlocked);

    result();
    vTaskSuspend(NULL);
}

int main(void)
{
    bao_init();
    xTaskCreate(runner, "runner", 768, NULL, 2, NULL);
    s_micros_base += micros();
    start_us = micros();
    vTaskStartScheduler();

    for (;;) {
    }
}
