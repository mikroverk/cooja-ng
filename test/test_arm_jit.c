/*
 * arm-jit — differential test: GENERATED CODE vs the interpreter.
 *
 * WHY THIS EXISTS SEPARATELY FROM arm-decode
 *
 * `arm-decode` proves that arm_decode.c + arm_execute_decoded() agree with the
 * interpreter.  That is a necessary check and not a sufficient one: it
 * validates the *description* of an instruction, while the JIT ships a third
 * implementation — the machine code GNU Lightning emits from that description.
 * A correct decode can still be compiled wrongly, and the failure mode is the
 * worst kind, because it is host-specific.
 *
 * That is not hypothetical.  This suite was written after exactly such a bug:
 *
 *     jit_andi(dst, src, 0x80000000) returns 0 on x86-64 GNU Lightning 2.2.3.
 *
 * emit_cond() used it to extract the N flag, so MI, PL, GE, LT, GT and LE all
 * evaluated as though N were clear — on x86-64 only.  ARM64 was correct, the
 * decoder was correct, `arm-decode` passed 269952/269952 on both hosts, the
 * whole Cooja suite passed, and simulations still produced *plausible* output;
 * they just silently took the wrong branch.  It surfaced only because
 * `CSIM_ARM_JIT_VERIFY=1` was run on the other architecture.
 *
 * So this runs the compiled code itself, exhaustively over encodings, on
 * whatever host it is built for:
 *
 *   for every one of the 65536 Thumb halfwords
 *     if the decoder claims to support it
 *       compile a one-instruction block at that address
 *       for several pseudo-random register/flag states
 *         A: execute the encoding through arm_step()
 *         B: execute the compiled block
 *         require r0..r15, xPSR and the cycle count to match exactly
 *
 * The conditional-branch encodings (0xD000..0xDDFF) give all 14 condition
 * codes against random flags, which is the specific hole that let the bug
 * above ship.  Memory encodings are driven both at SRAM addresses (exercising
 * the inline fast path) and at wild ones (exercising the guarded side exit,
 * where the block must leave state untouched for the interpreter to redo).
 * SRAM contents are compared as well as registers, because a store writes no
 * register — a wrong access width or byte order would otherwise pass.
 *
 * A failure here means the emulator is executing something other than the
 * program it was given, and is a hard stop.
 */
#include "arm_cpu.h"
#include "arm_config.h"
#include "arm_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int run_arm_jit_tests(int verbose_flag);

#ifndef HAVE_LIGHTNING

int run_arm_jit_tests(int verbose_flag) {
    (void)verbose_flag;
    printf("=== ARM JIT differential tests ===\n");
    printf("  SKIP: built without GNU Lightning — there is no generated code "
           "to test.\n");
    return 0;
}

#else  /* HAVE_LIGHTNING */

#include "arm_jit.h"

#define CODE_BASE (ARM_FLASH_BASE + 0x100)
#define TRIALS    8

static int passed, failed, verbose;
static int n_side_exit, n_compiled, n_refused;

static uint32_t rng_state = 0x2468ACE0u;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void write_flash32(arm_cpu_t *cpu, uint32_t addr, uint32_t val) {
    uint32_t off = addr - ARM_FLASH_BASE;
    cpu->flash[off]     = val & 0xFF;
    cpu->flash[off + 1] = (val >> 8) & 0xFF;
    cpu->flash[off + 2] = (val >> 16) & 0xFF;
    cpu->flash[off + 3] = (val >> 24) & 0xFF;
}

/*
 * Lay down the encoding under test followed by a Thumb-2 prefix halfword.
 * The decoder rejects every 32-bit encoding, so the block decoder stops there
 * and the block is exactly the one instruction we mean to test — otherwise the
 * zero-filled flash after it (`lsl r0,r0,#0`, a supported MOV) would be pulled
 * into the block and we would be comparing something else.
 */
static void setup(arm_cpu_t *cpu, uint16_t hw) {
    arm_cpu_init(cpu, &cc2538_config);
    write_flash32(cpu, ARM_FLASH_BASE, ARM_SRAM_BASE + ARM_SRAM_SIZE);
    write_flash32(cpu, ARM_FLASH_BASE + 4, CODE_BASE + 1);
    arm_cpu_reset(cpu);
    uint32_t off = CODE_BASE - ARM_FLASH_BASE;
    cpu->flash[off]     = hw & 0xFF;
    cpu->flash[off + 1] = (hw >> 8) & 0xFF;
    cpu->flash[off + 2] = 0xFF;
    cpu->flash[off + 3] = 0xF7;          /* 0xF7FF — a 32-bit Thumb-2 prefix */
}

static uint32_t interesting(int i) {
    static const uint32_t v[] = {
        0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u,
        0xFFFFFFFFu, 0xFFFFFFFEu, 0x0000FFFFu, 0xFFFF0000u,
    };
    return v[i % (int)(sizeof(v) / sizeof(v[0]))];
}

static int compare(uint16_t hw, int trial, const arm_cpu_t *a,
                   const arm_cpu_t *b, const arm_basic_block_t *blk) {
    int bad = 0;
    for (int r = 0; r <= 15; r++) {
        if (a->reg[r] != b->reg[r]) {
            printf("  FAIL: hw=0x%04x trial=%d  r%d: interp=0x%08x jit=0x%08x\n",
                   hw, trial, r, a->reg[r], b->reg[r]);
            bad = 1;
        }
    }
    uint32_t fa = a->xpsr & 0xF0000000u, fb = b->xpsr & 0xF0000000u;
    if (fa != fb) {
        printf("  FAIL: hw=0x%04x trial=%d  APSR: interp=N%dZ%dC%dV%d "
               "jit=N%dZ%dC%dV%d\n", hw, trial,
               !!(fa & 0x80000000u), !!(fa & 0x40000000u),
               !!(fa & 0x20000000u), !!(fa & 0x10000000u),
               !!(fb & 0x80000000u), !!(fb & 0x40000000u),
               !!(fb & 0x20000000u), !!(fb & 0x10000000u));
        bad = 1;
    }
    if (a->cycles != b->cycles) {
        printf("  FAIL: hw=0x%04x trial=%d  cycles: interp=%lld jit=%lld\n",
               hw, trial, (long long)a->cycles, (long long)b->cycles);
        bad = 1;
    }
    /*
     * Memory, without which a store is barely tested at all: STR/STRB/STRH
     * write no register, so a wrong access width or a wrong byte order would
     * pass every check above.  Both CPUs start from identical SRAM, so a
     * straight compare is exact.
     */
    if (memcmp(a->sram, b->sram, ARM_SRAM_SIZE) != 0) {
        uint32_t i = 0;
        while (i < ARM_SRAM_SIZE && a->sram[i] == b->sram[i]) i++;
        printf("  FAIL: hw=0x%04x trial=%d  sram[0x%08x]: interp=0x%02x "
               "jit=0x%02x\n", hw, trial, ARM_SRAM_BASE + i,
               a->sram[i], b->sram[i]);
        bad = 1;
    }
    if (bad && verbose) {
        const arm_decoded_insn_t *di = &blk->insns[0];
        printf("        decoded: klass=%d op=%d rd=%u rn=%u rm=%u imm=0x%x "
               "cond=%u msize=%u sext=%d cycles=%u\n",
               (int)di->klass, (int)di->op, di->rd, di->rn, di->rm, di->imm,
               di->cond, di->msize, (int)di->sext, di->cycles);
    }
    return bad;
}

/*
 * Flash the firmware programs at run time (NVMC, the CC2538 ROM) can hold
 * code.  Compile a loop, reprogram one of its instructions, and the next pass
 * must run the new instruction, as the interpreter does: the compiled block
 * has to go (arm_flash_changed).  Before it did, the JIT kept r0 = 3 and the
 * interpreter gave 1.  A store to flash outside the compiled code must keep
 * the block.
 */
static int run_flash_rewrite(int jit) {
    arm_cpu_t cpu;
    setup(&cpu, 0x2003);                     /* movs r0, #3 */
    if (!jit) cpu.jit_cache_size = 0;
    uint32_t off = CODE_BASE - ARM_FLASH_BASE;
    cpu.flash[off + 2] = 0x01; cpu.flash[off + 3] = 0x31;   /* adds r1, #1 */
    cpu.flash[off + 4] = 0xFC; cpu.flash[off + 5] = 0xE7;   /* b    CODE_BASE */
    for (int i = 0; i < 400; i++) arm_step(&cpu, 3);
    int compiled = cpu.jit_blocks_run > 0;
    void *block = cpu.jit_cache ? cpu.jit_cache[(CODE_BASE >> 1) & (ARM_JIT_CACHE_SLOTS - 1)] : NULL;
    cpu.flash_wen = 1;
    arm_write32(&cpu, ARM_FLASH_BASE + 0x8000, 0);          /* data, not code */
    int kept = !block ||
               cpu.jit_cache[(CODE_BASE >> 1) & (ARM_JIT_CACHE_SLOTS - 1)] == block;
    arm_write16(&cpu, CODE_BASE, 0x2001);                    /* movs r0, #1 */
    cpu.flash_wen = 0;
    cpu.reg[0] = 0;
    for (int i = 0; i < 400; i++) arm_step(&cpu, 3);
    int r0 = (int)cpu.reg[0];
    arm_cpu_destroy(&cpu);
    if (jit && !compiled) return -1;
    if (!kept) return -2;
    return r0;
}

static void test_flash_rewrite(void) {
    int interp = run_flash_rewrite(0), jit = run_flash_rewrite(1);
    if (interp == 1 && jit == 1) {
        passed++;
        if (verbose) printf("  PASS: reprogrammed flash runs the new code under the JIT\n");
    } else {
        failed++;
        printf("  FAIL: reprogrammed flash: interpreter r0=%d, JIT r0=%d "
               "(-1: loop never compiled, -2: a data write dropped the block)\n",
               interp, jit);
    }
}

/*
 * A load followed by an arithmetic shift of the loaded register in the same
 * block: LDR r1, [r0]; ASRS r2, r1, #n; B back.  The one-instruction sweep
 * above never pairs them.  The compiled ASRS shifted a zero-extended copy of
 * a negative r1 (0x84562127 asr 1 gave 0x422b1093, the interpreter
 * 0xc22b1093): found by the idle-skip fuzzer.
 */
static uint32_t run_load_asr(int jit, uint32_t value, int n, int *compiled) {
    arm_cpu_t cpu;
    setup(&cpu, 0x6801);                                     /* ldr r1, [r0, #0] */
    if (!jit) cpu.jit_cache_size = 0;
    uint16_t asrs = (uint16_t)(0x1000u | ((unsigned)n << 6) | (1u << 3) | 2u);
    uint32_t off = CODE_BASE - ARM_FLASH_BASE;
    cpu.flash[off + 2] = asrs & 0xFF; cpu.flash[off + 3] = asrs >> 8;  /* asrs r2, r1, #n */
    cpu.flash[off + 4] = 0xFC;        cpu.flash[off + 5] = 0xE7;       /* b    CODE_BASE */
    uint32_t data = ARM_SRAM_BASE + 0x200;
    cpu.sram[0x200] = value & 0xFF;          cpu.sram[0x201] = (value >> 8) & 0xFF;
    cpu.sram[0x202] = (value >> 16) & 0xFF;  cpu.sram[0x203] = value >> 24;
    cpu.reg[0] = data;
    for (int i = 0; i < 400; i++) arm_step(&cpu, 3);
    *compiled = cpu.jit_blocks_run > 0;
    uint32_t r2 = cpu.reg[2];
    arm_cpu_destroy(&cpu);
    return r2;
}

static void test_load_then_asr(void) {
    static const uint32_t values[] = { 0x84562127u, 0x80000000u, 0xFFFFFFFFu,
                                       0x7FFFFFFFu, 0x00000001u };
    static const int shifts[] = { 1, 7, 16, 31 };
    int bad = 0, notcompiled = 0;
    for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++)
        for (size_t k = 0; k < sizeof(shifts) / sizeof(shifts[0]); k++) {
            int c0, c1;
            uint32_t a = run_load_asr(0, values[v], shifts[k], &c0);
            uint32_t b = run_load_asr(1, values[v], shifts[k], &c1);
            if (!c1) notcompiled++;
            if (a != b) {
                bad++;
                printf("  FAIL: ldr; asrs #%d of 0x%08x: interp=0x%08x jit=0x%08x\n",
                       shifts[k], values[v], a, b);
            }
        }
    if (!bad && !notcompiled) {
        passed++;
        if (verbose) printf("  PASS: a load then ASRS of the loaded register, compiled\n");
    } else {
        failed++;
        if (notcompiled) printf("  FAIL: ldr; asrs loop not compiled (%d cases)\n", notcompiled);
    }
}

/*
 * arm_flash_changed looks only at the cache slots of start addresses a block
 * overlapping the range can have (the block-size bound), not at all 65536.
 * Fill the cache with a block at every halfword of 1 KB of code (lengths 1
 * to the 32-instruction cap, so they overlap heavily), change random ranges
 * in and around it, and require exactly the overlapping blocks to be gone,
 * then every block for a change wider than the cache.
 */
#define FC_CODE  1024u
#define FC_N     (FC_CODE / 2)

/* The blocks' bounds, kept apart: a dropped block is freed. */
static uint32_t fc_start[FC_N], fc_end[FC_N];

static int fc_fill(arm_cpu_t *cpu, arm_compiled_block_t **blk) {
    int made = 0;
    for (uint32_t k = 0; k < FC_N; k++) {
        uint32_t pc = CODE_BASE + 2 * k, ci = (pc >> 1) & (ARM_JIT_CACHE_SLOTS - 1);
        if (blk[k] && cpu->jit_cache[ci] == blk[k]) continue;
        arm_basic_block_t b;
        arm_decode_block(cpu->flash, cpu->flash_base, cpu->flash_end - cpu->flash_base, pc, &b);
        blk[k] = arm_jit_compile(&b, cpu);
        if (!blk[k]) continue;
        cpu->jit_cache[ci] = blk[k];
        fc_start[k] = blk[k]->start_pc;
        fc_end[k] = blk[k]->end_pc;
        if (cpu->jit_code_hi == 0 || blk[k]->start_pc < cpu->jit_code_lo) cpu->jit_code_lo = blk[k]->start_pc;
        if (blk[k]->end_pc > cpu->jit_code_hi) cpu->jit_code_hi = blk[k]->end_pc;
        made++;
    }
    return made;
}

static void test_flash_changed_window(void) {
    arm_cpu_t cpu;
    setup(&cpu, 0x3101);
    uint32_t off = CODE_BASE - ARM_FLASH_BASE;
    for (uint32_t i = 0; i < FC_CODE + 64; i += 2) {
        uint16_t hw = (rnd() % 12 == 0) ? 0xE7FE : 0x3101;     /* b . / adds r1, #1 */
        cpu.flash[off + i] = hw & 0xFF; cpu.flash[off + i + 1] = hw >> 8;
    }
    static arm_compiled_block_t *blk[FC_N];
    memset(blk, 0, sizeof blk);
    fc_fill(&cpu, blk);
    int longest = 0, bad = 0;
    for (uint32_t k = 0; k < FC_N; k++)
        if (blk[k] && (int)(fc_end[k] - fc_start[k]) > longest)
            longest = (int)(fc_end[k] - fc_start[k]);
    for (int t = 0; t < 400 && !bad; t++) {
        uint32_t addr = CODE_BASE - 100 + rnd() % (FC_CODE + 200);
        uint32_t len = (t % 4 == 0) ? 1u + rnd() % 4 : 1u + rnd() % 200;
        if (t == 399) { addr = cpu.flash_base; len = cpu.flash_end - cpu.flash_base; }
        arm_flash_changed(&cpu, addr, len);
        for (uint32_t k = 0; k < FC_N; k++) {
            if (!blk[k]) continue;
            uint32_t ci = ((CODE_BASE + 2 * k) >> 1) & (ARM_JIT_CACHE_SLOTS - 1);
            int overlaps = fc_start[k] < (uint64_t)addr + len && fc_end[k] > addr;
            int kept = cpu.jit_cache[ci] == blk[k];
            if (kept == overlaps) {
                printf("  FAIL: flash change [0x%x, +%u): block [0x%x, 0x%x) %s\n", addr, len,
                       fc_start[k], fc_end[k], kept ? "kept" : "dropped");
                bad = 1;
                break;
            }
            if (!kept) blk[k] = NULL;
        }
        if (t < 399) fc_fill(&cpu, blk);
    }
    arm_cpu_destroy(&cpu);
    if (!bad && longest == ARM_MAX_BLOCK_SIZE * 2) {
        passed++;
        if (verbose) printf("  PASS: a flash change drops exactly the blocks it overlaps\n");
    } else {
        failed++;
        if (!bad) printf("  FAIL: flash-change window: longest block %d bytes, want %d\n",
                         longest, ARM_MAX_BLOCK_SIZE * 2);
    }
}

int run_arm_jit_tests(int verbose_flag) {
    verbose = verbose_flag;
    passed = failed = 0;
    n_side_exit = n_compiled = n_refused = 0;

    /* One-instruction blocks are the whole point here, and the default floor
     * would refuse most of them.  Set it before anything can compile, since
     * arm_jit_min_block() caches on first use. */
    setenv("CSIM_ARM_JIT_MIN_BLOCK", "1", 1);
    csim_lightning_init();

    printf("=== ARM JIT differential tests (generated code vs interpreter) ===\n");
    printf("Exhaustive over all 65536 Thumb halfwords, %d random states each;\n"
           "compiled one-instruction blocks vs arm_step().\n\n", TRIALS);

    /* arm_jit_compile() reads only the memory layout off the CPU, which is
     * identical for every cc2538 instance, so one template serves the whole
     * sweep and each encoding is compiled once rather than once per trial. */
    arm_cpu_t tmpl;
    setup(&tmpl, 0);

    for (uint32_t h = 0; h < 0x10000u; h++) {
        uint16_t hw = (uint16_t)h;

        uint8_t img[8] = { (uint8_t)(hw & 0xFF), (uint8_t)(hw >> 8),
                           0xFF, 0xF7, 0, 0, 0, 0 };
        arm_basic_block_t blk;
        arm_decode_block(img, CODE_BASE, sizeof(img), CODE_BASE, &blk);
        if (blk.length != 1) continue;          /* unsupported encoding */

        arm_compiled_block_t *cb = arm_jit_compile(&blk, &tmpl);
        if (!cb) { n_refused++; continue; }     /* refused — never mis-run */
        n_compiled++;

        for (int t = 0; t < TRIALS; t++) {
            arm_cpu_t a, b;
            setup(&a, hw);
            setup(&b, hw);

            for (int r = 0; r <= 12; r++) {
                uint32_t v = (t < 4) ? interesting(r + t) : rnd();
                a.reg[r] = b.reg[r] = v;
            }
            a.reg[ARM_SP] = b.reg[ARM_SP] = ARM_SRAM_BASE + 0x400;

            /*
             * Half the trials aim the base register into SRAM so the inline
             * memory path runs; the rest leave it wild so the guarded side
             * exit runs.  Both have to agree with the interpreter — the side
             * exit by leaving state untouched, which is the property the whole
             * block design depends on.
             */
            const arm_decoded_insn_t *di = &blk.insns[0];
            if ((t & 1) && (di->klass == ARM_DEC_LOAD ||
                            di->klass == ARM_DEC_STORE)) {
                uint32_t base = ARM_SRAM_BASE + (rnd() % (ARM_SRAM_SIZE / 2));
                base &= ~3u;
                a.reg[di->rn] = b.reg[di->rn] = base;
                if (di->rm != ARM_DEC_NO_RM)
                    a.reg[di->rm] = b.reg[di->rm] = 0;
            }

            uint32_t flags = (rnd() & 0xF0000000u);
            a.xpsr = (a.xpsr & ~0xF0000000u) | flags;
            b.xpsr = (b.xpsr & ~0xF0000000u) | flags;

            /* A: the interpreter, with its own JIT cache disabled so this
             * side of the comparison is never the thing under test. */
            a.jit_cache_size = 0;
            arm_step(&a, 1);

            /* B: the generated code, with the dispatcher's own accounting. */
            uint32_t entry_reg[16];
            memcpy(entry_reg, b.reg, sizeof(entry_reg));
            uint32_t entry_xpsr   = b.xpsr;
            int64_t  entry_cycles = b.cycles;

            b.jit_iter_budget = 1;
            b.jit_partial     = -1;
            cb->fn(&b);

            int partial = b.jit_partial;
            if (partial >= 0) {
                /*
                 * Side exit at instruction 0: nothing ran.  The contract says
                 * architectural state is untouched and PC is left at the
                 * faulting instruction, so check exactly that rather than
                 * comparing against the interpreter (which did execute it).
                 */
                n_side_exit++;
                int bad = 0;
                for (int r = 0; r < 15; r++)
                    if (b.reg[r] != entry_reg[r]) bad = 1;
                if (b.reg[ARM_PC] != CODE_BASE) bad = 1;
                if ((b.xpsr & 0xF0000000u) != (entry_xpsr & 0xF0000000u)) bad = 1;
                if (b.cycles != entry_cycles) bad = 1;
                if (bad) {
                    printf("  FAIL: hw=0x%04x trial=%d  side exit did not "
                           "leave state untouched (pc=0x%08x)\n",
                           hw, t, b.reg[ARM_PC]);
                    failed++;
                } else {
                    passed++;
                }
            } else {
                b.cycles       += cb->cycles_total;
                b.instructions += cb->length;
                if (compare(hw, t, &a, &b, &blk)) failed++;
                else passed++;
            }

            arm_cpu_destroy(&a);
            arm_cpu_destroy(&b);
        }
        arm_jit_free(cb);
    }
    arm_cpu_destroy(&tmpl);

    test_flash_rewrite();
    test_load_then_asr();
    test_flash_changed_window();

    printf("  blocks compiled: %d, refused: %d, guarded side exits: %d\n",
           n_compiled, n_refused, n_side_exit);
    printf("\n--- Results: %d passed, %d failed ---\n", passed, failed);
    return failed;
}

#endif /* HAVE_LIGHTNING */
