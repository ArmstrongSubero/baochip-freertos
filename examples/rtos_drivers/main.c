/*
 * rtos_drivers: SDK drivers called from tasks
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two tasks print at once and every line has to come out whole. Then the
 * I2C, SPI and SHA drivers are each called 20 times from a task while a
 * lower priority task spins and a 1 kHz interrupt runs.
 *
 * Wiring:
 *   MPU6050 on PB11 (SCL) and PB12 (SDA)
 *   PC1 (MOSI) jumpered to PC2 (MISO)
 *   PB14 (TX) to the USB serial adapter RX
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/sha.h"

#define LINES   50
#define SPI_LEN 256
#define SPI_DIV 4

static volatile uint32_t talkers_done, timer_irqs, spins;

static void on_timer(void)
{
    timer_irqs++;
}

static void talker(void *arg)
{
    const char *name = (const char *)arg;

    for (int i = 0; i < LINES; i++) {
        mini_printf("talker %s line %02d abcdefghijklmnopqrstuvwxyz\r\n", name, i);
    }
    talkers_done++;
    vTaskDelete(NULL);
}

static void spinner(void *arg)
{
    (void)arg;

    for (;;) {
        spins++;
    }
}

static void runner(void *arg)
{
    (void)arg;
    static uint8_t tx[SPI_LEN], rx[SPI_LEN];
    uint8_t digest[32];
    bool ok;

    mini_printf("rtos_drivers: start\r\n");

    xTaskCreate(talker, "talker_a", 384, "A", 2, NULL);
    xTaskCreate(talker, "talker_b", 384, "B", 2, NULL);
    while (talkers_done < 2) {
        vTaskDelay(10);
    }
    check("two tasks printed 50 lines each", talkers_done == 2);

    xTaskCreate(spinner, "spinner", 256, NULL, 1, NULL);
    test_timer_start(1000, on_timer);

    i2c_init(0, 100000);
    ok = true;
    for (int i = 0; i < 20; i++) {
        uint8_t reg = 0x75, who = 0;
        i2c_write_read_blocking(0, 0x68, &reg, 1, &who, 1);
        ok &= who == 0x68;
        delay_ms(2);
    }
    check("I2C: MPU6050 who am i read 20 times from a task", ok);

    spi_init(2);
    ok = true;
    for (int i = 0; i < 20; i++) {
        for (int k = 0; k < SPI_LEN; k++) {
            tx[k] = (uint8_t)(k * 7 + i);
            rx[k] = 0;
        }
        spi_write_read_blocking(2, tx, rx, SPI_LEN, SPI_DIV);
        for (int k = 0; k < SPI_LEN; k++) {
            ok &= rx[k] == tx[k];
        }
        delay_ms(2);
    }
    check("SPI: 256 byte loopback 20 times from a task", ok);

    sha256_init();
    ok = true;
    for (int i = 0; i < 20; i++) {
        ok &= sha256_hash((const uint8_t *)"abc", 3, digest) == 0;
        ok &= digest[0] == 0xba && digest[1] == 0x78 && digest[2] == 0x16 && digest[3] == 0xbf;
        delay_ms(2);
    }
    check("SHA256 of abc 20 times from a task", ok);

    test_timer_stop();
    mini_printf("     %u timer interrupts, the spinning task ran %u loops\r\n", timer_irqs, spins);
    check("the interrupt and the lower priority task ran meanwhile",
          timer_irqs > 100 && spins > 1000);

    result();
    vTaskSuspend(NULL);
}

int main(void)
{
    bao_init();
    xTaskCreate(runner, "runner", 768, NULL, 3, NULL);
    vTaskStartScheduler();

    for (;;) {
    }
}
