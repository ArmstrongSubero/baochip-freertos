"""
run_examples.py: run every Dabao SDK example on baoemu and check its output

Copyright (c) 2026 Armstrong Subero
SPDX-License-Identifier: Apache-2.0

Usage:
  python tests/run_examples.py <dabao-sdk folder>

Each example is built first with bao_flash (the .elf files in build/ are
what is run). Parts the example expects on its pins are attached for it,
and each one has a line of console output that has to appear. The PKE and
big number ALU examples are listed as not modelled.
"""

import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "python"))
from baoemu import Bao  # noqa: E402

NOT_MODELLED = set()

# BAO_MMU test programs whose sandbox fault report is the expected result.
MMU_FAULT_EXPECTED = {"mmu_stack", "mmu_gap", "mmu_svc"}


def mmu_table_problems(chip):
    """Walk the sandbox's page tables from the host after the run.

    Bao.peek32 reads peripheral registers only and refuses SRAM, so the
    table words are read with Bao.read. Every range listed must be unmapped
    in the supervisor table, the page tables must be read only to it, and
    the boot chain, security region and IFR must be unmapped in every
    service table too.
    """
    def rd(a):
        return struct.unpack("<I", chip.read(a, 4))[0]

    def leaf(root, va):
        e = rd(root + (va >> 22) * 4)
        if not e & 1:
            return 0
        if e & 0xA:
            return e
        e = rd(((e >> 10) << 12) + ((va >> 12) & 0x3FF) * 4)
        return e if e & 1 else 0

    def mapped(root, lo, hi):
        return [va for va in range(lo, hi, 0x1000) if leaf(root, va)]

    sym = chip.symbol
    lay = struct.unpack("<8I", chip.read(sym("bao_mmu_layout"), 32))
    main_lo, main_hi, trap_lo, trap_hi, s_end, tables, tables_end, m_end = lay
    bad = []
    satp = rd(sym("bao_mmu_satp_app"))
    if not satp >> 31:
        return ["satp for supervisor code is not Sv32"]
    root = (satp & 0x3FFFFF) << 12
    forbidden = [
        (0x40000000, 0x40080000, "boot chain registers"),
        (0x60000000, 0x60060000, "boot0 and boot1"),
        (0x603DA000, 0x603DC000, "one way counter page"),
        (0x603DA000, 0x60400000, "security region"),
        (0x60400000, 0x60800000, "IFR"),
        (0x60FFF000, 0x61000000, "below SRAM"),
        (main_lo - 0x1000, main_lo, "guard below the main stack"),
        (main_hi, trap_lo, "trap stack guard"),
        (s_end, 0x61200000, "SRAM gap"),
        (0x61000000, m_end, "machine data and stack"),
    ]
    for lo, hi, what in forbidden:
        m = mapped(root, lo, hi)
        if m:
            bad.append("%s mapped at 0x%08X" % (what, m[0]))
    for va in range(tables, tables_end, 0x1000):
        if (leaf(root, va) & 0x6) != 0x2:
            bad.append("page table at 0x%08X not read only" % va)
            break
    groups = sym("bao_mmu_satp_group")
    for g in range(5):
        groot = (rd(groups + 4 * g) & 0x3FFFFF) << 12
        for lo, hi, what in forbidden[1:5]:
            m = mapped(groot, lo, hi)
            if m:
                bad.append("service table %d maps %s at 0x%08X" % (g, what, m[0]))
    return bad

# SCE registers after the run, read from the host since programs may only
# reach the SCE through the SDK drivers: each engine sets its own FRDONE
# bit (sce.sv 248), SUBEN resets to 0x1F (sce_glbsfra.sv 76), TS0 bit 31 is
# always 1 (sce_sec.sv sce_ts), the TRNG leaves FR bit 0 and the words
# counted in FIFO 5 as the driver stopped it (4 words, bit 1 set), the undecoded block at 0x4002A000 reads 0.
SCE_AFTER = {
    "sha": {0x40028014: 0x08, 0x40028004: 0x1F, 0x400280E0: 0x80000000, 0x4002A000: 0},
    "aes": {0x40028014: 0x02, 0x40028004: 0x1F},
    "aes_modes": {0x40028014: 0x02, 0x40028004: 0x1F},
    "sha_stream": {0x40028014: 0x08, 0x40028004: 0x1F, 0x4002B00C: 0, 0x4002B008: 0},
    "hmac": {0x40028014: 0x08, 0x40028004: 0x1F, 0x4002B00C: 0, 0x4002B000: 0x60, 0x40020100: 0, 0x4002013C: 0},
    "trng": {0x40028014: 0x01, 0x4002E018: 0x01, 0x40028054: 0x42, 0x40028030: 0x20},
    "alu_bigint": {0x40028014: 0x10},
    "pke_bigint": {0x40028014: 0x14},
}


def make_sd_image(path):
    try:
        from pyfatfs.PyFat import PyFat
        from pyfatfs.PyFatFS import PyFatFS
    except ImportError:
        return False
    with open(path, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    pf = PyFat()
    pf.mkfs(path, fat_type=32, label="DABAO")
    pf.close()
    fs = PyFatFS(path)
    fs.writetext("/HOST.TXT", "hello from the host\n")
    fs.close()
    return True


def toggles(chip, pin):
    seen = []
    port = "ABCDEF".index(pin[1])
    bit = int(pin[2:])
    last = {"v": None}

    def cb(p, out, oe):
        if p == port:
            v = (out >> bit) & 1
            if v != last["v"]:
                last["v"] = v
                seen.append(chip.cycles)
    chip.on_gpio(cb)
    return seen


def check(name, elf, tmp):
    chip = Bao()
    chip.load(elf)
    # A BAO_MMU build calls its machine mode drivers with ecall and reports
    # its own faults, so exceptions are the program's to handle.
    mmu = chip.symbol("bao_mmu_init") != 0
    if mmu:
        chip.set_stop_on_exception(False)
    need = []
    ms = 1500
    edges = None

    if name == "hello_uart":
        need = ["Hello from Dabao!", "count = 1"]
    elif name == "hello_uart_async":
        need = ["[Done] Async UART demo complete."]
    elif name == "rram":
        need = ["Verify: PASS", "[Done] ReRAM demo complete."]
    elif name == "sha":
        need = ["Got:    e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                "Got:    ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"]
    elif name == "aes":
        need = ["Cipher:    3925841d02dc09fbdc118597196a0b32", "Round-trip: PASS"]
        ms = 5300
    elif name == "trng":
        need = ["Generate 8 words: OK"]
    elif name == "spi_async":
        chip.attach("spi-loopback", 2)
        need = ["Errors: 0", "Round 5: Errors=0", "[Done]"]
    elif name in ("i2c_mpu6050", "i2c_async"):
        chip.attach("mpu6050", 0, 0x68)
        need = ["WHO_AM_I = 0x68", "AZ=16384"]
    elif name == "w25q_flash":
        chip.attach("w25q", 1, 0, os.path.join(tmp, "flash.bin"))
        need = ["JEDEC ID: ef 40 18", "Verify: PASS", "=== Test Complete ==="]
    elif name == "fatfs":
        img = os.path.join(tmp, "sd.img")
        if not make_sd_image(img):
            return "skip", "pyfatfs is not installed"
        chip.attach("sd", 2, 0, img)
        need = ["HOST.TXT", "Read 53 bytes", "[Done] FatFS test complete."]
    elif name in ("ch20_2001", "ch20_2002", "ch20_2003"):
        img = os.path.join(tmp, "sd.img")
        if not make_sd_image(img):
            return "skip", "pyfatfs is not installed"
        chip.attach("sd", 2, 0, img)
        need = {"ch20_2001": ["card type 0x0c, block addressing", "sector 0 ends 55 aa",
                              "bytes per sector 512", "type \"FAT32   \""],
                "ch20_2002": ["mounted, FAT type 3", "wrote 63 bytes to NOTE.TXT", "read 63 bytes back:",
                              "Second line of the file.", "  HOST.TXT      20 bytes", "  NOTE.TXT      63 bytes"],
                "ch20_2003": ["logged 5,32.55", "TEMPLOG.CSV, 62 bytes:", "seconds,celsius"]}[name]
        ms = 7000 if name == "ch20_2003" else 1500
    elif name == "ch21_2101":
        need = ["CFGINFO 0x10000408: version 8, 4 cores, 4096 bytes each"] + \
               ["FIFO %d holds 1 word, hello from core %d" % (i, i) for i in range(4)]
        ms = 50
    elif name == "ch21_2102":
        need = ["core 0 waiting at pc 0x000, FIFO 1 holds 0", "FIFO 1 holds 8",
                "1000 squared is 1000000", "8000 squared is 64000000"]
        ms = 50
    elif name == "ch21_2103":
        edges = toggles(chip, "PB1")
        need = ["CPU second 2, BIO core 0 at pc 0x028"]
        ms = 2600
    elif name == "ch22_2201":
        need = ["core 0 asleep at pc 0x008, events 0x00000000",
                "ring 5 answered with count 5", "events now 0x00000000"]
        ms = 700
    elif name == "ch22_2202":
        need = ["pushed 3, FIFO 0 holds 3, FIFO 1 holds 0", "pushed 4, FIFO 0 holds 0, FIFO 1 holds 1",
                "sum 1 is 10", "sum 2 is 26", "sum 3 is 42"]
        ms = 300
    elif name == "ch22_2203":
        need = ["BIO second 5, tick timer at"]
        ms = 5200
    elif name == "ch22_2204":
        edges = toggles(chip, "PB1")
        need = ["period 4: 700044870 cycles, 1000064 us"]
        ms = 4700
    elif name == "appb_b01":
        edges = toggles(chip, "PB1")
        need = ["main stack 0x6101d000 to 0x6101f000", "satp 0x80061008, page table at 0x61008000",
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "toggle 3"]
        ms = 1200
    elif name == "ch22_2205":
        need = ["core 3 finished, events 0x00000001", "0 words differ", "dst[1023] 0x030302ff"]
        ms = 300
    elif name == "bio_dma":
        need = ["Round 5: Errors=0", "[Done] BIO DMA tests complete."]
    elif name == "bio_snap":
        edges = toggles(chip, "PB7")
        need = ["snap input on: 0 of 9 pairs differ", "snap output done"]
        ms = 30
    elif name == "bio_fifo":
        need = ["core fill level: 9", "x26 at reset: 0xffffffff", "core words: 1 to 9, level now 0",
                "empty read returns: 9", "level events: 000E00GGGGGG", "host fill level: 9",
                "host words kept: 1 to 9", "double read: 20 40, FIFO0 left 1",
                "restart clears FIFO2: 3 then 0", "x30 with mask 0 ran: 1", "x27 reads: 0x01000000",
                "x30 woke after push 3, marker 2", "[Done] bio_fifo"]
        ms = 50
    elif name == "bio_gpio":
        need = ["pins 7..4: 00 a0 b0 30, mask 0x30", "core 0 pc 0x044 trap 0",
                "DBG_PADOUT 0x00000000 DBG_PADOE 0x00000000", "core set: 0x000123",
                "host clear: 0x000120, EVENT_CLR reads 0x000003", "core clear: 0x000020",
                "host set: 0x400020, EVENT_SET reads 0x400000", "x31: core 2, step 4",
                "core 3 trap 1 at pc 0x004", "[Done] bio_gpio"]
        ms = 40
    elif name == "bio_filter":
        need = ["gutters read back 0x0 0x0, filter base reads 0x0",
                "round 1: window stored, outside untouched, gutter stored, load 0x1001, window load 0xcafe",
                "round 2: window stored, outside untouched, gutter stored, load 0x1002, window load 0xcafe",
                "round 3: window stored, outside untouched, gutter stored, load 0x1003, window load 0xcafe",
                "round 4: window stored, outside untouched, gutter stored, load 0x1004, window load 0xcafe",
                "round 5: window stored, outside stored, gutter untouched, load 0x1005, window load 0xcafe",
                "round 6: window stored, outside stored, gutter untouched, load 0x1006, window load 0xcafe",
                # with bio_start_cores starting only parked (trapped) cores, core 1
                # no longer runs on from bio_init before its restart; the program
                # still gives up after 500 ms and prints the stuck report if not
                "low alias load 0x5a, RAM word 0x5a, undecoded load 0x0", "[Done] bio_filter"]
        ms = 600
    elif name == "bio_start":
        w = ": 8 words: 0 1 2 3 4 5 6 7"
        need = ["after bio_init" + w, "restart, no reload" + w,
                "quantum wait stopped: trap 1", "after quantum wait" + w,
                "x20 loop stopped: trap 1", "after x20 loop" + w,
                "x20 loops stopped: 16 of 16 trapped, RAM kept 16 of 16", "after x20 loops" + w,
                "after FIFO read stall" + w, "full FIFO0 kept: 9", "after FIFO write stall" + w,
                "FIFO0 after start: 0", "after plain CTRL stop" + w,
                "core 0 when core 1 started: enabled 0, trap 1", "after start of core 1" + w,
                "[Done] bio_start"]
        ms = 80
    elif name == "bio_irq":
        need = ["pioirq0 handled 3 events",
                "edge mode lines: 10 10, after event 5: 00, level mode: 18",
                "core 1 waited with level 0, woke with marker 7", "[Done] bio_irq"]
        ms = 40
    elif name == "bio_div":
        need = ["divider 1000+0/256: periods 1000 to 1000, 8 periods 8000",
                "divider 1000+128/256: periods 1000 to 1002, 8 periods 8004",
                "divider 37+64/256: periods 36 to 38, 8 periods 298",
                "started together: 9 of 9 quanta equal",
                "addi 21 took 8 cycles, addi 20 took 3 quanta", "[Done] bio_div"]
        ms = 40
    elif name == "bio_isa":
        need = ["isa: 28 of 28 match",
                "misaligned: lh 0x203 = ffff8765, lw 0x202 = 87654321, after sh and sb 879bff9b",
                "mul with x16: 3, marker 1, FIFO0 left 1, FIFO3 left 0",
                "illegal half word: trap 1 at pc 0x018", "[Done] bio_isa"]
        ms = 40
    elif name == "mmu_priv":
        chip.set_stop_on_exception(False)
        need = ["mstatus writable bits: PASS", "sstatus view: PASS", "medeleg bits: PASS",
                "mideleg bits: PASS", "mret to M sets MPP to U: PASS",
                "ecall from U tval: PASS", "ecall from S MPP: PASS", "mret to S clears MPRV: PASS",
                "ebreak tval is the instruction: PASS", "illegal CSR tval is the instruction: PASS",
                "csrw cycle is illegal: PASS", "csrrs cycle with rs1 is legal: PASS",
                "csrw 0xDC0 is illegal: PASS", "sfence.vma legal in U: PASS",
                "sret clears SPP and MPRV, SPIE to 1: PASS", "illegal from U goes to S: PASS",
                "illegal in M never delegated: PASS", "ebreak is not delegable: PASS",
                "SSIP delegated, taken in U: PASS", "epc is the next instruction: PASS",
                "interrupt leaves mtval: PASS", "M level STIP before delegated SSIP: PASS",
                "SEIP from 0x9C0 delegated: PASS", "handler read 0xDC0: PASS",
                "MEIP from 0xBC0 in U: PASS", "mtvec mode 1 traps to base: PASS",
                "wfi woke with MIE 0: PASS", "[Done] mmu_priv 95 of 95 passed"]
        ms = 400
    elif name == "mmu_vm":
        chip.set_stop_on_exception(False)
        need = ["S load through L0 page: PASS", "superpage load: PASS",
                "store to read only: tval: PASS", "load from invalid: tval: PASS",
                "store to W without R: PASS", "L1 PTE W without R: PASS",
                "misaligned superpage: PASS", "misaligned before page fault: PASS",
                "load from execute only, MXR 1: PASS", "S runs code in execute only page: PASS",
                "fetch fault tval is the word address: PASS", "its tval is the second word: PASS",
                "U load from S page tval: PASS", "U fetch from S page: PASS",
                "S load from U page, SUM 1: PASS", "S fetch from U page, SUM 1 runs it: PASS",
                "PTE A and D still clear: PASS", "amoadd on execute only: PASS",
                "sc on read only without reservation: PASS",
                "sc to another address after lr succeeds: PASS",
                "stale line: second store faults: PASS", "after sfence.vma the store lands: PASS",
                "satp write flushes: one fault: PASS", "faulting walk is not kept: PASS",
                "MPRV with MPP U skips the U check: PASS", "MPRV store fault stays in M: PASS",
                "U load fault to S: sepc: PASS", "U fetch fault to S: stval: PASS",
                "U loop on a U page saw the timer flag: PASS", "interrupt epc after the wfi: PASS",
                "[Done] mmu_vm 83 of 83 passed"]
        ms = 400
    elif name == "sha_stream":
        need = ["SHA-256 empty: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 PASS",
                "SHA-256 abc: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad PASS",
                "SHA-256 448 bit: 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1 PASS",
                "SHA-256 896 bit: cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1 PASS",
                "SHA-512 empty: cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e PASS",
                "SHA-512 abc: ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f PASS",
                "SHA-512 448 bit: 204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
                "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445 PASS",
                "SHA-512 896 bit: 8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909 PASS",
                "SHA-256 million a in pieces: cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0 PASS",
                "SHA-512 million a in pieces: e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b PASS",
                "two SHA-256 contexts in turn, 896 bit: cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1 PASS",
                "896 bit message at all 113 split points, wrong: 0 PASS",
                "SHA-256 600 bytes in one update: 8d93b9e8590a37c5c637e161c014db85a1d53577fbfe733fcdd1f20dab5050db",
                "SHA-256 600 bytes at all 601 split points, wrong: 0 PASS",
                "sha512_hash 600 bytes: 127099cf28aa9bcefe1619685a8b959975a4f4be8bd2d525260af0b89accd092"
                "c026824c47a55aea313ba656ed15f4219b2d08ec0ac0935262429a456f594012",
                "SHA-512 600 bytes at all 601 split points, wrong: 0 PASS",
                "return codes: zero length 0, NULL -1, after final -2 -2 PASS",
                "[Done] sha_stream 25 of 25 passed"]
        ms = 600
    elif name == "hmac":
        need = ["HMAC-SHA256 case 1: b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7 PASS",
                "HMAC-SHA256 case 2: 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843 PASS",
                "HMAC-SHA256 case 3: 773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe PASS",
                "HMAC-SHA256 case 4: 82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b PASS",
                "HMAC-SHA256 case 5: a3b6167473100ee06e0c796c2955552b PASS",
                "HMAC-SHA256 case 6: 60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54 PASS",
                "HMAC-SHA256 case 7: 9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2 PASS",
                "HMAC-SHA512 case 1: 87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
                "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854 PASS",
                "HMAC-SHA512 case 2: 164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
                "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737 PASS",
                "HMAC-SHA512 case 3: fa73b0089d56a284efb0f0756c890be9b1b5dbdd8ee81a3655f83e33b2279d39"
                "bf3e848279a722c806b485a47e67c807b946a337bee8942674278859e13292fb PASS",
                "HMAC-SHA512 case 4: b0ba465637458c6990e5a8c5f61d4af7e576d97ff94b872de76f8050361ee3db"
                "a91ca5c11aa25eb4d679275cc5788063a5f19741120c4f2de2adebeb10a298dd PASS",
                "HMAC-SHA512 case 5: 415fad6271580a531d4179bc891d87a6 PASS",
                "HMAC-SHA512 case 6: 80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f352"
                "6b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598 PASS",
                "HMAC-SHA512 case 7: e37b6a775dc87dbaa4dfa9f96e5e3ffddebd71f8867289865df5a32d20cdc944"
                "b6022cac3c4982b10d5eeb55c3e4de15134676fb6de0446065c97440fa8c6a58 PASS",
                "case 7 a byte at a time, SHA-256 and SHA-512 in turn: PASS",
                "case 7 at all 153 split points, wrong: 0 PASS",
                "two HMAC contexts and a SHA-256 context in turn: PASS",
                "HMAC-SHA256 against RFC 2104 for 10 key and 8 message lengths, wrong: 0 PASS",
                "HMAC-SHA512 against RFC 2104 for 10 key and 8 message lengths, wrong: 0 PASS",
                "HMAC-SHA256 600 byte key, 2000 bytes: 298833ab6861a1641e300eff8c9659954e56d48bc440f97de79fcb28171f2685",
                "HMAC-SHA512 600 byte key, 2000 bytes: 1046c005a4b1a4f9221f97a3bb3e54588d7c8af1c7643b501053aa3e1053d05a"
                "581b6ecce5915b0d897187c70262bb11c4f0ce3767accb653c173d97cd2cc3e2",
                "return codes: NULL key -1, after final -2 -2 PASS",
                "[Done] hmac 26 of 26 passed"]
        ms = 400
    elif name == "aes_modes":
        need = ["CBC 128 encrypt: 7649abac8119b246cee98e9b12e9197d PASS",
                "CTR 128 encrypt: 874d6191b620e3261bef6864990db6ce PASS",
                "OFB 128 encrypt: 3b3fd92eb72dad20333449f8e83cfb4a PASS",
                "CFB 128 encrypt: 3b3fd92eb72dad20333449f8e83cfb4a PASS",
                "CBC 192 encrypt: 4f021db243bc633d7178183a9fa071e8 PASS",
                "CTR 192 encrypt: 1abc932417521ca24f2b0459fe7e6e0b PASS",
                "OFB 192 encrypt: cdc80d6fddf18cab34c25909c99a4174 PASS",
                "CBC 256 encrypt: f58c4c04d6e5f1ba779eabfb5f7bfbd6 PASS",
                "CTR 256 encrypt: 601ec313775789a5b7a7f504bbf3d228 PASS",
                "OFB 256 encrypt: dc7e84bfda79164b7ecd8486985d3860 PASS",
                "CFB 256 decrypt: PASS",
                "CBC 320 bytes: fold 21a64c3e last 413b0d9dfb65d6ccec92195fcea44fe9 PASS",
                "CTR 320 bytes: fold df5d09f8 last 334b0e1b8f9bd6cf904d0f86b0ab1a8d PASS",
                "OFB 320 bytes: fold 3fc2e336 last aa2234823be7336229936277a087141e PASS",
                "CFB 320 bytes: fold 96733be2 last 58329811e4f6b0419fc8d6a59bfed7da PASS",
                "CTR counter wrap, block 2: c6bb8e1096003aa132af4f29a0dc5866 PASS",
                "[Done] aes_modes 44 of 44 passed"]
        ms = 300
    elif name == "aes_zkn":
        chip.set_stop_on_exception(False)
        need = ["16 instruction and bs pairs wrong: 00000000 PASS",
                "all 256 bytes folded: 431a0829 PASS",
                "AES-128 cipher 69c4e0d86a7b0430d8cdb78070b4c55a",
                "AES-128 encrypt: PASS", "AES-128 decrypt: PASS",
                "AES-256 cipher 8ea2b7ca516745bfeafc49904b496089",
                "AES-256 encrypt: PASS", "AES-256 decrypt: PASS",
                "rd x0 stays 0: 00000000 PASS",
                "funct3 1 with aes32esi funct7 traps: 00000002 PASS",
                "its mtval is the instruction: 22c59533 PASS",
                "funct3 2 to 7 trap: 00000002 PASS",
                "funct7 0x19 (bit 28) traps: 00000002 PASS",
                "funct7 0x10 (bit 25 clear) traps: 00000002 PASS",
                "funct7 0x21 (bit 29 clear) traps: 00000002 PASS",
                "the 16 aes32 funct7 values run: ffffffff PASS",
                "aes32esi then use, cycles: 00000004 PASS",
                "uncached load, nop, aes32esi: 000000fc PASS",
                "uncached load, aes32esi, add from t5: 0000007b PASS",
                "next reads the load's rd: value before it: 00000067 PASS",
                "next reads the aes rd: value before it: 000000fe PASS",
                "amoor.w, aes32esi, add from t5: 0000007b PASS",
                "amoor.w, nop, aes32esi: 000000fc PASS",
                "next is aes32dsmi bs 3: bank 1: 89894043 PASS",
                "next is c.add a2, a3: 000000fd PASS",
                "aes32esi waiting for the load's rd: 00000063 PASS",
                "[Done] aes_zkn 28 of 28 passed"]
        ms = 300
    elif name == "cpu_timing":
        chip.set_stop_on_exception(False)
        need = ["8 dependent adds: 8 cycles PASS", "load then use: 4 cycles PASS",
                "load, one between, use: 4 cycles PASS", "load, two between, use: 4 cycles PASS",
                "load then store of it: 4 cycles PASS", "load, li same rd, use: 4 cycles PASS",
                "shift then use: 3 cycles PASS", "shift then other: 2 cycles PASS",
                "mul then use: 4 cycles PASS", "amoor.w: 2 cycles PASS",
                "div then other: 35 cycles PASS",
                "divu then use: 36 cycles PASS", "jal: 2 cycles PASS",
                "forward branch not taken: 1 cycles PASS", "forward branch taken: 4 cycles PASS",
                "loop of 4: 15 cycles PASS", "la and jr: 6 cycles PASS", "csrr: 3 cycles PASS",
                "csrr then use: 5 cycles PASS", "three nops then csrr: 6 cycles PASS",
                "uncached load: 4 cycles PASS", "uncached load then use: 7 cycles PASS",
                "c.j to a split 32 bit addi: 4 cycles PASS", "c.j to c.addi: 3 cycles PASS",
                "sfence.vma: 5 cycles PASS", "fence.i: 133 cycles PASS",
                "data cache flush of one line: 4 cycles PASS", "data cache flush: 131 cycles PASS",
                "store then data cache flush: 134 cycles PASS",
                "ecall, handler, mret: 17 cycles PASS", "[Done] cpu_timing 30 of 30 match"]
        ms = 200
    elif name == "bio_ext":
        need = ["out of order: 0"]
        ms = 40
    elif name == "bio_blink":
        edges = toggles(chip, "PB2")
        need = ["BIO running"]
        ms = 10
    elif name in ("timer_irq", "irq"):
        need = ["tick 2"]
    elif name == "ticktimer":
        need = ["Elapsed milliseconds"]
    elif name == "heartbeat":
        need = ["beat 2"]
    elif name == "usb_cdc":
        chip.usb_serial_send("hello")
        need = ["USB: configured by the host, high speed", "USB: line state DTR 1 RTS 1",
                "USB: echoed 5 bytes"]
        ms = 600
    elif name == "rtc":
        need = ["(raw: 75"]
        ms = 2500
    elif name == "ch17_1701":
        need = ["At boot, CRSRC 0x0000001f, OPT 0x00010000", "256 words in "]
        ms = 1200
    elif name == "ch17_1702":
        need = ["one bits in 32768", "6000 rolls:", "rolled "]
        ms = 2500
    elif name == "ch18_1801":
        need = ["AES-128 3ad77bb40d7a3660a89ecaf32466ef97 encrypt PASS, decrypt PASS",
                "AES-192 bd334f1d6e45f25ff712a214571fa5cc encrypt PASS, decrypt PASS",
                "AES-256 f3eed1bdb5d2a03c064b5a7e3db181f8 encrypt PASS, decrypt PASS"]
        ms = 300
    elif name == "ch18_1802":
        need = ["CBC 7649abac8119b246cee98e9b12e9197d encrypt PASS, decrypt PASS",
                "CTR 874d6191b620e3261bef6864990db6ce encrypt PASS, decrypt PASS",
                "OFB 3b3fd92eb72dad20333449f8e83cfb4a encrypt PASS, decrypt PASS",
                "CFB 3b3fd92eb72dad20333449f8e83cfb4a encrypt PASS, decrypt PASS",
                "CBC in two calls PASS"]
        ms = 300
    elif name == "ch18_1803":
        need = ["plain  Hello from the Dabao, encrypted by the SCE."]
        ms = 300
    elif name == "ch19_1901":
        need = ["SHA-256 abc  ba7816bf8f01cfea... PASS", "SHA-256 448  248d6a61d20638b8... PASS",
                "SHA-256 896  cf5b16a778af8380... PASS", "SHA-512 abc  ddaf35a193617aba... PASS",
                "SHA-512 448  204a8fc6dda82f0a... PASS", "SHA-512 896  8e959b75dae313da... PASS"]
        ms = 300
    elif name == "ch19_1902":
        need = ["HMAC-SHA256 case 2        5bdcc146bf60754e... PASS",
                "HMAC-SHA512 case 2        164b7a7bfcf819e2... PASS",
                "HMAC-SHA256 case 2 stream 5bdcc146bf60754e... PASS",
                "HMAC-SHA512 case 2 stream 164b7a7bfcf819e2... PASS",
                "HMAC-SHA256 case 6        60e431591ee0b67f... PASS",
                "HMAC-SHA512 case 6        80b24263c7c1a3eb... PASS",
                "\"set the pump to 40 percent\" tag accepted",
                "\"set the pump to 90 percent\" tag rejected"]
        ms = 300
    elif name == "ch19_1903":
        need = ["public key d75a980182b10ab7... PASS", "signature  e5564300c360ac72... PASS",
                "verify 0 PASS", "\"Dabao firmware 1.2, build 7\" verify 0",
                "\"Dabao firmware 1.3, build 7\" verify -4"]
        ms = 400
    elif name == "ch19_1904":
        need = ["public key x 60fed4ba255a9d31... PASS", "r            efd48b2aacb6a8fd... PASS",
                "s            f7cb1c942d657c41... PASS", "\"sample\" verify 0", "\"sampLe\" verify -4"]
        ms = 400
    elif name == "ch14_1401":
        need = ["busy: ", "WFI:  "]
        ms = 2500
    elif name == "ch14_1402":
        need = ["powering down at 5 s, alarm at 15 s", "Woke from power down at 15 s"]
        ms = 19000
    elif name == "adc_read":
        chip.adc_set_mv(0, 604)
        need = ["raw=511"]
    elif name == "button":
        chip.run_ms(50)
        chip.set_input("PB3", 0)
        chip.run_ms(50)
        need = ["Button pressed"]
        ms = 50
    elif name == "gpio_irq":
        chip.run_ms(20)
        chip.set_input("PB3", 0)
        chip.run_ms(20)
        chip.set_input("PB3", 1)
        chip.run_ms(20)
        chip.set_input("PB3", 0)
        need = ["falling edges: 1", "falling edges: 2"]
        ms = 20
    elif name == "periph_regs":
        need = ["[Done] periph_regs 30 of 30 passed"]
        ms = 400
    elif name == "sw_reset":
        ms = 20
    elif name == "mdma":
        need = ["auto word copy of 64: 0x00000001 PASS",
                "memory scatter gather, three tasks: 0x00000001 PASS",
                "hash done requests one batch: 0x00000001 PASS",
                "interrupt handler saw one done: 0x00000001 PASS",
                "[Done] mdma 57 of 57 passed"]
        ms = 300
    elif name == "mbox":
        need = ["APB write lands in MAILBOX: 0x00000001 PASS",
                "1025 words after it in order: 0x00000001 PASS",
                "MBOX_APB acknowledged: 0x0000000a PASS",
                "MAILBOX acknowledged: 0x00000002 PASS",
                "1025 MB_CLIENT words in order: 0x00000001 PASS",
                "MB_CLIENT available on line 23: 0x00800000 PASS",
                "handler saw APB available: 0x00000001 PASS",
                "[Done] mbox 98 of 98 passed"]
        ms = 1200
    elif name == "blink":
        edges = toggles(chip, "PB1")
        ms = 2600
    elif name == "pmu":
        need = ["cycle counter: 350 cycles per us"]
    elif name == "pwm":
        need = ["PWM period"]
    elif name == "spi_mcp41010":
        need = ["Cycle 1"]
    elif name in ("alu_bigint", "pke_bigint", "pke_ecc"):
        need = ["[Done] all tests passed."]
        ms = 500
    elif name == "pke_clock":
        need = ["m ^ e mod n   PASS", "[Done]"]
        ms = 500
    elif name == "pke_diag":
        need = ["result 6 (expect 6)"]
        ms = 500
    elif name == "wdt":
        need = ["Watchdog will reset chip."]
        ms = 25000
    elif name == "ecdsa":
        need = ["P-256 public key: 60fed4ba...d4462299 PASS",
                "P-384 public key: ec3a4e41...33264720 PASS",
                "secp256k1 public key: 50863ad6...9c582ba6 PASS",
                "P-256 SHA-256 sample sign: r efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
                " s f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8 PASS",
                "P-256 SHA-512 sample sign: r 8496a60b5e9b47c825488827e0495b0e3fa109ec4568fd3f8d1097678eb97f00"
                " s 2362ab1adbe2b8adf9cb9edab740ea6049c028114f2460f96554f61fae3302fe PASS",
                "P-256 SHA-256 test sign: r f1abb023518351cd71d881567b1ea663ed3efcf6c5132b354f28d3b0b7d38367"
                " s 019f4113742a2b14bd25926b49c649155f267e60d3814b4c0cc84250e46f0083 PASS",
                "P-256 SHA-512 test sign: r 461d93f31b6540894788fd206c07cfa0cc35f46fa3c91816fff1040ad1581a04"
                " s 39af9f15de0db8d97e72719c74820d304ce5226e32dedae67519e840d1194e55 PASS",
                "P-384 SHA-256 sample sign: r 21b13d1e013c7fa1392d03c5f99af8b30c570c6f98d4ea8e354b63a21d3daa33"
                "bde1e888e63355d92fa2b3c36d8fb2cd s f3aa443fb107745bf4bd77cb3891674632068a10ca67e3d45db2266fa7d1feeb"
                "efdc63eccd1ac42ec0cb8668a4fa0ab0 PASS",
                "P-384 SHA-512 test sign: r a0d5d090c9980faf3c2ce57b7ae951d31977dd11c775d314af55f76c676447d0"
                "6fb6495cd21b4b6e340fc236584fb277 s 976984e59b4c77b0e8e4460dca3d9f20e07b9bb1f63beefaf576f6b2e8b22463"
                "4a2092cd3792e0159ad9cee37659c736 PASS",
                "P-256 SHA-256 sample one bit flipped in the message, r, s: -4 -4 -4 PASS",
                "secp256k1 Satoshi Nakamoto sign: r 934b1ea10a4b3c1757e2b0c017d0b6143ce3c9a7e6a4a49860d7a6ab210ee3d8"
                " n minus s 2442ce9d2b916064108014783e923ec36b49743e2ffa1c4496f01a512aafd9e5 PASS",
                "secp256k1 Satoshi Nakamoto verify, s and low s as published: 0 0 PASS",
                "r of 0 and n, s of 0 and n: -4 -4 -4 -4 PASS",
                "public key off the curve: -2 -2 PASS",
                "public key at infinity, x of p: -2 -2 -2 PASS",
                "u1 G + u2 Q at infinity: -4 PASS",
                "u1 G equal to u2 Q, so verify doubles: 0, r changed -4 PASS",
                "hash of 0 (u1 = 0): 0 0 PASS",
                "private key 0 and n: -2 -2 -2 PASS",
                "no copy of the key or the nonce on the stack after signing: 0 0 PASS",
                "P-256 time: sign ", "P-384 time: sign ",
                "public key of n minus 1 is minus G on all three curves: 0 0 0 PASS",
                "s = n minus r (u2 = n minus 1): -4 PASS",
                "[Done] ecdsa 49 of 49 passed"]
        ms = 600
    elif name == "ed25519":
        need = ["Ed25519 TEST 1 public key: d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a PASS",
                "Ed25519 TEST 1 sign: e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb882"
                "1590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b PASS",
                "Ed25519 TEST 2 sign: 92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1"
                "e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00 PASS",
                "Ed25519 TEST 3 sign: 6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b"
                "538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a PASS",
                "Ed25519 TEST SHA(abc) sign: dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
                "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704 PASS",
                "Ed25519 TEST SHA(abc) verify: 0 PASS",
                "Ed25519 TEST 1 changed message, one bit flipped in R, in S: -4 -4 -4 PASS",
                "S not canonical: S + L, L, all ones: -4 -4 -4 PASS",
                "public key with y = p, y = 2 (no x), x = 0 with the sign bit: -2 -2 -2 PASS",
                "R with no x, TEST 2 key on TEST 1 signature, identity as key: -4 -4 -4 PASS",
                "300 byte message: sign, verify, last bit flipped: 0 0 -4 PASS",
                "no copy of the scalar or the prefix on the stack after signing: 0 0 0 PASS",
                "Ed25519 time: sign ",
                "TEST 3 signed with the message in the signature buffer: 0 PASS",
                "[Done] ed25519 23 of 23 passed"]
        ms = 400
    elif name == "mmu_stack":
        need = ["mmu_stack: recursing until the guard page stops it",
                "MMU: store page fault at 0x6101c", "MMU: below the main stack, stack overflow",
                "MMU: halted"]
        ms = 100
    elif name == "mmu_gap":
        need = ["mmu_gap: store to .bss read back 0x5a5a1234",
                "MMU: store page fault at 0x61100000, pc", "MMU: halted"]
        ms = 100
    elif name == "mmu_svc":
        need = ["sha256 rc 0: ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "trng words differ: 1", "timer0 interrupts in supervisor mode: 1",
                "root entry for SRAM is a pointer: 1",
                "MMU: sha256_hash refused pointer 0x61008000 in argument 2", "MMU: halted"]
        ms = 200

    reason = chip.run_ms(ms)
    text = chip.console()
    if reason in ("guard", "exception"):
        return "FAIL", "%s: %s" % (reason, chip.stop_message)
    missing = [n for n in need if n not in text]
    for addr, want in SCE_AFTER.get(name, {}).items():
        got = chip.peek32(addr)
        if got != want:
            missing.append("0x%08X reads 0x%X, not 0x%X" % (addr, got, want))
    if name in ("periph_regs", "mdma", "mbox", "ecdsa", "ed25519"):
        missing += [l for l in text.splitlines() if l.endswith(" FAIL")]
    if name in ("ecdsa", "ed25519"):
        # Both programs end with a signature, after which the SDK's
        # hardware_sign clears the whole SCE RAM (the PKE driver's reset
        # and RAM clear), so no key, nonce or HMAC key is left in it.
        left = [a for a in range(0x40020000, 0x40024000, 4) if chip.peek32(a)]
        if left:
            missing.append("SCE RAM not cleared: %d words, first 0x%08X" % (len(left), left[0]))
    if name.startswith("mmu_") or name == "cpu_timing":
        missing += [l for l in text.splitlines() if ": FAIL" in l]
    if name == "sw_reset" and text.count("sw_reset boot") < 2:
        missing.append("banner after the software reset")
    if name == "wdt" and text.count("Watchdog Timer Demo") < 2:
        missing.append("banner after the watchdog reset")
    if name == "blink" and (edges is None or len(edges) < 5):
        missing.append("PB1 toggling")
    if name == "bio_snap":
        m = [l for l in text.splitlines() if l.startswith("snap input off: ")]
        if not m or m[0].startswith("snap input off: 0 "):
            missing.append("pairs that differ without snap input")
        if not edges or len(edges) < 20:
            missing.append("PB7 edges under snap output")
        else:
            edges = edges[1:]
            off = [(e - edges[0]) % 500 for e in edges]
            if any(off):
                missing.append("PB7 edges off the 500 cycle strobe")
            else:
                return ("PASS" if not missing else "FAIL"), "%d PB7 edges on the strobe" % len(edges)
    if name == "usb_cdc" and chip.usb_serial() != "hello":
        missing.append("hello back on the USB serial port")
    if name == "bio_ext":
        m = [l for l in text.splitlines() if l.startswith("edges counted by core 1: ")]
        if not m or int(m[0].split(":")[1].split(",")[0]) < 10:
            missing.append("core 1 counting edges")
    if name == "ch14_1401":
        import re
        m = re.search(r"busy: (\d+) cycles", text)
        if not m or abs(int(m.group(1)) - 350000000) > 1000:
            missing.append("a busy second of 350 million cycles")
    if name == "ch17_1701":
        import re
        m = re.search(r"256 words in (\d+) us", text)
        if not m or not 480 <= int(m.group(1)) <= 520:
            missing.append("a 1 KB batch in about 500 us, as on the board")
    if name == "ch21_2103":
        if not edges or len(edges) < 4:
            missing.append("PB1 blinking")
        else:
            half = (edges[-1] - edges[1]) / (len(edges) - 2) / 350e6
            if abs(half - 0.5) > 0.001:
                missing.append("PB1 half period %.4f s, not 0.5 s" % half)
            elif not missing:
                return "PASS", "PB1 half period %.4f s" % half
    if name == "bio_blink":
        if not edges or len(edges) < 100:
            missing.append("PB2 square wave")
        else:
            per = (edges[-1] - edges[1]) / ((len(edges) - 2) / 2) / 350e6
            return ("PASS" if not missing else "FAIL"), "PB2 at %.1f kHz" % (1e-3 / per)
    if mmu:
        missing += mmu_table_problems(chip)
        if name.startswith("mmu_"):
            missing += [l for l in text.splitlines() if "FAIL" in l]
    if name == "mmu_svc":
        m = [l for l in text.splitlines() if l.startswith("satp 0x")]
        want = "satp 0x%08x," % struct.unpack("<I", chip.read(chip.symbol("bao_mmu_satp_app"), 4))[0]
        if not m or not m[0].startswith(want):
            missing.append("satp as supervisor code reads it")
    if mmu and "MMU: halted" in text and name not in MMU_FAULT_EXPECTED:
        missing.append("sandbox halted: " + " ".join(l for l in text.splitlines() if l.startswith("MMU:")))
    if missing:
        return "FAIL", "missing: " + "; ".join(missing)
    return "PASS", reason


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    sdk = sys.argv[1]
    names = sorted(n for n in os.listdir(os.path.join(sdk, "examples")) if not n.startswith("zz_"))
    tmp = tempfile.mkdtemp(prefix="baoemu_")
    fails = 0
    for name in names:
        elf = os.path.join(sdk, "build", name + ".elf")
        if name.startswith("rtos_"):
            result, note = "skip", "FreeRTOS, run by third_party/freertos/tests/run_rtos.py"
        elif name in NOT_MODELLED:
            result, note = "n/a", "PKE and the big number ALU are not modelled yet"
        elif not os.path.exists(elf):
            result, note = "skip", "no build/%s.elf" % name
        else:
            result, note = check(name, elf, tmp)
        if result == "FAIL":
            fails += 1
        print("%-18s %-5s %s" % (name, result, note))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
