/*
 * rtos_notify: task notifications
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every notify action, the clear masks, three notification indexes
 * (rtos_config.h), a task above the sender running at once, and 32 bits
 * set one by one from a timer interrupt.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"

static TaskHandle_t self_h, waiter_h, isr_target_h;
static volatile uint32_t waiter_got, waiter_woke;
static volatile uint32_t isr_sent, isr_bits_seen, isr_wakes;

static uint32_t take_value(void)
{
    uint32_t v = 0;

    xTaskNotifyWait(0, 0xFFFFFFFFu, &v, 0);
    return v;
}

static void waiter(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        if (xTaskNotifyWait(0, 0xFFFFFFFFu, &v, portMAX_DELAY) == pdTRUE) {
            waiter_got = v;
            waiter_woke++;
        }
    }
}

static void isr_target(void *arg)
{
    (void)arg;
    uint32_t v;

    for (;;) {
        if (xTaskNotifyWait(0, 0xFFFFFFFFu, &v, portMAX_DELAY) == pdTRUE) {
            isr_bits_seen |= v;
            isr_wakes++;
        }
    }
}

static void isr_notify(void)
{
    if (isr_sent >= 32) {
        return;
    }
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(isr_target_h, 1u << isr_sent, eSetBits, &woken);
    isr_sent++;
    portYIELD_FROM_ISR(woken);
}

static void runner(void *arg)
{
    (void)arg;

    mini_printf("rtos_notify: start\r\n");

    self_h = xTaskGetCurrentTaskHandle();

    xTaskNotify(self_h, 0x1, eSetBits);
    xTaskNotify(self_h, 0x4, eSetBits);
    check("eSetBits ORs bits into the value", take_value() == 0x5);

    for (int i = 0; i < 3; i++) {
        xTaskNotify(self_h, 0, eIncrement);
    }
    check("eIncrement adds one each time", take_value() == 3);

    xTaskNotify(self_h, 7, eSetValueWithOverwrite);
    xTaskNotify(self_h, 9, eSetValueWithOverwrite);
    check("eSetValueWithOverwrite keeps the newest value", take_value() == 9);

    BaseType_t first = xTaskNotify(self_h, 11, eSetValueWithoutOverwrite);
    BaseType_t second = xTaskNotify(self_h, 12, eSetValueWithoutOverwrite);
    check("eSetValueWithoutOverwrite refuses while one is pending",
          first == pdPASS && second == pdFAIL && take_value() == 11);

    xTaskNotify(self_h, 20, eSetValueWithOverwrite);
    take_value();
    xTaskNotify(self_h, 0, eNoAction);
    uint32_t v = 0;
    BaseType_t r = xTaskNotifyWait(0, 0, &v, 0);
    check("eNoAction marks it pending without changing the value", r == pdTRUE && v == 0);

    xTaskNotify(self_h, 0xF0, eSetValueWithOverwrite);
    v = 0;
    xTaskNotifyWait(0, 0x30, &v, 0);
    uint32_t left = 0;
    xTaskNotify(self_h, 0, eNoAction);
    xTaskNotifyWait(0, 0, &left, 0);
    check("the clear on exit mask clears only its bits", v == 0xF0 && left == 0xC0);

    xTaskNotifyWait(0xFFFFFFFFu, 0, &v, 0);
    uint32_t prev = 0;
    xTaskNotify(self_h, 5, eSetValueWithOverwrite);
    xTaskNotifyAndQuery(self_h, 8, eSetValueWithOverwrite, &prev);
    check("xTaskNotifyAndQuery hands back the value before", prev == 5 && take_value() == 8);

    xTaskNotify(self_h, 0xFF, eSetValueWithOverwrite);
    uint32_t old = ulTaskNotifyValueClear(self_h, 0x0F);
    xTaskNotifyStateClear(self_h);
    v = 0;
    r = xTaskNotifyWait(0, 0, &v, 0);
    uint32_t cur = 0;
    xTaskNotify(self_h, 0, eNoAction);
    xTaskNotifyWait(0, 0xFFFFFFFFu, &cur, 0);
    check("ulTaskNotifyValueClear clears bits, xTaskNotifyStateClear clears pending",
          old == 0xFF && r == pdFALSE && cur == 0xF0);

    TickType_t t0 = xTaskGetTickCount();
    r = xTaskNotifyWait(0, 0, &v, 10);
    TickType_t waited = xTaskGetTickCount() - t0;
    check("a wait with nothing sent times out after 10 ticks",
          r == pdFALSE && waited >= 10 && waited <= 12);

    xTaskNotifyGiveIndexed(self_h, 1);
    xTaskNotifyGiveIndexed(self_h, 1);
    xTaskNotifyIndexed(self_h, 2, 0x55, eSetValueWithOverwrite);
    BaseType_t idx0 = xTaskNotifyWaitIndexed(0, 0, 0, &v, 0);
    uint32_t count1 = ulTaskNotifyTakeIndexed(1, pdTRUE, 0);
    uint32_t v2 = 0;
    xTaskNotifyWaitIndexed(2, 0, 0xFFFFFFFFu, &v2, 0);
    check("indexes 1 and 2 are separate and leave index 0 alone",
          idx0 == pdFALSE && count1 == 2 && v2 == 0x55);

    xTaskNotifyGiveIndexed(self_h, 1);
    xTaskNotifyGiveIndexed(self_h, 1);
    xTaskNotifyGiveIndexed(self_h, 1);
    uint32_t a = ulTaskNotifyTakeIndexed(1, pdFALSE, 0);
    uint32_t b = ulTaskNotifyTakeIndexed(1, pdFALSE, 0);
    uint32_t c = ulTaskNotifyTake(pdTRUE, 0);
    ulTaskNotifyTakeIndexed(1, pdTRUE, 0);
    check("ulTaskNotifyTake without clearing counts down by one", a == 3 && b == 2 && c == 0);

    xTaskCreate(waiter, "waiter", 256, NULL, 3, &waiter_h);
    vTaskDelay(2);
    xTaskNotify(waiter_h, 1234, eSetValueWithOverwrite);
    check("a higher priority task waiting runs at once with the value",
          waiter_woke == 1 && waiter_got == 1234);

    xTaskCreate(isr_target, "isr_target", 256, NULL, 3, &isr_target_h);
    vTaskDelay(2);
    test_timer_start(1000, isr_notify);
    vTaskDelay(100);
    test_timer_stop();
    vTaskDelay(2);
    check("32 bits set from an interrupt all arrived",
          isr_sent == 32 && isr_bits_seen == 0xFFFFFFFFu);

    mini_printf("     interrupt notifications: 32 sent, task woke %u times\r\n",
                (uint32_t)isr_wakes);

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
