/*
 * rtos_suite: the FreeRTOS tests that need no wiring, in one firmware
 *
 * Written by third_party/freertos/tests/make_suite.py. Do not edit by
 * hand: change a test or the list in that script and run it again.
 *
 * The tests run one after the other. When a test prints its result, or
 * runs past its time limit, the suite notes the outcome in mscratch and
 * jumps back to _start, so the next test begins with clean memory and a
 * fresh kernel. No reset is used and nothing outside SRAM is written.
 * micros() is set back to zero before each test so it starts where it
 * does when the test runs alone.
 * The last line reads SUITE: n tests, n failed.
 *
 * Wiring:
 *   PB14 (TX) to the USB serial adapter RX
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../../third_party/freertos/tests/rtos_test.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "timers.h"
#include "event_groups.h"
#include "stream_buffer.h"
#include "message_buffer.h"
#include "hardware/uart.h"

#define SUITE_MAGIC             0xB1A70000UL

/* rtos_heap counts on the free heap it has when built alone with 20 KB. */
#define SUITE_HEAP_TEST_FREE    17000U

extern void _start( void );

typedef struct
{
    const char * name;
    int ( * run )( void );
    uint32_t limit;
} suite_test_t;

static const suite_test_t suite_tests[];
static uint32_t suite_index, suite_failed;
static volatile uint32_t suite_ticks;

static uint32_t suite_count( void );

void rtos_suite_next( int failed )
{
    __asm volatile ( "csrci mstatus, 8" );
    test_timer_stop();

    suite_failed += ( failed != 0 ) ? 1U : 0U;
    suite_index++;

    if( suite_index >= suite_count() )
    {
        mini_printf( "\r\nSUITE: %u tests, %u failed\r\n", suite_index, suite_failed );

        for( ; ; )
        {
        }
    }

    __asm volatile ( "csrw mscratch, %0" ::"r" ( SUITE_MAGIC | ( suite_failed << 8 ) | suite_index ) );

    /* uart_init() works out the peripheral clock from the divider boot1
     * left in UART2, so the 1 Mbaud divider has to be back before it runs
     * again. */
    uart_init( 2, 1000000 );
    _start();
}

/* ---- rtos_hello ---- */
#define fast rtos_hello_fast
#define fast_runs rtos_hello_fast_runs
#define main rtos_hello_main
#define runner rtos_hello_runner
#define slow rtos_hello_slow
#define slow_runs rtos_hello_slow_runs
#include "../rtos_hello/main.c"
#undef fast
#undef fast_runs
#undef main
#undef runner
#undef slow
#undef slow_runs

/* ---- rtos_isr ---- */
#define compute rtos_isr_compute
#define computing rtos_isr_computing
#define damaged rtos_isr_damaged
#define main rtos_isr_main
#define on_soft rtos_isr_on_soft
#define on_timer rtos_isr_on_timer
#define rounds rtos_isr_rounds
#define runner rtos_isr_runner
#define scramble rtos_isr_scramble
#define seed rtos_isr_seed
#define soft_irqs rtos_isr_soft_irqs
#define soft_sem rtos_isr_soft_sem
#define soft_task rtos_isr_soft_task
#define soft_wakes rtos_isr_soft_wakes
#define timer_irqs rtos_isr_timer_irqs
#define timer_sem rtos_isr_timer_sem
#define timer_task rtos_isr_timer_task
#define timer_wakes rtos_isr_timer_wakes
#include "../rtos_isr/main.c"
#undef compute
#undef computing
#undef damaged
#undef main
#undef on_soft
#undef on_timer
#undef rounds
#undef runner
#undef scramble
#undef seed
#undef soft_irqs
#undef soft_sem
#undef soft_task
#undef soft_wakes
#undef timer_irqs
#undef timer_sem
#undef timer_task
#undef timer_wakes
#undef ROUNDS
#undef SOFT_TRIGGER

/* ---- rtos_queue ---- */
#define consumer rtos_queue_consumer
#define consumer_h rtos_queue_consumer_h
#define consumer_in_time rtos_queue_consumer_in_time
#define got rtos_queue_got
#define got_bad rtos_queue_got_bad
#define isr_full rtos_queue_isr_full
#define isr_next rtos_queue_isr_next
#define isr_send rtos_queue_isr_send
#define isr_sent rtos_queue_isr_sent
#define isrq rtos_queue_isrq
#define mailbox rtos_queue_mailbox
#define main rtos_queue_main
#define q rtos_queue_q
#define runner rtos_queue_runner
#define sent_at_us rtos_queue_sent_at_us
#define spinner rtos_queue_spinner
#define worst_wake_us rtos_queue_worst_wake_us
#include "../rtos_queue/main.c"
#undef consumer
#undef consumer_h
#undef consumer_in_time
#undef got
#undef got_bad
#undef isr_full
#undef isr_next
#undef isr_send
#undef isr_sent
#undef isrq
#undef mailbox
#undef main
#undef q
#undef runner
#undef sent_at_us
#undef spinner
#undef worst_wake_us

/* ---- rtos_sync ---- */
#define BINARY rtos_sync_BINARY
#define BITS rtos_sync_BITS
#define COUNTING rtos_sync_COUNTING
#define IDLE rtos_sync_IDLE
#define Mode rtos_sync_Mode
#define NOTIFY rtos_sync_NOTIFY
#define STREAM rtos_sync_STREAM
#define armed rtos_sync_armed
#define bin rtos_sync_bin
#define counting rtos_sync_counting
#define events rtos_sync_events
#define isr rtos_sync_isr
#define isr_count rtos_sync_isr_count
#define isr_refused rtos_sync_isr_refused
#define low rtos_sync_low
#define low_done rtos_sync_low_done
#define low_h rtos_sync_low_h
#define low_has_mutex rtos_sync_low_has_mutex
#define low_prio_while_held rtos_sync_low_prio_while_held
#define main rtos_sync_main
#define medium rtos_sync_medium
#define medium_runs rtos_sync_medium_runs
#define medium_runs_while_held rtos_sync_medium_runs_while_held
#define messages rtos_sync_messages
#define mode rtos_sync_mode
#define mtx rtos_sync_mtx
#define other rtos_sync_other
#define other_got_rmtx rtos_sync_other_got_rmtx
#define rmtx rtos_sync_rmtx
#define runner rtos_sync_runner
#define runner_h rtos_sync_runner_h
#define stream rtos_sync_stream
#define taken rtos_sync_taken
#define taker rtos_sync_taker
#define taker_h rtos_sync_taker_h
#define waiter_all rtos_sync_waiter_all
#define woke_at rtos_sync_woke_at
#define woke_with rtos_sync_woke_with
#include "../rtos_sync/main.c"
#undef BINARY
#undef BITS
#undef COUNTING
#undef IDLE
#undef Mode
#undef NOTIFY
#undef STREAM
#undef armed
#undef bin
#undef counting
#undef events
#undef isr
#undef isr_count
#undef isr_refused
#undef low
#undef low_done
#undef low_h
#undef low_has_mutex
#undef low_prio_while_held
#undef main
#undef medium
#undef medium_runs
#undef medium_runs_while_held
#undef messages
#undef mode
#undef mtx
#undef other
#undef other_got_rmtx
#undef rmtx
#undef runner
#undef runner_h
#undef stream
#undef taken
#undef taker
#undef taker_h
#undef waiter_all
#undef woke_at
#undef woke_with

/* ---- rtos_timing ---- */
#define bad_wakes rtos_timing_bad_wakes
#define main rtos_timing_main
#define one_shot_cb rtos_timing_one_shot_cb
#define one_shot_runs rtos_timing_one_shot_runs
#define periodic rtos_timing_periodic
#define reload_bad_gap rtos_timing_reload_bad_gap
#define reload_cb rtos_timing_reload_cb
#define reload_first_us rtos_timing_reload_first_us
#define reload_last_tick rtos_timing_reload_last_tick
#define reload_last_us rtos_timing_reload_last_us
#define reload_runs rtos_timing_reload_runs
#define runner rtos_timing_runner
#define spin_a rtos_timing_spin_a
#define spin_a_task rtos_timing_spin_a_task
#define spin_b rtos_timing_spin_b
#define spin_b_task rtos_timing_spin_b_task
#define worst_late_us rtos_timing_worst_late_us
#include "../rtos_timing/main.c"
#undef bad_wakes
#undef main
#undef one_shot_cb
#undef one_shot_runs
#undef periodic
#undef reload_bad_gap
#undef reload_cb
#undef reload_first_us
#undef reload_last_tick
#undef reload_last_us
#undef reload_runs
#undef runner
#undef spin_a
#undef spin_a_task
#undef spin_b
#undef spin_b_task
#undef worst_late_us

/* ---- rtos_sched ---- */
#define gate rtos_sched_gate
#define gate_task rtos_sched_gate_task
#define gate_task_ran rtos_sched_gate_task_ran
#define isr rtos_sched_isr
#define isr_ticks rtos_sched_isr_ticks
#define main rtos_sched_main
#define quitter rtos_sched_quitter
#define raised rtos_sched_raised
#define raised_ran rtos_sched_raised_ran
#define resume_from_isr rtos_sched_resume_from_isr
#define runner rtos_sched_runner
#define sleeper rtos_sched_sleeper
#define sleeper_h rtos_sched_sleeper_h
#define slept rtos_sched_slept
#define woke_early rtos_sched_woke_early
#define worker rtos_sched_worker
#define worker_count rtos_sched_worker_count
#define worker_h rtos_sched_worker_h
#include "../rtos_sched/main.c"
#undef gate
#undef gate_task
#undef gate_task_ran
#undef isr
#undef isr_ticks
#undef main
#undef quitter
#undef raised
#undef raised_ran
#undef resume_from_isr
#undef runner
#undef sleeper
#undef sleeper_h
#undef slept
#undef woke_early
#undef worker
#undef worker_count
#undef worker_h

/* ---- rtos_heap ---- */
#define idle_task rtos_heap_idle_task
#define main rtos_heap_main
#define runner rtos_heap_runner
#include "../rtos_heap/main.c"
#undef idle_task
#undef main
#undef runner

/* ---- rtos_notify ---- */
#define isr_bits_seen rtos_notify_isr_bits_seen
#define isr_notify rtos_notify_isr_notify
#define isr_sent rtos_notify_isr_sent
#define isr_target rtos_notify_isr_target
#define isr_target_h rtos_notify_isr_target_h
#define isr_wakes rtos_notify_isr_wakes
#define main rtos_notify_main
#define runner rtos_notify_runner
#define self_h rtos_notify_self_h
#define take_value rtos_notify_take_value
#define waiter rtos_notify_waiter
#define waiter_got rtos_notify_waiter_got
#define waiter_h rtos_notify_waiter_h
#define waiter_woke rtos_notify_waiter_woke
#include "../rtos_notify/main.c"
#undef isr_bits_seen
#undef isr_notify
#undef isr_sent
#undef isr_target
#undef isr_target_h
#undef isr_wakes
#undef main
#undef runner
#undef self_h
#undef take_value
#undef waiter
#undef waiter_got
#undef waiter_h
#undef waiter_woke

/* ---- rtos_fromisr ---- */
#define box rtos_fromisr_box
#define empty_after rtos_fromisr_empty_after
#define empty_was rtos_fromisr_empty_was
#define fired rtos_fromisr_fired
#define fired_tick rtos_fromisr_fired_tick
#define full_was rtos_fromisr_full_was
#define got rtos_fromisr_got
#define isr rtos_fromisr_isr
#define main rtos_fromisr_main
#define mie_after rtos_fromisr_mie_after
#define mie_before rtos_fromisr_mie_before
#define mie_inside rtos_fromisr_mie_inside
#define mstatus rtos_fromisr_mstatus
#define one_shot rtos_fromisr_one_shot
#define overwrite_ok rtos_fromisr_overwrite_ok
#define peek_ok rtos_fromisr_peek_ok
#define q rtos_fromisr_q
#define run_stage rtos_fromisr_run_stage
#define runner rtos_fromisr_runner
#define sem rtos_fromisr_sem
#define stage rtos_fromisr_stage
#define stage_done rtos_fromisr_stage_done
#define stream rtos_fromisr_stream
#define stream_buf rtos_fromisr_stream_buf
#define stream_n rtos_fromisr_stream_n
#define take1 rtos_fromisr_take1
#define take2 rtos_fromisr_take2
#define tick_inside rtos_fromisr_tick_inside
#define timer_cb rtos_fromisr_timer_cb
#define waiting_was_3 rtos_fromisr_waiting_was_3
#include "../rtos_fromisr/main.c"
#undef box
#undef empty_after
#undef empty_was
#undef fired
#undef fired_tick
#undef full_was
#undef got
#undef isr
#undef main
#undef mie_after
#undef mie_before
#undef mie_inside
#undef mstatus
#undef one_shot
#undef overwrite_ok
#undef peek_ok
#undef q
#undef run_stage
#undef runner
#undef sem
#undef stage
#undef stage_done
#undef stream
#undef stream_buf
#undef stream_n
#undef take1
#undef take2
#undef tick_inside
#undef timer_cb
#undef waiting_was_3

/* ---- rtos_delay ---- */
#define high rtos_delay_high
#define low rtos_delay_low
#define low_count rtos_delay_low_count
#define main rtos_delay_main
#include "../rtos_delay/main.c"
#undef high
#undef low
#undef low_count
#undef main

/* ---- rtos_static ---- */
#define bin_struct rtos_static_bin_struct
#define cnt_struct rtos_static_cnt_struct
#define eg_struct rtos_static_eg_struct
#define main rtos_static_main
#define mb_storage rtos_static_mb_storage
#define mb_struct rtos_static_mb_struct
#define mtx_struct rtos_static_mtx_struct
#define q rtos_static_q
#define q_storage rtos_static_q_storage
#define q_struct rtos_static_q_struct
#define runner rtos_static_runner
#define sb_storage rtos_static_sb_storage
#define sb_struct rtos_static_sb_struct
#define tick_cb rtos_static_tick_cb
#define timer_runs rtos_static_timer_runs
#define tmr_struct rtos_static_tmr_struct
#define worker rtos_static_worker
#define worker_runs rtos_static_worker_runs
#define worker_stack rtos_static_worker_stack
#define worker_tcb rtos_static_worker_tcb
#include "../rtos_static/main.c"
#undef bin_struct
#undef cnt_struct
#undef eg_struct
#undef main
#undef mb_storage
#undef mb_struct
#undef mtx_struct
#undef q
#undef q_storage
#undef q_struct
#undef runner
#undef sb_storage
#undef sb_struct
#undef tick_cb
#undef timer_runs
#undef tmr_struct
#undef worker
#undef worker_runs
#undef worker_stack
#undef worker_tcb

/* ---- rtos_qset ---- */
#define bad rtos_qset_bad
#define from_a rtos_qset_from_a
#define from_b rtos_qset_from_b
#define from_sem rtos_qset_from_sem
#define isr_send rtos_qset_isr_send
#define isr_sent rtos_qset_isr_sent
#define main rtos_qset_main
#define qa rtos_qset_qa
#define qb rtos_qset_qb
#define qc rtos_qset_qc
#define reader rtos_qset_reader
#define runner rtos_qset_runner
#define sem rtos_qset_sem
#define sent_tick rtos_qset_sent_tick
#define set rtos_qset_set
#define woke_ticks_worst rtos_qset_woke_ticks_worst
#include "../rtos_qset/main.c"
#undef bad
#undef from_a
#undef from_b
#undef from_sem
#undef isr_send
#undef isr_sent
#undef main
#undef qa
#undef qb
#undef qc
#undef reader
#undef runner
#undef sem
#undef sent_tick
#undef set
#undef woke_ticks_worst

/* ---- rtos_stats ---- */
#define busy rtos_stats_busy
#define main rtos_stats_main
#define runner rtos_stats_runner
#define same rtos_stats_same
#define sleeper rtos_stats_sleeper
#define start_us rtos_stats_start_us
#define status rtos_stats_status
#include "../rtos_stats/main.c"
#undef busy
#undef main
#undef runner
#undef same
#undef sleeper
#undef start_us
#undef status

/* ---- rtos_hooks ---- */
#define idle_calls rtos_hooks_idle_calls
#define main rtos_hooks_main
#define malloc_fails rtos_hooks_malloc_fails
#define runner rtos_hooks_runner
#define spinner rtos_hooks_spinner
#define tick_calls rtos_hooks_tick_calls
#define vApplicationIdleHook rtos_hooks_vApplicationIdleHook
#define vApplicationMallocFailedHook rtos_hooks_vApplicationMallocFailedHook
#define vApplicationTickHook rtos_hooks_vApplicationTickHook
#include "../rtos_hooks/main.c"
#undef idle_calls
#undef main
#undef malloc_fails
#undef runner
#undef spinner
#undef tick_calls
#undef vApplicationIdleHook
#undef vApplicationMallocFailedHook
#undef vApplicationTickHook

/* ---- rtos_overflow ---- */
#define deep rtos_overflow_deep
#define greedy rtos_overflow_greedy
#define main rtos_overflow_main
#include "../rtos_overflow/main.c"
#undef deep
#undef greedy
#undef main

/* ---- rtos_soak ---- */
#define SOAK_SECONDS 20
#define added rtos_soak_added
#define adder rtos_soak_adder
#define consumed rtos_soak_consumed
#define consumer rtos_soak_consumer
#define float_bad rtos_soak_float_bad
#define float_loops rtos_soak_float_loops
#define float_task rtos_soak_float_task
#define isr rtos_soak_isr
#define isr_runs rtos_soak_isr_runs
#define isr_sem rtos_soak_isr_sem
#define main rtos_soak_main
#define note_task rtos_soak_note_task
#define notes_taken rtos_soak_notes_taken
#define notified_h rtos_soak_notified_h
#define ping rtos_soak_ping
#define pinger rtos_soak_pinger
#define pings rtos_soak_pings
#define ponger rtos_soak_ponger
#define pongs rtos_soak_pongs
#define produced rtos_soak_produced
#define producer rtos_soak_producer
#define q rtos_soak_q
#define queue_bad rtos_soak_queue_bad
#define runner rtos_soak_runner
#define sem_taken rtos_soak_sem_taken
#define sem_task rtos_soak_sem_task
#define series rtos_soak_series
#define shared rtos_soak_shared
#define shared_mtx rtos_soak_shared_mtx
#define stop rtos_soak_stop
#define stream rtos_soak_stream
#define stream_bad rtos_soak_stream_bad
#define stream_got rtos_soak_stream_got
#define stream_reader rtos_soak_stream_reader
#define stream_sent rtos_soak_stream_sent
#define stream_writer rtos_soak_stream_writer
#define timer_cb rtos_soak_timer_cb
#define timer_runs rtos_soak_timer_runs
#include "../rtos_soak/main.c"
#undef added
#undef adder
#undef consumed
#undef consumer
#undef float_bad
#undef float_loops
#undef float_task
#undef isr
#undef isr_runs
#undef isr_sem
#undef main
#undef note_task
#undef notes_taken
#undef notified_h
#undef ping
#undef pinger
#undef pings
#undef ponger
#undef pongs
#undef produced
#undef producer
#undef q
#undef queue_bad
#undef runner
#undef sem_taken
#undef sem_task
#undef series
#undef shared
#undef shared_mtx
#undef stop
#undef stream
#undef stream_bad
#undef stream_got
#undef stream_reader
#undef stream_sent
#undef stream_writer
#undef timer_cb
#undef timer_runs
#undef SOAK_SECONDS

static const suite_test_t suite_tests[] =
{
    { "rtos_hello", rtos_hello_main, 3000 },
    { "rtos_isr", rtos_isr_main, 3000 },
    { "rtos_queue", rtos_queue_main, 3000 },
    { "rtos_sync", rtos_sync_main, 4000 },
    { "rtos_timing", rtos_timing_main, 7000 },
    { "rtos_sched", rtos_sched_main, 3000 },
    { "rtos_heap", rtos_heap_main, 3000 },
    { "rtos_notify", rtos_notify_main, 3000 },
    { "rtos_fromisr", rtos_fromisr_main, 4000 },
    { "rtos_delay", rtos_delay_main, 4000 },
    { "rtos_static", rtos_static_main, 3000 },
    { "rtos_qset", rtos_qset_main, 3000 },
    { "rtos_stats", rtos_stats_main, 3000 },
    { "rtos_hooks", rtos_hooks_main, 3000 },
    { "rtos_overflow", rtos_overflow_main, 3000 },
    { "rtos_soak", rtos_soak_main, 25000 },
};

static uint32_t suite_count( void )
{
    return sizeof( suite_tests ) / sizeof( suite_tests[ 0 ] );
}

void vApplicationIdleHook( void )
{
    rtos_hooks_vApplicationIdleHook();
}

void vApplicationMallocFailedHook( void )
{
    rtos_hooks_vApplicationMallocFailedHook();
}

void vApplicationTickHook( void )
{
    rtos_hooks_vApplicationTickHook();

    if( ++suite_ticks > suite_tests[ suite_index ].limit )
    {
        mini_printf( "SUITE: %s ran past its time limit\r\n", suite_tests[ suite_index ].name );
        rtos_suite_next( 1 );
    }
}

void vApplicationStackOverflowHook( TaskHandle_t task, char * name )
{
    ( void ) task;
    bool expected = suite_tests[ suite_index ].run == rtos_overflow_main;

    __asm volatile ( "csrci mstatus, 8" );
    mini_printf( "\r\nSTACK OVERFLOW in task %s\r\n", name );
    mini_printf( expected ? "RESULT PASS\r\n" : "RESULT FAIL 1\r\n" );
    rtos_suite_next( !expected );
}

int main( void )
{
    uint32_t word;

    __asm volatile ( "csrr %0, mscratch" : "=r" ( word ) );

    if( ( ( word & 0xFFFF0000UL ) == SUITE_MAGIC ) && ( ( word & 0xFFU ) < suite_count() ) )
    {
        suite_index = word & 0xFFU;
        suite_failed = ( word >> 8 ) & 0xFFU;
    }

    __asm volatile ( "csrw mscratch, zero" );
    bao_init();
    mini_printf( "\r\n=== %u of %u: %s ===\r\n", suite_index + 1U, suite_count(), suite_tests[ suite_index ].name );

    if( suite_tests[ suite_index ].run == rtos_heap_main )
    {
        vPortFree( pvPortMalloc( 8 ) );
        ( void ) pvPortMalloc( xPortGetFreeHeapSize() - SUITE_HEAP_TEST_FREE );
    }

    s_micros_base = micros();
    suite_tests[ suite_index ].run();

    for( ; ; )
    {
    }
}
