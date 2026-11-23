/*
 * rtos_tickless: tickless idle
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * With every task blocked the core sleeps in WFI through the ticks
 * instead of waking for each one. A 1000 tick delay has to count 1000
 * ticks with the core asleep for nearly all of it. Then 40 falling edges
 * on PB4 wake the core early, and the tick count has to stay in step with
 * the TickTimer.
 *
 * The edges have to come from outside, because nothing the program could
 * start keeps running while the CPU clock is stopped. run_rtos.py pulses
 * the pin on the emulator. On the board, a 20 Hz square wave or a wire
 * tapped to GND on PB4 does it.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 *   PB4 to a source of falling edges, for the second half only
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#define IOX_INTCR0 REG32(IOX_BASE + 0x100)
#define IOX_INTFR  REG32(IOX_BASE + 0x120)
#define WAKE_PIN   4

static volatile uint32_t tick_irqs, sleeps, asked_ticks;

void vApplicationTickHook(void)
{
    tick_irqs++;
}

void rtos_test_pre_sleep(uint32_t expected)
{
    sleeps++;
    asked_ticks += expected;
}

static SemaphoreHandle_t sem;
static volatile uint32_t gives, takes;

static void on_edge(uint32_t pending)
{
    (void)pending;
    BaseType_t woken = pdFALSE;

    IOX_INTFR = 1;
    if (gives >= 40) {
        return;
    }
    gives++;
    xSemaphoreGiveFromISR(sem, &woken);
    portYIELD_FROM_ISR(woken);
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_tickless: start\r\n");

    sem = xSemaphoreCreateBinary();

    vTaskDelay(5);
    uint32_t irq0 = tick_irqs, sl0 = sleeps, ask0 = asked_ticks;
    TickType_t k0 = xTaskGetTickCount();
    uint32_t m0 = (uint32_t)millis();
    vTaskDelay(1000);
    uint32_t real = (uint32_t)millis() - m0;
    TickType_t kernel = xTaskGetTickCount() - k0;
    uint32_t irqs = tick_irqs - irq0;
    uint32_t naps = sleeps - sl0;
    uint32_t asked = asked_ticks - ask0;
    mini_printf(
        "     1000 tick delay: %u ms on the TickTimer, %u tick interrupts, %u sleeps covering %u ticks\r\n",
        real, irqs, naps, asked);
    check("the delay counted 1000 ticks", kernel >= 1000 && kernel <= 1001);
    check("[hw] and took 1000 ms, within 1 percent", real >= 990 && real <= 1010);
    check("the core slept through the ticks instead of waking for each",
          irqs <= 20 && naps >= 1 && asked >= 950);

    gpio_init(GPIO_PORT_B, WAKE_PIN);
    gpio_set_dir(GPIO_PORT_B, WAKE_PIN, false);
    gpio_pull_up(GPIO_PORT_B, WAKE_PIN);
    IOX_INTCR0 = (1u << 9) | (1u << 7) | (16 + WAKE_PIN);
    IOX_INTFR = 1;
    irq_set_handler(IRQ_ARRAY10, on_edge);
    irq_enable_events(IRQ_ARRAY10, 1);

    mini_printf("     waiting for 40 falling edges on PB4\r\n");
    irq0 = tick_irqs;
    k0 = xTaskGetTickCount();
    m0 = (uint32_t)millis();
    for (int i = 0; i < 40; i++) {
        if (xSemaphoreTake(sem, 200) == pdTRUE) {
            takes++;
        }
    }
    real = (uint32_t)millis() - m0;
    kernel = xTaskGetTickCount() - k0;
    irqs = tick_irqs - irq0;
    irq_disable(IRQ_ARRAY10);
    uint32_t off = kernel > real ? kernel - real : real - kernel;
    mini_printf(
        "     40 early wakes: %u ticks against %u ms on the TickTimer, %u tick interrupts\r\n",
        (uint32_t)kernel, real, irqs);
    check("an interrupt woke the sleeping core 40 times and its task ran each time",
          gives == 40 && takes == 40);
    check("[hw] after 40 early wakes the tick count is still within 3 ms of the TickTimer",
          off <= 3);
    check("between wakes it still slept through the ticks", irqs <= 200);

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
