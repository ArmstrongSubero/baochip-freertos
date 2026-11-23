/*
 * FreeRTOSConfig.h: FreeRTOS kernel configuration for the Baochip-1x
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * The tick comes from the TickTimer alarm, which keeps running while the
 * CPU clock is stopped in WFI. The TickTimer counts milliseconds, so
 * configTICK_RATE_HZ has to divide 1000.
 *
 * An example changes a setting by putting a file named rtos_config.h in
 * its own folder with the #define lines it wants. Everything below is
 * only the value used when that file does not set it.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "hardware/regs/addressmap.h"

#if defined( __has_include )
    #if __has_include( "rtos_config.h" )
        #include "rtos_config.h"
    #endif
#endif

#define configCPU_CLOCK_HZ                          ( ACLK_HZ )
#define configMTIME_BASE_ADDRESS                    ( 0 )
#define configMTIMECMP_BASE_ADDRESS                 ( 0 )
#define configTICK_TYPE_WIDTH_IN_BITS               TICK_TYPE_WIDTH_32_BITS
#define configUSE_PORT_OPTIMISED_TASK_SELECTION     0
#define configUSE_CO_ROUTINES                       0
#define configENABLE_BACKWARD_COMPATIBILITY         0
#define configSUPPORT_DYNAMIC_ALLOCATION            1

#ifndef configTICK_RATE_HZ
    #define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )
#endif
#ifndef configISR_STACK_SIZE_WORDS
    #define configISR_STACK_SIZE_WORDS              ( 1024 )
#endif
#ifndef configUSE_PREEMPTION
    #define configUSE_PREEMPTION                    1
#endif
#ifndef configUSE_TIME_SLICING
    #define configUSE_TIME_SLICING                  1
#endif
#ifndef configUSE_TICKLESS_IDLE
    #define configUSE_TICKLESS_IDLE                 0
#endif
#ifndef configEXPECTED_IDLE_TIME_BEFORE_SLEEP
    #define configEXPECTED_IDLE_TIME_BEFORE_SLEEP   2
#endif
#ifndef configMAX_PRIORITIES
    #define configMAX_PRIORITIES                    ( 15 )
#endif
#ifndef configMINIMAL_STACK_SIZE
    #define configMINIMAL_STACK_SIZE                ( ( uint16_t ) 256 )
#endif
#ifndef configMAX_TASK_NAME_LEN
    #define configMAX_TASK_NAME_LEN                 ( 16 )
#endif
#ifndef configIDLE_SHOULD_YIELD
    #define configIDLE_SHOULD_YIELD                 1
#endif
#ifndef configUSE_TASK_NOTIFICATIONS
    #define configUSE_TASK_NOTIFICATIONS            1
#endif
#ifndef configTASK_NOTIFICATION_ARRAY_ENTRIES
    #define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#endif
#ifndef configUSE_MUTEXES
    #define configUSE_MUTEXES                       1
#endif
#ifndef configUSE_RECURSIVE_MUTEXES
    #define configUSE_RECURSIVE_MUTEXES             1
#endif
#ifndef configUSE_COUNTING_SEMAPHORES
    #define configUSE_COUNTING_SEMAPHORES           1
#endif
#ifndef configQUEUE_REGISTRY_SIZE
    #define configQUEUE_REGISTRY_SIZE               8
#endif
#ifndef configUSE_QUEUE_SETS
    #define configUSE_QUEUE_SETS                    0
#endif
#ifndef configRECORD_STACK_HIGH_ADDRESS
    #define configRECORD_STACK_HIGH_ADDRESS         1
#endif
#ifndef configSUPPORT_STATIC_ALLOCATION
    #define configSUPPORT_STATIC_ALLOCATION         0
#endif
#ifndef configTOTAL_HEAP_SIZE
    #define configTOTAL_HEAP_SIZE                   ( ( size_t ) ( 64 * 1024 ) )
#endif
#ifndef configUSE_IDLE_HOOK
    #define configUSE_IDLE_HOOK                     0
#endif
#ifndef configUSE_TICK_HOOK
    #define configUSE_TICK_HOOK                     0
#endif
#ifndef configUSE_MALLOC_FAILED_HOOK
    #define configUSE_MALLOC_FAILED_HOOK            0
#endif
#ifndef configCHECK_FOR_STACK_OVERFLOW
    #define configCHECK_FOR_STACK_OVERFLOW          2
#endif
#ifndef configGENERATE_RUN_TIME_STATS
    #define configGENERATE_RUN_TIME_STATS           0
#endif
#ifndef configUSE_TRACE_FACILITY
    #define configUSE_TRACE_FACILITY                0
#endif
#ifndef configUSE_STATS_FORMATTING_FUNCTIONS
    #define configUSE_STATS_FORMATTING_FUNCTIONS    0
#endif
#ifndef configUSE_TIMERS
    #define configUSE_TIMERS                        1
#endif
#ifndef configTIMER_TASK_PRIORITY
    #define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#endif
#ifndef configTIMER_QUEUE_LENGTH
    #define configTIMER_QUEUE_LENGTH                4
#endif
#ifndef configTIMER_TASK_STACK_DEPTH
    #define configTIMER_TASK_STACK_DEPTH            ( 512 )
#endif

#define INCLUDE_vTaskPrioritySet                    1
#define INCLUDE_uxTaskPriorityGet                   1
#define INCLUDE_vTaskDelete                         1
#define INCLUDE_vTaskSuspend                        1
#define INCLUDE_xTaskResumeFromISR                  1
#define INCLUDE_vTaskDelayUntil                     1
#define INCLUDE_vTaskDelay                          1
#define INCLUDE_eTaskGetState                       1
#define INCLUDE_xTaskAbortDelay                     1
#define INCLUDE_xTaskGetHandle                      1
#define INCLUDE_xTaskGetCurrentTaskHandle           1
#define INCLUDE_xTaskGetSchedulerState              1
#define INCLUDE_xTaskGetIdleTaskHandle              1
#define INCLUDE_xSemaphoreGetMutexHolder            1
#define INCLUDE_uxTaskGetStackHighWaterMark         1
#define INCLUDE_xTimerPendFunctionCall              1

#ifndef __ASSEMBLER__
    void vPortAssertFailed( const char * pcFile, int lLine );
    void vPortSuppressTicksAndSleep( uint32_t ulExpectedIdleTime );
    #define configASSERT( x )    do { if( ( x ) == 0 ) { vPortAssertFailed( __FILE__, __LINE__ ); } } while( 0 )
#endif

#if ( configUSE_TICKLESS_IDLE == 1 )
    #define portSUPPRESS_TICKS_AND_SLEEP( xExpectedIdleTime )    vPortSuppressTicksAndSleep( xExpectedIdleTime )
#endif

#endif
