"""
make_suite.py: write examples/rtos_suite/main.c from the single tests

Copyright (c) 2026 Armstrong Subero
SPDX-License-Identifier: Apache-2.0

Usage:
  python3 third_party/freertos/tests/make_suite.py

rtos_suite is one firmware that runs the tests listed below one after the
other. Each test's main.c is included unchanged, with its file scope names
given the test's name as a prefix so they do not clash. Run this again
after adding or changing a test.
"""

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# name, time limit in ticks, extra defines
TESTS = [
    ("rtos_hello", 3000, {}),
    ("rtos_isr", 3000, {}),
    ("rtos_queue", 3000, {}),
    ("rtos_sync", 4000, {}),
    ("rtos_timing", 7000, {}),
    ("rtos_sched", 3000, {}),
    ("rtos_heap", 3000, {}),
    ("rtos_notify", 3000, {}),
    ("rtos_fromisr", 4000, {}),
    ("rtos_delay", 4000, {}),
    ("rtos_static", 3000, {}),
    ("rtos_qset", 3000, {}),
    ("rtos_stats", 3000, {}),
    ("rtos_hooks", 3000, {}),
    ("rtos_overflow", 3000, {}),
    ("rtos_soak", 25000, {"SOAK_SECONDS": "20"}),
]

SHARED = {"fails", "check", "result", "micros", "test_timer_handler", "test_timer_start",
          "test_timer_stop", "s_test_timer_fn", "s_micros_base", "rtos_stats_micros"}

KEYWORDS = {"static", "volatile", "const", "void", "int", "bool", "char", "unsigned", "signed",
            "long", "short", "struct", "enum", "typedef", "inline", "extern", "noinline", "sizeof"}


def strip(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(\\.|[^"\\])*"', '""', text)
    return text


def top_level(text):
    """The text with every brace block replaced by {}; so only file scope is left."""
    out, depth = [], 0
    for ch in text:
        if ch == "{":
            if depth == 0:
                out.append("{")
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                out.append("}")
        elif depth == 0:
            out.append(ch)
    return "".join(out)


def names(path):
    raw = open(path).read()
    text = strip(raw)
    found, macros = set(), set()
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)", text, re.M):
        macros.add(m.group(1))
    for m in re.finditer(r"enum\s*\w*\s*\{([^}]*)\}\s*(\w*)", text):
        found.update(n.strip().split("=")[0].strip() for n in m.group(1).split(",") if n.strip())
        if m.group(2):
            found.add(m.group(2))
    flat = top_level(re.sub(r"^[ \t]*#.*$", "", text, flags=re.M))
    flat = re.sub(r"enum\s*\w*\s*\{\}\s*\w*\s*;", ";", flat)
    for stmt in re.split(r"[;}]", flat):
        stmt = stmt.strip()
        if not stmt:
            continue
        stmt = re.sub(r"__attribute__\s*\(\(.*?\)\)", " ", stmt)
        stmt = re.sub(r"\[[^\]]*\]", "", stmt)
        if "(" in stmt and not re.search(r"=\s*[^=]*\(", stmt.split("(")[0] + "("):
            m = re.search(r"(\w+)\s*\($", stmt.split("(")[0] + "(")
            if m and m.group(1) not in KEYWORDS:
                found.add(m.group(1))
            continue
        decl = re.sub(r"\[[^\]]*\]", "", stmt)
        decl = re.sub(r"=[^,]*", "", decl)
        parts = decl.replace("{", " ").split(",")
        for i, part in enumerate(parts):
            words = re.findall(r"\w+", part)
            if words and words[-1] not in KEYWORDS and not words[-1][0].isdigit():
                found.add(words[-1])
    return sorted(found - SHARED), sorted(macros)


def main():
    out = []
    out.append("""/*
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
        mini_printf( "\\r\\nSUITE: %u tests, %u failed\\r\\n", suite_index, suite_failed );

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

""")
    for name, limit, defines in TESTS:
        path = os.path.join(SDK, "examples", name, "main.c")
        idents, macros = names(path)
        out.append("/* ---- %s ---- */\n" % name)
        for key, value in defines.items():
            out.append("#define %s %s\n" % (key, value))
        for ident in idents:
            out.append("#define %s %s_%s\n" % (ident, name, ident))
        out.append('#include "../%s/main.c"\n' % name)
        for ident in idents:
            out.append("#undef %s\n" % ident)
        for macro in sorted(set(macros) | set(defines)):
            out.append("#undef %s\n" % macro)
        out.append("\n")
    out.append("static const suite_test_t suite_tests[] =\n{\n")
    for name, limit, defines in TESTS:
        out.append('    { "%s", %s_main, %d },\n' % (name, name, limit))
    out.append("""};

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
        mini_printf( "SUITE: %s ran past its time limit\\r\\n", suite_tests[ suite_index ].name );
        rtos_suite_next( 1 );
    }
}

void vApplicationStackOverflowHook( TaskHandle_t task, char * name )
{
    ( void ) task;
    bool expected = suite_tests[ suite_index ].run == rtos_overflow_main;

    __asm volatile ( "csrci mstatus, 8" );
    mini_printf( "\\r\\nSTACK OVERFLOW in task %s\\r\\n", name );
    mini_printf( expected ? "RESULT PASS\\r\\n" : "RESULT FAIL 1\\r\\n" );
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
    mini_printf( "\\r\\n=== %u of %u: %s ===\\r\\n", suite_index + 1U, suite_count(), suite_tests[ suite_index ].name );

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
""")
    path = os.path.join(SDK, "examples", "rtos_suite", "main.c")
    open(path, "w").write("".join(out))
    print("wrote", path)


if __name__ == "__main__":
    main()
