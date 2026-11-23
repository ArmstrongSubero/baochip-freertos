/*
 * port_bao1x.c: Baochip-1x part of the FreeRTOS RISC-V port
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * The stock FreeRTOS RISC-V port (port.c, portASM.S) does the context
 * switch. This file supplies what the chip needs on top of it:
 *
 *   the tick, on the TickTimer alarm
 *   the route from the FreeRTOS trap handler into the SDK's dispatcher
 *   the trap, assert and stack overflow reports
 *   delay_ms() and mini_printf() that behave inside tasks
 *   tickless idle
 *
 * The TickTimer alarm is a level: it stays raised while its target is at
 * or before the current time, so a tick that could not be taken on time
 * is taken as soon as interrupts come back on and none are lost. Timer0
 * is left free for the application. It counts on the CPU clock, which
 * stops in WFI, so it cannot keep time across a sleep.
 *
 * Interrupt handlers are registered with irq_set_handler() exactly as in
 * a bare metal program. Do not call irq_init() in an RTOS program: it
 * turns interrupts on before the scheduler is ready.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "bao/stdlib.h"
#include "hardware/irq.h"
#include "hardware/regs/timer.h"

_Static_assert( ( 1000 % configTICK_RATE_HZ ) == 0,
                "configTICK_RATE_HZ must divide 1000. The tick comes from the millisecond TickTimer." );

#define portTICK_MS    ( 1000U / ( uint32_t ) configTICK_RATE_HZ )

extern void freertos_risc_v_trap_handler( void );
extern void trap_dispatch( void );

static uint64_t s_next_tick_ms;

#if ( configUSE_MUTEXES == 1 )
    static SemaphoreHandle_t s_stdio_mutex;
#endif

/* xTaskGetHandle() needs strlen and the SDK's stdlib has none. */
__attribute__( ( weak ) ) size_t strlen( const char * pcString )
{
    size_t xLength = 0;

    while( pcString[ xLength ] != 0 )
    {
        xLength++;
    }

    return xLength;
}

static void prvHalt( void )
{
    __asm volatile ( "csrci mstatus, 8" );

    for( volatile int s_halt = 0; s_halt == 0; s_halt = 0 )
    {
    }
}

static int prvTaskContext( void )
{
    uint32_t ulStatus;

    __asm volatile ( "csrr %0, mstatus" : "=r" ( ulStatus ) );

    return ( xTaskGetSchedulerState() == taskSCHEDULER_RUNNING ) && ( ( ulStatus & 0x8U ) != 0U );
}

static void prvAlarmAt( uint64_t ullMs )
{
    TICKTIMER_EV_ENABLE = 0;
    TICKTIMER_MSLEEP_TARGET1 = ( uint32_t ) ( ullMs >> 32 );
    TICKTIMER_MSLEEP_TARGET0 = ( uint32_t ) ullMs;
    TICKTIMER_EV_PENDING = 1;
    memory_fence();
    TICKTIMER_EV_ENABLE = 1;
}

static void prvTickHandler( uint32_t ulPending )
{
    ( void ) ulPending;

    s_next_tick_ms += portTICK_MS;
    prvAlarmAt( s_next_tick_ms );

    if( xTaskIncrementTick() != pdFALSE )
    {
        vTaskSwitchContext();
    }
}

void vPortSetupTimerInterrupt( void )
{
    #if ( configUSE_MUTEXES == 1 )
        s_stdio_mutex = xSemaphoreCreateMutex();
    #endif

    irq_set_handler( IRQ_TICKTIMER, prvTickHandler );
    __asm volatile ( "csrw mtvec, %0" ::"r" ( freertos_risc_v_trap_handler ) );

    s_next_tick_ms = millis() + portTICK_MS;
    prvAlarmAt( s_next_tick_ms );

    irq_enable( IRQ_TICKTIMER );
    __asm volatile ( "csrs mie, %0" ::"r" ( 0x800 ) );
}

void freertos_risc_v_application_interrupt_handler( void )
{
    trap_dispatch();
}

void freertos_risc_v_application_exception_handler( void )
{
    uint32_t ulCause, ulEpc, ulTval;

    __asm volatile ( "csrr %0, mcause" : "=r" ( ulCause ) );
    __asm volatile ( "csrr %0, mepc" : "=r" ( ulEpc ) );
    __asm volatile ( "csrr %0, mtval" : "=r" ( ulTval ) );

    mini_printf( "\r\n*** TRAP *** mcause=%08x mepc=%08x mtval=%08x task=%s\r\n",
                 ulCause, ulEpc, ulTval, pcTaskGetName( NULL ) );
    prvHalt();
}

void vPortAssertFailed( const char * pcFile,
                        int lLine )
{
    __asm volatile ( "csrci mstatus, 8" );
    mini_printf( "\r\nASSERT %s:%d\r\n", pcFile, lLine );
    prvHalt();
}

__attribute__( ( weak ) ) void vApplicationStackOverflowHook( TaskHandle_t xTask,
                                                              char * pcTaskName )
{
    ( void ) xTask;

    __asm volatile ( "csrci mstatus, 8" );
    mini_printf( "\r\nSTACK OVERFLOW in task %s\r\n", pcTaskName );
    prvHalt();
}

/* Called by delay_ms(). In a task it blocks so other tasks run, with one
 * extra tick so the wait is never shorter than asked. Anywhere else it
 * returns 0 and delay_ms() busy waits as it does without the RTOS. */
int bao_rtos_delay_ms( uint32_t ulMs )
{
    if( ( ulMs == 0U ) || ( prvTaskContext() == 0 ) )
    {
        return 0;
    }

    vTaskDelay( ( TickType_t ) ( ( ulMs + portTICK_MS - 1U ) / portTICK_MS ) + 1U );
    return 1;
}

/* Called by mini_printf() around each call, so lines from two tasks do
 * not mix and the UART driver is never entered twice. */
void bao_rtos_stdio_lock( void )
{
    #if ( configUSE_MUTEXES == 1 )
        if( ( s_stdio_mutex != NULL ) && ( prvTaskContext() != 0 ) )
        {
            ( void ) xSemaphoreTake( s_stdio_mutex, portMAX_DELAY );
        }
    #endif
}

void bao_rtos_stdio_unlock( void )
{
    #if ( configUSE_MUTEXES == 1 )
        if( ( s_stdio_mutex != NULL ) && ( prvTaskContext() != 0 ) &&
            ( xSemaphoreGetMutexHolder( s_stdio_mutex ) == xTaskGetCurrentTaskHandle() ) )
        {
            ( void ) xSemaphoreGive( s_stdio_mutex );
        }
    #endif
}

#if ( configUSE_TICKLESS_IDLE == 1 )

/* The alarm is moved out to the tick the kernel next needs, the core
 * sleeps in WFI, and on waking the kernel is stepped by the whole ticks
 * the TickTimer says went by. After a full sleep the alarm is left
 * pending so its interrupt counts the last tick. */
    void vPortSuppressTicksAndSleep( uint32_t ulExpectedIdleTime )
    {
        uint64_t ullLast, ullTarget, ullNow;
        uint32_t ulDone;

        __asm volatile ( "csrci mstatus, 8" );

        if( eTaskConfirmSleepModeStatus() == eAbortSleep )
        {
            __asm volatile ( "csrsi mstatus, 8" );
            return;
        }

        ullLast = s_next_tick_ms - portTICK_MS;
        ullTarget = ullLast + ( ( uint64_t ) ulExpectedIdleTime * portTICK_MS );
        prvAlarmAt( ullTarget );

        configPRE_SLEEP_PROCESSING( ulExpectedIdleTime );
        __asm volatile ( "wfi" );
        configPOST_SLEEP_PROCESSING( ulExpectedIdleTime );

        ullNow = millis();

        if( ullNow >= ullTarget )
        {
            ulDone = ulExpectedIdleTime - 1U;
            s_next_tick_ms = ullTarget;
        }
        else
        {
            ulDone = ( uint32_t ) ( ( ullNow - ullLast ) / portTICK_MS );
            s_next_tick_ms = ullLast + ( ( uint64_t ) ( ulDone + 1U ) * portTICK_MS );
            prvAlarmAt( s_next_tick_ms );
        }

        vTaskStepTick( ( TickType_t ) ulDone );
        __asm volatile ( "csrsi mstatus, 8" );
    }

#endif /* configUSE_TICKLESS_IDLE */
