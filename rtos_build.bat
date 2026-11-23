@echo off
REM rtos_build.bat: build and flash the FreeRTOS examples against the Dabao SDK
REM
REM Copyright (c) 2026 Armstrong Subero
REM SPDX-License-Identifier: Apache-2.0
REM
REM Usage:
REM   rtos_build build <example>             Build only
REM   rtos_build flash <port> <example>      Flash build\<example>.uf2
REM   rtos_build run <port> <example>        Build, then flash
REM   rtos_build list                        List the examples
REM
REM The Dabao SDK is found in DABAO_SDK, or in a folder named dabao-sdk-main
REM beside this one. Its drivers, startup code, linker script and tools are
REM used as they are. BAO_OPT sets the optimisation level, -Os by default.
REM
REM Example folder files:
REM   rtos_config.h    overrides settings in FreeRTOSConfig.h
REM   bao_tinyusb      adds the SDK's TinyUSB to the build

setlocal enabledelayedexpansion

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

if defined DABAO_SDK (
    set "SDK=%DABAO_SDK%"
) else (
    set "SDK=%ROOT%\..\dabao-sdk-main"
)

if not exist "%SDK%\bao1x.ld" (
    echo Dabao SDK not found at %SDK%
    echo Set DABAO_SDK to the SDK folder.
    goto :end
)

set "CMD=%~1"
if /I "%CMD%"=="list" goto :cmd_list
if /I "%CMD%"=="build" goto :cmd_build
if /I "%CMD%"=="flash" goto :cmd_flash
if /I "%CMD%"=="run" goto :cmd_run

echo Usage:
echo   rtos_build build ^<example^>
echo   rtos_build flash ^<port^> ^<example^>
echo   rtos_build run ^<port^> ^<example^>
echo   rtos_build list
goto :end

REM ================================================================
:cmd_list
for /D %%d in ("%ROOT%\examples\*") do echo   %%~nd
goto :end

REM ================================================================
:cmd_build
if "%~2"=="" (
    echo Usage: rtos_build build ^<example^>
    goto :end
)
set "EXAMPLE=%~2"
set "AFTER=end"
goto :do_build

REM ================================================================
:cmd_flash
if "%~3"=="" (
    echo Usage: rtos_build flash ^<port^> ^<example^>
    goto :end
)
set "PORT=%~2"
set "EXAMPLE=%~3"
goto :do_flash

REM ================================================================
:cmd_run
if "%~3"=="" (
    echo Usage: rtos_build run ^<port^> ^<example^>
    goto :end
)
set "PORT=%~2"
set "EXAMPLE=%~3"
set "AFTER=do_flash"
goto :do_build

REM ================================================================
:do_build
set "DIR=%ROOT%\examples\%EXAMPLE%"
set "RTOS=%ROOT%\third_party\freertos"
set "TUSB=%SDK%\third_party\tinyusb\src"
set "OUT=%ROOT%\build"

if not exist "%DIR%\" (
    echo Example "%EXAMPLE%" not found in examples\
    goto :end
)

set "CROSS="
for /D %%d in ("%SDK%\xpack-riscv-none-elf-gcc-*" "%ROOT%\xpack-riscv-none-elf-gcc-*") do (
    if exist "%%~fd\bin\riscv-none-elf-gcc.exe" set "CROSS=%%~fd\bin\riscv-none-elf-"
)
if not defined CROSS (
    where riscv-none-elf-gcc.exe >nul 2>nul
    if errorlevel 1 (
        echo RISC-V toolchain not found.
        echo Put xpack-riscv-none-elf-gcc-15.2.0-1 in the SDK folder or add it to PATH.
        goto :fail
    )
    set "CROSS=riscv-none-elf-"
)

if not defined BAO_OPT set "BAO_OPT=-Os"
set "ARCH=-march=rv32imac_zicsr_zifencei -mabi=ilp32"
REM -fno-tree-loop-distribute-patterns: same reason as in the SDK's bao_flash.
set "CFLAGS=!BAO_OPT! -Wall -Wextra -ffreestanding -nostdlib -fno-tree-loop-distribute-patterns -g"

echo Building %EXAMPLE% against %SDK%
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

python "%SDK%\tools\audit_addresses.py" "%DIR%"
if errorlevel 1 goto :fail
python "%SDK%\tools\audit_addresses.py" "%RTOS%\portable"
if errorlevel 1 goto :fail

set INC=-I"%DIR%" -I"%SDK%\src\common\bao_base\include" -I"%SDK%\src\common\bao_stdlib\include"
set INC=!INC! -I"%SDK%\src\boards\include" -I"%SDK%\src\sevs"
for /D %%d in ("%SDK%\src\bao1x\hardware_*") do (
    if exist "%%~fd\include\" set INC=!INC! -I"%%~fd\include"
)
set INC=!INC! -I"%RTOS%" -I"%RTOS%\kernel\include" -I"%RTOS%\portable"
if exist "%DIR%\bao_tinyusb" set INC=!INC! -I"%TUSB%" -I"%TUSB%\portable\corigine\udc"

"!CROSS!gcc" !ARCH! -g -c -o "%OUT%\obj\crt0.o" "%SDK%\src\runtime\crt0.S"
if errorlevel 1 goto :fail
"!CROSS!gcc" !ARCH! -g !INC! -c -o "%OUT%\obj\portASM.o" "%RTOS%\portable\portASM.S"
if errorlevel 1 goto :fail
set OBJS="%OUT%\obj\crt0.o" "%OUT%\obj\portASM.o"

for /D %%d in ("%SDK%\src\bao1x\hardware_*") do (
    for %%f in ("%%~fd\*.c") do (
        call :cc "%%~ff"
        if errorlevel 1 goto :fail
    )
)
for %%f in ("%SDK%\src\common\bao_stdlib\*.c" "%SDK%\src\sevs\*.c") do (
    call :cc "%%~ff"
    if errorlevel 1 goto :fail
)
for %%f in (tasks queue list timers event_groups stream_buffer) do (
    call :cc "%RTOS%\kernel\%%f.c"
    if errorlevel 1 goto :fail
)
for %%f in (port heap_4 port_bao1x) do (
    call :cc "%RTOS%\portable\%%f.c"
    if errorlevel 1 goto :fail
)
if exist "%DIR%\bao_tinyusb" (
    for %%f in (tusb.c common\tusb_fifo.c device\usbd.c device\usbd_control.c class\cdc\cdc_device.c portable\corigine\udc\dcd_corigine_udc.c) do (
        call :cc "%TUSB%\%%f"
        if errorlevel 1 goto :fail
    )
)

set "MAIN="
for %%f in ("%DIR%\*.c") do (
    if not defined MAIN set "MAIN=%%~ff"
)
if not defined MAIN (
    echo No .c file found in %DIR%
    goto :fail
)

"!CROSS!gcc" !ARCH! !CFLAGS! !INC! -c -o "%OUT%\obj\example_main.o" "!MAIN!"
if errorlevel 1 goto :fail

"!CROSS!gcc" !ARCH! -T "%SDK%\bao1x.ld" -nostdlib -nostartfiles -Wl,--gc-sections -o "%OUT%\%EXAMPLE%.elf" !OBJS! "%OUT%\obj\example_main.o" -lgcc
if errorlevel 1 goto :fail

"!CROSS!objcopy" -O binary "%OUT%\%EXAMPLE%.elf" "%OUT%\%EXAMPLE%.bin"
if errorlevel 1 goto :fail

python "%SDK%\tools\sign_and_uf2.py" "%OUT%\%EXAMPLE%.bin" "%OUT%\%EXAMPLE%.uf2" 0x60060000 0xa7d76373
if errorlevel 1 goto :fail

"!CROSS!size" "%OUT%\%EXAMPLE%.elf"
echo BUILD SUCCESS: build\%EXAMPLE%.uf2
goto :%AFTER%

REM ================================================================
:cc
"!CROSS!gcc" !ARCH! !CFLAGS! !INC! -c -o "%OUT%\obj\%~n1.o" "%~1"
if errorlevel 1 exit /b 1
set OBJS=!OBJS! "%OUT%\obj\%~n1.o"
exit /b 0

REM ================================================================
:do_flash
if not exist "%ROOT%\build\%EXAMPLE%.uf2" (
    echo build\%EXAMPLE%.uf2 not found. Build it first.
    goto :end
)
python "%SDK%\tools\serial_flash.py" %PORT% "%ROOT%\build\%EXAMPLE%.uf2"
goto :end

REM ================================================================
:fail
echo.
echo *** FAILED ***
echo.

:end
endlocal
