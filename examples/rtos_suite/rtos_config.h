/*
 * rtos_config.h: FreeRTOS settings for rtos_suite
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every optional feature the single tests turn on, all at once, so one
 * image can run them all.
 */

#define RTOS_TEST_SUITE                             1

#define configTOTAL_HEAP_SIZE                       ( ( size_t ) ( 32 * 1024 ) )
#define configTASK_NOTIFICATION_ARRAY_ENTRIES       3
#define configSUPPORT_STATIC_ALLOCATION             1
#define configKERNEL_PROVIDED_STATIC_MEMORY         1
#define configUSE_QUEUE_SETS                        1
#define configUSE_IDLE_HOOK                         1
#define configUSE_TICK_HOOK                         1
#define configUSE_MALLOC_FAILED_HOOK                1
#define configUSE_TRACE_FACILITY                    1
#define configGENERATE_RUN_TIME_STATS               1
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE()            rtos_stats_micros()

#ifndef __ASSEMBLER__
    #include <stdint.h>
    uint32_t rtos_stats_micros( void );
#endif
