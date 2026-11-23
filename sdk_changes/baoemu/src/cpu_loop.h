/*
 * cpu_loop.h: the interpreter loop, included twice by cpu.c
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * cpu.c defines LOOP_NAME, LOOP_PAGED, LOOP_LOAD and LOOP_STORE before each
 * include. The copy built with LOOP_PAGED 0 has no translation checks at
 * all and runs while satp is bare or the core is in machine mode without
 * MPRV; the other copy translates. Turning translation on or off kicks the
 * run loop, so each block runs in the right copy.
 *
 * Every instruction finds the slot it can leave decode in (sources ready,
 * fetch done, CSR drain), and at the end counts its cycles and leaves the
 * hazard window and the next fetch slot for the one after it.
 */

static int LOOP_NAME(bao_t *b)
{
    uint32_t *x = b->x;

    while (b->cycles < b->limit && !b->stop && !b->wfi) {
        uint32_t pc = b->pc, ins, lo, hi = 0, len;
        uint32_t npc;
        uint64_t at = later(b->slot, b->fetch_ok);
        uint32_t wrd = NO_RD, lat = 0, bub = 1;

#if LOOP_PAGED
        if (b->vm_i) {
            uint32_t tv;
            int e = fetch_paged(b, pc, &lo, &hi, &tv);
            if (e) {
                if (b->stop) goto next;
                TRAP((uint32_t)e, tv);
            }
        } else
#endif
        {
            if (fetch16(b, pc, &lo))
                TRAP(1, pc & ~3u);
            if ((lo & 3) == 3 && fetch16(b, pc + 2, &hi))
                TRAP(1, (pc + 2) & ~3u);
        }
        if ((lo & 3) == 3) {
            ins = lo | (hi << 16);
            len = 4;
            if ((pc & 2) && b->redirected)
                at = later(at, b->fetch_ok + JUMP_SPLIT);
        } else {
            ins = bao_expand_c(lo);
            len = 2;
            if (!ins)
                TRAP(2, lo);
        }
        npc = pc + len;

        switch (ins & 0x7F) {
        case 0x37: WR(RD(ins), ins & 0xFFFFF000u); break;
        case 0x17: WR(RD(ins), pc + (ins & 0xFFFFF000u)); break;
        case 0x6F: {
            int32_t imm = sext((((ins >> 31) & 1) << 20) | (((ins >> 21) & 0x3FF) << 1) |
                               (((ins >> 20) & 1) << 11) | (((ins >> 12) & 0xFF) << 12), 21);
            /* A jump to itself is the end of the program unless a machine
             * interrupt can still be taken, as in an RTOS task that spins. */
            if (imm == 0 && RD(ins) == 0 &&
                !(b->priv == PRIV_M && (b->mstatus & MSTATUS_MIE) && (b->mie & MIP_MEIP) && b->mim)) {
                bao_halt(b, BAO_STOP_HALT, "self loop at %s", bao_where(b, pc));
                goto next;
            }
            WR(RD(ins), npc);
            npc = pc + (uint32_t)imm;
            bub = JUMP_DECODE;
            if (imm < 0 && RD(ins) == 0 && b->ff_on) ff_backedge(b, pc);
            break;
        }
        case 0x67: {
            uint32_t t = (x[RS1(ins)] + (uint32_t)sext(ins >> 20, 12)) & ~1u;
            USE(RS1(ins));
            WR(RD(ins), npc);
            npc = t;
            bub = JUMP_MEMORY;
            break;
        }
        case 0x63: {
            uint32_t a = x[RS1(ins)], c = x[RS2(ins)];
            int t;
            int32_t imm = sext((((ins >> 31) & 1) << 12) | (((ins >> 7) & 1) << 11) |
                               (((ins >> 25) & 0x3F) << 5) | (((ins >> 8) & 0xF) << 1), 13);
            USE(RS1(ins));
            USE(RS2(ins));
            /* static prediction: backward taken, forward not taken */
            if (imm < 0)
                bub = JUMP_MEMORY;
            switch (F3(ins)) {
            case 0: t = a == c; break;
            case 1: t = a != c; break;
            case 4: t = (int32_t)a < (int32_t)c; break;
            case 5: t = (int32_t)a >= (int32_t)c; break;
            case 6: t = a < c; break;
            case 7: t = a >= c; break;
            default: TRAP(2, ins);
            }
            if (t) {
                npc = pc + (uint32_t)imm;
                bub = imm < 0 ? JUMP_DECODE : JUMP_MEMORY;
                if (imm < 0 && b->ff_on) ff_backedge(b, pc);
            }
            break;
        }
        case 0x03: {
            uint32_t a = x[RS1(ins)] + (uint32_t)sext(ins >> 20, 12), v;
            int e;
            USE(RS1(ins));
            GO();
            b->last_mem = at;
            b->wb_hold = 0;
            LATE(RD(ins), LAT_WB);
            switch (F3(ins)) {
            case 0: e = LOOP_LOAD(b, a, 1, &v); v = (uint32_t)(int8_t)v; break;
            case 1: e = LOOP_LOAD(b, a, 2, &v); v = (uint32_t)(int16_t)v; break;
            case 2: e = LOOP_LOAD(b, a, 4, &v); break;
            case 4: e = LOOP_LOAD(b, a, 1, &v); break;
            case 5: e = LOOP_LOAD(b, a, 2, &v); break;
            default: TRAP(2, ins);
            }
            if (b->stop) goto next;
            if (e) TRAP((uint32_t)e, a);
            HELD(RD(ins));
            WR(RD(ins), v);
            break;
        }
        case 0x23: {
            uint32_t a = x[RS1(ins)] + (uint32_t)sext(((ins >> 25) << 5) | ((ins >> 7) & 31), 12);
            uint32_t v = x[RS2(ins)];
            int e;
            USE(RS1(ins));
            USE(RS2(ins));
            GO();
            b->last_mem = at;
            switch (F3(ins)) {
            case 0: e = LOOP_STORE(b, a, 1, v); break;
            case 1: e = LOOP_STORE(b, a, 2, v); break;
            case 2: e = LOOP_STORE(b, a, 4, v); break;
            default: TRAP(2, ins);
            }
            if (b->stop) goto next;
            if (e) TRAP((uint32_t)e, a);
            break;
        }
        case 0x13: {
            uint32_t a = x[RS1(ins)];
            int32_t imm = sext(ins >> 20, 12);
            uint32_t sh = (ins >> 20) & 31;
            USE(RS1(ins));
            switch (F3(ins)) {
            case 0: WR(RD(ins), a + (uint32_t)imm); break;
            case 2: WR(RD(ins), (int32_t)a < imm); break;
            case 3: WR(RD(ins), a < (uint32_t)imm); break;
            case 4: WR(RD(ins), a ^ (uint32_t)imm); break;
            case 6: WR(RD(ins), a | (uint32_t)imm); break;
            case 7: WR(RD(ins), a & (uint32_t)imm); break;
            case 1:
                if (F7(ins)) TRAP(2, ins);
                WR(RD(ins), a << sh);
                LATE(RD(ins), LAT_MEM);
                break;
            case 5:
                if (F7(ins) == 0x20) WR(RD(ins), (uint32_t)((int32_t)a >> sh));
                else if (F7(ins) == 0) WR(RD(ins), a >> sh);
                else TRAP(2, ins);
                LATE(RD(ins), LAT_MEM);
                break;
            }
            break;
        }
        case 0x33: {
            uint32_t a = x[RS1(ins)], c = x[RS2(ins)], f7 = F7(ins), r;
            USE(RS1(ins));
            USE(RS2(ins));
            if (f7 == 1) {
                if (F3(ins) < 4) {
                    LATE(RD(ins), LAT_WB);
                } else {
                    LATE(RD(ins), LAT_MEM);
                    b->cycles += FREEZE_DIV;
                }
                switch (F3(ins)) {
                case 0: r = a * c; break;
                case 1: r = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)c) >> 32); break;
                case 2: r = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)c) >> 32); break;
                case 3: r = (uint32_t)(((uint64_t)a * (uint64_t)c) >> 32); break;
                case 4:
                    if (c == 0) r = 0xFFFFFFFFu;
                    else if (a == 0x80000000u && c == 0xFFFFFFFFu) r = a;
                    else r = (uint32_t)((int32_t)a / (int32_t)c);
                    break;
                case 5: r = c ? a / c : 0xFFFFFFFFu; break;
                case 6:
                    if (c == 0) r = a;
                    else if (a == 0x80000000u && c == 0xFFFFFFFFu) r = 0;
                    else r = (uint32_t)((int32_t)a % (int32_t)c);
                    break;
                default: r = c ? a % c : a; break;
                }
            } else if (f7 == 0) {
                switch (F3(ins)) {
                case 0: r = a + c; break;
                case 1: r = a << (c & 31); break;
                case 2: r = (int32_t)a < (int32_t)c; break;
                case 3: r = a < c; break;
                case 4: r = a ^ c; break;
                case 5: r = a >> (c & 31); break;
                case 6: r = a | c; break;
                default: r = a & c; break;
                }
                if ((F3(ins) & 3) == 1)
                    LATE(RD(ins), LAT_MEM);
            } else if (f7 == 0x20 && F3(ins) == 0) {
                r = a - c;
            } else if (f7 == 0x20 && F3(ins) == 5) {
                r = (uint32_t)((int32_t)a >> (c & 31));
                LATE(RD(ins), LAT_MEM);
            } else if ((ins & 0x32007000u) == 0x22000000u) {
                r = aes32(b, ins, pc, a, c, at);
                LATE(RD(ins), LAT_WB);
            } else {
                TRAP(2, ins);
            }
            WR(RD(ins), r);
            break;
        }
        case 0x2F: {
            uint32_t a = x[RS1(ins)], f5 = ins >> 27, v, pa = a;
            int e;
            if (F3(ins) != 2) TRAP(2, ins);
            USE(RS1(ins));
            if (f5 != 0x02)
                USE(RS2(ins));
            GO();
            b->last_mem = at;
            b->wb_hold = 0;
            LATE(RD(ins), LAT_WB);
            if (f5 == 0x02) {
                e = LOOP_LOAD(b, a, 4, &v);
                if (b->stop) goto next;
                if (e) TRAP((uint32_t)e, a);
                b->reserved_valid = 1;
                HELD(RD(ins));
                WR(RD(ins), v);
            } else if (f5 == 0x03) {
                if (a & 3) TRAP(6, a);
                if (LOOP_PAGED && b->vm_d && (e = xlate(b, a, ACC_STORE, &pa)) != 0) {
                    if (b->stop) goto next;
                    TRAP((uint32_t)e, a);
                }
                if (b->reserved_valid) {
                    e = store_phys(b, pa, 4, x[RS2(ins)]);
                    if (b->stop) goto next;
                    if (e) TRAP((uint32_t)e, a);
                    WR(RD(ins), 0);
                } else {
                    WR(RD(ins), 1);
                }
                b->reserved_valid = 0;
            } else {
                uint32_t s = x[RS2(ins)];
                if (a & 3) TRAP(6, a);
                if (LOOP_PAGED && b->vm_d && (e = xlate(b, a, ACC_AMO, &pa)) != 0) {
                    if (b->stop) goto next;
                    TRAP((uint32_t)e, a);
                }
                e = load_phys(b, pa, 4, &v);
                if (b->stop) goto next;
                if (e) TRAP(7, a);
                e = store_phys(b, pa, 4, amo(f5, v, s));
                if (b->stop) goto next;
                if (e) TRAP((uint32_t)e, a);
                if (!io_range(pa)) {
                    b->cycles += FREEZE_AMO;
                    b->wb_hold = 1;
                }
                HELD(RD(ins));
                WR(RD(ins), v);
            }
            break;
        }
        case 0x0F:
            switch (F3(ins)) {
            case 0:
                break;
            case 1:
                bub = FENCE_I;
                break;
            case 5:
                USE(RS1(ins));
                at = later(at, b->last_mem + CSR_DRAIN) + (RS1(ins) ? DFLUSH_LINE : DFLUSH_ALL);
                break;
            default:
                TRAP(2, ins);
            }
            break;
        case 0x73: {
            uint32_t f3 = F3(ins), csr = ins >> 20, old, src, wold;
            int rd_op, wr_op;
            if (f3 == 0) {
                if (ins == 0x00000073u) TRAP(b->priv == PRIV_U ? 8 : b->priv == PRIV_S ? 9 : 11, ins);
                if (ins == 0x00100073u) TRAP(3, ins);
                if (ins == 0x30200073u || ins == 0x10200073u) {
                    int to_s = ins == 0x10200073u;
                    if (b->priv < (to_s ? PRIV_S : PRIV_M)) TRAP(2, ins);
                    npc = (to_s ? b->sepc : b->mepc) & ~1u;
                    xret(b, to_s);
                    bub = JUMP_XRET;
                    break;
                }
                if (ins == 0x10500073u) {
                    cpu_wfi_enter(b);
                    break;
                }
                if ((ins & 0xFE007FFFu) == 0x12000073u) {
                    at = later(at, b->last_issue + CSR_DRAIN);
                    tlb_flush(b);
                    bub = JUMP_REDO;
                    break;
                }
                TRAP(2, ins);
            }
            if ((f3 & 3) == 0) TRAP(2, ins);
            if (!(f3 & 4))
                USE(RS1(ins));
            at = later(at, b->last_issue + CSR_DRAIN);
            GO();
            LATE(RD(ins), LAT_MEM);
            rd_op = !((f3 & 3) == 1 && RD(ins) == 0);
            wr_op = !((f3 & 3) != 1 && RS1(ins) == 0);
            if (wr_op && csr == 0x180)
                bub = JUMP_REDO;
            if (csr_read(b, csr, &old) || b->priv < ((csr >> 8) & 3) ||
                (!rd_op && csr_read_only(csr)))
                TRAP(2, ins);
            src = (f3 & 4) ? RS1(ins) : x[RS1(ins)];
            if ((csr & 0xF00) == 0xB00 || (csr & 0xF00) == 0xC00) b->ff_block = 1;
            /* set and clear of mip and sip work on SEIP_SOFT, not the line */
            wold = (csr == 0x344 || csr == 0x144) ? (old & ~MIP_SEIP) | (b->mip_soft & MIP_SEIP) : old;
            if (wr_op) {
                b->side++;
                if (!csr_read_only(csr))
                    csr_write(b, csr, (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? wold | src : wold & ~src);
            }
            WR(RD(ins), old);
            break;
        }
        default:
            TRAP(2, ins);
        }

        b->pc = npc;
        b->instret++;
next:
        b->cycles += at - b->slot + 1;
        b->hz_rd[1] = b->hz_rd[0];
        b->hz_ready[1] = b->hz_ready[0];
        b->hz_rd[0] = (uint8_t)wrd;
        b->hz_ready[0] = at + lat;
        b->last_issue = at;
        b->slot = at + 1;
        b->fetch_ok = at + bub;
        b->redirected = bub > 1 && bub != FENCE_I;
    }
    return 0;
}
