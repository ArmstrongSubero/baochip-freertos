# FreeRTOS for the Baochip-1x (Dabao)

Armstrong Subero, RV Embedded

FreeRTOS V11.3.3 on the Baochip-1x, running on the Dabao board on top of the [Dabao SDK](https://github.com/ArmstrongSubero/Dabao-SDK). Every program in this folder has been run on Dabao hardware.

The kernel and the RISC-V port layer are the stock FreeRTOS files, unmodified. The chip specific code is one file, `third_party/freertos/portable/port_bao1x.c`.



## What you get

| Feature                                                                                                      | State                                    |
| ------------------------------------------------------------------------------------------------------------ | ---------------------------------------- |
| Tasks, queues, semaphores, mutexes, notifications, event groups, stream and message buffers, software timers | proven on the board                      |
| FromISR calls from SDK interrupt handlers                                                                    | proven on the board                      |
| heap_4, static allocation, queue sets, run time stats, hooks                                                 | proven on the board                      |
| `delay_ms()` that blocks the task and `mini_printf()` that is safe from two tasks                            | proven on the board                      |
| I2C, SPI and SHA from tasks                                                                                  | proven on the board                      |
| USB serial with TinyUSB in FreeRTOS mode                                                                     | enumeration and echo proven on the board |
| BAO_MMU sandbox                                                                                              | not supported                            |

## What you need

* The Dabao SDK, with the files in `sdk_changes` copied over it
* xPack `riscv-none-elf-gcc` 15.2.0, in the SDK folder or on PATH

## Two ways to use it

**As a folder of its own.** Keep this folder beside the SDK, or set `DABAO_SDK` to where the SDK is.

```
rtos_build build rtos_hello
rtos_build run COM24 rtos_hello
```

On Linux, macOS or WSL:

```
DABAO_SDK=/path/to/dabao-sdk-main ./rtos_build.sh build rtos_hello
```

Output goes to `build/` in this folder.

**Inside the SDK.** Copy `third_party/freertos` and the `examples/rtos_*` folders into the SDK. The SDK's own `bao_flash` then builds them:

```
bao_flash run COM24 rtos_hello
```

## The files in sdk_changes

These are whole replacement files for the SDK. Copy them over the SDK folder, keeping the paths.

| File                                                               | Why                                                                                                                   |
| ------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------- |
| `src/common/bao_stdlib/delay.c`, `stdio.c`, `include/bao/stdlib.h` | weak hooks so `delay_ms()` blocks the calling task and `mini_printf()` takes a lock. Without an RTOS they do nothing. |
| `bao_flash.sh`, `bao_flash.bat`                                    | build FreeRTOS examples from inside the SDK                                                                           |
| `baoemu/src/bus.c`, `baoemu/src/cpu_loop.h`                        | the emulator's TickTimer alarm and its end of program check, corrected so RTOS programs run on it                     |
| `baoemu/tests/run_examples.py`                                     | skips the `rtos_*` examples, which have their own runner                                                              |

The port builds and runs against an SDK without these. `delay_ms()` then busy waits inside a task and `mini_printf()` is not safe from two tasks, so `rtos_delay` and `rtos_drivers` will not pass.



## Writing a program

A FreeRTOS program is an example folder with a `main.c`. It calls `bao_init()`, creates its tasks and calls `vTaskStartScheduler()`. `examples/rtos_hello` is the shortest one to copy.

* Do not call `irq_init()`. It turns interrupts on before the scheduler is ready.
* Register interrupt handlers with `irq_set_handler()` as in a bare metal program. They may call the FromISR functions.
* The kernel tick uses the TickTimer alarm. Timer0 is free, but it stops while the core sleeps in WFI.
* `configTICK_RATE_HZ` has to divide 1000.
* A file named `rtos_config.h` in the example folder overrides any setting in `FreeRTOSConfig.h`.
* A file named `bao_tinyusb` in the example folder adds the SDK's TinyUSB to the build.
  
  

## Testing

On the board, one firmware runs the sixteen tests that need no wiring and ends with `SUITE: 16 tests, 0 failed`:

```
rtos_build run COM24 rtos_suite
```

Read the console from PB14 at 115200 baud.



## Licence

The port, the examples and the tests are Apache 2.0. 
