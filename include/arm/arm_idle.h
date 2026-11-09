/*
 * Idle-loop skipping for ARM cores (opt-in; docs/idle-skip.md).
 *
 * Firmware that polls instead of sleeping (OpenThread's CLI examples spin in
 * their main loop) spends most of its simulated time repeating one loop
 * iteration that changes nothing.  The kernel still wakes such a node every
 * microsecond, exactly as without skipping; a slice that would only repeat
 * that iteration is done without running the CPU, and the CPU is brought to
 * the exact instruction boundary, cycle and state the unskipped slices would
 * have left it at the moment anything could observe or change it.
 *
 * Three parts, each exact or refusing:
 *
 *  - The proof (a dry run).  At a slice boundary the CPU is copied and the
 *    copy is interpreted: SRAM through a private overlay, peripheral reads
 *    only where the SoC's classifier (arm_idle_io_fn) vouches that the read
 *    has no side effect, peripheral stores buffered and only where the
 *    classifier vouches that a store merely stores.  The real node is never
 *    touched.  When the copy is back at the starting PC with the registers,
 *    flags, masks, stack pointers, FP state, every overlaid SRAM word and
 *    every buffered peripheral register equal to the real node's, that
 *    instruction sequence (N instructions, P cycles) is a fixed point: the
 *    real CPU will repeat it, iteration after iteration, until an input
 *    changes.  An event coming due, an exception, WFI, a ROM or firmware
 *    trap, an unknown or refused register, too many stores or too many
 *    instructions: no proof, the node runs unskipped.
 *  - The landing model.  Where a slice ends is not "the first boundary past
 *    the target": arm_step_until asks for instruction batches and the JIT
 *    dispatcher runs whole compiled blocks.  The model replays exactly that
 *    algorithm (arm_step_until_batch, arm_jit_admit, the dispatcher's probe
 *    order, side exits) over the proven instruction sequence and the JIT's
 *    current block set, and refuses whenever the dispatcher would change
 *    state of its own (count a miss, compile, evict).  It must predict the
 *    next real slices exactly before any slice is skipped.
 *  - The fast-forward.  From the CPU's real position to the predicted one:
 *    the rest of the current iteration interpreted, whole iterations skipped
 *    (cycles += k*P, instructions += k*N; nothing else changes, the state at
 *    the start of each is the same), the rest interpreted.  A tripwire
 *    checks the core arrives where the proof says, and aborts the run if
 *    not.
 *
 * Inputs from outside the CPU go through the mote (arm_elf_mote.c), which
 * brings the CPU up to date first and then drops the proof.
 */
#ifndef ARM_IDLE_H
#define ARM_IDLE_H

#include <stdbool.h>
#include <stdint.h>

struct arm_cpu;
struct arm_io_region;

/* Classifier verdicts for one peripheral access made in a dry run. */
enum {
    ARM_IDLE_IO_REFUSE = 0, /* side effect, unknown register: no proof     */
    ARM_IDLE_IO_PURE,       /* read: no side effect; the value changes only by
                               a store, an event or an outside input        */
    ARM_IDLE_IO_TIMED,      /* read: no side effect; the value is a function
                               of the cycle count alone: info->value is what
                               it reads at `now`, the same before info->until */
    ARM_IDLE_IO_STORE,      /* write: its only effect is on register state
                               info->key (the value written), so stores that
                               leave every key as they found it leave the node
                               as it was.  No interrupt raised, no task, no
                               callback, no DMA, in the state it finds; key
                               reads back with a PURE read (32 bits); nothing
                               outside the CPU reads it.  It must not schedule,
                               re-arm or cancel an event: the proof takes the
                               next event from the CPU once, at its start, so
                               an event a store arms part-way through a pass
                               (and another store cancels) is one a skip could
                               run past (an nRF52840 RTC overflow interrupt was
                               lost that way).  Nor may a PURE, TIMED or STORE
                               access depend on cpu->sim_time_ns: the catch-up
                               replays a pass with it already set to the last
                               skipped slice's time.  */
};

/* Rules every classifier entry must keep, which arm_idle.c relies on:
 *  - info->key names a register in the access's own region, and every
 *    address whose read depends on a storable key reports that key (an
 *    alias such as INTENSET/INTENCLR or EVTEN/EVTENSET/EVTENCLR reports one
 *    key); a read that combines a storable key with other state is refused.
 *  - A STORE's key reads back the value written, bit for bit: a register whose
 *    write masks, clears or transforms the value is not a STORE.
 *  - A narrow (8/16-bit) access is accepted only at the key's own address:
 *    arm_idle.c returns the low bits of the 32-bit value.
 *  - A verdict, and a PURE or TIMED value, depends only on state no accepted
 *    STORE changes other than through its own key: the classifier sees the
 *    real peripheral, not the dry run's buffered stores.
 *  - (Above) no STORE schedules, re-arms or cancels an event, and nothing
 *    accepted depends on cpu->sim_time_ns.
 * A missing register costs speed; a wrong verdict costs exactness. */

typedef struct {
    int64_t  until;      /* TIMED: the value holds for every cycle before this */
    uint32_t value;      /* TIMED: the value at `now`                          */
    uint32_t key;        /* the register (address) whose state the access reads
                            or a STORE changes; preset to the access address  */
} arm_idle_io_info_t;

/* SoC classifier.  `now` is the copy's cycle count at the access. */
typedef int (*arm_idle_io_fn)(void *user, uint32_t addr, bool is_write,
                              uint32_t value, int width, int64_t now,
                              arm_idle_io_info_t *info);

/* Why a node is not skipping (CSIM_IDLE_SKIP_STATS=1). */
enum {
    ARM_IDLE_WHY_EVENT = 0, ARM_IDLE_WHY_EXCEPTION, ARM_IDLE_WHY_WFI,
    ARM_IDLE_WHY_TRAP, ARM_IDLE_WHY_IO, ARM_IDLE_WHY_WLOG, ARM_IDLE_WHY_BUDGET,
    ARM_IDLE_WHY_RESET, ARM_IDLE_WHY_FLASH, ARM_IDLE_WHY_MODEL,
    ARM_IDLE_WHY_MISMATCH, ARM_IDLE_WHY_EXTERNAL, ARM_IDLE_WHY_COUNT
};

enum {
    ARM_IDLE_OFF = 0,        /* no proof                                     */
    ARM_IDLE_VALIDATING,     /* proof made; real slices checked against it  */
    ARM_IDLE_LATCHED,        /* slices may be skipped                        */
    ARM_IDLE_DRY,            /* this arm_cpu_t is a dry-run copy             */
};

#define ARM_IDLE_MAXN     2048  /* instructions a dry run may take            */
#define ARM_IDLE_VISITS     16  /* returns to the start a dry run may take    */
#define ARM_IDLE_WLOG      256  /* SRAM words one iteration may store to     */
#define ARM_IDLE_IOLOG      16  /* peripheral registers it may store to      */
#define ARM_IDLE_TIMED      16  /* cycle-timed peripheral reads              */
#define ARM_IDLE_MEMO     1024  /* slice transitions remembered (power of 2) */
#define ARM_IDLE_VALIDATE    8  /* real slices the model must predict first  */

/* A position on the proven sequence: instruction index i (0..npre-1 in the
 * pre-period, npre..npre+n-1 in the loop), j iterations of the loop done,
 * and the cycle count there. */
typedef struct { int64_t j; int i; int64_t c; } arm_idle_pos_t;

/* The proof and the model's memory; heap, one per opted-in CPU. */
typedef struct arm_idle_proof {
    int      npre;                   /* instructions before the loop starts  */
    int      n;                      /* instructions per iteration           */
    int64_t  period;                 /* cycles per iteration                 */
    uint32_t pc[ARM_IDLE_MAXN];      /* PC at each boundary                  */
    uint32_t pre[ARM_IDLE_MAXN + 1]; /* cycles from the start to boundary i;
                                        pre[npre + n] = pre[npre] + period   */
    uint8_t  it[ARM_IDLE_MAXN];      /* ITSTATE at each boundary             */
    uint8_t  jit_ok[ARM_IDLE_MAXN];  /* a compiled load/store here would not
                                        side-exit (aligned, in SRAM)         */
    int8_t   blk_ok[ARM_IDLE_MAXN];  /* compiled block starting here matches
                                        the sequence: 0 unchecked, 1, -1
                                        (pre-period positions)               */
    /* The loop, rotated to its least rotation (Booth), so a proof of the
     * same loop entered at another instruction finds the model's memory of
     * it: loop position q is canonical index (q - rot) mod n.  Kept across
     * proofs while the loop and the JIT's block set are the same. */
    int      rot;
    bool     c_valid;
    int      c_n, c_maxb;
    int64_t  c_period;
    uint64_t c_jit_gen;
    uint64_t c_key[ARM_IDLE_MAXN];   /* pc | cycles | ITSTATE | jit_ok      */
    int8_t   c_blk_ok[ARM_IDLE_MAXN];
    struct { uint64_t key; int32_t dn; } memo[ARM_IDLE_MEMO];
    uint8_t  memo_used[ARM_IDLE_MEMO];
    uint64_t loop_key[ARM_IDLE_MAXN];
    uint64_t scratch_key[2 * ARM_IDLE_MAXN];
    int32_t  scratch_f[2 * ARM_IDLE_MAXN];
} arm_idle_proof_t;

typedef struct arm_idle_dry arm_idle_dry_t;

typedef struct arm_idle_state {
    bool     enabled;          /* the node opted in (config idle_skip)    */
    uint8_t  state;            /* ARM_IDLE_OFF / _VALIDATING / _LATCHED   */
    uint32_t sram_end;         /* DRY copy: the real SRAM window's end    */
    arm_idle_dry_t *dry;       /* DRY copy: its overlay and trace         */

    arm_idle_proof_t *proof;
    struct arm_cpu   *copy;    /* the dry-run copy (heap, reused)         */

    /* The proof's start (an instruction boundary, index 0), where its
     * loop's iteration 0 starts (iter0), and how far iterations may be
     * skipped: iteration j only while iter0 + j*period < limit (the next
     * event, the cycle-timed reads' bounds).  next_event is the event the
     * limit was taken from. */
    int64_t  ref, ref_insns, limit, next_event, iter0;
    uint64_t jit_gen_at_proof;
    int      maxb;             /* largest compiled block's cycles met      */

    arm_idle_pos_t phys;       /* where the real CPU is                    */
    arm_idle_pos_t model;      /* where the slices done so far left it     */
    arm_idle_pos_t predicted;  /* VALIDATING: the slice being run          */
    int      validated;

    /* Lazy slices: the real CPU is behind `model`. */
    bool     lazy;
    int64_t  lazy_sim_ns;

    /* Every compile, eviction or flush of this CPU's JIT blocks. */
    uint64_t jit_gen;

    /* Retry pacing: slices to wait before the next proof attempt, and what
     * the current proof cost (cycles the dry run interpreted) and gained
     * (slices done without the CPU). */
    int      cooldown, backoff;
    int64_t  cost, gained, last_period;

    arm_idle_io_fn io_class;
    void    *io_user;

    /* Diagnostics only (CSIM_IDLE_SKIP_STATS=1, stderr at exit). */
    uint64_t attempts, proofs, lazy_slices, skipped_iters, skipped_insns;
    uint64_t why[ARM_IDLE_WHY_COUNT];
} arm_idle_state_t;

/* Opt a CPU in or out.  Off by default; off means every hook is one
 * predicted branch on a flag and nothing else changes. */
void arm_idle_enable(struct arm_cpu *cpu, bool on);
void arm_idle_free(struct arm_cpu *cpu);
void arm_idle_set_io_classifier(struct arm_cpu *cpu, arm_idle_io_fn fn, void *user);

/* Try to prove the CPU sits in a fixed-point loop, starting at its current
 * instruction boundary.  On success the state is ARM_IDLE_VALIDATING. */
bool arm_idle_prove(struct arm_cpu *cpu);
/* Forget the proof (the CPU must already be up to date). */
void arm_idle_drop(struct arm_cpu *cpu, int why);

/* Where a slice from `from` with cycle target `target` lands, by the
 * stepping algorithm.  False: the model cannot say (the node must run). */
bool arm_idle_predict(struct arm_cpu *cpu, const arm_idle_pos_t *from,
                      int64_t target, arm_idle_pos_t *to);
/* The position is one the proof covers: every iteration up to it may be
 * skipped and no event or bound is within reach. */
bool arm_idle_safe(const struct arm_cpu *cpu, const arm_idle_pos_t *p);
/* After a real slice in ARM_IDLE_VALIDATING: did it land where predicted? */
void arm_idle_check_slice(struct arm_cpu *cpu);
/* Diagnostics: one more slice done without the CPU. */
void arm_idle_count_lazy(void);
/* Move the real CPU to `cpu->idle.model`. */
void arm_idle_catch_up(struct arm_cpu *cpu);

/* Dry-run hooks (arm_cpu.c). */
uint32_t arm_idle_dry_access(struct arm_cpu *cpu, uint32_t addr, int n,
                             bool is_write, uint32_t val);
uint32_t arm_idle_dry_sram(struct arm_cpu *cpu, uint32_t addr, int n,
                           bool is_write, uint32_t val);
uint32_t arm_idle_dry_io(struct arm_cpu *cpu, struct arm_io_region *r,
                         uint32_t addr, bool is_write, uint32_t val, int width);
void arm_idle_dry_fail(struct arm_cpu *cpu, int why);

#endif /* ARM_IDLE_H */
