"""
run_rtos.py: build the FreeRTOS examples and run them on baoemu

Copyright (c) 2026 Armstrong Subero
SPDX-License-Identifier: Apache-2.0

Usage:
  python3 third_party/freertos/tests/run_rtos.py [-Og] [-Os] [-O2] [--soak SECONDS] [name ...]

Each example is built (with bao_flash.sh inside the SDK, with rtos_build.sh
when this is a folder of its own) at every optimisation level given
(default -Og and -Os), run on the emulator, and must print its pass line
with no FAIL, TRAP or ASSERT line. rtos_drivers gets an MPU6050 and an
SPI loopback attached, and its 100 printed lines must each be whole.
rtos_suite is written by make_suite.py first and must pass all 16 tests.
rtos_usb_cdc has 4000 bytes sent through its USB serial port in mixed
chunk sizes and every one must come back. The soak runs for 10 emulated seconds
unless --soak says otherwise. rtos_tickless needs falling edges on PB4
while the core sleeps, which this script supplies.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# Inside the SDK the examples are built by bao_flash.sh. As a folder of its
# own they are built by rtos_build.sh against the SDK named in DABAO_SDK.
if os.path.exists(os.path.join(ROOT, "bao_flash.sh")):
    SDK = ROOT
    BUILDER = os.path.join(ROOT, "bao_flash.sh")
else:
    SDK = os.path.abspath(os.environ.get("DABAO_SDK", os.path.join(ROOT, "..", "dabao-sdk-main")))
    BUILDER = os.path.join(ROOT, "rtos_build.sh")

sys.path.insert(0, os.environ.get("BAOEMU_PY", os.path.join(SDK, "baoemu", "python")))
from baoemu import Bao  # noqa: E402

PASS = "RESULT PASS"

# name, emulated ms, line that must appear
EXAMPLES = [
    ("rtos_hello", 2500, PASS),
    ("rtos_queue", 2000, PASS),
    ("rtos_sync", 3000, PASS),
    ("rtos_timing", 5500, PASS),
    ("rtos_sched", 2500, PASS),
    ("rtos_heap", 1500, PASS),
    ("rtos_notify", 2500, PASS),
    ("rtos_fromisr", 3000, PASS),
    ("rtos_isr", 2500, PASS),
    ("rtos_delay", 3000, PASS),
    ("rtos_static", 2500, PASS),
    ("rtos_qset", 2500, PASS),
    ("rtos_stats", 2500, PASS),
    ("rtos_hooks", 2500, PASS),
    ("rtos_drivers", 3000, PASS),
    ("rtos_usb_cdc", 0, "USB: configured by the host, high speed"),
    ("rtos_tickless", 0, PASS),
    ("rtos_wfidiag", 1000, PASS),
    ("rtos_overflow", 1500, "STACK OVERFLOW in task greedy"),
    ("rtos_soak", 0, PASS),
    ("rtos_suite", 60000, "SUITE: 16 tests, 0 failed"),
]


def text(chip):
    out = chip.console()
    return out.decode("latin-1") if isinstance(out, bytes) else out


def run_tickless(chip):
    chip.set_input("PB4", 1)
    for _ in range(300):
        chip.run_ms(10)
        if "waiting for 40 falling edges" in text(chip):
            break
    chip.run_ms(20)
    for _ in range(40):
        chip.set_input("PB4", 0)
        chip.run_ms(25)
        chip.set_input("PB4", 1)
        chip.run_ms(25)
    chip.run_ms(300)


def run_usb(chip):
    """Push 4000 bytes through the echo in mixed chunk sizes. True when they all come back."""
    chip.run_ms(800)
    data = bytes((i * 7 + 13) % 251 for i in range(4000))
    sizes = [1, 5, 64, 63, 65, 200, 512, 17, 300]
    pos = k = 0
    while pos < len(data):
        n = sizes[k % len(sizes)]
        k += 1
        chip.usb_serial_send(data[pos:pos + n])
        pos += n
        chip.run_ms(20)
    chip.run_ms(1500)
    return bytes(chip.usb_text) == data


def run(example, ms, expect, opt, soak):
    flags = opt + (" -DSOAK_SECONDS=%d" % soak if example == "rtos_soak" else "")
    env = dict(os.environ, BAO_OPT=flags)
    if example == "rtos_suite":
        subprocess.run([sys.executable, os.path.join(HERE, "make_suite.py")], check=True, capture_output=True)
    built = subprocess.run(["bash", BUILDER, "build", example],
                           env=env, capture_output=True, text=True)
    log = built.stdout + built.stderr
    if built.returncode != 0 or "BUILD SUCCESS" not in log:
        return False, "build failed\n" + log[-3000:]
    if "warning" in log:
        return False, "build warnings\n" + log[-3000:]
    chip = Bao()
    chip.set_stop_on_exception(False)
    chip.load(os.path.join(ROOT, "build", example + ".elf"))
    if example == "rtos_drivers":
        chip.attach("mpu6050", 0, 0x68)
        chip.attach("spi-loopback", 2)
    usb_ok = True
    if example == "rtos_usb_cdc":
        usb_ok = run_usb(chip)
    elif example == "rtos_tickless":
        run_tickless(chip)
    elif example == "rtos_soak":
        chip.run_ms(soak * 1000 + 1500)
    else:
        chip.run_ms(ms)
    out = text(chip)
    bad = any(word in out for word in ("FAIL", "TRAP", "ASSERT"))
    if example == "rtos_suite":
        bad = bad or out.count("RESULT PASS") != 16
    if example == "rtos_usb_cdc":
        bad = bad or not usb_ok or "echoed" not in out
    if example == "rtos_drivers":
        whole = re.findall(r"^talker [AB] line \d\d abcdefghijklmnopqrstuvwxyz\r?$", out, re.M)
        bad = bad or len(whole) != 100 or out.count("talker") != 100
    return (expect in out) and not bad, out


def main():
    args = sys.argv[1:]
    soak = 10
    if "--soak" in args:
        i = args.index("--soak")
        soak = int(args[i + 1])
        del args[i:i + 2]
    opts = [a for a in args if a.startswith("-O")] or ["-Og", "-Os"]
    names = [a for a in args if not a.startswith("-")]
    chosen = [e for e in EXAMPLES if not names or e[0] in names]
    failed = 0
    for opt in opts:
        for example, ms, expect in chosen:
            ok, out = run(example, ms, expect, opt, soak)
            print("%s  %-14s %s" % ("PASS" if ok else "FAIL", example, opt), flush=True)
            if not ok:
                failed += 1
                print(out)
    total = len(opts) * len(chosen)
    print("%d of %d passed" % (total - failed, total))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
