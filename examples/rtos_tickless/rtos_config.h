/*
 * rtos_config.h: FreeRTOS settings for rtos_tickless
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 */

#define configUSE_TICKLESS_IDLE                1
#define configUSE_TICK_HOOK                    1
#define configPRE_SLEEP_PROCESSING( x )        rtos_test_pre_sleep( x )

#ifndef __ASSEMBLER__
    #include <stdint.h>
    void rtos_test_pre_sleep( uint32_t expected );
#endif
