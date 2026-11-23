/*
 * rtos_isr: waking a task from an interrupt, and tasks keeping their registers
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * The soft trigger of IRQ array 19 raises an interrupt on demand, and the
 * task it wakes has to run before the trigger returns. A 500 Hz Timer0
 * interrupt then wakes a high priority task while two equal priority
 * tasks compute, and each result is compared with the one worked out
 * before the interrupts started.
 *
 * This stands in for the nesting and FPU tests of the CH32V307 pack. The
 * Baochip-1x has no interrupt nesting and no FPU.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define SOFT_TRIGGER REG32(0xE000F000UL)
#define ROUNDS       1000

static SemaphoreHandle_t soft_sem, timer_sem;
static volatile uint32_t soft_irqs, soft_wakes, timer_irqs, timer_wakes;
static volatile uint32_t rounds[2], damaged[2];
static volatile bool computing;
static volatile uint32_t seed = 100;

static void on_soft(uint32_t pending)
{
    (void)pending;
    BaseType_t woken = pdFALSE;

    soft_irqs++;
    xSemaphoreGiveFromISR(soft_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

static void on_timer(void)
{
    BaseType_t woken = pdFALSE;

    timer_irqs++;
    xSemaphoreGiveFromISR(timer_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

static void soft_task(void *arg)
{
    (void)arg;

    for (;;) {
        xSemaphoreTake(soft_sem, portMAX_DELAY);
        soft_wakes++;
    }
}

static void timer_task(void *arg)
{
    (void)arg;

    for (;;) {
        xSemaphoreTake(timer_sem, portMAX_DELAY);
        timer_wakes++;
    }
}

static uint32_t __attribute__((noinline)) scramble(uint32_t start)
{
    uint32_t a = start, b = start ^ 0x9E3779B9u, c = 0x7F4A7C15u, d = 0x1234567u;
    uint32_t e = 1, f = 2, g = 3, h = 4;

    for (int i = 0; i < 2000; i++) {
        a += b ^ (c << 5);
        b = (b << 7) | (b >> 25);
        c ^= a + d;
        d = d * 1664525u + 1013904223u;
        e += a ^ d;
        f ^= (e >> 3) + b;
        g += f * 31u;
        h ^= g + (c >> 11);
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
}

static void compute(void *arg)
{
    uint32_t id = (uint32_t)arg;
    uint32_t expected = scramble(seed + id);

    for (;;) {
        if (computing) {
            if (scramble(seed + id) != expected) {
                damaged[id]++;
            }
            rounds[id]++;
        } else {
            vTaskDelay(1);
        }
    }
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_isr: start\r\n");

    soft_sem = xSemaphoreCreateBinary();
    timer_sem = xSemaphoreCreateBinary();
    xTaskCreate(soft_task, "soft", 256, NULL, 5, NULL);
    xTaskCreate(timer_task, "timer", 256, NULL, 6, NULL);
    xTaskCreate(compute, "compute0", 256, (void *)0, 1, NULL);
    xTaskCreate(compute, "compute1", 256, (void *)1, 1, NULL);

    irq_set_handler(IRQ_ARRAY19, on_soft);
    irq_enable_events(IRQ_ARRAY19, 1);

    bool each = true;
    for (uint32_t i = 0; i < ROUNDS; i++) {
        uint32_t before = soft_irqs;
        SOFT_TRIGGER = 1;
        for (volatile int k = 0; k < 1000 && soft_irqs == before; k++) {
        }
        each &= soft_irqs == before + 1 && soft_wakes == i + 1;
    }
    check("1000 soft interrupts each woke the task before the trigger returned", each);

    test_timer_start(500, on_timer);
    computing = true;
    vTaskDelay(300);
    computing = false;
    test_timer_stop();
    vTaskDelay(3);

    mini_printf("     %u timer interrupts, %u wakes, compute rounds %u and %u\r\n", timer_irqs,
                timer_wakes, rounds[0], rounds[1]);
    check("a 500 Hz interrupt ran for 300 ticks", timer_irqs >= 148 && timer_irqs <= 151);
    check("every one woke its task", timer_wakes == timer_irqs);
    check("the preempted tasks kept their registers", damaged[0] == 0 && damaged[1] == 0);

    uint32_t lo = rounds[0] < rounds[1] ? rounds[0] : rounds[1];
    uint32_t hi = rounds[0] < rounds[1] ? rounds[1] : rounds[0];
    check("the two equal priority tasks shared the CPU within 10 percent",
          lo > 0 && hi - lo <= hi / 10);

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
