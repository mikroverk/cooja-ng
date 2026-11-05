/*
 * arm-idle — idle-loop skipping (include/arm/arm_idle.h): the proof, the
 * landing model and the fast-forward, against a reference CPU.
 *
 * Every case runs the same program on two CPUs from the same state, sliced
 * the way the ARM mote slices a node (one microsecond of cycles per slice,
 * arm_step_until to each target): the reference runs every slice, the other
 * does what arm_elf_mote_tick does with skipping on (prove, run validation
 * slices for real, then do covered slices without the CPU and catch up).
 * At the end, and wherever the case looks, the two must be identical:
 * registers, flags, cycles, instructions, SRAM, peripheral registers.
 *
 * Loops that may be skipped: a poll of a RAM flag, a poll that stores the
 * same value each pass, one that masks and unmasks a peripheral interrupt
 * (a store the classifier calls plain), one that reads a cycle-timed
 * counter, one that a scheduled event ends.  Loops that must not be
 * skipped: one that changes RAM each pass, one that reads a free-running
 * counter the classifier does not vouch for, one that reads a FIFO (a read
 * with a side effect, which the dry run must not perform), one with an
 * interrupt pending, one that stores a changing value to a peripheral.
 * Each runs with the JIT off and, when built with Lightning, on.
 */
#include "arm_cpu.h"
#include "arm_config.h"
#include "arm_nvic.h"
#include "arm_idle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int run_arm_idle_tests(int verbose_flag);

#define CODE_BASE  (ARM_FLASH_BASE + 0x100)
#define DATA       (ARM_SRAM_BASE + 0x100)
#define PERIPH     0x40001000u
#define SLICE      32            /* cc2538_config: 32 MHz, 1 µs slices */

static int passed, failed, verbose;

/* --- a test peripheral at PERIPH ---
 *   +0  counter: cycles / 16 (free-running)
 *   +4  FIFO data: each read pops one value
 *   +8  mask register: plain read/write
 *   +12 slow counter: cycles / 1000 */
typedef struct { arm_cpu_t *cpu; uint32_t fifo[8]; int fifo_n; uint32_t mask; int reads; } periph_t;

static int periph_read(void *u, uint32_t addr) {
    periph_t *p = (periph_t *)u;
    p->reads++;
    switch (addr - PERIPH) {
    case 0:  return (int)(p->cpu->cycles / 16);
    case 4:  return p->fifo_n > 0 ? (int)p->fifo[--p->fifo_n] : 0;
    case 8:  return (int)p->mask;
    case 12: return (int)(p->cpu->cycles / 1000);
    default: return 0;
    }
}
static void periph_write(void *u, uint32_t addr, uint32_t v) {
    periph_t *p = (periph_t *)u;
    if (addr - PERIPH == 8) p->mask = v;
}
/* What each register is to the proof.  The free-running counter is left
 * undeclared on purpose (a classifier that does not know a register must
 * refuse it). */
static int periph_class(void *u, uint32_t addr, bool is_write, uint32_t value,
                        int width, int64_t now, arm_idle_io_info_t *info) {
    (void)u; (void)value; (void)width;
    switch (addr - PERIPH) {
    case 8:
        return is_write ? ARM_IDLE_IO_STORE : ARM_IDLE_IO_PURE;
    case 12:
        if (is_write) return ARM_IDLE_IO_REFUSE;
        info->value = (uint32_t)(now / 1000);
        info->until = (now / 1000 + 1) * 1000;
        return ARM_IDLE_IO_TIMED;
    default:
        return ARM_IDLE_IO_REFUSE;     /* the counter, the FIFO */
    }
}

typedef struct {
    arm_cpu_t cpu;
    arm_nvic_t nvic;
    periph_t periph;
} node_t;

static void put16(arm_cpu_t *c, uint32_t addr, uint16_t hw) {
    c->flash[addr - ARM_FLASH_BASE] = hw & 0xFF;
    c->flash[addr - ARM_FLASH_BASE + 1] = (hw >> 8) & 0xFF;
}
static void put32(arm_cpu_t *c, uint32_t addr, uint32_t v) {
    put16(c, addr, (uint16_t)v);
    put16(c, addr + 2, (uint16_t)(v >> 16));
}

static void node_init(node_t *n, const uint16_t *code, int ncode, bool jit) {
    memset(n, 0, sizeof(*n));
    arm_cpu_init(&n->cpu, &cc2538_config);
    if (!jit) n->cpu.jit_cache_size = 0;
    arm_nvic_init(&n->nvic, &n->cpu);
    put32(&n->cpu, ARM_FLASH_BASE, ARM_SRAM_BASE + ARM_SRAM_SIZE);
    put32(&n->cpu, ARM_FLASH_BASE + 4, CODE_BASE + 1);
    for (int i = 0; i < ncode; i++) put16(&n->cpu, CODE_BASE + 2u * (uint32_t)i, code[i]);
    arm_cpu_reset(&n->cpu);
    n->periph.cpu = &n->cpu;
    arm_register_io(&n->cpu, PERIPH, 0x100, periph_read, periph_write, &n->periph);
    arm_idle_set_io_classifier(&n->cpu, periph_class, &n->periph);
    n->cpu.reg[1] = DATA;
    n->cpu.reg[3] = PERIPH;
}

/* One slice as arm_elf_mote_tick does it, with or without skipping. */
static void slice(node_t *n, int64_t target, bool skip) {
    arm_cpu_t *c = &n->cpu;
    arm_idle_state_t *s = &c->idle;
    if (skip && s->state == ARM_IDLE_LATCHED) {
        arm_idle_pos_t to;
        if (arm_idle_predict(c, &s->model, target, &to) && arm_idle_safe(c, &to)) {
            s->model = to;
            s->lazy = true;
            s->gained++;
            return;
        }
        if (s->lazy) { s->lazy = false; arm_idle_catch_up(c); }
        arm_idle_drop(c, ARM_IDLE_WHY_EVENT);
    } else if (skip && s->state == ARM_IDLE_OFF && !c->cpu_off) {
        if (s->cooldown > 0) s->cooldown--;
        else (void)arm_idle_prove(c);
    }
    if (skip && s->state == ARM_IDLE_VALIDATING &&
        (!arm_idle_predict(c, &s->phys, target, &s->predicted) ||
         !arm_idle_safe(c, &s->predicted)))
        arm_idle_drop(c, ARM_IDLE_WHY_MODEL);
    if (c->cycles < target) arm_step_until(c, target);
    if (skip && s->state == ARM_IDLE_VALIDATING) arm_idle_check_slice(c);
}

static void settle(node_t *n) {
    arm_idle_state_t *s = &n->cpu.idle;
    if (s->lazy) { s->lazy = false; arm_idle_catch_up(&n->cpu); }
}

static bool same(const node_t *a, const node_t *b, const char *name) {
    bool ok = memcmp(a->cpu.reg, b->cpu.reg, sizeof(a->cpu.reg)) == 0 &&
              a->cpu.xpsr == b->cpu.xpsr && a->cpu.cycles == b->cpu.cycles &&
              a->cpu.instructions == b->cpu.instructions &&
              memcmp(a->cpu.sram, b->cpu.sram, ARM_SRAM_SIZE) == 0 &&
              a->periph.mask == b->periph.mask &&
              a->periph.fifo_n == b->periph.fifo_n;
    if (!ok)
        printf("  FAIL %s: skipped run differs: pc %08x/%08x cycles %lld/%lld "
               "insns %lld/%lld\n", name, a->cpu.reg[ARM_PC], b->cpu.reg[ARM_PC],
               (long long)a->cpu.cycles, (long long)b->cpu.cycles,
               (long long)a->cpu.instructions, (long long)b->cpu.instructions);
    return ok;
}

typedef void (*prep_fn)(node_t *n);

/* Run `slices` slices on a reference and a skipping node; expect the
 * skipping one to skip (or not), and both to end identical. */
static void run_case(const char *name, const uint16_t *code, int ncode,
                     prep_fn prep, int slices, bool expect_skip, bool jit) {
    node_t *ref = calloc(1, sizeof(*ref)), *sk = calloc(1, sizeof(*sk));
    node_init(ref, code, ncode, jit);
    node_init(sk, code, ncode, jit);
    if (prep) { prep(ref); prep(sk); }
    arm_idle_enable(&sk->cpu, true);
    int64_t t0 = ref->cpu.cycles;
    bool ok = true;
    for (int k = 1; k <= slices && ok; k++) {
        int64_t target = t0 + (int64_t)k * SLICE;
        slice(ref, target, false);
        slice(sk, target, true);
        if (k % 997 == 0) { settle(sk); ok = same(sk, ref, name); }
    }
    settle(sk);
    if (ok) ok = same(sk, ref, name);
    uint64_t skipped = sk->cpu.idle.skipped_iters;
    if (ok && expect_skip && skipped == 0) {
        printf("  FAIL %s: expected iterations to be skipped, none were "
               "(proofs=%llu)\n", name, (unsigned long long)sk->cpu.idle.proofs);
        ok = false;
    }
    if (ok && !expect_skip && (sk->cpu.idle.proofs || skipped)) {
        printf("  FAIL %s: a loop that must not be skipped was proven "
               "(proofs=%llu skipped=%llu)\n", name,
               (unsigned long long)sk->cpu.idle.proofs, (unsigned long long)skipped);
        ok = false;
    }
    if (ok) {
        passed++;
        if (verbose)
            printf("  ok   %-34s jit=%d proofs=%llu skipped-iterations=%llu\n", name,
                   jit, (unsigned long long)sk->cpu.idle.proofs,
                   (unsigned long long)skipped);
    } else {
        failed++;
    }
    arm_cpu_destroy(&ref->cpu);
    arm_cpu_destroy(&sk->cpu);
    free(ref);
    free(sk);
}

/* Thumb-16 encodings used below. */
#define LDR_R0_R1_0   0x6808   /* ldr  r0, [r1, #0]   */
#define LDR_R2_R1_4   0x684A   /* ldr  r2, [r1, #4]   */
#define ADDS_R2_R0_R2 0x1882   /* adds r2, r0, r2     */
#define STR_R2_R1_8   0x608A   /* str  r2, [r1, #8]   */
#define CMP_R0_0      0x2800   /* cmp  r0, #0         */
#define ADDS_R0_1     0x3001   /* adds r0, #1         */
#define STR_R0_R1_0   0x6008   /* str  r0, [r1, #0]   */
#define LDR_R0_R3_0   0x6818   /* ldr  r0, [r3, #0]   */
#define LDR_R0_R3_4   0x6858   /* ldr  r0, [r3, #4]   */
#define LDR_R0_R3_8   0x6898   /* ldr  r0, [r3, #8]   */
#define LDR_R0_R3_12  0x68D8   /* ldr  r0, [r3, #12]  */
#define STR_R0_R3_8   0x6098   /* str  r0, [r3, #8]   */
#define MOVS_R2_64    0x2240   /* movs r2, #0x40      */
#define BICS_R0_R2    0x4390   /* bics r0, r2         */
#define ORRS_R0_R2    0x4310   /* orrs r0, r2         */
#define B_SELF        0xE7FE   /* b    .              */
/* b / beq back to CODE_BASE from instruction index i */
static uint16_t b_to0(int i)   { return (uint16_t)(0xE000 | ((uint32_t)(-(2 * i + 4) / 2) & 0x7FF)); }
static uint16_t beq_to0(int i) { return (uint16_t)(0xD000 | ((uint32_t)(-(2 * i + 4) / 2) & 0xFF)); }

static void prep_pending(node_t *n) {
    n->cpu.primask = 1;                 /* pending, not taken */
    n->nvic.iser[0] |= 1u << 5;
    arm_nvic_set_pending(&n->nvic, 5);
}
static void prep_fifo(node_t *n) {
    for (int i = 0; i < 8; i++) n->periph.fifo[i] = 0;
    n->periph.fifo_n = 8;
}

/* An event 3000 µs in sets the flag the poll waits on: the loop must be
 * left exactly when the unskipped run leaves it. */
static void flag_event(void *user, cpu_event_t *ev) {
    (void)ev;
    node_t *n = (node_t *)user;
    arm_write32(&n->cpu, DATA, 1);
}
static cpu_event_t ev_store[2];
static int ev_used;
static void prep_event(node_t *n) {
    cpu_event_t *ev = &ev_store[ev_used++ & 1];
    memset(ev, 0, sizeof(*ev));
    ev->callback = flag_event;
    ev->user_data = n;
    arm_schedule_event(&n->cpu, ev, n->cpu.cycles + 3000 * SLICE + 7);
}

int run_arm_idle_tests(int verbose_flag) {
    verbose = verbose_flag;
    passed = failed = 0;
    printf("=== ARM idle-loop skipping tests ===\n");

    /* Skippable. */
    const uint16_t poll[] = { LDR_R0_R1_0, CMP_R0_0, beq_to0(2) };
    const uint16_t poll_store[] = { LDR_R0_R1_0, LDR_R2_R1_4, ADDS_R2_R0_R2,
                                    STR_R2_R1_8, CMP_R0_0, beq_to0(5) };
    const uint16_t mask_toggle[] = { LDR_R0_R3_8, MOVS_R2_64, BICS_R0_R2,
                                     STR_R0_R3_8, ORRS_R0_R2, STR_R0_R3_8,
                                     b_to0(6) };
    const uint16_t timed[] = { LDR_R0_R3_12, b_to0(1) };
    const uint16_t until_event[] = { LDR_R0_R1_0, CMP_R0_0, beq_to0(2), B_SELF };
    /* Not skippable. */
    const uint16_t ram_count[] = { LDR_R0_R1_0, ADDS_R0_1, STR_R0_R1_0, b_to0(3) };
    const uint16_t counter[] = { LDR_R0_R3_0, b_to0(1) };
    const uint16_t fifo[] = { LDR_R0_R3_4, b_to0(1) };
    const uint16_t periph_count[] = { LDR_R0_R3_8, ADDS_R0_1, STR_R0_R3_8, b_to0(3) };

    for (int jit = 0; jit <= 1; jit++) {
#ifndef HAVE_LIGHTNING
        if (jit) break;
#endif
        run_case("poll a RAM flag", poll, 3, NULL, 20000, true, jit);
        run_case("poll, store the same value", poll_store, 6, NULL, 20000, true, jit);
        run_case("mask/unmask a peripheral IRQ", mask_toggle, 7, NULL, 20000, true, jit);
        run_case("cycle-timed counter (1000 cycles)", timed, 2, NULL, 20000, true, jit);
        ev_used = 0;
        run_case("poll ended by an event", until_event, 4, prep_event, 6000, true, jit);
        run_case("RAM changes each pass", ram_count, 4, NULL, 4000, false, jit);
        run_case("free-running counter", counter, 2, NULL, 4000, false, jit);
        run_case("FIFO read (side effect)", fifo, 2, prep_fifo, 4000, false, jit);
        run_case("interrupt pending", poll, 3, prep_pending, 4000, false, jit);
        run_case("changing peripheral store", periph_count, 4, NULL, 4000, false, jit);
    }

    /* A refused read is never performed by the dry run: the FIFO keeps
     * every value the program has not popped itself. */
    {
        node_t *n = calloc(1, sizeof(*n));
        node_init(n, fifo, 2, false);
        prep_fifo(n);
        arm_idle_enable(&n->cpu, true);
        int before = n->periph.fifo_n;
        bool proven = arm_idle_prove(&n->cpu);
        if (proven || n->periph.fifo_n != before || n->periph.reads != 0) {
            printf("  FAIL dry run touched the FIFO (proven=%d fifo %d -> %d, reads=%d)\n",
                   proven, before, n->periph.fifo_n, n->periph.reads);
            failed++;
        } else {
            passed++;
        }
        arm_cpu_destroy(&n->cpu);
        free(n);
    }

    printf("  %d passed, %d failed\n", passed, failed);
    return failed;
}
