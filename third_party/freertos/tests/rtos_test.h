/*
 * rtos_test.h: shared helpers for the FreeRTOS test programs
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * check() and result() print the lines run_rtos.py looks for. micros()
 * reads the CPU cycle counter, which stops while the core sleeps in WFI.
 * test_timer_start() runs a handler from the Timer0 interrupt, the one
 * timer the kernel leaves free; it also stops in WFI.
 *
 * With RTOS_TEST_SUITE defined (examples/rtos_suite), result() hands over
 * to the next test in the suite, which also sets s_micros_base so micros()
 * starts from zero for each test.
 */

#ifndef RTOS_TEST_H
#define RTOS_TEST_H

#include <stdbool.h>
#include "bao.h"
#include "hardware/irq.h"
#include "hardware/regs/timer.h"
#include "FreeRTOS.h"
#include "task.h"

static int fails;
static void ( * s_test_timer_fn )( void );
static uint32_t s_micros_base;

static inline void check( const char * what, bool ok )
{
    mini_printf( "%s %s\r\n", ok ? "ok  " : "FAIL", what );

    if( !ok )
    {
        fails++;
    }
}

#ifdef RTOS_TEST_SUITE
    void rtos_suite_next( int failed );
#endif

static inline void result( void )
{
    if( fails != 0 )
    {
        mini_printf( "RESULT FAIL %d\r\n", fails );
    }
    else
    {
        mini_printf( "RESULT PASS\r\n" );
    }

    #ifdef RTOS_TEST_SUITE
        rtos_suite_next( fails != 0 );
    #endif
}

static inline uint32_t micros( void )
{
    uint32_t hi_a, lo, hi_b;

    do
    {
        __asm volatile ( "csrr %0, mcycleh" : "=r" ( hi_a ) );
        __asm volatile ( "csrr %0, mcycle" : "=r" ( lo ) );
        __asm volatile ( "csrr %0, mcycleh" : "=r" ( hi_b ) );
    } while( hi_a != hi_b );

    return ( uint32_t ) ( ( ( ( uint64_t ) hi_b << 32 ) | lo ) / ( ACLK_HZ / 1000000UL ) ) - s_micros_base;
}

static inline void test_timer_handler( uint32_t pending )
{
    ( void ) pending;
    s_test_timer_fn();
}

static inline void test_timer_start( uint32_t hz, void ( * fn )( void ) )
{
    s_test_timer_fn = fn;
    TIMER0_EN = 0;
    TIMER0_EV_ENABLE = 0;
    TIMER0_EV_PENDING = 1;
    memory_fence();
    TIMER0_LOAD = ( ACLK_HZ / hz ) - 1UL;
    TIMER0_RELOAD = ( ACLK_HZ / hz ) - 1UL;
    irq_set_handler( IRQ_TIMER0, test_timer_handler );
    TIMER0_EV_ENABLE = 1;
    memory_fence();
    irq_enable( IRQ_TIMER0 );
    TIMER0_EN = 1;
}

static inline void test_timer_stop( void )
{
    TIMER0_EN = 0;
    TIMER0_EV_ENABLE = 0;
    TIMER0_EV_PENDING = 1;
    memory_fence();
    irq_disable( IRQ_TIMER0 );
}

#endif
