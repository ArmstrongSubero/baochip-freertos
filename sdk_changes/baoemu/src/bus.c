/*
 * bus.c: Baochip-1x address map, guard, IRQ arrays, timers and IOX
 *
 * Copyright (c) 2026 Armstrong Subero
 * SPDX-License-Identifier: Apache-2.0
 *
 * SRAM, IFRAM and RRAM reads go straight through the page map. Everything
 * else lands here. Registers that are not modelled behave as plain storage
 * and are reported once, so a program never hangs silently on a missing
 * model without a line in the log saying where it was looking.
 *
 * The guard carries the rules of tools/emu/baochip_emu.py from the Dabao
 * SDK. A store into boot0 or boot1, any access to RRC other than the two
 * RRC_CR constants, and any access to the boot chain register region that
 * is not on the reviewed list stops the run, because on a real board each
 * of those can cost the chip. Reads of RRAM are unrestricted.
 */

#include <string.h>
#include <stdlib.h>
#include "bao_int.h"

#define RRC_CR_ADDR      0x40000000u
#define RRC_CR_NORMAL    0xFC00u
#define RRC_CR_WRITE_CMD 0xFC02u
#define RRC_LOAD_BUFFER  0x5200u
#define RRC_WRITE_BUFFER 0x9528u

#define IOX_BASE         0x5012F000u
#define PWM_BASE         0x50120000u

static uint64_t wdt_next_timeout(bao_t *b);
static void wdt_update(bao_t *b);

static const char *const irq_names[20][16] = {
    {"mdmairq", "", "", "", "pioirq0", "pioirq1", "pioirq2", "pioirq3"},
    {"usbc"},
    {"qfcirq", "mdmairq", "mbox_irq_available", "mbox_irq_abort_init", "mbox_irq_done",
     "mbox_irq_error", "", "", "", "", "", "", "", "", "", "aowkupint"},
    {"trng_done", "aes_done", "pke_done", "hash_done", "alu_done", "sdma_ichdone",
     "sdma_schdone", "sdma_xchdone"},
    {"trng_done", "aes_done", "pke_done", "hash_done", "alu_done", "sdma_ichdone",
     "sdma_schdone", "sdma_xchdone"},
    {"uart0_rx", "uart0_tx", "uart0_rx_char", "uart0_err", "uart1_rx", "uart1_tx",
     "uart1_rx_char", "uart1_err", "uart2_rx", "uart2_tx", "uart2_rx_char", "uart2_err",
     "uart3_rx", "uart3_tx", "uart3_rx_char", "uart3_err"},
    {"spim0_rx", "spim0_tx", "spim0_cmd", "spim0_eot", "spim1_rx", "spim1_tx", "spim1_cmd",
     "spim1_eot", "spim2_rx", "spim2_tx", "spim2_cmd", "spim2_eot", "spim3_rx", "spim3_tx",
     "spim3_cmd", "spim3_eot"},
    {"i2c0_rx", "i2c0_tx", "i2c0_cmd", "i2c0_eot", "i2c1_rx", "i2c1_tx", "i2c1_cmd",
     "i2c1_eot", "i2c2_rx", "i2c2_tx", "i2c2_cmd", "i2c2_eot", "i2c3_rx", "i2c3_tx",
     "i2c3_cmd", "i2c3_eot"},
    {"sdio_rx", "sdio_tx", "sdio_eot", "sdio_err", "i2s_rx", "i2s_tx", "", "", "cam_rx",
     "adc_rx", "", "", "filter_eot", "filter_act"},
    {"scif_rx", "scif_tx", "scif_rx_char", "scif_err", "spis0_rx", "spis0_tx", "spis0_eot",
     "", "spis1_rx", "spis1_tx", "spis1_eot", "", "pwm0_ev", "pwm1_ev", "pwm2_ev", "pwm3_ev"},
    {"ioxirq", "usbc", "sddcirq", "pioirq0", "pioirq1", "pioirq2", "pioirq3"},
    {"i2s_rx", "i2s_tx"},
    {"", "", "", "", "", "", "", "", "i2c0_nack", "i2c1_nack", "i2c2_nack", "i2c3_nack",
     "i2c0_err", "i2c1_err", "i2c2_err", "i2c3_err"},
    {"coresuberr", "sceerr", "ifsuberr", "secirq"},
    {"uart2_rx", "uart2_tx", "uart2_rx_char", "uart2_err", "uart3_rx", "uart3_tx",
     "uart3_rx_char", "uart3_err", "trng_done"},
    {"sec0"},
    {"cam_rx", "i2s_rx", "i2s_tx", "", "spim1_rx", "spim1_tx", "spim1_cmd", "spim1_eot",
     "spim2_rx", "spim2_tx", "spim2_cmd", "spim2_eot", "i2c0_rx", "i2c0_tx", "i2c0_cmd",
     "i2c0_eot"},
    {"i2c1_rx", "i2c1_tx", "i2c1_cmd", "i2c1_eot", "pioirq0", "pioirq1", "pioirq2", "pioirq3",
     "qfcirq", "adc_rx", "ioxirq", "sddcirq"},
    {"pioirq0", "pioirq1", "pioirq2", "pioirq3", "i2c2_rx", "i2c2_tx", "i2c2_cmd", "i2c2_eot",
     "i2c0_nack", "i2c1_nack", "i2c2_nack", "i2c0_err", "i2c1_err", "i2c2_err", "ioxirq",
     "cam_rx"},
    {"mbox_irq_available", "mbox_irq_abort_init", "mbox_irq_done", "mbox_irq_error",
     "pioirq0", "pioirq1", "pioirq2", "pioirq3", "sdio_rx", "sdio_tx", "sdio_eot", "sdio_err"},
};

static const uint32_t irq_bases[20] = {
    0xE0004000, 0xE0005000, 0xE0010000, 0xE0011000, 0xE0012000,
    0xE0013000, 0xE0014000, 0xE0015000, 0xE0016000, 0xE0017000,
    0xE0006000, 0xE0007000, 0xE0008000, 0xE0009000, 0xE000A000,
    0xE000B000, 0xE000C000, 0xE000D000, 0xE000E000, 0xE000F000,
};

/* banks 2 to 13 are also the BIO dmareq inputs (soc_ifsub.sv 359, vexsys.sv 397) */
/* banks 2 to 13 also reach the MDMA request select as line 16a+i
 * (mdma.sv 111, soc_coresub.sv 571); banks 0 and 1 carry no raw lines */
void irq_raise(bao_t *b, const char *ev)
{
    int a, i, n = 0, lines[8];
    for (a = 0; a < 20; a++)
        for (i = 0; i < 16; i++)
            if (irq_names[a][i] && irq_names[a][i][0] && strcmp(irq_names[a][i], ev) == 0) {
                b->irq[a].pending |= 1u << i;
                if (a >= 2 && a <= 13) {
                    bio_dmareq(b, (a - 2) * 16 + i);
                    if (n < 8)
                        lines[n++] = a * 16 + i;
                }
            }
    bao_kick(b);
    for (i = 0; i < n; i++)
        mdma_event(b, lines[i]);
}

/* a level source: in level mode pending follows the line, in edge mode it
 * takes the rising or falling edge the polarity picks (cram_axi.sv 6064) */
void irq_level(bao_t *b, const char *ev, int was, int now)
{
    int a, i;
    for (a = 0; a < 20; a++)
        for (i = 0; i < 16; i++)
            if (irq_names[a][i] && irq_names[a][i][0] && strcmp(irq_names[a][i], ev) == 0) {
                irqarr_t *r = &b->irq[a];
                uint32_t bit = 1u << i;
                if (r->edge & bit) {
                    if ((r->polarity & bit) ? (!was && now) : (was && !now))
                        r->pending |= bit;
                } else if (now) {
                    r->pending |= bit;
                }
                if (now && !was && a >= 2 && a <= 13)
                    mdma_event(b, a * 16 + i);
            }
    bao_kick(b);
}

static void irq_relevel(bao_t *b, int a)
{
    uint32_t lines = bio_irq_lines(b);
    int i;
    for (i = 0; lines && i < 16; i++) {
        const char *n = irq_names[a][i];
        if (n && strncmp(n, "pioirq", 6) == 0 && !(b->irq[a].edge & (1u << i)) && (lines >> (n[6] - '0')) & 1)
            b->irq[a].pending |= 1u << i;
    }
    if (usb_irq_line(b))
        for (i = 0; i < 16; i++) {
            const char *n = irq_names[a][i];
            if (n && strcmp(n, "usbc") == 0 && !(b->irq[a].edge & (1u << i)))
                b->irq[a].pending |= 1u << i;
        }
}

static uint64_t tt_ms(bao_t *b)
{
    return b->tt_base_ms + (b->cycles - b->tt_base_cycle) / b->tt_cpt;
}

static void tt_rebase(bao_t *b)
{
    b->tt_base_ms = tt_ms(b);
    b->tt_base_cycle = b->cycles;
}

/*
 * The msleep alarm is a level (bao_core.py, TickTimer: EventSourceLevel with
 * trigger msleep_target <= time). It stays raised until a later target is
 * written or the event is masked, and writing EV_PENDING does not clear it.
 * The short lockout after a target write is not modelled.
 */
static int tt_alarm_level(bao_t *b)
{
    return tt_ms(b) >= b->tt_target;
}

static uint64_t tt_alarm_cycle(bao_t *b)
{
    if (tt_alarm_level(b))
        return NEVER;
    return b->tt_base_cycle + (b->tt_target - b->tt_base_ms) * b->tt_cpt;
}

/* Timer0 counts on clkvex, which stops while the core is gated in WFI,
 * so its times are kept in vex_now() cycles rather than b->cycles. */
static uint32_t t0_value(bao_t *b)
{
    uint64_t el, period;
    if (!b->t0_en)
        return b->t0_load;
    el = vex_now(b) - b->t0_start;
    if (el <= b->t0_start_val)
        return b->t0_start_val - (uint32_t)el;
    if (b->t0_reload == 0)
        return 0;
    period = (uint64_t)b->t0_reload + 1;
    el -= (uint64_t)b->t0_start_val + 1;
    return b->t0_reload - (uint32_t)(el % period);
}

static uint64_t t0_next_zero(bao_t *b, uint64_t after)
{
    uint64_t first, period, k;
    if (!b->t0_en)
        return NEVER;
    first = b->t0_start + b->t0_start_val;
    if (first > after)
        return first;
    if (b->t0_reload == 0)
        return NEVER;
    period = (uint64_t)b->t0_reload + 1;
    k = (after - first) / period + 1;
    return first + k * period;
}

uint32_t soc_irq_lines(bao_t *b)
{
    uint32_t l = 0;
    int a;
    for (a = 0; a < 20; a++)
        if (b->irq[a].pending & b->irq[a].enable)
            l |= 1u << a;
    if (tt_alarm_level(b) && b->tt_enable)
        l |= 1u << 20;
    if (b->t0_pending & b->t0_enable)
        l |= 1u << 30;
    return l | mbox_irq_lines(b);
}

static void pd_update(bao_t *b);
static uint64_t pd_next_event(bao_t *b);
static int pmu_read(bao_t *b, uint32_t a, uint32_t *v);
static int pmu_write(bao_t *b, uint32_t a, uint32_t v);

void soc_update(bao_t *b)
{
    uint64_t n = NEVER, t;

    if (b->sw_reset) {
        b->sw_reset = 0;
        bao_logf(b, BAO_LOG_WARN, "software reset through RCURST");
        cpu_reset(b);
        soc_reset(b);
    }

    t = tt_alarm_cycle(b);
    if (t < n) n = t;

    if (b->t0_en) {
        uint64_t vn = vex_now(b);
        t = t0_next_zero(b, b->t0_seen);
        while (t != NEVER && t <= vn) {
            b->t0_pending = 1;
            b->t0_seen = t;
            t = t0_next_zero(b, t);
        }
        if (t != NEVER && !(b->wfi && b->wfi_gate) && b->cycles + (t - vn) < n)
            n = b->cycles + (t - vn);
    }

    pd_update(b);
    t = pd_next_event(b);
    if (t < n) n = t;

    t = sce_next_event(b);
    if (t < n) n = t;

    udma_update(b);
    t = udma_next_event(b);
    if (t < n) n = t;

    wdt_update(b);
    t = wdt_next_timeout(b);
    if (t < n) n = t;

    pwm_update(b);
    t = pwm_next_event(b);
    if (t < n) n = t;

    usb_update(b);
    t = usb_next_event(b);
    if (t < n) n = t;

    if (n <= b->cycles)
        n = b->cycles + 1;
    b->next_event = n;
}

void bao_kick(bao_t *b)
{
    b->limit = 0;
    b->next_event = b->cycles;
}

uint32_t *bao_mmio_slot(bao_t *b, uint32_t addr)
{
    uint32_t pg = addr >> 16;
    if (!b->mmio_pages[pg]) {
        b->mmio_pages[pg] = calloc(0x10000 / 4, 4);
        if (!b->mmio_pages[pg])
            return NULL;
    }
    return &b->mmio_pages[pg][(addr & 0xFFFF) >> 2];
}

/* one bit per register word; the low 20 bits of the word address cover
 * every peripheral block without two registers sharing a bit */
int bao_first_time(bao_t *b, uint32_t addr)
{
    uint32_t k = (addr >> 2) & 0xFFFFFu;
    if (b->warned[k >> 3] & (1u << (k & 7)))
        return 0;
    b->warned[k >> 3] |= (uint8_t)(1u << (k & 7));
    return 1;
}

/* the guard's notes take the bit of the word 2 MB up (address bit 21 is
 * clear in every peripheral block), so a reviewed register that is also
 * acting as storage still gets its own warning */
static void note_once(bao_t *b, uint32_t addr, const char *what)
{
    if (bao_first_time(b, addr ^ (1u << 21)))
        bao_logf(b, BAO_LOG_WARN, "%s 0x%08X at %s", what, addr, bao_where(b, b->pc));
}

static void warn_once(bao_t *b, uint32_t addr, const char *what)
{
    if (bao_first_time(b, addr))
        bao_logf(b, BAO_LOG_WARN, "%s 0x%08X at %s (not modelled, acting as storage)",
                 what, addr, bao_where(b, b->pc));
}

static int reviewed_cgu(uint32_t a, uint32_t v, int w)
{
    if (a == 0x40040000u)
        return !w || !(v & 0x1000u);
    if (w) {
        if (a == 0x40040014u && (v == 0x070001FFu || v == 0x0700017Fu)) return 1;
        if (a == 0x4004002Cu && v == 0x32u) return 1;
        if (a == 0x40040034u) return 1;
        if (a == 0x40040080u && v == 0x55AAu) return 1;
        if (a == 0x40040010u && (v == 0 || v == 1)) return 1;
        if (a == 0x40040090u && v == 0x32u) return 1;
        if (a == 0x40040098u) return 1;
        if (a == 0x400400A8u && (v & 0x7F00u) == 0x2300u) return 1;
        return 0;
    }
    switch (a) {
    case 0x40040010u: case 0x40040034u: case 0x40040040u: case 0x4004004Cu:
    case 0x40040050u: case 0x40040098u: case 0x400400A0u: case 0x400400A8u:
        return 1;
    default:
        return 0;
    }
}

int guard_check(bao_t *b, uint32_t a, int size, uint32_t v, int w)
{
    const char *kind = w ? "write" : "read";

    if (!b->guard_on)
        return 0;
    if (a < 0x40010000u) {
        if (w && a == RRC_CR_ADDR && size == 4 && (v == RRC_CR_NORMAL || v == RRC_CR_WRITE_CMD))
            return 0;
        bao_halt(b, BAO_STOP_GUARD, "RRC touched: %s 0x%08X at %s", kind, a, bao_where(b, b->pc));
        return 1;
    }
    if (a >= 0x40040000u && a < 0x40041000u) {
        if (size == 4 && reviewed_cgu(a, v, w)) {
            note_once(b, a, w ? "reviewed CGU write" : "reviewed CGU read");
            return 0;
        }
        bao_halt(b, BAO_STOP_GUARD, "CGU/sysctrl touched: %s 0x%08X (0x%08X) at %s",
                 kind, a, v, bao_where(b, b->pc));
        return 1;
    }
    if ((a >= 0x40041000u && a < 0x40042000u) || (a >= 0x40043000u && a < 0x40044000u) ||
        (a >= 0x40060000u && a < 0x40062000u) || (a >= 0x40063000u && a < 0x40064000u)) {
        note_once(b, a, w ? "reviewed boot chain block write" : "reviewed boot chain block read");
        return 0;
    }
    if (a >= 0x40040000u && a < 0x40080000u) {
        bao_halt(b, BAO_STOP_GUARD, "boot chain register region touched: %s 0x%08X at %s",
                 kind, a, bao_where(b, b->pc));
        return 1;
    }
    return 0;
}

static void rram_store(bao_t *b, uint32_t a, int size, uint32_t v)
{
    uint32_t page = a & ~31u;
    int i;

    if (a < BOOT_END) {
        if (b->guard_on) {
            bao_halt(b, BAO_STOP_GUARD, "store into boot0/boot1: 0x%08X <= 0x%X at %s",
                     a, v, bao_where(b, b->pc));
            return;
        }
    }
    if (a >= SECURITY_BASE && b->guard_on) {
        bao_halt(b, BAO_STOP_GUARD, "store into security region: 0x%08X at %s",
                 a, bao_where(b, b->pc));
        return;
    }
    if ((b->rrc_cr & 0xFF) == 0) {
        if (b->rrc_staged && page != b->rrc_page) {
            if (b->guard_on) {
                bao_halt(b, BAO_STOP_GUARD, "staging store crosses pages: 0x%08X after page 0x%08X at %s",
                         a, b->rrc_page, bao_where(b, b->pc));
                return;
            }
            memset(b->rrc_mask, 0, sizeof(b->rrc_mask));
        }
        b->rrc_staged = 1;
        b->rrc_page = page;
        for (i = 0; i < size; i++) {
            b->rrc_buf[(a & 31) + i] = (uint8_t)(v >> (8 * i));
            b->rrc_mask[(a & 31) + i] = 1;
        }
        return;
    }
    if (size != 4 || (v != RRC_LOAD_BUFFER && v != RRC_WRITE_BUFFER)) {
        bao_halt(b, BAO_STOP_GUARD, "unexpected store in RRC write mode: 0x%08X <= 0x%X at %s",
                 a, v, bao_where(b, b->pc));
        return;
    }
    if (!b->rrc_staged || page != b->rrc_page) {
        bao_halt(b, BAO_STOP_GUARD, "RRC commit aimed at 0x%08X but nothing staged there, at %s",
                 a, bao_where(b, b->pc));
        return;
    }
    if (v == RRC_WRITE_BUFFER) {
        for (i = 0; i < 32; i++)
            if (b->rrc_mask[i])
                b->rram[(page - RRAM_BASE) + (uint32_t)i] = b->rrc_buf[i];
        memset(b->rrc_mask, 0, sizeof(b->rrc_mask));
        b->rrc_staged = 0;
        b->rrc_commits++;
    }
}

static uint32_t bio_sel(bao_t *b, int p)
{
    uint32_t sel = b->iox[0x200 >> 2];
    if (p == 1) return sel & 0xFFFF;
    if (p == 2) return sel >> 16;
    return 0;
}

static uint32_t bio_bits(uint32_t v, int p)
{
    return p == 1 ? (v & 0xFFFF) : p == 2 ? (v >> 16) : 0;
}

uint32_t gpio_eff_out(bao_t *b, int p)
{
    uint32_t sel = bio_sel(b, p);
    uint32_t v = ((b->iox[(0x130 >> 2) + p] & ~sel) | (bio_bits(b->bio_out, p) & sel)) & 0xFFFF;
    int pin, lvl;
    for (pin = 0; pin < 4; pin++)
        if (pwm_pin(b, p, pin, &lvl))
            v = (v & ~(1u << pin)) | ((uint32_t)lvl << pin);
    return v;
}

uint32_t gpio_eff_oe(bao_t *b, int p)
{
    uint32_t sel = bio_sel(b, p);
    uint32_t v = ((b->iox[(0x148 >> 2) + p] & ~sel) | (bio_bits(b->bio_oe, p) & sel)) & 0xFFFF;
    int pin, lvl;
    for (pin = 0; pin < 4; pin++)
        if (pwm_pin(b, p, pin, &lvl))
            v |= 1u << pin;
    return v;
}

static uint32_t gpio_in(bao_t *b, int p);

static void iox_int_check(bao_t *b)
{
    int i;
    if (!(b->iox[0x100 >> 2] | b->iox[0x104 >> 2] | b->iox[0x108 >> 2] | b->iox[0x10C >> 2] |
          b->iox[0x110 >> 2] | b->iox[0x114 >> 2] | b->iox[0x118 >> 2] | b->iox[0x11C >> 2]))
        return;
    for (i = 0; i < 8; i++) {
        uint32_t cr = b->iox[(0x100 >> 2) + i], sel = IOX_INT_SEL(cr), mode = IOX_INT_MODE(cr);
        int lvl = sel < 96 ? (int)((gpio_in(b, (int)(sel >> 4)) >> (sel & 15)) & 1) : 0;
        int prev = (b->iox_last >> i) & 1;
        int fire = 0;
        if (cr & IOX_INT_EN) {
            if (mode == 0) fire = lvl && !prev;
            else if (mode == 1) fire = !lvl && prev;
            else if (mode == 2) fire = lvl;
            else fire = !lvl;
        }
        if (lvl) b->iox_last |= 1u << i; else b->iox_last &= ~(1u << i);
        if (fire && !(b->iox_fr & (1u << i))) {
            b->iox_fr |= 1u << i;
            irq_raise(b, "ioxirq");
        } else if (fire && mode >= 2) {
            irq_raise(b, "ioxirq");
        }
    }
}

void gpio_notify(bao_t *b)
{
    int p;
    iox_int_check(b);
    dev_gpio_changed(b);
    for (p = 0; p < 6; p++) {
        uint32_t out = gpio_eff_out(b, p);
        uint32_t oe = gpio_eff_oe(b, p);
        if (out != b->gpio_last_out[p] || oe != b->gpio_last_oe[p]) {
            b->gpio_last_out[p] = out;
            b->gpio_last_oe[p] = oe;
            if (b->gpio_cb)
                b->gpio_cb(b->gpio_user, p, out, oe);
        }
    }
}

/* width of each IOX SFR, 0 where nothing decodes (iox.sv 75 to 259) */
static uint32_t iox_mask(uint32_t off)
{
    if (off < 0x030) return 0xFFFFu;
    if (off >= 0x100 && off < 0x120) return 0x7FFu;
    if (off == 0x120) return 0xFFu;
    if (off >= 0x130 && off < 0x190) return 0xFFFFu;
    if (off == 0x200) return 0xFFFFFFFFu;
    if (off >= 0x230 && off < 0x260) return 0xFFFFu;
    if (off >= 0x260 && off < 0x278) return 0xFFFFFFFFu;
    return 0;
}

static uint32_t gpio_in(bao_t *b, int p)
{
    uint32_t out = gpio_eff_out(b, p), oe = gpio_eff_oe(b, p);
    uint32_t pu = b->iox[(0x160 >> 2) + p];
    uint32_t ext = (b->gpio_ext[p] & b->gpio_ext_set[p]) | (pu & ~b->gpio_ext_set[p]);
    return ((out & oe) | (ext & ~oe)) & 0xFFFF;
}

static int cpu_periph_read(bao_t *b, uint32_t a, uint32_t *v)
{
    uint32_t off = a & 0xFFF;
    int i;

    if (a >= 0xE0000000u && a < 0xE0001000u) {
        b->ff_hbdep = 1;
        if (off == 0) *v = b->hb_count;
        else if (off == 4) *v = b->hb_count ? (uint32_t)((b->cycles / b->hb_count) & 1) : 0;
        else *v = 0;
        return 0;
    }
    for (i = 0; i < 20; i++) {
        if ((a & ~0xFFFu) == irq_bases[i]) {
            irqarr_t *r = &b->irq[i];
            switch (off) {
            case 0x00: *v = r->soft; break;
            case 0x04: *v = r->edge; break;
            case 0x08: *v = r->polarity; break;
            case 0x0C: *v = 0; break;
            case 0x10: *v = r->pending; break;
            case 0x14: *v = r->enable; break;
            default: *v = 0;
            }
            return 0;
        }
    }
    if ((a & ~0xFFFu) == 0xE001B000u) {
        uint64_t ms = tt_ms(b);
        b->ff_ttdep = 1;
        switch (off) {
        case 0x00: *v = 0; break;
        case 0x04: *v = (uint32_t)(ms >> 32); break;
        case 0x08: *v = (uint32_t)ms; break;
        case 0x0C: *v = b->tt_target1; break;
        case 0x10: *v = b->tt_target0; break;
        case 0x14: *v = tt_alarm_level(b); break;
        case 0x18: *v = tt_alarm_level(b); break;
        case 0x1C: *v = b->tt_enable; break;
        case 0x20: *v = b->tt_cpt; break;
        default: *v = 0;
        }
        return 0;
    }
    if ((a & ~0xFFFu) == 0xE001C000u) {
        if (off == 0x14) b->ff_block = 1;
        switch (off) {
        case 0x00: *v = b->t0_load; break;
        case 0x04: *v = b->t0_reload; break;
        case 0x08: *v = b->t0_en; break;
        case 0x0C: *v = 0; break;
        case 0x10: *v = b->t0_latched; break;
        case 0x14: *v = t0_value(b) == 0; break;
        case 0x18: *v = b->t0_pending; break;
        case 0x1C: *v = b->t0_enable; break;
        default: *v = 0;
        }
        return 0;
    }
    return -1;
}

static int cpu_periph_write(bao_t *b, uint32_t a, uint32_t v)
{
    uint32_t off = a & 0xFFF;
    int i;

    if (a >= 0xE0000000u && a < 0xE0001000u) {
        if (off == 0) b->hb_count = v;
        return 0;
    }
    for (i = 0; i < 20; i++) {
        if ((a & ~0xFFFu) == irq_bases[i]) {
            irqarr_t *r = &b->irq[i];
            switch (off) {
            case 0x00: r->soft = v & 0xFFFF; r->pending |= v & 0xFFFF; break;
            case 0x04: r->edge = v & 0xFFFF; break;
            case 0x08: r->polarity = v & 0xFFFF; break;
            case 0x10: r->pending &= ~v; irq_relevel(b, i); break;
            case 0x14: r->enable = v & 0xFFFF; break;
            default: break;
            }
            bao_kick(b);
            return 0;
        }
    }
    if ((a & ~0xFFFu) == 0xE001B000u) {
        switch (off) {
        case 0x00:
            if (v & 1) { b->tt_base_ms = 0; b->tt_base_cycle = b->cycles; }
            break;
        case 0x0C: b->tt_target1 = v; b->tt_target = ((uint64_t)v << 32) | b->tt_target0; break;
        case 0x10: b->tt_target0 = v; b->tt_target = ((uint64_t)b->tt_target1 << 32) | v; break;
        case 0x18: break;
        case 0x1C: b->tt_enable = v & 1; break;
        case 0x20:
            if (v) { tt_rebase(b); b->tt_cpt = v; }
            break;
        default: break;
        }
        bao_kick(b);
        return 0;
    }
    if ((a & ~0xFFFu) == 0xE001C000u) {
        switch (off) {
        case 0x00: b->t0_load = v; break;
        case 0x04: b->t0_reload = v; break;
        case 0x08:
            if ((v & 1) && !b->t0_en) {
                uint64_t vn = vex_now(b);
                b->t0_en = 1;
                b->t0_start = vn;
                b->t0_start_val = b->t0_load;
                b->t0_seen = vn ? vn - 1 : 0;
            } else if (!(v & 1)) {
                b->t0_en = 0;
            }
            break;
        case 0x0C: b->t0_latched = t0_value(b); break;
        case 0x18: if (v & 1) b->t0_pending = 0; break;
        case 0x1C: b->t0_enable = v & 1; break;
        default: break;
        }
        bao_kick(b);
        return 0;
    }
    return -1;
}

#define WDT_TICK 8u

static uint64_t wdt_next_timeout(bao_t *b)
{
    if (!(b->wdt_ctrl & 1))
        return NEVER;
    return b->wdt_base + ((uint64_t)b->wdt_load + 1) * WDT_TICK;
}

static uint32_t wdt_value(bao_t *b)
{
    uint64_t el;
    if (!(b->wdt_ctrl & 1))
        return b->wdt_load;
    el = (b->cycles - b->wdt_base) / WDT_TICK;
    return el >= b->wdt_load ? 0 : b->wdt_load - (uint32_t)el;
}

static void wdt_update(bao_t *b)
{
    while ((b->wdt_ctrl & 1) && b->cycles >= wdt_next_timeout(b)) {
        b->wdt_base = wdt_next_timeout(b);
        if (b->wdt_ris && (b->wdt_ctrl & 2)) {
            b->wdt_resets++;
            bao_logf(b, BAO_LOG_WARN, "watchdog reset (%u so far)", b->wdt_resets);
            cpu_reset(b);
            soc_reset(b);
            return;
        }
        b->wdt_ris = 1;
    }
}

/*
 * RTC: ARM PL031 at 0x40061000 counting rising edges of clk1hz, which
 * ao_sysctrl.sv 493 to 498 makes by toggling a register every CR_CLK1HZFD
 * plus 1 cycles of clk32k, so the count rate is clk32k / (2 * (FD + 1)).
 * The Dabao has no 32 kHz crystal; clk32k is the internal RC oscillator,
 * measured on the board at about 23,970 Hz (749 ticks/s at FD 15, 1198 at
 * FD 9, 5994 at FD 1, 120 at FD 99). FD 0 measured 8382 and 8337, about
 * 0.70 of the formula, and is modelled as that measured rate. FD is 14
 * bits (cr_clk1hzfd, reset 0x3FFF). The block is in the always on domain,
 * reset only at power on, so a watchdog or soft reset leaves it running.
 */
#define RTC_OSC_HZ      23970u
#define RTC_FD0_HZ      8360u

static uint64_t rtc_ticks(bao_t *b, uint64_t dc)
{
    if (b->rtc_fd == 0)
        return dc * RTC_FD0_HZ / BAO_ACLK_HZ;
    return dc * RTC_OSC_HZ / ((uint64_t)BAO_ACLK_HZ * 2u * (b->rtc_fd + 1u));
}

static uint32_t rtc_raw(bao_t *b)
{
    if (!(b->rtc_cr & 1))
        return b->rtc_frozen;
    return b->rtc_lr + (uint32_t)rtc_ticks(b, b->cycles - b->rtc_base);
}

static uint32_t rtc_value(bao_t *b)
{
    uint32_t v = rtc_raw(b);
    if (v != b->rtc_seen) {
        if ((uint32_t)(b->rtc_mr - b->rtc_seen - 1u) < (uint32_t)(v - b->rtc_seen))
            b->rtc_ris = 1;
        b->rtc_seen = v;
    }
    return v;
}

/* the first cycle at which the RTC count reaches its match register */
static uint64_t rtc_match_cycle(bao_t *b)
{
    uint64_t k, per;
    if (!(b->rtc_cr & 1) || b->rtc_fd == 0)
        return NEVER;
    rtc_value(b);
    k = (uint32_t)(b->rtc_mr - b->rtc_lr);
    if (k == 0)
        k = 1ull << 32;
    if (k <= rtc_ticks(b, b->cycles - b->rtc_base))
        return b->cycles;
    per = (uint64_t)BAO_ACLK_HZ * 2u * (b->rtc_fd + 1u);
    return b->rtc_base + (k * per + RTC_OSC_HZ - 1u) / RTC_OSC_HZ;
}

static void rtc_rebase(bao_t *b)
{
    uint32_t v = rtc_value(b);
    b->rtc_lr = v;
    b->rtc_frozen = v;
    b->rtc_base = b->cycles;
}

/*
 * ATimer: the PULP apb_timer_unit (pulp_soc apb_timer_unit.sv), twice.
 * Counters 0 and 1 are the lo and hi timers at 0x40043000 in sysctrl,
 * clocked from pclk (aclk / 8) with the 48 MHz crystal divided to 48 kHz
 * as ref_clk (soc_top.sv ref32k; the board measured 48,000 Hz). Counters
 * 2 and 3 are the always on instance at 0x40063000 (aoperi.sv), which the
 * board measured at 21,875,061 Hz (aclk / 16); its ref_clk is clk1k, the
 * RC clk32k gated once every 32 cycles, about 23,970 / 32 Hz.
 *
 * CFG bits (apb_timer_unit.sv 23 to 33): 0 enable, 1 reset (self
 * clearing), 2 irq, 4 compare and clear, 5 one shot, 6 prescaler enable,
 * 7 ref clock, 15:8 prescaler compare, 31 64 bit mode on the lo CFG. A
 * prescaler of P ticks the counter every P + 1 clock edges. With compare
 * and clear the counter reaches CMP and clears on the next HCLK cycle, so
 * the period is CMP + 1 ticks when it counts every HCLK and CMP ticks when
 * it counts on the ref clock or through the prescaler. In 64 bit mode the
 * hi counter counts each time lo goes past 0xFFFFFFFF, and lo's clock
 * and prescaler set the rate. Writing CNT loads the counter, any write to
 * START sets enable and any write to RESET clears the counter.
 */
static uint64_t at_hclk_div(int i) { return i < 2 ? 8u : 16u; }

static uint64_t at_ticks(bao_t *b, int i, uint64_t dc)
{
    uint32_t cfg = b->at_cfg[i];
    uint64_t div = (cfg & 0x40) ? ((cfg >> 8) & 0xFFu) + 1u : 1u;
    if (cfg & 0x80) {
        uint64_t num = i < 2 ? 48000u : 23970u;
        uint64_t den = (uint64_t)BAO_ACLK_HZ * (i < 2 ? 1u : 32u);
        return dc * num / den / div;
    }
    return dc / at_hclk_div(i) / div;
}

static int at_pair64(bao_t *b, int i)
{
    int lo = i & ~1;
    return (b->at_cfg[lo] & 0x80000000u) != 0;
}

static uint32_t at_value(bao_t *b, int i)
{
    uint32_t cfg = b->at_cfg[i], cmp = b->at_cmp[i];
    uint64_t n, v, per;
    int lo = i & ~1;

    if (at_pair64(b, i)) {
        uint32_t lcfg = b->at_cfg[lo];
        uint64_t v64;
        if (!(lcfg & 1))
            return b->at_base_val[i];
        n = at_ticks(b, lo, b->cycles - b->at_base[lo]);
        v64 = (((uint64_t)b->at_base_val[lo + 1] << 32) | b->at_base_val[lo]) + n;
        return i == lo ? (uint32_t)v64 : (uint32_t)(v64 >> 32);
    }
    if (!(cfg & 1))
        return b->at_base_val[i];
    n = at_ticks(b, i, b->cycles - b->at_base[i]);
    v = (uint64_t)b->at_base_val[i] + n;
    if (cfg & 0x20) {
        if (v >= cmp) {
            b->at_cfg[i] &= ~1u;
            b->at_base_val[i] = (cfg & 0x10) ? 0 : cmp;
            return b->at_base_val[i];
        }
        return (uint32_t)v;
    }
    if ((cfg & 0x10) && v >= cmp && b->at_base_val[i] <= cmp) {
        uint64_t first = cmp - b->at_base_val[i];
        per = (cfg & 0xC0) ? (uint64_t)cmp : (uint64_t)cmp + 1u;
        if (per == 0) return 0;
        return (uint32_t)((n - first) % per);
    }
    return (uint32_t)v;
}

static void at_freeze(bao_t *b, int i)
{
    int lo = i & ~1;
    uint32_t vl = at_value(b, lo), vh = at_value(b, lo + 1);
    b->at_base_val[lo] = vl;
    b->at_base_val[lo + 1] = vh;
    b->at_base[lo] = b->at_base[lo + 1] = b->cycles;
}

static int at_index(uint32_t a)
{
    if (a >= 0x40043000u && a < 0x40043028u) return (int)((a >> 2) & 1);
    if (a >= 0x40063000u && a < 0x40063028u) return 2 + (int)((a >> 2) & 1);
    return -1;
}

static int ao_read(bao_t *b, uint32_t a, uint32_t *v)
{
    if (at_index(a) >= 0) {
        int i = at_index(a);
        switch (a & 0x38) {
        case 0x00: at_value(b, i); *v = b->at_cfg[i]; return 0;
        case 0x08: *v = at_value(b, i); return 0;
        case 0x10: *v = b->at_cmp[i]; return 0;
        default: *v = 0; return 0;
        }
    }
    if (a >= 0x40041000u && a < 0x40042000u) {
        switch (a & 0xFFF) {
        case 0x000: *v = b->wdt_load; return 0;
        case 0x004: *v = wdt_value(b); return 0;
        case 0x008: *v = b->wdt_ctrl; return 0;
        case 0x010: *v = b->wdt_ris; return 0;
        case 0x014: *v = b->wdt_ris & b->wdt_ctrl & 1; return 0;
        case 0xC00: *v = (uint32_t)b->wdt_locked; return 0;
        default: return -1;
        }
    }
    if (a == 0x40060004u) {
        *v = b->rtc_fd;
        return 0;
    }
    if (a >= 0x40060000u && a < 0x40061000u)
        return pmu_read(b, a, v);
    if (a >= 0x40061000u && a < 0x40062000u) {
        switch (a & 0xFFF) {
        case 0x000: *v = rtc_value(b); return 0;
        case 0x004: *v = b->rtc_mr; return 0;
        case 0x008: *v = b->rtc_lr; return 0;
        case 0x00C: *v = b->rtc_cr & 1; return 0;
        case 0x010: *v = b->rtc_imsc & 1; return 0;
        case 0x014: rtc_value(b); *v = b->rtc_ris; return 0;
        case 0x018: rtc_value(b); *v = b->rtc_ris & b->rtc_imsc & 1; return 0;
        default: *v = 0; return 0;
        }
    }
    return -1;
}

static int ao_write(bao_t *b, uint32_t a, uint32_t v)
{
    if (at_index(a) >= 0) {
        int i = at_index(a);
        at_freeze(b, i);
        switch (a & 0x38) {
        case 0x00:
            b->at_cfg[i] = v & ~2u;
            if (v & 2) b->at_base_val[i] = 0;
            break;
        case 0x08: b->at_base_val[i] = v; break;
        case 0x10: b->at_cmp[i] = v; break;
        case 0x18: b->at_cfg[i] |= 1; break;
        case 0x20: b->at_base_val[i] = 0; break;
        default: break;
        }
        return 0;
    }
    if (a >= 0x40041000u && a < 0x40042000u) {
        if ((a & 0xFFF) == 0xC00) { b->wdt_locked = v != 0x1ACCE551u; return 0; }
        if (b->wdt_locked) return 0;
        switch (a & 0xFFF) {
        case 0x000: b->wdt_load = v; b->wdt_base = b->cycles; break;
        case 0x008:
            if ((v & 1) && !(b->wdt_ctrl & 1)) b->wdt_base = b->cycles;
            b->wdt_ctrl = v & 3;
            break;
        case 0x00C: b->wdt_ris = 0; b->wdt_base = b->cycles; break;
        default: return -1;
        }
        bao_kick(b);
        return 0;
    }
    if (a == 0x40060004u) {
        rtc_rebase(b);
        b->rtc_fd = v & 0x3FFFu;
        return 0;
    }
    if (a >= 0x40060000u && a < 0x40061000u)
        return pmu_write(b, a, v);
    if (a >= 0x40061000u && a < 0x40062000u) {
        switch (a & 0xFFF) {
        case 0x004:
            rtc_value(b);
            b->rtc_mr = v;
            return 0;
        case 0x008:
            rtc_value(b);
            b->rtc_lr = v;
            b->rtc_frozen = v;
            b->rtc_base = b->cycles;
            b->rtc_seen = v;
            return 0;
        case 0x00C:
            if ((v & 1) && !(b->rtc_cr & 1)) { b->rtc_lr = b->rtc_frozen; b->rtc_base = b->cycles; }
            if (!(v & 1) && (b->rtc_cr & 1)) b->rtc_frozen = rtc_value(b);
            b->rtc_cr = v & 1;
            return 0;
        case 0x010: b->rtc_imsc = v & 1; return 0;
        case 0x01C: if (v & 1) { rtc_value(b); b->rtc_ris = 0; } return 0;
        default: return 0;
        }
    }
    return -1;
}

/*
 * Power down (ao_sysctrl.sv 379 to 404 and 573 to 610). The PMU block at
 * 0x40060000 keeps its registers through every reset but power on and
 * the RESET pin. Writing 0x5A to PDAR (+0x44) starts a state machine on
 * clk32k that sets aopdreg two clk32k cycles after the write reaches it,
 * which holds the SoC in reset (socresetnin) and switches the PMU to the
 * PMUCRPD regulator settings. aopdreg is cleared, and held clear, while
 * any wake source is high and unmasked: WKUPMASK (+0x08) bits 17:8 mask
 * {PF0 low, PF1 low, 0, 0, keypad, keypad, AO watchdog interrupt, AO
 * ATimer, RTC interrupt, AO watchdog reset} from bit 17 down. When it
 * clears, the SoC reset is released 312 clk32k cycles later and the chip
 * boots again. A wake source already high when PDAR is written means the
 * chip never powers down and the program carries on.
 *
 * On the Dabao the 0.85 V buck feeds VDD85D beside the internal regulator
 * (dabao_v3c schematic, U4), so power down holds the core in reset with
 * its supply still on; here that is the SoC held in reset. Modelled wake
 * sources are the RTC interrupt (RIS and IMSC) and PF1, which reads low
 * on the board (AO_FR 0x100, PF1 is tied to ground under the chip); PF0
 * reads high. AO_FR latches the sources as flags that a write of 1
 * clears (apb_sfr.sv apb_sfr2), and the AO watchdog, AO
 * ATimer and keypad sources are not modelled.
 */
#define CLK32K_CYC ((uint64_t)BAO_ACLK_HZ / RTC_OSC_HZ)

void ao_por(bao_t *b)
{
    b->ao_cr = 0x6;
    b->ao_wkup = 0;
    b->ao_rstcrmask = 0x1F;
    b->ao_pmucrpd = 0x7C;
    b->ao_fr = 0;
    b->pd_state = 0;
    b->pd_at = NEVER;
}

static uint32_t ao_wake_sources(bao_t *b)
{
    uint32_t s = 1u << 8;
    rtc_value(b);
    if (b->rtc_ris & b->rtc_imsc & 1)
        s |= 1u << 1;
    return s;
}

static uint32_t ao_wake_live(bao_t *b)
{
    return ao_wake_sources(b) & ~(b->ao_wkup >> 8) & 0x3FFu;
}

static void pd_update(bao_t *b)
{
    if (b->pd_state == 1 && b->cycles >= b->pd_at) {
        if (ao_wake_live(b)) {
            bao_logf(b, BAO_LOG_WARN, "power down not entered: wake source 0x%03X already high",
                     ao_wake_live(b));
            b->pd_state = 0;
            b->pd_at = NEVER;
            return;
        }
        uint32_t keep[8];
        uint64_t keep_base = b->rtc_base;
        b->pd_count++;
        bao_logf(b, BAO_LOG_INFO, "power down %u: SoC held in reset", b->pd_count);
        /* the SoC goes into reset now, but boot1's RTC start only runs
         * when the reset is released, so the RTC is kept as it is */
        keep[0] = b->rtc_lr; keep[1] = b->rtc_cr; keep[2] = b->rtc_frozen; keep[3] = b->rtc_fd;
        keep[4] = b->rtc_mr; keep[5] = b->rtc_imsc; keep[6] = b->rtc_ris; keep[7] = b->rtc_seen;
        cpu_reset(b);
        soc_reset(b);
        b->rtc_lr = keep[0]; b->rtc_cr = keep[1]; b->rtc_frozen = keep[2]; b->rtc_fd = keep[3];
        b->rtc_mr = keep[4]; b->rtc_imsc = keep[5]; b->rtc_ris = keep[6]; b->rtc_seen = keep[7];
        b->rtc_base = keep_base;
        b->pd_state = 2;
        b->pd_at = NEVER;
    }
    if (b->pd_state == 2 && ao_wake_live(b)) {
        b->pd_state = 3;
        b->pd_at = b->cycles + 312u * CLK32K_CYC;
        bao_logf(b, BAO_LOG_INFO, "wake source 0x%03X, leaving power down", ao_wake_live(b));
    }
    if (b->pd_state == 3 && b->cycles >= b->pd_at) {
        b->pd_state = 0;
        b->pd_at = NEVER;
        cpu_reset(b);
        soc_reset(b);
    }
}

static uint64_t pd_next_event(bao_t *b)
{
    if (b->pd_state == 1 || b->pd_state == 3)
        return b->pd_at;
    if (b->pd_state == 2) {
        if (!(b->ao_wkup & (1u << 9)) && (b->rtc_imsc & 1))
            return rtc_match_cycle(b);
        return NEVER;
    }
    return NEVER;
}

static int pmu_read(bao_t *b, uint32_t a, uint32_t *v)
{
    switch (a & 0xFFF) {
    case 0x000: *v = b->ao_cr; return 0;
    case 0x008: *v = b->ao_wkup; return 0;
    case 0x00C: *v = b->ao_rstcrmask; return 0;
    case 0x018: *v = b->ao_pmucrpd; return 0;
    case 0x040: b->ao_fr |= ao_wake_sources(b); *v = b->ao_fr; return 0;
    case 0x044: *v = 0; return 0;
    default: return -1;
    }
}

static int pmu_write(bao_t *b, uint32_t a, uint32_t v)
{
    switch (a & 0xFFF) {
    case 0x000: b->ao_cr = v & 7u; return 0;
    case 0x008: b->ao_wkup = v & 0x3FFFFu; return 0;
    case 0x00C: b->ao_rstcrmask = v & 0x1Fu; return 0;
    case 0x018: b->ao_pmucrpd = v & 0xFFu; return 0;
    case 0x040: b->ao_fr = (b->ao_fr & ~v) | ao_wake_sources(b); return 0;
    case 0x044:
        if (v == 0x5Au && b->pd_state == 0) {
            b->pd_state = 1;
            b->pd_at = b->cycles + 3u * CLK32K_CYC;
            bao_kick(b);
        }
        return 0;
    default: return -1;
    }
}

/*
 * CGU and sysctrl registers (sysctrl.sv 831 to 884, ASIC build): width and
 * reset value of each CR; AR offsets act on one value and read 0; the
 * frequency sensor status (0x40 to 0x50) and the one time write register
 * at 0xC0 are not modelled. kind 0 is CR, 1 AR, 2 not modelled.
 */
typedef struct { uint16_t off; uint8_t kind, count; uint32_t mask, iv; } cgu_sfr_t;

static const cgu_sfr_t cgu_sfrs[] = {
    {0x00, 0, 1, 0xFFFFu, 0}, {0x04, 0, 1, 0xFFFFu, 0}, {0x08, 0, 1, 0xFFFFFFFFu, 0},
    {0x0C, 1, 1, 0, 0}, {0x10, 0, 1, 0x3u, 0}, {0x14, 0, 5, 0xFFFFFFFFu, 0xF7Fu},
    {0x28, 0, 1, 0xFFFFFFFFu, 0x0F0F0F0Fu}, {0x2C, 1, 1, 0, 0}, {0x30, 0, 1, 0x1u, 0},
    {0x34, 0, 1, 0xFFu, 0xFFu}, {0x38, 0, 1, 0xFFFFu, 0x0F0Fu}, {0x3C, 0, 1, 0xFFFFFFFFu, 0xFFFFFFu},
    {0x40, 2, 5, 0, 0}, {0x54, 0, 1, 0xFFFFu, 48}, {0x60, 0, 4, 0xFFu, 0xFFu},
    {0x80, 1, 2, 0, 0}, {0x88, 2, 1, 0, 0}, {0x90, 1, 1, 0, 0},
    {0x94, 0, 1, 0xFFFFu, 1}, {0x98, 0, 1, 0xFFFFu, 1}, {0x9C, 0, 1, 0x7Fu, 0x26u},
    {0xA0, 0, 1, 0x1FFFFu, 0}, {0xA4, 0, 1, 0x1FFFFFFu, 0xFFu}, {0xA8, 0, 1, 0x7FFFu, 0},
    {0xAC, 0, 1, 0xFFFFu, 0x53u}, {0xC0, 2, 1, 0, 0},
};

static const cgu_sfr_t *cgu_sfr(uint32_t a)
{
    uint32_t off = a & 0xFFF;
    size_t i;
    for (i = 0; i < sizeof(cgu_sfrs) / sizeof(cgu_sfrs[0]); i++)
        if (off >= cgu_sfrs[i].off && off < cgu_sfrs[i].off + 4u * cgu_sfrs[i].count)
            return &cgu_sfrs[i];
    return NULL;
}

static void cgu_reset(bao_t *b)
{
    size_t i;
    uint32_t c, *slot;
    for (i = 0; i < sizeof(cgu_sfrs) / sizeof(cgu_sfrs[0]); i++)
        for (c = 0; c < cgu_sfrs[i].count; c++)
            if ((slot = bao_mmio_slot(b, 0x40040000u + cgu_sfrs[i].off + 4u * c)) != NULL)
                *slot = cgu_sfrs[i].iv;
}

static uint32_t cgu_read(bao_t *b, uint32_t a)
{
    const cgu_sfr_t *s = cgu_sfr(a);
    uint32_t *slot = bao_mmio_slot(b, a);
    if (!s) {
        note_once(b, a, "CGU read with no register, reads 0,");
        return 0;
    }
    if (s->kind == 2) {
        warn_once(b, a, "read");
        return slot ? *slot : 0;
    }
    return s->kind == 1 || !slot ? 0 : *slot & s->mask;
}

static void cgu_write(bao_t *b, uint32_t a, uint32_t v)
{
    const cgu_sfr_t *s = cgu_sfr(a);
    uint32_t *slot = bao_mmio_slot(b, a);
    if (!s) {
        note_once(b, a, "CGU write with no register, dropped,");
        return;
    }
    if (s->kind == 2) {
        warn_once(b, a, "write");
        if (slot) *slot = v;
    } else if (s->kind == 0 && slot) {
        *slot = v & s->mask;
    } else if (s->kind == 1 && (s->off == 0x80) && v == 0x55AAu) {
        /* sysreset_sw and corereset_sw (sysctrl.sv 794, 803, 871, 872) */
        b->sw_reset = 1;
        bao_kick(b);
    }
}

static int is_mbox(uint32_t a)
{
    return (a & ~0xFFFu) == 0x40013000u || (a & ~0xFFFu) == 0xE0018000u ||
           (a & ~0xFFFu) == 0xE0019000u;
}

static int mapped(uint32_t a)
{
    return (a >= 0x40000000u && a < 0x40100000u) ||
           (a >= 0x50100000u && a < 0x50130000u) ||
           (a >= 0x50202000u && a < 0x50205000u) ||
           (a >= 0xE0000000u && a < 0xE0100000u);
}

int mmio_read(bao_t *b, uint32_t a, int size, uint32_t *v)
{
    uint32_t w = 0, aw = a & ~3u;
    uint32_t *slot;

    if (!mapped(a))
        return -1;
    if (aw >= 0x40000000u && aw < 0x40080000u && guard_check(b, aw, 4, 0, 0))
        { *v = 0; return 0; }
    if (((aw >= 0x40041000u && aw < 0x40064000u)) && ao_read(b, aw, &w) == 0)
        goto done;
    if (aw >= 0x40040000u && aw < 0x40041000u) {
        w = cgu_read(b, aw);
        goto done;
    }

    if (is_mbox(aw)) {
        mbox_read(b, aw, &w);
    } else if (aw >= 0xE0000000u) {
        if (cpu_periph_read(b, aw, &w)) {
            slot = bao_mmio_slot(b, aw);
            warn_once(b, aw, "read");
            w = slot ? *slot : 0;
        }
    } else if (aw >= 0x50100000u && aw < 0x50120000u) {
        if (udma_read(b, aw, &w)) {
            slot = bao_mmio_slot(b, aw);
            warn_once(b, aw, "read");
            w = slot ? *slot : 0;
        }
    } else if (aw >= 0x50124000u && aw < 0x5012D000u) {
        if (bio_read(b, aw, &w)) {
            slot = bao_mmio_slot(b, aw);
            warn_once(b, aw, "read");
            w = slot ? *slot : 0;
        }
    } else if (aw >= IOX_BASE && aw < IOX_BASE + 0x1000u) {
        uint32_t off = aw - IOX_BASE;
        if (!iox_mask(off)) {
            note_once(b, aw, "IOX read with no register, reads 0,");
        } else if (off >= 0x178 && off < 0x190) {
            if (b->iox[0x200 >> 2] && bio_active(b)) {
                bio_catch_up(b);
                b->ff_block = 1;
            }
            w = gpio_in(b, (int)((off - 0x178) >> 2));
        } else if (off == 0x120)
            w = b->iox_fr;
        else
            w = b->iox[off >> 2];
    } else if (aw >= PWM_BASE && aw < PWM_BASE + 0x200u) {
        pwm_read(b, aw - PWM_BASE, &w);
    } else if (aw >= 0x50202000u && aw < 0x50205000u) {
        usb_read(b, aw, &w);
    } else if (aw >= 0x40011000u && aw < 0x40013000u) {
        mdma_read(b, aw, &w);
    } else if (aw >= 0x40020000u && aw < 0x40030000u) {
        if (sce_read(b, aw, &w)) {
            slot = bao_mmio_slot(b, aw);
            w = slot ? *slot : 0;
        }
    } else {
        slot = bao_mmio_slot(b, aw);
        warn_once(b, aw, "read");
        w = slot ? *slot : 0;
    }

done:
    w >>= 8 * (a & 3);
    if (size == 1) w &= 0xFF;
    else if (size == 2) w &= 0xFFFF;
    *v = w;
    return 0;
}

int mmio_write(bao_t *b, uint32_t a, int size, uint32_t v)
{
    uint32_t aw = a & ~3u, *slot;

    if (!mapped(a))
        return -1;
    if (is_mbox(aw)) {
        /* no read before a narrow store here, since a read pops the FIFO:
         * the core drives the byte or half word on every lane
         * (VexRiscv_CramSoC.v 5159 to 5165) and neither bus looks at the
         * strobes (cram_axi.sv 13050, apb_sfr.sv 339) */
        if (size == 1) v = (v & 0xFFu) * 0x01010101u;
        else if (size == 2) v = (v & 0xFFFFu) * 0x00010001u;
        return mbox_write(b, aw, v);
    }
    if (size < 4) {
        uint32_t cur = 0, sh = 8 * (a & 3), m = size == 1 ? 0xFFu : 0xFFFFu;
        mmio_read(b, aw, 4, &cur);
        v = (cur & ~(m << sh)) | ((v & m) << sh);
    }
    if (aw >= 0x40000000u && aw < 0x40080000u) {
        if (guard_check(b, aw, size, v, 1))
            return 0;
        if (aw == 0x40040014u && (v == 0x070001FFu || v == 0x0700017Fu))
            bio_clock_change(b, v == 0x070001FFu ? 2 : 1);
        if (aw == RRC_CR_ADDR) {
            b->rrc_cr = v;
            return 0;
        }
        if (ao_write(b, aw, v) == 0)
            return 0;
        if (aw >= 0x40040000u && aw < 0x40041000u) {
            cgu_write(b, aw, v);
            return 0;
        }
    }

    if (aw >= 0xE0000000u) {
        if (!cpu_periph_write(b, aw, v))
            return 0;
    } else if (aw >= 0x50100000u && aw < 0x50120000u) {
        if (!udma_write(b, aw, v))
            return 0;
    } else if (aw >= 0x50124000u && aw < 0x5012D000u) {
        if (!bio_write(b, aw, v))
            return 0;
    } else if (aw >= IOX_BASE && aw < IOX_BASE + 0x1000u) {
        uint32_t off = aw - IOX_BASE;
        if (!iox_mask(off)) {
            note_once(b, aw, "IOX write with no register, dropped,");
            return 0;
        }
        if (off >= 0x178 && off < 0x190)
            return 0;
        v &= iox_mask(off);
        if (off == 0x120) {
            b->iox_fr &= ~v;
            iox_int_check(b);
            return 0;
        }
        bio_catch_up(b);
        b->iox[off >> 2] = v;
        bio_host_pins(b);
        gpio_notify(b);
        bao_kick(b);
        return 0;
    } else if (aw >= PWM_BASE && aw < PWM_BASE + 0x200u) {
        pwm_write(b, aw - PWM_BASE, v);
        return 0;
    } else if (aw >= 0x50202000u && aw < 0x50205000u) {
        usb_write(b, aw, v);
        return 0;
    } else if (aw >= 0x40011000u && aw < 0x40013000u) {
        mdma_write(b, aw, v);
        return 0;
    } else if (aw >= 0x40020000u && aw < 0x40030000u) {
        if (!sce_write(b, aw, v))
            return 0;
        slot = bao_mmio_slot(b, aw);
        if (slot) *slot = v;
        return 0;
    }
    slot = bao_mmio_slot(b, aw);
    warn_once(b, aw, "write");
    if (slot)
        *slot = v;
    return 0;
}

int bus_read(bao_t *b, uint32_t a, int size, uint32_t *v)
{
    return mmio_read(b, a, size, v);
}

int bus_write(bao_t *b, uint32_t a, int size, uint32_t v)
{
    if (a >= RRAM_BASE && a < RRAM_BASE + RRAM_SIZE) {
        rram_store(b, a, size, v);
        return 0;
    }
    return mmio_write(b, a, size, v);
}

int bus_dma_read(bao_t *b, uint32_t a, uint8_t *dst, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        uint8_t *p = b->rdmap[(a + i) >> 16];
        if (!p) return -1;
        dst[i] = p[(a + i) & 0xFFFF];
    }
    return 0;
}

int bus_dma_write(bao_t *b, uint32_t a, const uint8_t *src, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        uint8_t *p = b->wrmap[(a + i) >> 16];
        if (!p) return -1;
        p[(a + i) & 0xFFFF] = src[i];
    }
    return 0;
}

void soc_reset(bao_t *b)
{
    int p;
    memset(b->irq, 0, sizeof(b->irq));
    b->tt_cpt = BAO_ACLK_HZ / 1000;
    b->tt_base_cycle = b->cycles;
    b->tt_base_ms = 0;
    b->tt_target = 0;
    b->tt_target0 = b->tt_target1 = 0;
    b->tt_pending = b->tt_enable = 0;
    b->t0_load = b->t0_reload = b->t0_en = b->t0_latched = 0;
    b->t0_pending = b->t0_enable = 0;
    b->hb_count = 0;
    b->wdt_load = 0xFFFFFFFFu;
    b->wdt_ctrl = 0;
    b->wdt_ris = 0;
    b->wdt_locked = 0;
    /* The RTC is always on and survives this reset. boot1 starts it on a
     * cold boot, or after any reset that finds it stopped, with FD 15 and
     * a count two minutes short of rolling over (bao1x.rs 116 to 134). */
    if (!(b->rtc_cr & 1)) {
        b->rtc_fd = 15;
        b->rtc_lr = b->rtc_frozen = b->rtc_seen = 0xFFFE2000u;
        b->rtc_cr = 1;
        b->rtc_base = b->cycles;
    }
    /* The sysctrl ATimer resets with the core; the always on one only at
     * power on or the RESET pin, in bao_reset. */
    for (p = 0; p < 2; p++) {
        b->at_cfg[p] = b->at_cmp[p] = b->at_base_val[p] = 0;
        b->at_base[p] = b->cycles;
    }
    b->rng_state = 0x9E3779B97F4A7C15ull;
    b->rng_count = 0;
    b->rrc_cr = RRC_CR_NORMAL;
    b->rrc_staged = 0;
    memset(b->rrc_mask, 0, sizeof(b->rrc_mask));
    memset(b->iox, 0, sizeof(b->iox));
    for (p = 0; p < 6; p++) {
        b->iox[(0x160 >> 2) + p] = 0xFFFF;
        b->gpio_last_out[p] = 0;
        b->gpio_last_oe[p] = 0;
    }
    memset(b->pwm, 0, sizeof(b->pwm));
    pwm_reset(b);
    udma_reset(b);
    sce_reset(b);
    cgu_reset(b);
    bio_reset(b);
    mdma_reset(b);
    mbox_reset(b);
    usb_reset(b);
    b->bio_mul = 1;
    b->next_event = b->cycles;
}

void ff_backedge(bao_t *b, uint32_t pc)
{
    uint64_t pipe = cpu_pipe_state(b);
    if (pc == b->ff_pc && b->side == b->ff_side && !b->ff_block && pipe == b->ff_pipe &&
        memcmp(b->x, b->ff_x, sizeof(b->x)) == 0) {
        uint64_t period = b->cycles - b->ff_cycles, horizon = b->limit, k;
        /* the edge that matters is the first one after the loop last looked,
         * which is at most one period back */
        if (b->ff_ttdep) {
            uint64_t edge = b->cycles;
            if (b->ff_cycles >= b->tt_base_cycle) {
                uint64_t ms = b->tt_base_ms + (b->ff_cycles - b->tt_base_cycle) / b->tt_cpt;
                edge = b->tt_base_cycle + (ms + 1 - b->tt_base_ms) * b->tt_cpt;
            }
            if (edge < horizon) horizon = edge;
        }
        if (b->ff_hbdep && b->hb_count) {
            uint64_t edge = (b->ff_cycles / b->hb_count + 1) * b->hb_count;
            if (edge < horizon) horizon = edge;
        }
        if (period > 0 && horizon > b->cycles + 2 * period) {
            k = (horizon - b->cycles) / period - 1;
            b->cycles += k * period;
            b->instret += k * (b->instret - b->ff_instret);
            b->ff_skipped += k * period;
            /* measure the next period from here, not across the skip */
            b->ff_cycles = b->cycles;
            b->ff_instret = b->instret;
            b->ff_ttdep = b->ff_hbdep = 0;
            return;
        }
    }
    if (pc != b->ff_pc || (++b->ff_tick & 63) == 0 || b->side != b->ff_side) {
        b->ff_pc = pc;
        b->ff_pipe = pipe;
        memcpy(b->ff_x, b->x, sizeof(b->x));
        b->ff_cycles = b->cycles;
        b->ff_instret = b->instret;
        b->ff_side = b->side;
        b->ff_ttdep = b->ff_hbdep = b->ff_block = 0;
    }
}
