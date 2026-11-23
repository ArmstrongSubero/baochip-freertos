#!/bin/bash
# rtos_build.sh: build and flash the FreeRTOS examples against the Dabao SDK
#
# Copyright (c) 2026 Armstrong Subero
# SPDX-License-Identifier: Apache-2.0
#
# Usage:
#   ./rtos_build.sh build <example>             Build only
#   ./rtos_build.sh flash <port> <example>      Flash build/<example>.uf2
#   ./rtos_build.sh run <port> <example>        Build, then flash
#   ./rtos_build.sh list                        List the examples
#
# The Dabao SDK is found in DABAO_SDK, or in a folder named dabao-sdk-main
# beside this one. Its drivers, startup code, linker script and tools are
# used as they are. BAO_OPT sets the optimisation level, -Os by default.
#
# Example folder files:
#   rtos_config.h    overrides settings in FreeRTOSConfig.h
#   bao_tinyusb      adds the SDK's TinyUSB to the build

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
SDK="${DABAO_SDK:-$ROOT/../dabao-sdk-main}"

if [ ! -f "$SDK/bao1x.ld" ]; then
    echo "Dabao SDK not found at $SDK"
    echo "Set DABAO_SDK to the SDK folder."
    exit 1
fi
SDK="$(cd "$SDK" && pwd)"

CROSS="riscv-none-elf-"
for d in "$SDK"/xpack-riscv-none-elf-gcc-* "$ROOT"/xpack-riscv-none-elf-gcc-*; do
    if [ -x "$d/bin/riscv-none-elf-gcc" ]; then
        CROSS="$d/bin/riscv-none-elf-"
    fi
done

ARCH="-march=rv32imac_zicsr_zifencei -mabi=ilp32"
# -fno-tree-loop-distribute-patterns: same reason as in the SDK's bao_flash.
CFLAGS="${BAO_OPT:--Os} -Wall -Wextra -ffreestanding -nostdlib -fno-tree-loop-distribute-patterns -g"
RTOS="$ROOT/third_party/freertos"
TUSB="$SDK/third_party/tinyusb/src"
OUT="$ROOT/build"

do_build() {
    local EXAMPLE="$1"
    local DIR="$ROOT/examples/$EXAMPLE"
    local OBJS=""
    local INC=""
    local MAIN=""
    local SRCS=""
    local f
    local obj

    if [ ! -d "$DIR" ]; then
        echo "Example \"$EXAMPLE\" not found in examples/"
        exit 1
    fi

    for f in "$DIR"/*.c; do
        if [ -f "$f" ]; then
            MAIN="$f"
            break
        fi
    done

    echo "Building $EXAMPLE against $SDK"
    mkdir -p "$OUT/obj"

    python3 "$SDK/tools/audit_addresses.py" "$DIR"
    python3 "$SDK/tools/audit_addresses.py" "$RTOS/portable"

    INC="-I$DIR -I$SDK/src/common/bao_base/include -I$SDK/src/common/bao_stdlib/include"
    INC="$INC -I$SDK/src/boards/include -I$SDK/src/sevs"
    for f in "$SDK"/src/bao1x/hardware_*/include; do
        INC="$INC -I$f"
    done
    INC="$INC -I$RTOS -I$RTOS/kernel/include -I$RTOS/portable"

    SRCS="$(ls "$SDK"/src/bao1x/hardware_*/*.c "$SDK"/src/common/bao_stdlib/*.c "$SDK"/src/sevs/*.c)"
    SRCS="$SRCS $RTOS/kernel/tasks.c $RTOS/kernel/queue.c $RTOS/kernel/list.c $RTOS/kernel/timers.c"
    SRCS="$SRCS $RTOS/kernel/event_groups.c $RTOS/kernel/stream_buffer.c"
    SRCS="$SRCS $RTOS/portable/port.c $RTOS/portable/heap_4.c $RTOS/portable/port_bao1x.c"

    if [ -f "$DIR/bao_tinyusb" ]; then
        INC="$INC -I$TUSB -I$TUSB/portable/corigine/udc"
        SRCS="$SRCS $TUSB/tusb.c $TUSB/common/tusb_fifo.c $TUSB/device/usbd.c $TUSB/device/usbd_control.c"
        SRCS="$SRCS $TUSB/class/cdc/cdc_device.c $TUSB/portable/corigine/udc/dcd_corigine_udc.c"
    fi

    ${CROSS}gcc $ARCH -g -c -o "$OUT/obj/crt0.o" "$SDK/src/runtime/crt0.S"
    ${CROSS}gcc $ARCH -g $INC -c -o "$OUT/obj/portASM.o" "$RTOS/portable/portASM.S"
    OBJS="$OUT/obj/crt0.o $OUT/obj/portASM.o"

    for f in $SRCS; do
        obj="$OUT/obj/$(basename "${f%.c}").o"
        ${CROSS}gcc $ARCH $CFLAGS $INC -c -o "$obj" "$f"
        OBJS="$OBJS $obj"
    done

    ${CROSS}gcc $ARCH $CFLAGS $INC -c -o "$OUT/obj/example_main.o" "$MAIN"

    ${CROSS}gcc $ARCH -T "$SDK/bao1x.ld" -nostdlib -nostartfiles -Wl,--gc-sections \
        -o "$OUT/$EXAMPLE.elf" $OBJS "$OUT/obj/example_main.o" -lgcc

    ${CROSS}objcopy -O binary "$OUT/$EXAMPLE.elf" "$OUT/$EXAMPLE.bin"
    python3 "$SDK/tools/sign_and_uf2.py" "$OUT/$EXAMPLE.bin" "$OUT/$EXAMPLE.uf2" 0x60060000 0xa7d76373

    ${CROSS}size "$OUT/$EXAMPLE.elf"
    echo "BUILD SUCCESS: build/$EXAMPLE.uf2"
}

do_flash() {
    local PORT="$1"
    local EXAMPLE="$2"

    if [ ! -f "$OUT/$EXAMPLE.uf2" ]; then
        echo "build/$EXAMPLE.uf2 not found. Build it first."
        exit 1
    fi

    python3 "$SDK/tools/serial_flash.py" "$PORT" "$OUT/$EXAMPLE.uf2"
}

case "$1" in
    build)
        [ -n "$2" ] || { echo "Usage: ./rtos_build.sh build <example>"; exit 1; }
        do_build "$2"
        ;;
    flash)
        [ -n "$3" ] || { echo "Usage: ./rtos_build.sh flash <port> <example>"; exit 1; }
        do_flash "$2" "$3"
        ;;
    run)
        [ -n "$3" ] || { echo "Usage: ./rtos_build.sh run <port> <example>"; exit 1; }
        do_build "$3"
        do_flash "$2" "$3"
        ;;
    list)
        ls "$ROOT/examples"
        ;;
    *)
        echo "Usage:"
        echo "  ./rtos_build.sh build <example>"
        echo "  ./rtos_build.sh flash <port> <example>"
        echo "  ./rtos_build.sh run <port> <example>"
        echo "  ./rtos_build.sh list"
        exit 1
        ;;
esac
