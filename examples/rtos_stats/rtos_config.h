/*
 * rtos_config.h: FreeRTOS settings for rtos_stats
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * The run time counter is the CPU cycle counter in microseconds.
 * configUSE_STATS_FORMATTING_FUNCTIONS stays 0: vTaskListTasks and
 * vTaskGetRunTimeStatistics need snprintf, which the SDK does not have.
 */

#define configUSE_TRACE_FACILITY                    1
#define configGENERATE_RUN_TIME_STATS               1
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE()            rtos_stats_micros()

#ifndef __ASSEMBLER__
    #include <stdint.h>
    uint32_t rtos_stats_micros( void );
#endif
