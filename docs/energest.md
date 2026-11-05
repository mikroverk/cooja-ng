# energest: time in state and energy

The built-in **energest** plugin records, for every node, how long each
component spent in each power state — the CPU's mode, the radio's state and
the peripherals the chip models can state honestly — and puts a datasheet
power model on top: microjoules per component, the average current and a
battery lifetime.  Time in state is **exact**: every change is stamped by the
node's own cycle clock at the instruction where the model made it, and every
component's states sum to the node's lifetime to the nanosecond.  Energy is
an **estimate**: a typical datasheet current per state, summed over the
components.

```yaml
plugins: [energest]
energest:                     # all optional
  bin_ms: 10                  # UI timeline bin (1..1000 ms, default 10)
  frame_ms: 1000              # one UI frame per this much simulated time
  battery_mah: 2600           # print a battery lifetime from the average current
  csv: energest.csv           # node x component x state, written at the end
  frames: energest.frames     # the UI frames, one JSON object per line
  trace: energest.trace       # every state change and console line (CSV)
  currents:                   # override the datasheet defaults, per board
    sky: { supply_v: 3.0, led.on: 4.0, cpu.active: 2.2 }
```

```sh
cngsim run energy-sky --ui                       # the Sky pair, with the live panel
build/test_runner test examples/energy-sky.yaml  # headless: the table at the end
```

File paths are relative to the directory the run starts in.  A key the plugin
does not know — in `energest:`, or a board, component or state in `currents:`
— stops the run before it starts (exit 2), as a misspelt plugin does.

With the plugin off nothing of this runs: the models stamp and emit no power
state, and a run is byte-identical to one without the feature
(`tools/check-baseline.sh`).  With it on the simulation is the same — the
plugin only observes (`tools/check-energest.sh` compares the PCAPs).

## What is recorded

| Lane | States | Where the model changes it |
|------|--------|----------------------------|
| `cpu` | `active`, `lpm0`..`lpm4` (MSP430), `sleep` (Cortex-M WFI/WFE; CC2538 PM0), `pm1`..`pm3` (CC2538 WFI with SCR.SLEEPDEEP and SYS_CTRL_PMCTL) | MSP430: where the interpreter stops executing after the instruction that set CPUOFF retired, and the interrupt entry that wakes it; Cortex-M: WFI/WFE, the exception entry or pending interrupt that wakes it, reset |
| `radio` | `off`, `listen`, `rx`, `tx`, `interfered` | the chip's own state funnel: CC2420 `set_state`, the CC2538 RF core, every nRF52840/nRF54L15 `RADIO.STATE` write |
| `led0`..`led2` | `off`, `on` | a GPIO OUT or DIR write: lit when the pin is an output driven to the board's active level (Sky/Z1 active low; FR5969, CC2538DK, OpenMote, Firefly active high) |
| `Timer_A`, `Timer_B` | `off`, `on` | MSP430 TxCTL.MC leaves or enters stop |
| `timer0`..`timer4`, `rtc0`..`rtc2` | `off`, `on` | nRF52840 TASKS_START / STOP / SHUTDOWN, the COMPARE_STOP short |
| `gpt0`..`gpt3`, `ssi0`, `ssi1`, `uart0`, `uart1`, `i2c`, `pka`, `aes`, `rfc` | `off`, `on` | CC2538 run-mode clock gates (SYS_CTRL RCGCGPT/SSI/UART/I2C/SEC/RFC bits) |

A lane appears when its component first changes; until then it was in its
first state (the reset state: CPU executing, everything else off).  When a
node enters its slot — the run starts, or it is added or rebooted — the plugin
reads every lane's current state from the models (`power_resync`), so a lane a
boot left on is not counted as off.

Classification notes: the radio's `listen` is "on, not receiving a frame";
Nordic's RADIO.STATE does not tell listening from receiving, so nRF radios
report `listen` throughout and never `rx`.  The CC2420's `off` covers its
VREG-off, power-down and idle states.  A CC2420 TX includes its 12-symbol
calibration; the CC2538 RF core's TX starts with the frame's first byte; the
nRF ramp-up (TXRU) counts as TX.

### What each chip model exposes, and what is left out

| Chip | Recorded | Left out, and why |
|------|----------|-------------------|
| MSP430 (Sky, Z1, FR5969) | CPU mode with the LPM level; LEDs; Timer_A/B running | UART TX (TXBUF completes at once: UTCTL.TXEPT always reads 1, no busy period to time); the internal flash controller (FCTL1-3 not modelled: flash writes land directly); ADC12 (stubbed) |
| CC2420 | radio state; TX power level (TXCTRL.PA_LEVEL, charged by SWRS041c Table 9) | the idle / power-down distinction within `off` |
| CC2538 | CPU mode incl. PM1-3; radio state; LEDs; clock gates | UART TX busy (instant); flash erase/program (instant ROM traps: no busy time); clock gates only *record* (no peripheral checks its gate); TXPOWER (stored, its current not modelled) |
| nRF52840 | CPU sleep; radio state; TIMER0-4, RTC0-2 running | GPIO and so LEDs (not modelled at all); UARTE TX (completes at once); NVMC (erase/write instant, READY always 1); HFCLK (no stop, no start-up time); SAADC/SPIM/TWIM (not modelled); TXPOWER (stored) |
| nRF54L15 | CPU sleep; radio state | peripherals not surveyed |
| Others (CC13x2, CC1200, native, JS) | CPU where the core is Cortex-M | radio state where the chip has no state funnel is the per-slice poll (slice-time stamps) |

Only what a model really does is stated: a busy lane for a peripheral the
model completes in zero time would put a datasheet number where the
simulation has none.

## Output

### The table (stdout, at the end of the run)

```
energest: node 1  Tmote Sky (MSP430F1611 + CC2420), 3.00 V  lifetime 60.211091 s
energest:   cpu      active 0.740959 s (1.23%) lpm0 59.470132 s (98.77%)  = 17.715 mJ
energest:   led1     off 60.183546 s (99.95%) on 0.027545 s (0.05%)  = 0.000 mJ
energest:   Timer_A  on 60.211091 s (100.00%)  = 0.000 mJ
energest:   radio    off 0.004955 s (0.01%) listen 60.138395 s (99.88%) tx 0.032721 s (0.05%) rx 0.035020 s (0.06%)  = 3395.489 mJ
energest:   total    3413.204 mJ, average 18.8958 mA, battery life 5.7 days
energest:   check    cpu low-power between the first and the last wake ...: -0.27 us per wake
energest:   check    radio tx 32720584 ns, frame air time 26912000 ns (...): equal to bytes x byte time
energest:   check    tx dwell - air time, 10 data frames: min 256.976 max 265.232 mean 260.737 us
energest: network total ~6827.107 mJ (datasheet estimate), 61 UI frames
```

The `check` lines are the cross-checks of "Validation" below; a component
whose states did not sum to its lifetime is printed as `CHECK FAILED`.

### CSV (`csv:`)

`node,board,component,state,time_ns,fraction,current_ma,energy_uj,source` —
one row per node, component and state with time in it, then a `total,all`
row per node (its lifetime in `time_ns`, the average current in
`current_ma`, the energy in `energy_uj`) and, with `battery_mah`, a
`battery,lifetime_h` row.  `current_ma` is the current charged (for the
CC2420's TX, the mean over its PA levels); `source` is the datasheet row, or
`config`.

### Trace (`trace:`)

`t_ns,node,component,state`: every lane's state at its creation and every
change, in the order the plugin saw them (by node; nodes interleave), and
every console line as `component = log`, `state` = the line.  A component's
last state lasts to the end of its node's life.  `tools/check-energest-output.py`
re-sums it to the CSV; `tools/check-energest-agreement.py` reads the
firmware's own Energest reports from its `log` rows.

### UI panel (`"energest"`, the latest value)

`{"title":"Energy (estimate)","rows":[["node 1","56.9 mJ 18.97 mA duty 99.9%"],...]}`,
drawn by the generic panel renderer and refreshed with every frame.

## UI frames

The data a timeline graph is drawn from.  One frame covers `frame_ms` of
simulated time (default 1 s) cut into bins of `bin_ms` (default 10 ms) and
carries the cumulative totals at its end.  Frames are sent **once** each —
unlike a panel, which is the latest value and is re-sent every tick:

- live: a WebSocket text message `{"type":"frames","frames":[F,...]}` where
  each `F` is `{"id":"energest","data":FRAME}`;
- in a `--ui-record` document: key `"f"` of a delta, the same array;
- with `frames:` in the config: one `FRAME` per line of that file.

A frame is emitted once simulated time is a full frame past its end (so a
node whose clock lags the others' still lands in the right bin), and the
last, partial one at the end of the run.  Frames are consecutive: each `t0`
is the previous `t0 + n * bin`.  Clients joining late get the frames from
then on (the queue holds 64); the totals in each frame make it complete on
its own.

```json
{"v": 1, "t0": 10000000000, "bin": 100000000, "n": 10,
 "nodes": [
  {"id": 1,
   "lanes": [
     {"c": "cpu", "base": "lpm0", "f": {"active": [8, 98, 8, 8, 8, 8, 7, 53, 142, 8]}},
     {"c": "led1", "base": "off"},
     {"c": "Timer_A", "base": "on"},
     {"c": "radio", "base": "listen", "f": {"tx": [0, 36, -5, 7, 7, 0], "rx": [-7, 33, 33, 0]}}],
   "us": {"cpu": {"active": 161553, "lpm0": 10949914},
          "led1": {"off": 10086550, "on": 27545},
          "Timer_A": {"on": 10114095},
          "radio": {"off": 3701, "listen": 10091291, "tx": 9133, "rx": 9970}},
   "uj": {"cpu": 3408.8, "led1": 0.0, "Timer_A": 0.0, "radio": 570187.9},
   "ma": 18.9042}]}
```

(`bin_ms: 100` here, to keep the example short.)

| Field | Meaning |
|-------|---------|
| `v` | format version, 1 |
| `t0` | simulated time of the first bin's start, ns |
| `bin` | bin width, ns |
| `n` | bins in this frame (`frame_ms / bin_ms`; fewer in the last frame) |
| `nodes[]` | every node alive at some point in the frame |
| `nodes[].id` | Cooja node id |
| `nodes[].b0`, `b1` | present only when the node started (`b0`: its first bin) or left (`b1`: one past its last bin) inside the frame; bins outside are not the node's |
| `lanes[]` | one per component, in the order they appeared |
| `lanes[].c` | component name (the lane names above) |
| `lanes[].base` | the state with the most time in this frame; its share of a bin is what the other states leave: `1000 - sum(f[s][k])` permille |
| `lanes[].f` | `{state: [bins]}` for every other state with time in the frame; absent when the lane spent the whole frame in `base` |
| `f[s][k]` | permille (0..1000, rounded) of bin `k` spent in state `s`.  **A negative entry `-k` stands for `k` bins of 0** (runs of two or more), so `[0, 36, -5, 7, 7, 0]` is ten bins: `0,36,0,0,0,0,0,7,7,0` |
| `us` | cumulative time in each state, from the node's start to the frame's end (the run's end in the last frame), µs, states with time only |
| `uj` | cumulative energy per component, µJ (datasheet estimate) |
| `ma` | the node's average current so far, mA |

Decoding a lane (JavaScript):

```js
function bins(arr) { const out = []; for (const v of arr) { if (v < 0) for (let i = 0; i < -v; i++) out.push(0); else out.push(v); } return out; }
function lanePermille(lane, n) {              // -> {state: [n permille]}
  const r = {}, base = new Array(n).fill(1000);
  for (const [s, a] of Object.entries(lane.f || {})) { r[s] = bins(a); r[s].forEach((v, k) => base[k] -= v); }
  r[lane.base] = base.map(v => Math.max(0, v));
  return r;
}
```

Size: the Sky pair (2 nodes, 60 s) records 61 frames of about 1.3 KB, ~80 KB
in all; the run's `--ui-record` document goes from 112 KB to 202 KB.  A larger
`bin_ms` shrinks it in proportion.

## Power model

Per board, a supply voltage and a typical current per component state
(`src/services/energest_model.c`, with the document and table next to each
figure).  Components add.  Override any figure per board in `currents:` —
`<lane>.<state>` (`cpu.active`, `led0.on`, `uart0.on`) or `<class>.<state>`
for every lane of a kind (`led.on`, `timer.on`, `gate.on`) — and `supply_v`.

| Board | Component state | mA | Source |
|-------|-----------------|---:|--------|
| Tmote Sky, 3.0 V | cpu.active | 1.95 | SLAS368G p.27: I(AM) 500 µA at 1 MHz × the 3.9 MHz MCLK Contiki-NG runs |
| | cpu.lpm0 / lpm1 | 0.075 | SLAS368G p.27: I(LPM0) (LPM1 is not in the table: LPM0's figure) |
| | cpu.lpm2 / lpm3 / lpm4 | 0.017 / 0.0026 / 0.0002 | SLAS368G p.27 (25 °C) |
| | radio.listen / rx / interfered | 18.8 | SWRS041c 6.10 p.13: receive mode |
| | radio.tx | 17.4 at PA_LEVEL 31 (8.5 at 3) | SWRS041c Table 9 p.51, by the PA level the firmware set (linear between the table's eight points) |
| | radio.off, led.on, timer.on | 0 | idle (0.426 mA) uncharged, like Contiki's Energest; no LED figure; timers are in the MCU mode currents |
| CC2538DK, OpenMote, 3.0 V | cpu.active | 13 | SWRS096D 5.4 p.10: 32-MHz XOSC, CPU running with flash access (Contiki-NG clocks the core at 16 MHz from it; no row for that: an upper bound) |
| | cpu.sleep (PM0) | 0.6 | assumption: PM0 is not in the table; PM1's figure, a lower bound |
| | cpu.pm1 / pm2 / pm3 | 0.6 / 0.0013 / 0.0004 | SWRS096D 5.4 p.10 |
| | radio.listen | 24 | SWRS096D 5.4 p.10: RX at -100 dBm (waiting for a signal), CPU idle |
| | radio.rx / interfered | 20 | SWRS096D 5.4 p.10: RX at -50 dBm input, CPU idle |
| | radio.tx | 24 | SWRS096D 5.4 p.10: TX at 0 dBm, CPU idle (TXPOWER not modelled) |
| | gpt0-3.on / ssi0-1.on / uart0-1.on / i2c.on | 0.12 / 0.3 / 0.7 / 0.1 | SWRS096D 5.4 p.10, peripheral current per unit, read as "its clock gate open" |
| nRF52840-DK, Dongle, 3.0 V | cpu.active / sleep | 3.3 / 0.0032 | **unverified** (nRF52840 PS v1.7 5.2 as commonly quoted; no copy at hand) |
| | radio.listen / rx / tx | 4.8 | **unverified** (802.15.4 RX; TX at 0 dBm; DC/DC) |

The CC2538's radio rows include the SoC's own idle draw, so a node with the
radio on and the CPU asleep is charged a little twice (about the sleep row).
A board with no model is charged nothing unless the config gives currents.
The summary says which model a node ran on, and marks unverified figures.

## Clocks

Each stamp is the node's own simulated time at the cycle where the change
took effect — the clock the rest of the node's output uses — clamped so a
node's stamps never go backwards:

- MSP430: `msp430_cpu_now_ns()` — the slice's pin plus the cycles executed
  since, less the LPM time fast-forwarded since (a sleeping core keeps its
  stale cycle count, which the runner re-pins at a slice start).
- Cortex-M: the console clock (`arm_cpu_stamp_ns`) — the last execute time
  plus the cycles run since, carried across CLOCK_CTRL frequency changes,
  capped at the slice end.  On the CC2538 agreement run it gives the same
  result as the raw cycles→ns form to 0.05 ticks.

## Validation

`tools/check-energest.sh` (CI) runs all of these:

1. **Files agree** (`tools/check-energest-output.py`): every component's
   states sum to its node's lifetime; the trace re-sums to the CSV to the
   nanosecond; frames are consecutive, every bin decodes to 0..1000 permille,
   and the last frame's totals equal the CSV's — on a Sky pair and on
   Sky + CC2538 + nRF52840 in one network.
2. **CPU sleep = the core's own accounting.**  Between a node's first and
   last wake, the CPU lane's low-power time equals the core's fast-forward
   total (`lpm_ns`, carried on each transition): CC2538 +0.015 µs, nRF52840
   +0.011 µs, MSP430 -0.27 µs per wake (the MSP430 wakes on the scheduler's
   microsecond grid; the core counts cycles).  The ends are left out: the
   core has not fast-forwarded a sleep that is still going on.  On ARM the
   core's total is `lpm_ns - lpm_rewound_ns`: a WFI at the slice horizon
   winds an overshooting core back (issue #76) and `lpm_ns` alone counts the
   overshoot twice (40 µs per wake on the CC2538 rpl-udp run).
3. **TX = frame air time + a steady overhead.**  Every frame's air time is
   its bytes on air × the byte time (32 µs at 250 kbit/s), exactly, and every
   TX dwell is paired with the frame sent in it: CC2420 data frames take
   257-266 µs more (its 192 µs calibration and the model's preamble start),
   5-byte ACKs 319-323 µs; CC2538 data frames 0 µs, its automatic ACKs
   223 µs (RX→TX turnaround); nRF52840 1.8-1.9 µs.
4. **Deterministic, and only observes**: two runs write identical files; a
   run's PCAP is the same with the plugin and without it.
5. **The firmware's own Energest** (`tools/check-energest-agreement.py`):
   rpl-udp built with Contiki-NG's simple-energest
   (`firmware/src/rpl-udp-energest`) prints its per-state rtimer ticks every
   10 s; the trace's sums over the same windows, 22 periods per board:

   | Board | tx (mean / max \|diff\|) | listen | CPU per wake |
   |-------|--------------------------|--------|--------------|
   | CC2538DK (32768 Hz ticks) | -0.23 / 1.37 ticks | +0.28 / 1.37 | +165 cycles at 16 MHz, spread 4 |
   | nRF52840-DK (62500 Hz) | -0.25 / 1.76 ticks | +0.25 / 1.76 | +48 cycles at 64 MHz, spread 6 |
   | Tmote Sky (32768 Hz), `--tx-from-sfd 5` | -0.63 / 1.68 ticks | -3.34 / 10.19 | +240 cycles at 3.9 MHz, spread 1.3 |

   The radio agrees to about one tick per 10 s.  The CPU difference is a
   constant per wake: the firmware switches its Energest from CPU to LPM
   before its `lpm_enter` housekeeping and back after `lpm_exit`, so those
   cycles run while it counts LPM.  The CC2538's automatic ACKs (a TX
   straight from RX) are real transmit time and charged as such, but
   Contiki's Energest cannot see them (no driver call): the comparison moves
   them to listening, as the firmware counts them.
   On the Sky, Contiki's CC2420 driver switches its Energest to TRANSMIT
   only once SFD has gone out and back when it falls, so the firmware's TX
   leaves out the chip's 192 µs calibration and the 5-byte SHR, which it
   counts as listening; `--tx-from-sfd 5` compares on the same terms, and
   tx then agrees within 1.68 ticks.  The Sky's listening is **an open
   item**: the chip model goes to IDLE (an SRFOFF strobe) for 129-160 µs
   after some received frames, a few times per 10 s, while the firmware
   counts that time as listening; mean -3.3, worst -10.2 ticks per period,
   so the Sky check uses `--listen-tolerance 12`.  Which driver path strobes
   it was not traced in this change.
6. **Refusals**: an unknown board, component or state in `currents:`, a
   negative current, a `frame_ms` that is not a multiple of `bin_ms`.

### Cross-checking against Java Cooja's PowerTracker (Sky)

PowerTracker (Cooja's `PowerTracker` plugin) reports, per mote, the
fraction of time the radio was on, transmitting and receiving (and
interfered), from the radio's events.  To compare, run the same `.csc` in
Java Cooja with PowerTracker open (or its `PowerTracker` script API: the
`radioOn`, `radioTx`, `radioRx` ratios), convert it with `tools/csc2json.py`,
add `plugins: [energest]` and compare the Sky nodes' `radio` lane
fractions: `listen + rx + tx + interfered` against "radio on", `tx` against
"radio TX", `rx + interfered` against "radio RX".  Expect PowerTracker's TX
to be shorter by the CC2420's calibration per frame (PowerTracker counts
from the frame's start) and its RX to start at the SFD.  This was not run
for this change: no Java Cooja PowerTracker set-up was at hand.
