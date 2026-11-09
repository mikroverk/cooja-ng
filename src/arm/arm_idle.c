/*
 * Idle-loop skipping for ARM cores: the proof (a dry run on a copy of the
 * CPU), the landing model and the fast-forward.  The rule is in
 * include/arm/arm_idle.h, the design in docs/idle-skip.md; the mote side
 * (lazy execute slices, catching up before anything outside the CPU looks)
 * is in src/motes/arm_elf_mote.c.
 */
#include "arm_idle.h"
#include "arm_cpu.h"
#include "arm_nvic.h"
#include "arm_decode.h"
#ifdef HAVE_LIGHTNING
#include "arm_jit.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Retry pacing.  An attempt cut short by something happening (an event
 * coming due, an exception, WFI) is retried at the next slice: the loop is
 * probably idle around it.  One that found work that is not a fixed point
 * (a register the classifier refuses, stores that do not undo themselves,
 * a long computation) waits twice as long each time, up to
 * ARM_IDLE_BACKOFF_MAX slices, so busy firmware pays for one short dry run
 * every few milliseconds. */
#define ARM_IDLE_BACKOFF_MAX 4096
#define ARM_IDLE_WHASH       1024   /* SRAM overlay hash slots (power of 2) */
#define ARM_IDLE_SNAPS          4   /* returns to the start compared against  */
#define ARM_IDLE_FIRST_VISIT 1024   /* instructions to the first return       */

/* A core state the dry run compares: registers, flags, masks, banked stack
 * pointers and limits, FP state, the WFE latch. */
typedef struct {
    uint32_t reg[16], xpsr, primask, basepri, faultmask, msp, psp;
    uint32_t msplim_s, msplim_ns, psplim_s, psplim_ns, fpscr;
    uint32_t vfp_s[32];
    uint8_t  it_state, event_latch;
    bool     use_psp;
} idle_core_t;

struct arm_idle_dry {
    arm_cpu_t *real;
    bool     failed, proven;
    int      why;
    uint32_t head;
    int64_t  c0, i0;
    int      k, visits;
    /* SRAM overlay: the copy's stores, by aligned word. */
    uint32_t w_addr[ARM_IDLE_WLOG], w_val[ARM_IDLE_WLOG];
    int      w_n;
    int16_t  w_hash[ARM_IDLE_WHASH];
    /* Peripheral stores the copy made (buffered, never performed). */
    struct arm_io_region *io_r[ARM_IDLE_IOLOG];
    uint32_t io_addr[ARM_IDLE_IOLOG], io_val[ARM_IDLE_IOLOG], io_old[ARM_IDLE_IOLOG];
    uint8_t  io_w[ARM_IDLE_IOLOG];
    int      io_n;
    int64_t  timed_off[ARM_IDLE_TIMED], timed_until[ARM_IDLE_TIMED];
    int      timed_n;
    /* The state at the start and at the last few returns to the start PC:
     * a return that matches one of them closes the loop, with everything
     * before that snapshot as the pre-period. */
    struct arm_idle_snap {
        bool     valid;
        int      k;
        int      w_n, io_n;
        idle_core_t core;
        uint32_t w_val[ARM_IDLE_WLOG];
        uint32_t io_val[ARM_IDLE_IOLOG];
    } snap[ARM_IDLE_SNAPS];
    int      snap_next;
};

/* --- Diagnostics (CSIM_IDLE_SKIP_STATS=1 / CSIM_IDLE_SKIP_TRACE=1) --- */

static int idle_env(const char *name, int *cache) {
    if (*cache < 0) *cache = getenv(name) != NULL;
    return *cache;
}
static int idle_trace_on(void) { static int v = -1; return idle_env("CSIM_IDLE_SKIP_TRACE", &v); }
static int idle_stats_on(void) { static int v = -1; return idle_env("CSIM_IDLE_SKIP_STATS", &v); }
/* CSIM_IDLE_SKIP_CHECK=1: a proof never latches, so every slice it covers is
 * predicted by the model and then run for real and compared (a check of
 * the model over whole runs; nothing is skipped). */
static int idle_check_on(void) { static int v = -1; return idle_env("CSIM_IDLE_SKIP_CHECK", &v); }

static struct {
    uint64_t attempts, proofs, latches, lazy_slices, catch_ups;
    uint64_t memo_hits, memo_misses, memo_pre, canon_reused, checked, mismatches;
    uint64_t skipped_iters, skipped_insns;
    uint64_t why[ARM_IDLE_WHY_COUNT];
} idle_total;

static const char *const idle_why_name[ARM_IDLE_WHY_COUNT] = {
    "event", "exception", "wfi", "trap", "io", "store-log", "budget",
    "reset", "flash", "model", "mismatch", "external",
};

__attribute__((destructor, used)) static void arm_idle_dump(void) {
    if (!idle_stats_on() || idle_total.attempts == 0) return;
    fprintf(stderr, "IDLE-SKIP: dry-runs=%llu proofs=%llu latched=%llu "
            "lazy-slices=%llu catch-ups=%llu skipped-iterations=%llu "
            "skipped-instructions=%llu; ended by:",
            (unsigned long long)idle_total.attempts,
            (unsigned long long)idle_total.proofs,
            (unsigned long long)idle_total.latches,
            (unsigned long long)idle_total.lazy_slices,
            (unsigned long long)idle_total.catch_ups,
            (unsigned long long)idle_total.skipped_iters,
            (unsigned long long)idle_total.skipped_insns);
    fprintf(stderr, " model: memo-hits=%llu misses=%llu pre-period=%llu loops-reused=%llu "
            "slices-checked=%llu mismatches=%llu;",
            (unsigned long long)idle_total.memo_hits, (unsigned long long)idle_total.memo_misses,
            (unsigned long long)idle_total.memo_pre, (unsigned long long)idle_total.canon_reused,
            (unsigned long long)idle_total.checked, (unsigned long long)idle_total.mismatches);
    for (int i = 0; i < ARM_IDLE_WHY_COUNT; i++)
        if (idle_total.why[i])
            fprintf(stderr, " %s=%llu", idle_why_name[i],
                    (unsigned long long)idle_total.why[i]);
    fputc('\n', stderr);
}

void arm_idle_count_lazy(void) { idle_total.lazy_slices++; }

/* --- Setup --- */

void arm_idle_enable(arm_cpu_t *cpu, bool on) {
    arm_idle_state_t *s = &cpu->idle;
    if (!on) {
        arm_idle_drop(cpu, ARM_IDLE_WHY_EXTERNAL);
        s->enabled = false;
        return;
    }
    if (!s->proof) s->proof = calloc(1, sizeof(*s->proof));
    if (!s->copy) s->copy = malloc(sizeof(arm_cpu_t));
    if (!s->dry) s->dry = malloc(sizeof(arm_idle_dry_t));
    if (!s->proof || !s->copy || !s->dry) {
        fprintf(stderr, "idle-skip: out of memory; node runs unskipped\n");
        arm_idle_free(cpu);
        return;
    }
    s->enabled = true;
    s->backoff = 1;
    s->cooldown = 0;
}

void arm_idle_free(arm_cpu_t *cpu) {
    arm_idle_state_t *s = &cpu->idle;
    if (s->state == ARM_IDLE_DRY) return;   /* a copy owns nothing */
    free(s->proof); free(s->copy); free(s->dry);
    s->proof = NULL; s->copy = NULL; s->dry = NULL;
    s->enabled = false;
    s->state = ARM_IDLE_OFF;
    s->lazy = false;
}

void arm_idle_set_io_classifier(arm_cpu_t *cpu, arm_idle_io_fn fn, void *user) {
    cpu->idle.io_class = fn;
    cpu->idle.io_user = user;
}

static void idle_why(arm_idle_state_t *s, int why) {
    if (why < 0 || why >= ARM_IDLE_WHY_COUNT) return;
    s->why[why]++;
    idle_total.why[why]++;
}

void arm_idle_drop(arm_cpu_t *cpu, int why) {
    arm_idle_state_t *s = &cpu->idle;
    if (s->state == ARM_IDLE_DRY) return;
    if (s->state != ARM_IDLE_OFF) {
        if (idle_trace_on())
            fprintf(stderr, "[idle] cpu=%p cyc=%lld drop (%s) after %lld lazy slices, cost %lld\n",
                    (void *)cpu, (long long)cpu->cycles, idle_why_name[why], (long long)s->gained, (long long)s->cost);
        idle_why(s, why);
        /* Did the proof pay for its dry run?  A dry run costs a few times
         * the cycles it interprets; a proof that bought fewer skipped
         * cycles than that (a loop that reads a fast clock, a burst of
         * radio bytes) makes the next attempt wait twice as long. */
        int64_t per_slice = (int64_t)cpu->cpu_freq_hz / 1000000LL;
        if (s->gained * per_slice < 4 * s->cost) {
            s->cooldown = s->backoff;
            if (s->backoff < ARM_IDLE_BACKOFF_MAX) s->backoff *= 2;
        } else {
            s->backoff = 1;
            s->cooldown = 1;
        }
    }
    s->state = ARM_IDLE_OFF;
    s->lazy = false;
}

/* --- The dry run --- */

void arm_idle_dry_fail(arm_cpu_t *copy, int why) {
    arm_idle_dry_t *d = copy->idle.dry;
    if (!d->failed) { d->failed = true; d->why = why; }
    copy->stopping = true;
}

static int idle_whash(uint32_t w) { return (int)((w >> 2) * 2654435761u >> 22) & (ARM_IDLE_WHASH - 1); }

/* The overlay slot for SRAM word `w`, or -1; with `make`, created from the
 * real word on first use. */
static int idle_overlay(arm_idle_dry_t *d, uint32_t w, bool make) {
    int h = idle_whash(w);
    for (int probe = 0; probe < ARM_IDLE_WHASH; probe++, h = (h + 1) & (ARM_IDLE_WHASH - 1)) {
        int e = d->w_hash[h];
        if (e == 0) break;
        if (d->w_addr[e - 1] == w) return e - 1;
    }
    if (!make || d->w_n == ARM_IDLE_WLOG) return -1;
    const arm_cpu_t *real = d->real;
    const uint8_t *p = real->sram + (w - real->sram_base);
    int e = d->w_n++;
    d->w_addr[e] = w;
    d->w_val[e] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                  ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    d->w_hash[h] = (int16_t)(e + 1);
    return e;
}

uint32_t arm_idle_dry_sram(arm_cpu_t *copy, uint32_t addr, int n, bool is_write,
                           uint32_t val) {
    arm_idle_dry_t *d = copy->idle.dry;
    const arm_cpu_t *real = d->real;
    uint32_t v = 0;
    if ((addr & 3u) + (uint32_t)n <= 4) {          /* within one word */
        uint32_t w = addr & ~3u;
        int sh = (int)(addr & 3u) * 8;
        uint32_t m = n == 4 ? 0xFFFFFFFFu : ((1u << (8 * n)) - 1u) << sh;
        int e = idle_overlay(d, w, is_write);
        if (is_write) {
            if (e < 0) { arm_idle_dry_fail(copy, ARM_IDLE_WHY_WLOG); return 0; }
            d->w_val[e] = (d->w_val[e] & ~m) | ((val << sh) & m);
            return 0;
        }
        uint32_t word;
        if (e >= 0) word = d->w_val[e];
        else {
            const uint8_t *p = real->sram + (w - real->sram_base);
            word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }
        return (word & m) >> sh;
    }
    for (int b = 0; b < n; b++) {
        uint32_t a = addr + (uint32_t)b, w = a & ~3u;
        int sh = (int)(a & 3u) * 8;
        if (is_write) {
            int e = idle_overlay(d, w, true);
            if (e < 0) { arm_idle_dry_fail(copy, ARM_IDLE_WHY_WLOG); return 0; }
            d->w_val[e] = (d->w_val[e] & ~(0xFFu << sh)) | (((val >> (8 * b)) & 0xFFu) << sh);
        } else {
            int e = idle_overlay(d, w, false);
            uint32_t byte = e >= 0 ? (d->w_val[e] >> sh) & 0xFFu
                                   : real->sram[a - real->sram_base];
            v |= byte << (8 * b);
        }
    }
    return v;
}

static uint32_t idle_mask(uint32_t v, int width) {
    return width == 1 ? (v & 0xFFu) : width == 2 ? (v & 0xFFFFu) : v;
}

/* A read through the real register map, as the real core would make it. */
static uint32_t idle_real_read(arm_cpu_t *real, struct arm_io_region *r,
                               uint32_t addr, int width) {
    uint8_t w = real->io_width;
    real->io_width = (uint8_t)width;
    uint32_t v = (uint32_t)r->read(r->user_data, addr);
    real->io_width = w;
    return idle_mask(v, width);
}

/* The core's own registers.  Reads of the NVIC and SCB configuration and
 * status words have no side effect and change only by a store, an event
 * or an exception (each of which ends a dry run).  SysTick's CTRL clears
 * COUNTFLAG when read and its counter runs every cycle, the DWT counter
 * likewise: refused, as is every store. */
static int idle_core_class(uint32_t addr, bool is_write) {
    if (is_write) return ARM_IDLE_IO_REFUSE;
    if (addr >= 0xE000E100u && addr < 0xE000E500u) return ARM_IDLE_IO_PURE; /* NVIC */
    if (addr >= 0xE000ED00u && addr < 0xE000ED40u) return ARM_IDLE_IO_PURE; /* SCB  */
    return ARM_IDLE_IO_REFUSE;
}

uint32_t arm_idle_dry_io(arm_cpu_t *copy, struct arm_io_region *r, uint32_t addr,
                         bool is_write, uint32_t val, int width) {
    arm_idle_dry_t *d = copy->idle.dry;
    arm_cpu_t *real = d->real;
    const arm_idle_state_t *s = &real->idle;
    arm_idle_io_info_t info = { .until = 0, .value = 0, .key = addr };
    int v = addr >= 0xE0000000u ? idle_core_class(addr, is_write)
          : s->io_class ? s->io_class(s->io_user, addr, is_write, val, width,
                                      copy->cycles, &info)
          : ARM_IDLE_IO_REFUSE;
    /* A verdict that does not fit the direction is a classifier bug; it
     * must not become a proof. */
    if ((is_write && (v != ARM_IDLE_IO_STORE || width != 4)) ||
        (!is_write && v != ARM_IDLE_IO_PURE && v != ARM_IDLE_IO_TIMED))
        v = ARM_IDLE_IO_REFUSE;
    if (v == ARM_IDLE_IO_REFUSE) {
        if (idle_trace_on())
            fprintf(stderr, "[idle] cpu=%p refused %s 0x%08x (w%d val=0x%x) at pc~0x%08x\n",
                    (void *)real, is_write ? "store" : "read", addr, width, val,
                    copy->reg[ARM_PC]);
        arm_idle_dry_fail(copy, ARM_IDLE_WHY_IO);
        return 0;
    }
    int e;
    for (e = 0; e < d->io_n; e++)
        if (d->io_r[e] == r && d->io_addr[e] == info.key) break;
    if (is_write) {
        if (e == d->io_n) {
            if (d->io_n == ARM_IDLE_IOLOG) { arm_idle_dry_fail(copy, ARM_IDLE_WHY_WLOG); return 0; }
            d->io_r[e] = r;
            d->io_addr[e] = info.key;
            d->io_w[e] = 4;
            d->io_old[e] = d->io_val[e] = idle_real_read(real, r, info.key, 4);
            d->io_n++;
        }
        d->io_val[e] = val;
        return 0;
    }
    if (e < d->io_n) {                                     /* state the copy stored */
        /* The buffered value is the key's 32 bits: a narrow read is answered
         * from it only at the key's own address (arm_idle.h). */
        if (width != 4 && addr != info.key) { arm_idle_dry_fail(copy, ARM_IDLE_WHY_IO); return 0; }
        return idle_mask(d->io_val[e], width);
    }
    if (v == ARM_IDLE_IO_TIMED) {
        if (info.until <= copy->cycles || d->timed_n == ARM_IDLE_TIMED) {
            arm_idle_dry_fail(copy, ARM_IDLE_WHY_IO);
            return 0;
        }
        d->timed_off[d->timed_n] = copy->cycles - d->c0;
        d->timed_until[d->timed_n] = info.until;
        d->timed_n++;
        return idle_mask(info.value, width);
    }
    return idle_real_read(real, r, addr, width);
}

static void idle_core_save(const arm_cpu_t *c, idle_core_t *o) {
    memcpy(o->reg, c->reg, sizeof(o->reg));
    o->xpsr = c->xpsr; o->primask = c->primask; o->basepri = c->basepri;
    o->faultmask = c->faultmask; o->msp = c->msp; o->psp = c->psp;
    o->msplim_s = c->msplim_s; o->msplim_ns = c->msplim_ns;
    o->psplim_s = c->psplim_s; o->psplim_ns = c->psplim_ns; o->fpscr = c->fpscr;
    memcpy(o->vfp_s, c->vfp_s, sizeof(o->vfp_s));
    o->it_state = c->it_state; o->event_latch = c->event_latch; o->use_psp = c->use_psp;
}

static bool idle_core_eq(const arm_cpu_t *c, const idle_core_t *o) {
    return memcmp(o->reg, c->reg, sizeof(o->reg)) == 0 &&
           o->xpsr == c->xpsr && o->primask == c->primask &&
           o->basepri == c->basepri && o->faultmask == c->faultmask &&
           o->msp == c->msp && o->psp == c->psp &&
           o->msplim_s == c->msplim_s && o->msplim_ns == c->msplim_ns &&
           o->psplim_s == c->psplim_s && o->psplim_ns == c->psplim_ns &&
           o->fpscr == c->fpscr &&
           memcmp(o->vfp_s, c->vfp_s, sizeof(o->vfp_s)) == 0 &&
           o->it_state == c->it_state && o->event_latch == c->event_latch &&
           o->use_psp == c->use_psp && !c->cpu_off && !c->reset_pending;
}

static uint32_t idle_real_word(const arm_cpu_t *real, uint32_t w) {
    const uint8_t *p = real->sram + (w - real->sram_base);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Take a snapshot of the copy at a return to the start PC. */
static void idle_snap_take(arm_idle_dry_t *d, const arm_cpu_t *copy, int slot) {
    struct arm_idle_snap *sn = &d->snap[slot];
    idle_core_save(copy, &sn->core);
    sn->k = d->k;
    sn->w_n = d->w_n;
    memcpy(sn->w_val, d->w_val, (size_t)d->w_n * sizeof(uint32_t));
    sn->io_n = d->io_n;
    memcpy(sn->io_val, d->io_val, (size_t)d->io_n * sizeof(uint32_t));
    sn->valid = true;
}

/* The copy is in the state snapshot `sn` recorded: same core, and every
 * SRAM word and peripheral register it has stored to holds what it held
 * then (a word first stored to after the snapshot must hold its original,
 * real value, which is what it held then). */
static bool idle_snap_match(const arm_idle_dry_t *d, const arm_cpu_t *copy,
                            const struct arm_idle_snap *sn) {
    if (!sn->valid || !idle_core_eq(copy, &sn->core)) return false;
    for (int e = 0; e < d->w_n; e++) {
        uint32_t then = e < sn->w_n ? sn->w_val[e] : idle_real_word(d->real, d->w_addr[e]);
        if (d->w_val[e] != then) return false;
    }
    for (int e = 0; e < d->io_n; e++) {
        uint32_t then = e < sn->io_n ? sn->io_val[e] : d->io_old[e];
        if (d->io_val[e] != then) return false;
    }
    return true;
}

/* Would a compiled load/store at this boundary stay inline?  The JIT's two
 * guards (arm_jit.c emit_mem): aligned, and inside SRAM.  The address is
 * worked out from the Thumb-16 load/store encodings the decoder accepts
 * (arm_decode.c, top5 10-19; 9 is the literal load, which has no guard);
 * an encoding the decoder refuses is never inside a block, so its answer
 * here is never used. */
static uint8_t idle_jit_ok(const arm_cpu_t *copy, const arm_cpu_t *real, uint32_t pc) {
    /* 64-bit: pc 0xFFFFFFFE (a BLX to an erased-flash pointer) wrapped pc + 2
     * to 0 and read the host's flash array 4 GiB out of range. */
    if (pc < real->flash_base || (uint64_t)pc + 2 > real->flash_end) return 1;
    uint16_t hw = (uint16_t)(real->flash[pc - real->flash_base] |
                             (real->flash[pc - real->flash_base + 1] << 8));
    unsigned top5 = hw >> 11;
    uint32_t a, msize;
    if (top5 == 10 || top5 == 11) {
        static const uint8_t sz[8] = { 4, 2, 1, 1, 4, 2, 1, 2 };
        msize = sz[(hw >> 9) & 7];
        a = copy->reg[(hw >> 3) & 7] + copy->reg[(hw >> 6) & 7];
    } else if (top5 >= 12 && top5 <= 17) {
        static const uint8_t sz[6] = { 4, 4, 1, 1, 2, 2 };
        msize = sz[top5 - 12];
        a = copy->reg[(hw >> 3) & 7] + ((hw >> 6) & 0x1Fu) * msize;
    } else if (top5 == 18 || top5 == 19) {
        msize = 4;
        a = copy->reg[ARM_SP] + ((uint32_t)(hw & 0xFF) << 2);
    } else {
        return 1;
    }
    uint32_t size = real->sram_end - real->sram_base;
    if (msize > 1 && (a & (msize - 1))) return 0;
    return (uint32_t)(a - real->sram_base) < size - (msize - 1);
}

static void idle_record(arm_idle_dry_t *d, arm_cpu_t *copy, int k, uint32_t pc) {
    arm_idle_proof_t *pr = d->real->idle.proof;
    pr->pc[k] = pc;
    pr->it[k] = copy->it_state;
    pr->jit_ok[k] = idle_jit_ok(copy, d->real, pc);
}

static void idle_dry_hook(void *user, uint32_t pc) {
    arm_cpu_t *copy = (arm_cpu_t *)user;
    arm_idle_dry_t *d = copy->idle.dry;
    if (d->failed || d->proven) { copy->stopping = true; return; }
    int64_t k = copy->instructions - d->i0;
    /* One call per instruction (IT-skipped ones included), or the trace
     * would not index instructions. */
    if (k != d->k + 1) { arm_idle_dry_fail(copy, ARM_IDLE_WHY_MODEL); return; }
    d->k = (int)k;
    int64_t dc = copy->cycles - d->c0;
    if (dc <= 0 || dc > (int64_t)UINT32_MAX / 2) { arm_idle_dry_fail(copy, ARM_IDLE_WHY_BUDGET); return; }
    arm_idle_proof_t *pr = d->real->idle.proof;
    pr->pre[k] = (uint32_t)dc;
    if (pc == d->head) {
        for (int i = 0; i < ARM_IDLE_SNAPS; i++) {
            const struct arm_idle_snap *sn = &d->snap[i];
            if (idle_snap_match(d, copy, sn)) {
                d->proven = true;
                pr->npre = sn->k;
                pr->n = (int)k - sn->k;
                pr->period = dc - pr->pre[sn->k];
                copy->stopping = true;
                return;
            }
        }
        if (++d->visits > ARM_IDLE_VISITS) { arm_idle_dry_fail(copy, ARM_IDLE_WHY_BUDGET); return; }
        /* Keep the start (slot 0) and the latest returns. */
        idle_snap_take(d, copy, 1 + d->snap_next);
        d->snap_next = (d->snap_next + 1) % (ARM_IDLE_SNAPS - 1);
    }
    /* A loop comes back to the instruction it was entered at within one
     * pass; a dry run that has not, a while in, started outside any loop
     * (in code that runs once) and will not prove anything. */
    if (k >= ARM_IDLE_MAXN || (d->visits == 0 && k >= ARM_IDLE_FIRST_VISIT)) {
        arm_idle_dry_fail(copy, ARM_IDLE_WHY_BUDGET);
        return;
    }
    idle_record(d, copy, (int)k, pc);
}

#ifdef HAVE_LIGHTNING
extern int arm_jit_batch_size(void);
#else
static int arm_jit_batch_size(void) { return 0; }   /* no JIT, no batches */
#endif

/* The interpreter's diagnostic environment hooks see every instruction it
 * runs (a PC trace, a PC or memory watch, the SP audit, the wild-jump and
 * LR traps, the exception and MMIO traces): with one on, a dry run would
 * show up in them and skipped instructions would not, so nothing is
 * skipped. */
static bool idle_debug_env(void) {
    static int v = -1;
    if (v < 0) {
        static const char *const names[] = {
            "ARM_TRACE_PC", "ARM_PC_WATCH", "ARM_MEM_WATCH", "ZEPHYR_THREADS",
            "ARM_SP_AUDIT", "ARM_BL_PROBE", "ARM_WILD_TRAP", "ARM_LR_WATCH",
            "ARM_WATCH", "ARM_EXC_TRACE", "ARM_MMIO_TRACE", "ARM_TZ_TRACE", NULL
        };
        v = 0;
        for (int i = 0; names[i]; i++)
            if (getenv(names[i])) v = 1;
    }
    return v != 0;
}

/* Booth's least rotation of key[0..n): the start index of the
 * lexicographically least rotation. */
static int idle_least_rotation(arm_idle_proof_t *pr, const uint64_t *key, int n) {
    uint64_t *S = pr->scratch_key;
    int32_t *f = pr->scratch_f;
    for (int i = 0; i < 2 * n; i++) { S[i] = key[i % n]; f[i] = -1; }
    int k = 0;
    for (int j = 1; j < 2 * n; j++) {
        uint64_t sj = S[j];
        int i = f[j - k - 1];
        while (i != -1 && sj != S[k + i + 1]) {
            if (sj < S[k + i + 1]) k = j - i - 1;
            i = f[i];
        }
        if (sj != S[k + i + 1]) {          /* i == -1 */
            if (sj < S[k]) k = j;
            f[j - k] = -1;
        } else {
            f[j - k] = i + 1;
        }
    }
    return k % n;
}

/* Describe the proof's loop in canonical rotation; keep the model's memory
 * if it is the loop (and block set) the memory was built for. */
static void idle_canonical(arm_idle_state_t *s) {
    arm_idle_proof_t *pr = s->proof;
    int n = pr->n;
    uint64_t *loopkey = pr->loop_key;
    for (int q = 0; q < n; q++) {
        int i = pr->npre + q;
        uint64_t cyc = pr->pre[i + 1] - pr->pre[i];
        loopkey[q] = ((uint64_t)pr->pc[i] << 32) | ((cyc & 0xFFFFu) << 16) |
                     ((uint64_t)pr->it[i] << 8) | pr->jit_ok[i];
    }
    int r = idle_least_rotation(pr, loopkey, n);
    pr->rot = r;
    bool same = pr->c_valid && pr->c_n == n && pr->c_period == pr->period &&
                pr->c_jit_gen == s->jit_gen;
    for (int t = 0; same && t < n; t++)
        same = pr->c_key[t] == loopkey[(t + r) % n];
    if (same) { idle_total.canon_reused++; return; }
    for (int t = 0; t < n; t++) pr->c_key[t] = loopkey[(t + r) % n];
    pr->c_n = n;
    pr->c_period = pr->period;
    pr->c_jit_gen = s->jit_gen;
    pr->c_maxb = 0;
    pr->c_valid = true;
    memset(pr->c_blk_ok, 0, (size_t)n * sizeof(pr->c_blk_ok[0]));
    memset(pr->memo_used, 0, sizeof(pr->memo_used));
}

bool arm_idle_prove(arm_cpu_t *cpu) {
    arm_idle_state_t *s = &cpu->idle;
    if (!s->enabled || s->state != ARM_IDLE_OFF || !s->proof) return false;
    /* Outside what the proof covers: run unskipped.  The security
     * extension and the bus-side permission check keep per-instruction
     * fault state; a co-processor shares the bus; a debugger or a PC hook
     * watches every instruction; the JIT's verify and batch modes change
     * how the dispatcher steps.  These do not go away: look again rarely. */
    if (cpu->tz_enabled || cpu->io_access_check || cpu->coproc ||
        cpu->gdb_stub || cpu->dbg_count > 0 || cpu->pc_callback ||
        cpu->cpu_freq_hz == 0 || cpu->jit_verify ||
        (cpu->jit_cache_size && arm_jit_batch_size() > 0) || idle_debug_env()) {
        s->cooldown = ARM_IDLE_BACKOFF_MAX;
        return false;
    }
    /* A proof needs a whole iteration before the next event: wait it out
     * (slices are 1 µs). */
    int64_t to_event = cpu->next_event_cycle - cpu->cycles;
    if (cpu->next_event_cycle != INT64_MAX && to_event <= 2 * s->last_period + 64) {
        int64_t us = to_event > 0 ? to_event * 1000000LL / (int64_t)cpu->cpu_freq_hz : 0;
        s->cooldown = us > 1000000 ? 1000000 : (int)us + 1;
        return false;
    }
    /* Passing states (in a handler, which returns; an interrupt pending,
     * about to be taken; WFI; a reset or a halt coming): a few slices. */
    arm_nvic_t *nv = (arm_nvic_t *)cpu->nvic;
    if (cpu->cpu_off || cpu->stopping || cpu->reset_pending || cpu->dbg_halted ||
        cpu->flash_wen || (cpu->xpsr & 0x1FFu) != 0 || (nv && nv->has_pending) ||
        (cpu->wfi_skip_guard && cpu->wfi_skip_guard(cpu->wfi_skip_user))) {
        s->cooldown = 4;
        return false;
    }

    s->attempts++;
    idle_total.attempts++;
    arm_idle_dry_t *d = s->dry;
    memset(d, 0, sizeof(*d));
    d->real = cpu;
    d->head = cpu->reg[ARM_PC];
    d->c0 = cpu->cycles;
    d->i0 = cpu->instructions;

    arm_cpu_t *copy = s->copy;
    memcpy(copy, cpu, sizeof(*copy));
    copy->idle.state = ARM_IDLE_DRY;
    copy->idle.dry = d;
    copy->idle.sram_end = cpu->sram_end;
    copy->sram_end = copy->sram_base;      /* every SRAM access out of line */
    copy->jit_cache = NULL;
    copy->jit_exec_count = NULL;
    copy->jit_cache_size = 0;              /* interpreted, instruction by instruction */
    copy->nvic = NULL;                     /* nothing pending; nothing to take */
    copy->event_queue = NULL;              /* an event due ends the dry run */
    copy->coproc = NULL;
    copy->coproc_step = NULL;
    copy->reset_hook = NULL;
    copy->rom_trap = NULL;
    copy->wfi_skip_guard = NULL;
    copy->cycle_limit = INT64_MAX;
    copy->pc_callback = idle_dry_hook;
    copy->pc_callback_data = copy;

    arm_idle_proof_t *pr = s->proof;
    idle_record(d, copy, 0, d->head);
    pr->pre[0] = 0;
    idle_snap_take(d, copy, 0);
    while (!d->failed && !d->proven && copy->instructions - d->i0 <= ARM_IDLE_MAXN) {
        copy->stopping = false;
        if (arm_step(copy, ARM_IDLE_MAXN) == ARM_IDLE_MAXN && !d->proven) {
            arm_idle_dry_fail(copy, ARM_IDLE_WHY_MODEL);   /* no progress */
            break;
        }
    }
    if (!d->proven) {
        if (!d->failed) arm_idle_dry_fail(copy, ARM_IDLE_WHY_BUDGET);
        if (idle_trace_on())
            fprintf(stderr, "[idle] cpu=%p cyc=%lld dry run at pc=0x%08x failed: %s after %d insns (backoff %d)\n",
                    (void *)cpu, (long long)cpu->cycles, d->head, idle_why_name[d->why], d->k, s->backoff);
        idle_why(s, d->why);
        switch (d->why) {
        case ARM_IDLE_WHY_EVENT: {
            /* Wait out the event that ended it (slices are 1 µs). */
            int64_t us = (cpu->next_event_cycle - cpu->cycles) * 1000000LL /
                         (int64_t)cpu->cpu_freq_hz;
            s->cooldown = us > 1000000 ? 1000000 : (int)us + 1;
            break;
        }
        case ARM_IDLE_WHY_EXCEPTION: case ARM_IDLE_WHY_WFI:
            s->cooldown = 1;
            break;
        default:
            s->cooldown = s->backoff;
            if (s->backoff < ARM_IDLE_BACKOFF_MAX) s->backoff *= 2;
            break;
        }
        return false;
    }

    /* After a pre-period of npre instructions, iteration j starts at
     * ref + pre[npre] + j*period.  It may be skipped only if it ends before
     * the next event (an event due at its last boundary would be taken
     * there) and each cycle-timed read in it still returns what the dry
     * run's did (taken at offset o into the iteration, the value held until
     * `until`).  The pre-period's own reads happen once, for real, at the
     * cycles the dry run made them. */
    s->ref = cpu->cycles;
    s->ref_insns = cpu->instructions;
    s->next_event = cpu->next_event_cycle;
    int64_t lim = cpu->next_event_cycle == INT64_MAX
                ? INT64_MAX : cpu->next_event_cycle - pr->period;
    for (int i = 0; i < d->timed_n; i++) {
        int64_t o = d->timed_off[i] - (int64_t)pr->pre[pr->npre];
        if (o < 0) continue;
        int64_t l = d->timed_until[i] - o;
        if (l < lim) lim = l;
    }
    s->limit = lim;
    s->jit_gen_at_proof = s->jit_gen;
    memset(pr->blk_ok, 0, (size_t)pr->npre * sizeof(pr->blk_ok[0]));
    idle_canonical(s);
    s->maxb = pr->c_maxb;
    s->phys.j = s->model.j = 0;
    s->phys.i = s->model.i = 0;
    s->phys.c = s->model.c = s->ref;
    s->iter0 = s->ref + pr->pre[pr->npre];
    s->validated = 0;
    s->lazy = false;
    s->gained = 0;
    s->cost = copy->cycles - d->c0;           /* cycles the dry run interpreted */
    s->last_period = pr->period;
    s->state = ARM_IDLE_VALIDATING;
    s->proofs++;
    idle_total.proofs++;
    if (idle_trace_on())
        fprintf(stderr, "[idle] cpu=%p proof at pc=0x%08x cyc=%lld: %d+%d insns, "
                "%lld cycles, %d stores, %d periph stores, %d timed, limit=%lld\n",
                (void *)cpu, d->head, (long long)cpu->cycles, pr->npre, pr->n,
                (long long)pr->period, d->w_n, d->io_n, d->timed_n,
                (long long)(lim == INT64_MAX ? -1 : lim));
    return true;
}

/* --- The landing model --- */

/* Positions: indices 0..npre-1 are the pre-period (iteration count 0),
 * npre..npre+n-1 the loop, which wraps back to npre. */
#ifdef HAVE_LIGHTNING
static int idle_next(const arm_idle_proof_t *pr, int i) {
    return i + 1 == pr->npre + pr->n ? pr->npre : i + 1;
}
#endif

static void idle_advance(const arm_idle_proof_t *pr, arm_idle_pos_t *p, int64_t n) {
    if (p->i < pr->npre) {
        int64_t m = pr->npre - p->i;
        if (m > n) m = n;
        p->c += (int64_t)pr->pre[p->i + m] - pr->pre[p->i];
        p->i += (int)m;
        n -= m;
        if (n == 0) return;
    }
    int top = pr->npre + pr->n;
    int rest = (int)n;
    if (n >= pr->n) {                     /* a slice is usually shorter than a pass */
        int64_t full = n / pr->n;
        rest = (int)(n % pr->n);
        p->j += full;
        p->c += full * pr->period;
    }
    if (p->i + rest < top) {
        p->c += (int64_t)pr->pre[p->i + rest] - pr->pre[p->i];
        p->i += rest;
    } else {
        p->c += ((int64_t)pr->pre[top] - pr->pre[p->i]) +
                ((int64_t)pr->pre[p->i + rest - pr->n] - pr->pre[pr->npre]);
        p->i += rest - pr->n;
        p->j++;
    }
}

/* A loop position's canonical index (the model's memory is kept by it). */
static int idle_canon(const arm_idle_proof_t *pr, int i) {
    int t = i - pr->npre - pr->rot;
    return t < 0 ? t + pr->n : t;
}

/* Instructions from the proof's start to position p. */
static int64_t idle_insns(const arm_idle_proof_t *pr, const arm_idle_pos_t *p) {
    return p->i + p->j * pr->n;
}

#ifdef HAVE_LIGHTNING
/* The compiled block at index i is the proven sequence's next instructions,
 * instruction for instruction and cycle for cycle. */
static bool idle_block_matches(arm_idle_proof_t *pr, int i,
                               const arm_compiled_block_t *cb) {
    int8_t *cache = i < pr->npre ? &pr->blk_ok[i] : &pr->c_blk_ok[idle_canon(pr, i)];
    if (*cache) return *cache > 0;
    bool ok = cb->length > 0 && cb->length <= ARM_MAX_BLOCK_SIZE;
    int idx = i;
    for (int k = 0; ok && k < cb->length; k++, idx = idle_next(pr, idx)) {
        uint32_t insn_cycles = pr->pre[idx + 1] - pr->pre[idx];
        ok = pr->pc[idx] == cb->start_pc + 2u * (uint32_t)k &&
             insn_cycles == (uint32_t)(cb->cyc_prefix[k + 1] - cb->cyc_prefix[k]);
    }
    *cache = ok ? 1 : -1;
    return ok;
}

/* One probe of arm_step's dispatcher loop at position p with `remaining`
 * instructions of budget and cycle target `limit`: the instructions a
 * compiled block runs (> 0), 0 when the dispatcher hands the budget to
 * the interpreter, -1 when it would change JIT state (count a miss,
 * compile, evict) or meet a block the model does not replay. */
static int idle_probe(arm_cpu_t *cpu, arm_idle_pos_t *p, int remaining,
                      int64_t limit) {
    arm_idle_proof_t *pr = cpu->idle.proof;
    uint32_t pc = pr->pc[p->i];
    if ((pr->it[p->i] & 0xF) != 0) return 0;               /* mid-IT: interpreter */
    if (pc < cpu->flash_base || pc >= cpu->flash_end) return 0;
    uint32_t ci = (pc >> 1) & (ARM_JIT_CACHE_SLOTS - 1);
    arm_compiled_block_t *cb = (arm_compiled_block_t *)cpu->jit_cache[ci];
    if (cb && cb->start_pc == pc) {
        if (!idle_block_matches(pr, p->i, cb)) return -1;
        if (cb->cycles_total > cpu->idle.maxb) cpu->idle.maxb = cb->cycles_total;
        if (p->i >= pr->npre && cb->cycles_total > pr->c_maxb) pr->c_maxb = cb->cycles_total;
        int iters_max = arm_jit_admit(cb, remaining, p->c, limit, cpu->next_event_cycle);
        if (iters_max < 1) return 0;
        /* One pass (a straight-line block), or up to iters_max passes of a
         * self-loop: each pass runs to the first load/store that would
         * side-exit (the pass is handed back) or to its back-branch, which
         * either goes round again or falls through, ending the block with
         * that pass complete (arm_jit.c, the loop block's code). */
        arm_idle_pos_t q = *p;
        int ran = 0;
        for (int it = 0; it < iters_max; it++) {
            if (it > 0 && !idle_block_matches(pr, q.i, cb)) return -1;
            int k = 0, idx = q.i;
            while (k < cb->length && pr->jit_ok[idx]) { k++; idx = idle_next(pr, idx); }
            if (k > 0) idle_advance(pr, &q, k);
            ran += k;
            if (k < cb->length || !cb->is_loop || pr->pc[q.i] != cb->start_pc) break;
        }
        *p = q;
        return ran;
    }
    if (cpu->jit_exec_count[ci] >= 0) return -1;          /* would count, maybe compile */
    return 0;
}
#endif

static int idle_memo_slot(uint64_t key) {
    return (int)((key * 0x9E3779B97F4A7C15ull) >> 52) & (ARM_IDLE_MEMO - 1);
}

bool arm_idle_predict(arm_cpu_t *cpu, const arm_idle_pos_t *from, int64_t target,
                      arm_idle_pos_t *to) {
    arm_idle_state_t *s = &cpu->idle;
    arm_idle_proof_t *pr = s->proof;
    *to = *from;
    if (from->c >= target) return true;      /* arm_step_until does nothing */
    /* The memory is of loop positions only (by canonical index); a slice
     * from the pre-period is worked out each time. */
    int64_t rem = target - from->c;
    uint64_t key = from->i >= pr->npre
        ? ((uint64_t)(uint32_t)idle_canon(pr, from->i) << 32) | (uint64_t)(uint32_t)rem : 0;
    int slot = (from->i >= pr->npre && rem < INT32_MAX) ? idle_memo_slot(key) : -1;
    if (slot >= 0) {
        for (int probe = 0; probe < 8; probe++) {
            int sl = (slot + probe) & (ARM_IDLE_MEMO - 1);
            if (!pr->memo_used[sl]) break;
            if (pr->memo[sl].key == key) {
                idle_total.memo_hits++;
                idle_advance(pr, to, pr->memo[sl].dn);
                return true;
            }
        }
    }
    if (slot >= 0) idle_total.memo_misses++; else idle_total.memo_pre++;
    /* arm_step_until(target) from `from`, as arm_cpu.c runs it. */
    arm_idle_pos_t p = *from;
    while (p.c < target) {
        int remaining = arm_step_until_batch(target - p.c);
        /* arm_step(remaining) */
        while (remaining > 0) {
#ifdef HAVE_LIGHTNING
            if (cpu->jit_cache_size) {
                int ran = idle_probe(cpu, &p, remaining, target);
                if (ran < 0) return false;
                if (ran > 0) {
                    remaining = ran >= remaining ? 0 : remaining - ran;
                    continue;
                }
            }
#endif
            idle_advance(pr, &p, remaining);  /* the interpreter takes the rest */
            remaining = 0;
        }
    }
    *to = p;
    /* Remember the transition only where it did not depend on the next
     * event: every probe was at least one block's length away from it. */
    if (slot >= 0 && (s->next_event == INT64_MAX ||
                      p.c + s->maxb + 1 < s->next_event)) {
        int64_t dn = idle_insns(pr, &p) - idle_insns(pr, from);
        for (int probe = 0; probe < 8; probe++) {
            int sl = (slot + probe) & (ARM_IDLE_MEMO - 1);
            if (pr->memo_used[sl]) continue;
            pr->memo_used[sl] = 1;
            pr->memo[sl].key = key;
            pr->memo[sl].dn = (int32_t)dn;
            break;
        }
    }
    return true;
}

bool arm_idle_safe(const arm_cpu_t *cpu, const arm_idle_pos_t *p) {
    const arm_idle_state_t *s = &cpu->idle;
    const arm_nvic_t *nv = (const arm_nvic_t *)cpu->nvic;
    if (s->state != ARM_IDLE_VALIDATING && s->state != ARM_IDLE_LATCHED) return false;
    if ((nv && nv->has_pending) || cpu->cpu_off) return false;
    if (cpu->next_event_cycle != s->next_event || s->jit_gen != s->jit_gen_at_proof)
        return false;
    if (s->iter0 + p->j * s->proof->period >= s->limit) return false;
    /* The dispatcher admits a block only if it ends before the next event
     * (arm_jit_admit): a probe near the event decides differently. */
    if (s->next_event != INT64_MAX && p->c + s->maxb + 1 >= s->next_event) return false;
    return true;
}

void arm_idle_check_slice(arm_cpu_t *cpu) {
    arm_idle_state_t *s = &cpu->idle;
    if (s->state != ARM_IDLE_VALIDATING) return;
    const arm_idle_pos_t *q = &s->predicted;
    int64_t insns = idle_insns(s->proof, q);
    if (cpu->cycles != q->c || cpu->instructions - s->ref_insns != insns ||
        cpu->reg[ARM_PC] != s->proof->pc[q->i] || s->jit_gen != s->jit_gen_at_proof) {
        if (idle_trace_on())
            fprintf(stderr, "[idle] cpu=%p model mismatch after %d slices: at pc=0x%08x "
                    "cyc=%lld insns=%lld, predicted pc=0x%08x cyc=%lld insns=%lld\n",
                    (void *)cpu, s->validated, cpu->reg[ARM_PC],
                    (long long)cpu->cycles,
                    (long long)(cpu->instructions - s->ref_insns),
                    s->proof->pc[q->i], (long long)q->c, (long long)insns);
        idle_total.mismatches++;
        arm_idle_drop(cpu, ARM_IDLE_WHY_MISMATCH);
        return;
    }
    s->phys = s->model = *q;
    idle_total.checked++;
    if (idle_check_on()) s->gained++;      /* keep proving: checking is the point */
    if (++s->validated >= ARM_IDLE_VALIDATE && !idle_check_on()) {
        s->state = ARM_IDLE_LATCHED;
        idle_total.latches++;
    }
}

/* --- The fast-forward --- */

static void idle_tripwire(const arm_cpu_t *cpu, const arm_idle_pos_t *p, const char *where) {
    const arm_idle_state_t *s = &cpu->idle;
    int64_t insns = idle_insns(s->proof, p);
    if (cpu->cycles == p->c && cpu->reg[ARM_PC] == s->proof->pc[p->i] &&
        cpu->instructions - s->ref_insns == insns)
        return;
    fprintf(stderr, "idle-skip: core left its proven loop %s (pc=0x%08x cycles=%lld "
            "instructions=+%lld, expected pc=0x%08x cycles=%lld instructions=+%lld) "
            "-- a classifier or the proof is wrong\n", where, cpu->reg[ARM_PC],
            (long long)cpu->cycles, (long long)(cpu->instructions - s->ref_insns),
            s->proof->pc[p->i], (long long)p->c, (long long)insns);
    abort();
}

static void idle_run_exact(arm_cpu_t *cpu, int64_t n) {
    while (n > 0) {
        int chunk = n > 1000000 ? 1000000 : (int)n;
        cpu->stopping = false;
        int left = arm_step_interpreted(cpu, chunk);
        if (left != 0) break;          /* the tripwire reports it */
        n -= chunk;
    }
}

void arm_idle_catch_up(arm_cpu_t *cpu) {
    arm_idle_state_t *s = &cpu->idle;
    arm_idle_proof_t *pr = s->proof;
    arm_idle_pos_t *ph = &s->phys;
    const arm_idle_pos_t *to = &s->model;
    if (ph->j == to->j && ph->i == to->i) return;
    idle_total.catch_ups++;
    if (to->j > ph->j || (ph->i < pr->npre && to->i >= pr->npre)) {
        /* To the start of the next iteration (the end of the pre-period,
         * or the end of this iteration). */
        int64_t rest = ph->i < pr->npre ? pr->npre - ph->i
                     : (ph->i == pr->npre ? 0 : pr->npre + pr->n - ph->i);
        if (rest > 0) {
            bool wraps = ph->i >= pr->npre;
            idle_run_exact(cpu, rest);
            if (wraps) ph->j++;
            ph->i = pr->npre;
            ph->c = s->iter0 + ph->j * pr->period;
            idle_tripwire(cpu, ph, "at an iteration start");
        }
        int64_t k = to->j - ph->j;
        if (k > 0) {
            /* Whole iterations: the state at the start of each is the one
             * the proof compared, so only the counts move. */
            cpu->cycles += k * pr->period;
            cpu->instructions += k * pr->n;
            ph->j += k;
            ph->c += k * pr->period;
            s->skipped_iters += (uint64_t)k;
            s->skipped_insns += (uint64_t)(k * pr->n);
            idle_total.skipped_iters += (uint64_t)k;
            idle_total.skipped_insns += (uint64_t)(k * pr->n);
        }
    }
    if (to->i > ph->i) idle_run_exact(cpu, to->i - ph->i);
    *ph = *to;
    idle_tripwire(cpu, ph, "after catching up");
}
