/*
 * rtos_timing: the tick rate, software timers and time slicing
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Each is measured against the CPU cycle counter: 1000 ticks of
 * vTaskDelay, one shot and auto reload timers, a changed period, a reset,
 * 100 periods of vTaskDelayUntil, and two equal priority tasks sharing
 * the processor.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

static volatile uint32_t one_shot_runs, reload_runs, reload_bad_gap;
static volatile uint32_t reload_first_us, reload_last_us;
static volatile TickType_t reload_last_tick;

static void one_shot_cb(TimerHandle_t timer)
{
    (void)timer;

    one_shot_runs++;
}

static void reload_cb(TimerHandle_t timer)
{
    (void)timer;
    TickType_t now = xTaskGetTickCount();
    uint32_t us = micros();

    if (reload_runs == 0) {
        reload_first_us = us;
    } else if (now - reload_last_tick != 20) {
        reload_bad_gap++;
    }
    reload_last_tick = now;
    reload_last_us = us;
    reload_runs++;
}

static volatile uint32_t spin_a, spin_b;

static void spin_a_task(void *arg)
{
    (void)arg;

    for (;;) {
        spin_a++;
    }
}

static void spin_b_task(void *arg)
{
    (void)arg;

    for (;;) {
        spin_b++;
    }
}

static uint32_t worst_late_us, bad_wakes;

static void periodic(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t start_us = 0;

    for (int i = 0; i <= 100; i++) {
        TickType_t before = last;
        xTaskDelayUntil(&last, 10);
        if (last - before != 10 || xTaskGetTickCount() != last) {
            bad_wakes++;
        }
        if (i == 0) {
            start_us = micros();
            continue;
        }
        int32_t late = (int32_t)(micros() - start_us) - i * 10000;
        if (late < 0) {
            late = -late;
        }
        if ((uint32_t)late > worst_late_us) {
            worst_late_us = late;
        }
    }
    vTaskSuspend(NULL);
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_timing: start\r\n");

    uint32_t us0 = micros();
    TickType_t t0 = xTaskGetTickCount();
    vTaskDelay(1000);
    uint32_t us = micros() - us0;
    TickType_t ticks = xTaskGetTickCount() - t0;
    mini_printf("     1000 ticks took %u us\r\n", (uint32_t)us);
    check("[hw] 1000 ticks of vTaskDelay last 1 second to within 0.2 percent",
          ticks == 1000 && us > 998000 && us < 1002000);

    TimerHandle_t once = xTimerCreate("once", 50, pdFALSE, NULL, one_shot_cb);
    TimerHandle_t every = xTimerCreate("every", 20, pdTRUE, NULL, reload_cb);
    xTimerStart(once, 0);
    xTimerStart(every, 0);
    vTaskDelay(1010);
    xTimerStop(every, 0);
    vTaskDelay(5);
    uint32_t runs = reload_runs;
    check("one shot timer fired exactly once", one_shot_runs == 1);
    check("20 ms auto reload timer fired 50 times in a second", runs >= 50 && runs <= 51);
    check("every auto reload gap was exactly 20 ticks", reload_bad_gap == 0);

    uint32_t span = reload_last_us - reload_first_us;
    uint32_t expect = (runs - 1) * 20000;
    check("[hw] auto reload period agrees with the hardware clock to 0.2 percent",
          span > expect - expect / 500 && span < expect + expect / 500);

    vTaskDelay(100);
    check("a stopped timer does not fire", reload_runs == runs);

    reload_runs = 0;
    xTimerChangePeriod(every, 5, 0);
    vTaskDelay(101);
    xTimerStop(every, 0);
    uint32_t fast = reload_runs;
    check("changing the period to 5 ms gives 20 runs in 100 ms", fast >= 19 && fast <= 21);

    one_shot_runs = 0;
    xTimerStart(once, 0);
    vTaskDelay(30);
    xTimerReset(once, 0);
    vTaskDelay(30);
    bool not_yet = one_shot_runs == 0;
    vTaskDelay(40);
    check("resetting a one shot timer restarts its 50 ms", not_yet && one_shot_runs == 1);

    TaskHandle_t a, b;
    xTaskCreate(periodic, "periodic", 384, NULL, 3, NULL);
    xTaskCreate(spin_a_task, "spin_a", 256, NULL, 1, &a);
    xTaskCreate(spin_b_task, "spin_b", 256, NULL, 1, &b);
    vTaskDelay(1100);
    mini_printf("     vTaskDelayUntil worst lateness %u us\r\n", (uint32_t)worst_late_us);
    check("vTaskDelayUntil woke on every 10th tick for 100 periods", bad_wakes == 0);
    check("[hw] vTaskDelayUntil drifted less than 100 us over a second", worst_late_us < 100);

    vTaskDelete(a);
    vTaskDelete(b);
    uint32_t sa = spin_a, sb = spin_b;
    uint32_t lo = sa < sb ? sa : sb, hi = sa < sb ? sb : sa;
    mini_printf("     equal priority loops: %u and %u\r\n", (uint32_t)sa, (uint32_t)sb);
    check("two equal priority tasks shared the CPU within 10 percent", lo > 0 && hi - lo < hi / 10);

    result();
    vTaskSuspend(NULL);
}

int main(void)
{
    bao_init();
    xTaskCreate(runner, "runner", 512, NULL, 4, NULL);
    vTaskStartScheduler();

    for (;;) {
    }
}
