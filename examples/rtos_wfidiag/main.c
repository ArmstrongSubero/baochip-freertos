/*
 * rtos_wfidiag: what wakes the Baochip-1x from WFI
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * A bare metal program, no kernel. It answers on the board the three
 * questions the FreeRTOS port's idle sleep rests on:
 *
 *   A  interrupts on, TickTimer alarm 50 ms away: WFI should wake at 50 ms
 *   B  interrupts off (mstatus.MIE clear), same alarm: WFI should still
 *      wake at 50 ms, which is what tickless idle does
 *   C  TickTimer alarm 300 ms away with Timer0 interrupting at 100 Hz:
 *      Timer0 counts on the CPU clock, which stops in WFI, so the core
 *      should sleep the whole 300 ms and Timer0 should not have fired
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "bao.h"
#include "hardware/irq.h"
#include "hardware/regs/timer.h"

static volatile uint32_t alarms, timer0_irqs;

static void alarm_at(uint64_t tick)
{
    TICKTIMER_EV_ENABLE = 0;
    TICKTIMER_MSLEEP_TARGET1 = (uint32_t)(tick >> 32);
    TICKTIMER_MSLEEP_TARGET0 = (uint32_t)tick;
    TICKTIMER_EV_PENDING = 1;
    memory_fence();
    TICKTIMER_EV_ENABLE = 1;
}

static void on_alarm(uint32_t pending)
{
    (void)pending;

    alarms++;
    TICKTIMER_EV_ENABLE = 0;
}

static void on_timer0(uint32_t pending)
{
    (void)pending;

    timer0_irqs++;
}

static uint32_t sleep_until_alarm(uint32_t ms)
{
    uint32_t start = (uint32_t)millis();
    uint32_t seen = alarms;

    alarm_at(millis() + ms);
    while (alarms == seen && ((uint32_t)millis() - start) < 2000) {
        __asm__ volatile("wfi");
        if (timer0_irqs != 0) {
            break;
        }
    }
    return (uint32_t)millis() - start;
}

int main(void)
{
    uint32_t took;
    int bad = 0;

    bao_init();
    mini_printf("rtos_wfidiag: start\r\n");

    irq_init();
    irq_set_handler(IRQ_TICKTIMER, on_alarm);
    irq_set_handler(IRQ_TIMER0, on_timer0);
    TICKTIMER_EV_ENABLE = 0;
    irq_enable(IRQ_TICKTIMER);

    took = sleep_until_alarm(50);
    mini_printf("A  interrupts on:  woke after %u ms\r\n", took);
    bad += !(took >= 49 && took <= 52);

    __asm__ volatile("csrci mstatus, 8");
    uint32_t start = (uint32_t)millis();
    alarm_at(millis() + 50);
    __asm__ volatile("wfi");
    took = (uint32_t)millis() - start;
    __asm__ volatile("csrsi mstatus, 8");
    mini_printf("B  interrupts off: woke after %u ms\r\n", took);
    bad += !(took >= 49 && took <= 52);

    TIMER0_EN = 0;
    TIMER0_EV_PENDING = 1;
    TIMER0_LOAD = (ACLK_HZ / 100u) - 1u;
    TIMER0_RELOAD = (ACLK_HZ / 100u) - 1u;
    TIMER0_EV_ENABLE = 1;
    memory_fence();
    irq_enable(IRQ_TIMER0);
    TIMER0_EN = 1;
    took = sleep_until_alarm(300);
    TIMER0_EN = 0;
    TIMER0_EV_ENABLE = 0;
    mini_printf("C  Timer0 at 100 Hz: woke after %u ms with %u Timer0 interrupts\r\n", took,
                timer0_irqs);
    bad += !(took >= 299 && took <= 302 && timer0_irqs == 0);

    mini_printf(bad ? "RESULT FAIL %d\r\n" : "RESULT PASS\r\n", bad);

    for (;;) {
        __asm__ volatile("wfi");
    }
}
