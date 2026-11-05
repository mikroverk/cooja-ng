# Idle-loop skipping (ARM)

Some firmware polls instead of sleeping. OpenThread's CLI examples
(`ot-cli-ftd` from ot-cc2538 and ot-nrf528xx) spin in their main loop,
`otTaskletsProcess(); otSysProcessDrivers();`, checking flags and status
registers, and almost never execute WFI. Every node then runs every cycle, so
a Thread network runs slower than real time as it grows. Most of those passes
change nothing: the same instructions, the same values, the same state at the
end as at the start.

`idle_skip` lets an ARM node skip such passes. **It changes nothing about the
run except its wall time**: the console, every timestamp, the PCAP capture,
the test verdict and each node's end-of-run cycle count, instruction count and
PC are byte-identical with it on and off (`tools/check-idle-skip.sh` checks
that). It is off by default.

```yaml
idle_skip: true        # top level of a setup, v1 or v2
```

```sh
cngsim run thread-cc2538                       # the example turns it on
cngsim run thread-cc2538 --no-idle-skip        # the same run, slower
test_runner test setup.yaml --idle-skip        # on for a setup that does not say so
```

`--idle-skip` and `--no-idle-skip` override the setup's key. The examples
`thread-cc2538` and `thread-nrf52840`, `configs/test-openthread-*.yaml` and
the setups `cngsim new thread` writes turn it on.

## What it buys

Measured on an M-series Mac, one run after the other
(`TIMING=1 tools/check-idle-skip.sh`), all byte-identical on and off:

| Setup | simulated | off | on | |
|---|---|---|---|---|
| `configs/test-openthread-cc2538dk.yaml` (3 nodes) | 25 s | 12.9 s | 2.23 s | 5.8x |
| `examples/thread-cc2538.yaml` (3 nodes) | 25 s | 13.1 s | 2.26 s | 5.8x |
| `cngsim new thread --board cc2538dk -n 6 --layout line` | 67 s | 88.1 s | 14.6 s | 6.0x |
| `configs/test-openthread-nrf52840-dk.yaml` (2 nodes) | 25 s | 14.4 s | 14.4 s | 1.0x |
| `examples/thread-nrf52840.yaml` (2 nodes) | 25 s | 14.6 s | 14.5 s | 1.0x |
| `cngsim new thread --board nrf52840-dk -n 6 --layout line` | 67 s | 145.6 s | 144.9 s | 1.0x |

The CC2538 Thread line goes from 0.8x to 4.6x real time. The nRF52840 port
gains nothing yet (see [What is not skipped](#what-is-not-skipped)).
Contiki-NG and Zephyr images sleep in WFI, which already skips idle time;
with the option on they run the same and no slower (within run-to-run noise:
the 90 s TSCH example on nRF52840, median 1.44 s on and off).

## The rule: exact or not at all

A pass is skipped only when it is *proven* to be the same as the one before it,
and the CPU is always brought to the exact instruction boundary, cycle and
state the unskipped run would have reached before anything can see it. Where
that cannot be proven, the node runs unskipped: correct, only slower. Nothing
is approximated.

Three parts make that hold.

### 1. The proof: a dry run on a copy

At the start of a slice the node's CPU is copied and the copy is
interpreted, instruction by instruction, while the real node does not move:

- SRAM is read through a private overlay and written into it; the real SRAM
  is never written.
- Peripheral registers are read only where the SoC's classifier
  (`arm_idle_io_fn`, include/arm/arm_idle.h) vouches that the read has no
  side effect (`PURE`), or that its value is a function of the cycle count
  alone and stays the same until a cycle it names (`TIMED`, the RTC
  counter).  A store is buffered, never performed, and only where the
  classifier vouches that it merely stores (`STORE`: no interrupt, task,
  callback or DMA in the state it finds, and whatever else it does is fixed
  by the peripheral's registers alone).  Any other access ends the dry run.
- An event coming due, an exception or exception return, WFI, a ROM or
  firmware trap, a flash write, too many SRAM words stored to, too many
  instructions: the dry run ends without a proof.

Each time the copy comes back to the PC it started at, its state is compared
with the state at the start and at the last few returns: the registers, flags,
masks, banked stack pointers and limits, FP registers, the WFE latch, every
SRAM word it stored to and every peripheral register it stored to. A match
means the instructions between the two returns (N instructions, P cycles) form
an iteration whose end state is its start state; everything before the first
of the two is a pre-period (the pass that, after an interrupt, writes the new
clock value into a stack slot is one). The iteration's inputs are that state,
SRAM, flash and the values its peripheral reads return, and those change
only through:

- the node's own scheduled events: no skipped iteration may reach the next one
  (an iteration must end before it);
- a `TIMED` read, whose value holds until the cycle the classifier named: no
  skipped iteration may read it at or after that cycle;
- something outside the CPU: a radio byte, console input, a reboot, a
  debugger, the shell, an overlay reading memory. Every such path goes
  through the ARM mote (src/motes/arm_elf_mote.c), which brings the CPU up to
  date first and then drops the proof.

So until one of those, the CPU repeats that iteration exactly.

### 2. The slices: the kernel sees the same run

The kernel keeps waking an active node every microsecond, exactly as without
skipping: the event queue, its sequence numbers, the order of same-time
events and every other node see the same run. A slice covered by a proof does
the slice's bookkeeping (the `step_micros` accumulator, the pinned times) and
does not run the CPU.

Where a slice leaves the CPU is not "the first instruction boundary after the
slice's cycle target": `arm_step_until` asks for instruction batches
(`arm_step_until_batch`) and the JIT dispatcher runs whole compiled blocks,
and self-loops for as many passes as it admits (`arm_jit_admit`). The landing
model (`arm_idle_predict`) replays that algorithm over the proven iteration
and the JIT's current block set: the dispatcher's probe order, block
admission, side exits (a load or store a block guards, outside SRAM or
unaligned), self-loop passes, the interpreter taking the rest of a batch. It
refuses wherever the dispatcher would change state of its own (count a miss
toward compiling, compile, evict). The model shares `arm_step_until_batch`
and `arm_jit_admit` with the real code, so they cannot drift apart. It must
predict the next eight real slices exactly, cycle, instruction count and PC,
before any slice is skipped; `CSIM_IDLE_SKIP_CHECK=1` runs it against every
covered slice of a whole run instead (61 million slices of the CC2538 Thread
run, none mispredicted).

### 3. The catch-up

When the CPU is needed (the next slice the proof does not cover, a radio
byte, console input, a reader of its cycles, PC or memory), `arm_idle_catch_up`
takes it from where it stopped to where the model says the slices left it:
the rest of the current iteration interpreted, then whole iterations skipped
(cycles += k·P, instructions += k·N; nothing else changes, since the state at
the start of each is the one the proof compared), then the rest interpreted,
exactly as many instructions as the model counted. A tripwire checks that the
core arrives where the proof says, and aborts the run if it does not.

Instruction counts are therefore the same too: a skipped iteration counts its
N instructions. What differs is diagnostics only: `CSIM_ARM_JIT_STATS=1`'s
counts (fewer instructions went through compiled code), and the
interpreter's diagnostic environment hooks (`ARM_TRACE_PC`, `ARM_PC_WATCH`,
`ARM_SP_AUDIT` and the rest), with any of which set nothing is skipped.

## What is not skipped

- **A pass that changes something**: RAM, a register, a peripheral. A counter
  in a loop, a FIFO read, a UART write.
- **A register the SoC's classifier does not list.** Only the CC2538 and the
  nRF52840 have classifiers, covering the registers OpenThread's loops touch:
  the CC2538 radio's status and mask registers (`RFIRQM0` stored only when
  the store raises no interrupt) and the UART flags; the nRF52840 TEMP
  data-ready event, the RTC events, masks and counter, and the RTC overflow
  mask (stored only with the tick interrupt off: the store re-arms the
  overflow event to the wrap, a time the counter's registers fix). Everything
  else, and every other SoC, ends a dry run: correct, unskipped.
- **A free-running counter with no named bound** (SysTick's current value,
  DWT CYCCNT): it changes every cycle.
- **OpenThread on the nRF52840, today.** Its main loop reads RTC2's counter on
  every pass (`nrf5TempProcess` asks the time to decide whether to start a
  temperature measurement every 30 s). The counter ticks every 30.5 µs, about
  four passes, and a pass after a tick differs from the one before (it stores
  the new value), so a proof lasts one tick. Re-proving every tick costs
  about what running the tick does, and a proof that does not pay for its dry
  run makes the next attempt wait twice as long, so the node runs at its
  unskipped speed. Skipping past a tick would need to show that the new value
  does not change the pass, which takes reasoning about how it is used
  (here: compared with a time 30 s off), not the comparison of states this
  does.
- **An interrupt pending, a node in an exception handler, WFI** (which skips
  idle time on its own), clock deviation other than 1.0, a debugger or shell
  breakpoint, the GDB stub, TrustZone-M (the nRF54L15), the FLPR co-processor,
  the JIT's verify and batch modes.
- **MSP430 and RISC-V nodes.** The option is for ARM nodes; others ignore it.

Attaching the shell or overlays that read node memory does not change the run
either, but each read drops the node's proof, so skipping pays less.

## Why it is opt-in

It is exact by construction, and the identity is gated (below); still, its
correctness rests on each SoC classifier telling the truth about its
registers, and on the landing model matching the stepping code. A classifier
that called a side-effecting read pure would make a run with the option on
differ from one with it off, silently, and only for firmware that reaches
that register in an idle loop. Off by default, a run is the reference; on, it
is that reference, faster, where the identity check says so. Firmware that
does not poll also gains nothing from it and pays for the occasional dry
run, which is small (the measurements above) but not zero.

## Diagnostics

```sh
CSIM_IDLE_SKIP_STATS=1   # stderr at exit: dry runs, proofs, slices skipped, iterations
                         # skipped, the model's memory, and why each attempt ended
CSIM_IDLE_SKIP_TRACE=1   # every proof, refused register (address, PC) and mismatch
CSIM_IDLE_SKIP_CHECK=1   # never skip; predict every covered slice and compare it with
                         # the real one (mismatches=0 is the expected count)
```

## Tests

```sh
./build/test_runner arm-idle -v      # the proof, the model and the catch-up against a
                                     # reference CPU: loops that skip (a RAM poll, a
                                     # poll that stores, a masked/unmasked IRQ, a timed
                                     # counter, a poll an event ends) and loops that
                                     # must not (RAM that changes, a free-running
                                     # counter, a FIFO, a pending interrupt, a changing
                                     # peripheral store), JIT off and on
tools/check-idle-skip.sh             # the Thread setups, Contiki-NG and Zephyr: off vs
                                     # on, stdout/stderr/exit/PCAP identical (CI)
CHECK=1 tools/check-idle-skip.sh     # ... and the model checked on every slice
JIT=0 tools/check-idle-skip.sh       # ... with the JIT off
TIMING=1 tools/check-idle-skip.sh    # off and on one after the other, for timings
```

## Adding a SoC

Give the SoC a classifier (`arm_idle_set_io_classifier` at platform init) that
returns `ARM_IDLE_IO_PURE`, `_TIMED` or `_STORE` for exactly the registers whose
behaviour you can state that way, and `_REFUSE` for everything else. A missing
register costs speed, a wrong one costs correctness: when in doubt, refuse.
Then run `tools/check-idle-skip.sh` (with `CHECK=1`) on a setup that uses it.
