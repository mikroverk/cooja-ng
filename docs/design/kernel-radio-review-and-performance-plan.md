# Kernel and radio-path review, with a performance and refactoring plan

Status: **review complete** (2026-09-25, revised 2026-09-27 after review on
PR #62). Tier 0 (items 1–4) and Tier 2 item 1 have landed (Tier 0 item 3
in PR #61, items 1, 2 and 4 in PR #64; Tier 2 item 1 in PR #60); the
current sequencing is in §7.
Findings F20–F33 (November 2026: radio timing, TSCH, and open items kept
for later PRs) are at the end of §2.

Revisions: the §4 measurements and the Tier 0 prototype were taken on this
machine (Apple Silicon, stock `make`, GNU Lightning present) at `252fc0e`;
the PR #60/#61 figures on those branches, based on `22a66f0`. Every file:line
is anchored to `main` at `e4e3271` (after #60 and #61).
Companion to [`arm-performance-plan.md`](arm-performance-plan.md) (the ARM
interpreter, which this plan deliberately does not revisit) and
[`refactor-plan.md`](refactor-plan.md) (whose Phases 1–12 this plan assumes).

## Summary

**The architecture is sound and the kernel is used the way it should be.**
One unified `(time_ns, seq)` heap, one pump that sets `now_ns` from the popped
event before dispatch, generation-stamped events, every scheduler going
through the `sim_schedule_*` wrappers, and per-byte `RX_BYTE` kernel events
for every emulated chip. No wall-clock or host time on the radio path. The
Cooja invariants hold for chip-to-chip traffic.

**Radio delivery has no live shortcut between emulated chips, but three
things need fixing:**

1. A whole synchronous delivery subsystem inside `sim_radio_bus.c` (direct
   per-byte stepping of *another* mote's CPU, RXFIFO mini-steps, drain
   queues, a second auto-ACK model) is **dead for every chip today** — all
   chips are `PER_BYTE` — yet still compiled, still documented as the nrf52840
   path, and one registration flag away from coming back. It should go.
2. **JS app motes cannot hear emulated senders.** Their frames land in a
   queue nothing drains; `configs/cross-level-demo.json` shows `64 queued,
   0 drained, 3 dropped`.
3. The CC2538 hardware auto-ACK is emitted **synchronously at the last data
   byte with no turnaround**; three other timing constants are wrong or
   fudged (nRF52840 96 µs ACK, nRF54L15 fixed 100 µs PHYEND, CC1200 byte
   period on a node's first sub-GHz frame).

**Where time goes depends entirely on the ISA.** ARM workloads are ~90%
interpreter and are the ARM plan's problem. MSP430 workloads are dominated by
**kernel overhead per event**, because an active mote is sliced 1 µs at a
time (Cooja `execute(t, 1)`) and each slice is a full round trip through a
dispatcher that does **O(N) work per event**. On the 100-node grid that is
~1.6x real time (20 s simulated in 12.55 s) with only 9% of samples in the
interpreter.

**Two byte-identical changes measured in a scratch copy** (skip the runner's
per-wakeup all-nodes loops when they cannot matter; stop installing a debug
PC-trace hook on every MSP430 node):

| Workload | Stock | Prototype | |
|---|---|---|---|
| `udgm-100node-grid-sky`, 20 s sim | 12.55 s | **2.34 s** | **5.4x** |
| `chain-4node-sky`, 180 s sim | 0.685 s | **0.551 s** | 1.24x |
| `chain-4node-cc2538dk`, 180 s sim | 0.666 s | 0.633 s | 1.05x |

stdout (every timestamped console line) was identical on all three. The
PC-trace half has since landed on its own (PR #61) and measured no speedup,
so the gain is the loop skipping (Tier 0 items 1–2). Measured alone (with
item 4) against `main` at `521d32c`: 5.78x on the grid, 1.27x on the Sky
chain, 1.05x on the CC2538 chain, byte-identical — see Tier 0.

---

## 0. Method

- Read: `src/sim/sim_runtime.c`, `src/common/sim_event_queue.c`,
  `src/sim/sim_radio_bus.c` (+ header), `src/common/radio_medium.c` hot
  path, `include/common/event_queue.h`, the runner's loop/dispatch
  (`test/test_mixed_multinode.c`), both ELF mote modules, all five radio
  chips, the native/JS/external/Renode motes.
- Measured: `CSIM_PHASE_TIMING=1`, `sample(1)` self/inclusive profiles, and
  an instrumented scratch build that counts kernel events by kind and
  histograms per-CPU peripheral events by callback. The scratch build was
  also used for the prototype above; nothing in the tree was changed.
- Workloads: `chain-4node-sky.yaml`, `chain-3node-nrf52840-dk.json`,
  `chain-4node-cc2538dk.json`, `test-tsch-nrf52840-dk.json`,
  `udgm-100node-grid-sky.json`.

## 1. Architecture verdict — what is clean (verified)

| Property | Where | Verdict |
|---|---|---|
| Single unified queue, `(time, seq)` FIFO, `now_ns` set from the popped event before dispatch, generation drop, stop/pause honoured | `src/sim/sim_runtime.c:268–323` | clean |
| No `sim_eq_schedule*` caller outside the kernel; runner only `sim_eq_init` + read-only `peek_time` | grep; `test/test_mixed_multinode.c:2968, 2997, 3114` | clean |
| Past-time wakeups clamped, counted, warned once | `sim_runtime.c:105–120` | clean |
| Mote never writes `now_ns`; slice re-pins `sim_time_ns` to kernel time at both ends; overshoot ≤ 1 instruction and debited from the next slice | `src/motes/msp430_elf_mote.c:613–619`, `arm_elf_mote.c:438–449`, `msp430_cpu.c:906–942` | clean, MSPSim-faithful |
| Idle mote's wakeup derived from its CPU event-queue head; active mote sliced 1 µs (Cooja `MspMote.execute(t,1)`) | `msp430_cpu.c:973–984`, `arm_cpu.c:4801–4814`, `mote_impl.h:111–115` | clean (and the performance story, §4) |
| Every emulated chip registers `PER_BYTE`: one `SIM_EV_RX_BYTE` per on-air byte at `first_byte + n·period`, clamped ≥ now | `msp430_elf_mote.c:567–571`, `arm_elf_mote.c:395–400`, `sim_radio_bus.c:351–357` | clean |
| RX entry is the kernel: `execute(t,0)` → `receive_byte` → same-time wakeup | runner `deliver_rx_byte` :631–685 | clean, Cooja `MspMoteTimeEvent` |
| TX byte stamp is the kernel's `now`, never the mote-local clock | `sim_radio_bus.c:420–442` | clean, Cooja radio-callback semantics |
| No chip TX callback runs from inside a JIT block; `cpu->cycles` current at every IO callback | `msp430_jit.c:334–347`, `arm_jit.c:246–300, 664–694` | clean |
| No wall-clock/host time or libc `rand` on the radio path; medium RNG is seeded xorshift | grep; `radio_medium.c:58–63` | clean |
| Native→native frames queued at end-of-air-time through the kernel | `sim_radio_bus.c:957–1031`, `native_cooja_mote.c:393–416` | clean |
| Medium filter: link block, spectrum/channel pair, one probabilistic roll per (sender, receiver, frame) | `radio_medium.c:639–684` | clean |

## 2. Findings — radio data delivery

Severity: **A** = wrong behaviour observable in a supported scenario;
**B** = timing deviation from hardware that firmware can notice; **C** = dead
or misleading code that is a hazard, not a defect.

### F1 (C, large) — the synchronous chip-delivery subsystem is dead code

`sim_radio_bus_frame_complete()` (`src/sim/sim_radio_bus.c:673–924`) still
carries the pre-Phase-5 delivery model: `sim_radio_bus_deliver_bytes()`
(`:541–589`) steps *another* mote's CPU per byte through `rx_byte_sync`,
`frame_complete` mini-steps a receiver `+5000` cycles when its RXFIFO is
full (`:814–819`), `drain_rx` mini-steps (`:610–618`), the `emu_rx_queue`,
`rx_pre_sync`, the `executing_node` guard, the `RX_TICKING_STEP` /
`DRAIN_MINI_STEP` caps, and a second auto-ACK flush with its own turnaround
model (`:844–885`, `ack_start = tx_end + 192 µs`, `sender_ack_start = now +
192 µs`).

All of it only runs for `BATCH` receivers, and the only `BATCH` receivers
(JS, external, Renode) have no `rx_byte_sync`. The header still says
nrf52840 is `BATCH` (`include/sim/sim_radio_bus.h:161–164`); it is not
(`arm_elf_mote.c:395`). The CC2420's `rx_incoming[]` replay
(`src/chips/cc2420.c:398–406`) is equally dead: `rx_incoming_count` is only
ever zeroed. The CC2538's ISTXON-time direct-to-RXFIFO ACK injection
(`src/arm/cc2538_rfcore.c:175–235`) cannot fire under `PER_BYTE` either.

Why it matters: the re-entrant ACK path never feeds the assembler, so a
re-entrant ACK would be stamped preamble@now, SFD@now+32 µs with no
turnaround (`sim_radio_bus.c:420–436` + `:459–467`); and `deliver_bytes` runs one mote's
CPU from inside another's slice — exactly the invariant the kernel exists to
enforce. One `SIM_RADIO_DELIVERY_BATCH` registration on a chip mote brings
all of it back. The runner also spends real time on it (§4, F8).

### F2 (A) — JS app motes are deaf to emulated senders

`src/motes/js_app_mote.c:58–62` registers `BATCH` with `caps=0` and
`rxfifo_available` returning 0. In `frame_complete` that forces a
`step_until` on the JS mote and a `queue_frame` into `emu_rx_queue[js]`,
which `drain_rx` can never deliver (no `rx_byte_sync`). Measured on
`configs/cross-level-demo.json`: `Emu RX frames: 0 direct, 64 queued, 0
drained, 3 dropped`. The header already describes this failure mode
(`sim_radio_bus.h:188–195`). `js_mote_receive_frame` is only reached from
the frame-level path (native/JS/external senders). Fix: give JS
`SIM_RADIO_CAP_FRAME_CONSUMER` like external and Renode motes, and decide
whether the JS mote should see the frame at air-time start (today's
`receive_frame(now)` semantics, `js_app_mote.c:117–124`) or end.

### F3 (B) — CC2538 auto-ACK: synchronous, no turnaround, no TX state (**fixed by PR #91**, Sky senders partly)

PR #91 sends the ACK from a chip event 192 µs after the frame ends (one byte
period plus 192 µs after the last byte is delivered, since a receiver gets
every byte at its start), with the radio in its ACK states (`TX_ACTIVE`,
deaf) until the ACK is off the air.  Measured in the PCAP, ACK start minus
frame end: nRF52840 and CC2538 senders 192 µs exactly (160 µs at the PR's
first cut, −32 µs before it).  Sky senders measure 225–232 µs, because a
CC2420 frame is stamped one byte early on the bus clock: the CC2420 model
(MSPSim's) leaves a byte period between the SFD and the length byte that the
bus's contiguous byte clock does not count, and the bus stamps its bytes
with the scheduler time, about 1 µs before the MSP430's own clock.  On the
CC2420's clock the ACK still lands 1–2 µs before its 12-symbol RX
calibration ends; the model drops that first preamble byte and needs four,
so a Sky sender misses some on-time ACKs (8 of 25 unicasts retried in
`test-mixed-platform-line`, 3 of 12 in `test-mixed-platform-rpl`, from 63
of 72 before).  MSPSim's own CC2420 ACK goes out 16 symbols after a frame,
which is why two Skys never hit this.  Making a Sky hear every on-time ACK
is a CC2420-model or bus-clock change, not a CC2538 one.  The original
finding:


`src/arm/cc2538_rfcore.c:828–850`: on the last payload byte the 11 ACK
bytes are pushed through `tx_callback` from inside the kernel `RX_BYTE`
dispatch. The bus stamps the ACK preamble at `now` = last data byte's air
time, so the ACK is on the air 32 µs *before the data frame ends* and
192 µs earlier than hardware; the chip stays in `SFD_WAIT` (`:853–855`)
during its own ACK, so it keeps receiving while "transmitting"; TXACKDONE
is raised at once. It works between two CC2538s because the sender's radio
is still in TX and buffers the early bytes (`:95–107`). It cannot work when
the data sender is a Sky: the ACK's preamble and SFD reach the CC2420 while it
is still in its TX states, and it drops bytes outside
`RX_SFD_SEARCH`/`RX_FRAME` (`cc2420.c:583`), so a **Sky→CC2538** unicast with
AR is never acknowledged. The other direction works: a CC2538→Sky frame is
acknowledged by the CC2420's own auto-ACK, which goes through
`TX_ACK_CALIBRATE` with a real turnaround (`cc2420.c:765–768`). Fix:
stage the ACK and emit it from a chip event at `+192 µs` after the last
byte, entering a TX state for its air time (the nRF52840 already does this;
`nrf52840_soc.c:900–908, 1564–1567`).

### F4 (B) — three timing constants

- (Gone since #88, which removed the stand-in ACK: the firmware sends its
  own.) nRF52840 auto-ACK fires at `cycles + 96·64` = **96 µs**
  (`nrf52840_soc.c:1564–1567`) while every comment says 192 µs (`:894,
  :1102, :1407, :2210`). Also hardcodes 64 MHz.
- nRF54L15 PHYEND is deferred a **fixed 100 µs** regardless of frame length
  (`nrf54l15_soc.c:1559–1571`). The comment's rationale ("the multinode
  harness delivers bytes … at synchronous (tx-start-anchored) timestamps")
  describes the pre-Phase-5 path that no longer exists; the per-byte path
  paces receivers correctly, so a full air-time PHYEND should now be
  re-tried. `nrf54l15-ack-gap.md` §"What remains approximate" already asks
  for real ramp durations.
- CC1200: `sender_byte_ns` is read *before* the assembler sees the byte
  (`sim_radio_bus.c:440–442`) and `subghz` only flips on the 4th sync byte
  (`:38–42`), so the first 8 bytes of a node's first sub-GHz frame (and of
  the first after a 2.4 GHz frame on a dual-radio Firefly — `tx_asm` is
  per node, not per radio) are stamped 32 µs apart, then jump ~1 ms.
  Harmless to reception (no `rx_stall` on those motes), wrong for the
  timeline and collision windows. Fix: per-(node, radio) assembler, or let
  the chip declare its byte period at registration.

### F5 (B) — emulated→native frames complete before their air time (**fixed by PR #56**)

At `252fc0e`, `SYNC` delivery fed each byte to the native assembler inside
the sender's TX call and queued the finished frame *as already ended* on the
receiver's stale clock, so a native mote could consume a frame ~4 ms before
it had finished arriving. PR #56 passes each byte's air time
(`receive_byte_at`, `sim_radio_bus.c:336–346`); the assembler queues the
frame so it ends on the bus clock (`src/native/native_radio.c:138–148`), the
native is woken at that end, and `native_dequeue_rx_frame` refuses a frame
whose end is still in the future (`native_node.c:278`).

### F6 (B) — CC2538 receives while RF is off; CCA never busy (**fixed**)

The CC2538 RF core now drops bytes while its receiver is off or
calibrating (192 µs after `ISRXON`), and `RSSI_VALID`/`FSMSTAT1.CCA` rise
128 µs after that (`test_runner cc2538-rfcore`).  CCA and RSSI on
CC2538, CC2420, nRF52840, CC1200 and CC13x2 read one medium query,
`sim_radio_bus_channel_energy` (the strongest frame on the air on the
node's channel, from every sender's on-air window), so CCA is busy while
a frame reaches the node; the runner-side `mixed_channel_busy` is gone
(`radio-bus`, `cc2538-rfcore`, `nrf52840-radio`, `mock-host`; Decisions
Log).  nRF54L15 CCA is still always idle.  The original finding:


`cc2538_rfcore.c:244–252` keeps the parser live after `ISRFOFF`
(`:691–693` "simulates perfect reception even when … turned the radio
off"); `ISTXONCCA` always transmits and `FSMSTAT1.CCA` is always clear
(`:68, :239–242, :381`); nRF52840 CCA was always idle, at once (since
the nRF52840 timing fix it reports 128 µs on, busy from the bus's on-air
notices: `radio_cca_done`). CC1200
CCA does consult the bus's `tx_busy_until` (`runner :817–848`). Documented
modelling gaps; listed here because they are the remaining "receiver never
misses" shortcuts.  A frame received this way gets no auto-ACK since
PR #91: silicon would not have heard it, so the sender sees the miss.

### F7 (C) — smaller items

- (**fixed by PR #64**, Tier 0 item 4) `deliver_rx_byte` schedules `if_earlier(next)` then `if_earlier(t)`
  (`runner :683–684`). `sync_to_time` returns a non-negative lead
  (`msp430_cpu.c:973–986`, `arm_cpu.c:4801–4814`), so `t ≤ next` and the
  second call always wins: the **first** one (`next_ns`) is redundant. The
  second is Cooja's same-time wakeup after `receivedByte()` and must stay.
- `frame_complete` uses `static` snapshot arrays (`sim_radio_bus.c:726–729`)
  — safe only because `tx_depth` prevents nesting; belongs on the bus.
- Per-process statics that break a second runtime: CC2420 stat counters
  (`cc2420.c:138–148`), CC2538 `rxfifo_overflow_count` (`cc2538_rfcore.c:23`), nRF54L15
  `trigger_task` depth guard (`nrf54l15_soc.c:1517–1519`), MSP430 PC-trace
  counters (`msp430_elf_mote.c:93–95`, live only under `CSIM_PC_TRACE=1`), `sim_service.c:74 dispatch_depth`,
  `sim_runtime.c:258, 270–271` spin diagnostics.
- `arm_elf_mote.c:376–377` says nRF54L15 has an `rx_incoming` buffer; it
  has none (`nrf54l15_soc.c:1822–1829`).
- `native_yield_callback` / `native_step_until_ns` (`runner :1315–1374`,
  `native_node.c:356–373`) would run other natives' ticks and hand an ACK
  back with zero air time — dead today (no caller reaches native
  `step_until`), a landmine.

### F17 (A) — sub-GHz byte clock re-armed on every preamble byte (**fixed by PR #67**)

`sim_radio_bus_tx_byte` arms a frame's byte clock (`first_byte_ns`, the
capture length) on its first preamble byte, guarded by `zero_count == 0`.
`zero_count` only counts `0x00`, so for a CC1200 frame (`0x55` preamble) the
guard held on **every** preamble byte: the clock restarted on each of the four,
so the preamble was stamped "now" and the sync word followed one byte period
later instead of after four.  For a node's first sub-GHz frame the byte period
was also still the 2.4 GHz one until the sync word (F4), so its whole preamble
and sync word took 128 µs instead of 1.28 ms; for every later frame the
assembler kept `subghz`, and the frame ran 3 byte periods (480 µs) early from
the sync word on, the capture short of 3 preamble bytes.

The CC1200 drops bytes it receives outside RX, and hunts for the whole sync
word.  A soft ACK sent back within ~0.3 ms of `RX_DONE` (the common case in
the emulator) therefore had its sync word on the air before the data sender's
radio was back in RX (the sender's own model ends TX ~0.7 ms after the
receiver's `RX_DONE`), and was lost.  The sender retried, the receiver ACKed
again with the same timing, and CSMA reported `MAC_TX_NOACK` after all 8
attempts — unicast links either worked on the first try or never.  RPL's ETX
for those links climbed past MRHOF's 512 limit, no parent was acceptable, DAOs
stopped, and `chain-4node-firefly-subghz` collapsed after ~40 s (node 4 never
got a response).  This is the "byte-delivery model bunches sub-GHz frames"
residual `b7c18bc` described in June; arming on `0x55` (added since) did not
fix it, because the arm was not once per frame.

The fix (PR #67, after review):
- The clock arms on the **first byte of a run** of one preamble byte while the
  assembler hunts for a preamble (`tx_asm.pre_run`).  Any other byte ends the
  run, so a rejected frame's remainder or a stray byte cannot stop the next
  frame from arming — a first version guarded on `sync_match == 0`, which a
  length reject left holding the SFD and trailing bytes re-dirtied.  For
  2.4 GHz the rule is exactly the old `zero_count == 0`.
- Every return to PREAMBLE (frame end, length reject, PHR reject) goes
  through one reset that clears both preamble detectors.
- The first preamble byte also sets the frame's byte period, so a node's first
  sub-GHz frame, and a 2.4 GHz frame right after a sub-GHz one on a dual-band
  Firefly, are timed at their own rate from the first byte (F4's CC1200 item).
- The sub-GHz special cases that anchored the busy, collision, delivery and
  interference windows to `now` ("first_byte_ns unarmed") are gone: with the
  clock armed they computed the same values (the Firefly runs are
  byte-identical with and without them).
- `test_radio_bus` pins it: one arm per sub-GHz frame at 160 µs from the first
  byte, two frames in a row, 2.4 GHz after sub-GHz, and the next frame after a
  length reject, a frame longer than its PHR, and a PHR reject.

2.4 GHz is unchanged: `check-baseline.sh` byte-identical.  Sub-GHz moves, as
it should: the 4-node chain passes (64 requests at the root, 19 responses at
node 4, two retries in 240 s where every failure used to be eight); 2-node
`-subghz-fixed` RPL-UDP completes 5/5 round trips in 60 s (was 2/5, with 50
NOACKs); nullnet broadcast is unchanged.  The chain is a CI step (~2 s) that
asks for at least 10 responses at node 4, so a mostly broken link does not
pass.

### F18 (C) — pcap and the packet analyzer misread sub-GHz frames

The pcap capture writes CC1200 frames with timestamps out of order (hundreds of
seconds into a 12 s run) and payloads that do not start at the MAC header
(`3a1a9b…`), and the verbose `[PKT]` decoder calls a 95-byte data frame
"ACK seq=155".  Both look like a 2.4 GHz PHY-wrap strip (4 preamble + SFD +
length) applied to 802.15.4g frames (4 preamble + 4 sync + PHR).  Not caused
by F17: a capture with PR #67 applied is the same.  Observation only — the
simulation is unaffected — but a sub-GHz pcap is currently useless for
debugging.

### Findings F20–F33 (November 2026): radio timing, TSCH and loose ends

Found while building the energest demos and fixing TSCH on the CC2538 and
nRF52840 (branch `fix/tsch-radio-timing`).  F20–F23 are fixed there; the
rest are open and are kept here so a later PR can pick them up.  Line
numbers are at that branch's head (e156e50).

#### F20 (B) — CC2538 RF core had no FSM timing (**fixed**, e0fe553)

TX went on the air at the STXON strobe, RX was ready at once, the SFD
signal, its interrupt and the MAC-timer capture came a byte early, and the
radio returned to RX right after TX.  TSCH unicast never worked: every
enhanced ACK missed the sender's window (0 of 8 heard; the rpl-udp client
never joined).  Now 192 µs TX and RX calibration (SWRU319C §23.8.2,
§23.9.2), SFD at the SFD's end (§23.9.5.2, Fig. 23-9, §12.1.10), TX final
2 µs plus TX/RX transit 190 µs (Fig. 23-18), deaf while transmitting or
calibrating; 26 of 26 EACKs heard.  Tests `test-tsch-unicast-cc2538dk`,
`test-tsch-chain-cc2538dk`.  Why `test-tsch-lpm-cc2538dk` passed anyway: a
unicast reaches its receiver on the first try whether or not the sender
hears the EACK, and that test waits for one DAO-ACK (41 retransmissions in
48 unicasts before, 2 in 136 after).

#### F21 (B) — nRF52840 RADIO.POWER = 0 did not reset the radio (**fixed**, 5c7164e)

The PS (register POWER) resets the peripheral and its registers on a power
cycle; the model kept every register and event.  Contiki-NG's TSCH runs the
radio in poll mode and relies on the power-down at each slot's end to clear
EVENTS_CRCERROR, so one collision at a router left `receiving_packet()`
"not receiving" for seconds: TSCH failed past about 3 hops (7-node chain:
nodes 6 and 7 never answered in 600 s; 4×4 grid 7 of 15).  Now every node
answers.  Test `test-tsch-chain-nrf52840`; arm-correctness
`test_nrf52840_power_reset`.

#### F22 (B) — nRF52840 ACK path too fast (**fixed**, f069081)

CCA answered idle at once, TXEN from RXIDLE had no turnaround, END/CRCOK
fired as the last byte began, ADDRESS/FRAMESTART as the SFD byte began,
and ramp-up was 40 µs whatever MODECNF0.RU said.  A Contiki-NG soft ACK
went out 7–47 µs after the frame, so a CC2538 (in its 192 µs turnaround
since F20) missed it and every CC2538/Sky→nRF unicast was sent 8 times.
Now: ramp-up 130 µs (40 µs with RU fast, PS §6.20.15.8), CCA 128 µs and
busy on the bus's on-air notices (§6.20.12.4/.6), FRAMESTART once the PHR
is in, END/CRCOK 23 µs after the last bit, RX↔TX turnaround 20 µs.  ACK
gaps 211–250 µs (OpenThread's nrf_802154 ACK exactly 192 µs), one try per
unicast.  Test `tools/check-acks.py` in CI.
**Not checked on hardware:** the 20 µs turnaround and the 23 µs END
latency come from Nordic's nrf_802154 (`RX_TX_TURNAROUND_TIME_HW`,
`RX_PHYEND_EVENT_LATENCY_US`), not the PS; the resulting TSCH per-hop
offset (+17 to +21 µs, a child late, was −93 to −98 µs) has not been
measured on silicon.

#### F23 (B) — nRF52840 and nRF54L15 never told the medium their channel (**fixed**, e156e50)

Frames on different channels collided and were received, so TSCH channel
hopping separated nothing.  RADIO.FREQUENCY now pushes the 802.15.4
channel (PS §6.20.14.45, §6.20.12.2).  Test `test-channels-nrf52840` (two
OpenThread networks on channels 15 and 20 stay apart).  As on silicon, a
joining TSCH node now hears only the EBs on its scan channel, so joins are
slower (`tsch52` passes at 53 s, was 22 s; the 7-node chain's timeout is
900 s).  Orchestra retransmissions 10.2% → 7.5%.

#### F24 (B, open) — a Sky (CC2420) misses about half of a CC2538's ACKs

On `test-mixed-platform-line`, 8 retransmissions in 16 Sky→CC2538 frames.
The CC2420 model needs all four preamble bytes to find an SFD, and Sky
frames are stamped about 31 µs early on the bus clock (see F3).  Silicon
needs fewer preamble bytes (CC2420 SYNCWORD/preamble detection); fixing it
moves every Sky workload.

#### F25 (C, open) — remaining nRF52840 radio gaps

- TX disable takes 10 cycles; the PS gives 21 µs.
- EDSTART (energy detect) completes at once.
- On transmit, ADDRESS and FRAMESTART fire at START.
- CCACTRL mode and thresholds are ignored: busy means any on-air notice,
  including from nodes only in interference range.
- `RADIO_INT_EDEND` is bit 16 (`nrf52840_soc.c:834`); the SVD says 15.
- `radio_abort_inflight_rx` (see `nrf-multihop-forwarding-plan.md`) raises
  CRCERROR when the radio is disabled mid-frame, which silicon does not.
- The F19 workaround ends the batch on TXEN/RXEN/START/CCASTART written by
  code; a task triggered by PPI or a SHORT does not end it.

#### F26 (C, open) — nRF54L15 radio

TX ends a fixed 100 µs after TASKS_START whatever the frame's length
(`nrf54l15_soc.c`; energest flags its radio lane approximate); there is no
SOFTRESET, which nrf_802154 uses on SoCs without a POWER register; its
timing was not reviewed against F22, and TSCH on it is untested.

#### F27 (C, open) — remaining CC2538 gaps

- `ns_to_mt_ticks` (`cc2538_rfcore.c:70`) computes `ns * 32000000` in
  int64, which overflows past about 288 s of simulated time; its callers
  pass absolute times (`:177`, `:790`).  Fixing it moves long CC2538 runs.
- The 12-symbol SFD-search pause after a frame (`FSMCTRL.RX2RX_TIME_OFF`,
  §23.9.2) and the 2 µs SFD stretch are not modelled.
- The TX FIFO is cleared after TX; silicon keeps it (§23.8.4).
- F6 (reception while RF is off): fixed, see F6 above.

#### F28 (C, open) — CC1200 two-byte PHR wait

The frame assembler waits `4 + 4 + phr_len + expected_len + 2` bytes for
an 802.15.4g frame (`sim_radio_bus.c:363`), and `frame_air_bytes()` adds 2
too.  Raised in the review of PR #89; check against the 802.15.4g framing
(PHR, then a PSDU whose length already counts the FCS) before changing it.

#### F29 (C, open) — a Zephyr hello-world node makes other nodes overrun the run's end

Seen on 87abceb: with a Zephyr hello-world node in the network, the other
nodes' output runs past the configured end.  Not yet reproduced on main.

#### F30 (C, open) — sanitizer findings outside the CI job

- ASan reports a leak in `nrf52840_soc_init` on the `check-save` path.
- UBSan: `lib/cbor.c:378` negates `unsigned_value + 1` as int64, overflow
  for INT64_MIN; `cbor-json` is not in the sanitizer job.

#### F31 (C, open) — the shared ARM-core batch overshoot (F19) is worked around, not fixed

F20 and F22 end the batch at radio strobes; the core still runs up to
about 250 µs past a slice after a WFI wake (F19).  Bounding the batch at
`cycle_limit` fixes it for every ARM radio and moves every ARM workload;
when that lands, check whether the CC2538/nRF stops are still needed.

#### F32 (C, open) — UI and runner leftovers

- Wheel zoom on the network canvas anchors about half a canvas away from
  the cursor (`ui/index.html`, the canvas `wheel` handler).
- From the review of PR #93: layer priority and fill alpha; senders that
  are not OpenThread drawn as Thread nodes.  The runner's service table
  fills after repeated shell restarts.  Not re-checked since.

#### F33 (C, open) — CI time

`check-cngsim` runs the 600 s Thread example (the test job takes about
2 h); the energest branch's energy ladder adds about 3.5 min (shortening
it to 60 s halves that); the nRF TSCH chain test now has a 900 s timeout
(it passes at about 450 s, 35 s of wall time).

## 3. Findings — event-queue usage and time-keeping

### F8 (perf, A-class for scale) — O(N) work on every `NODE_WAKEUP` (**addressed by PR #64**, Tier 0 items 1–2)

`dispatch_mote_wakeup` (`runner :1971–2040`) runs four loops over all
nodes on every wakeup: a native rx-count snapshot (`:1982–1991`), a
native got-frame scan (`:2008–2019`), `emu_rx_queue_drain(r)` for every
emulated node (`:2027–2030`, a bus call each — 16% of samples on the grid),
and `mixed_deliver_rf_bytes(r)` for every native (`:2033–2036`, dead work:
natives are `SYNC`, `rf_pending` is only staged for `BATCH`). With the
1 µs slicing (§4) this is `4·N` loop bodies per active microsecond per
mote — O(N²) in the node count. This is the single largest cost outside
the interpreters; the prototype removes it (Tier 0).

### F9 (perf) — one outer-loop iteration per distinct event time

Headless, the horizon is `min(end, next_event)` (`runner :3114–3138`), so
the pump drains only same-time events and the whole outer body (action
check, JS engine check, service-poll guard, console-injection scan,
radio-state guard, JSON step timeout, progress tick) runs once per event
timestamp: 10.2 M outer iterations for 11.8 M events on the Sky chain.
~6–11% of samples are `run_mixed_multinode_test` self time. The same horizon
is why Tier 1 cannot work on its own (see Tier 1's prerequisite).

### F10 (perf) — heap churn (**addressed by PR #60**)

At `252fc0e`, `sim_eq_schedule_gen` implemented "replace the pending wakeup"
as remove + sift-up + sift-down + insert; every `if_earlier` that fired paid
the same, and `sim_eq_pop` copied a 40-byte struct by value twice. Together
12% of Sky-chain samples, 13% on the grid. PR #60 reschedules in place and
sifts with a hole (`src/common/sim_event_queue.c`); the result and the
decision to stop there are under Tier 2 item 1.

### F11 (perf) — per-instruction debug hook on every MSP430 node (**fixed by PR #61**)

At `252fc0e`, `msp430_elf_mote_install_pc_trace` was called for every MSP430
node in every run, installing a callback the interpreter calls per
instruction (`msp430_cpu.c:1081–1082`). It compared against **hardcoded
firmware addresses `0xcb32` and `0xb138`** from one historical TSCH image and
fed three counters that appeared only in the end-of-run `FW
cc2420_transmit=…` line. PR #61 made it opt-in (`CSIM_PC_TRACE=1`,
`runner :2798`) and resolves the entry points from each node's own image
(`msp430_elf_mote.c:86–141`). The hook was 1.1% self time, but removing it
measured no speedup (Tier 0 item 3), so it contributes nothing measurable to
the prototype's gain.

### F12 (perf) — the 1 µs slice costs a function call per instruction

`msp430_step_until` (`msp430_cpu.c:906–942`) and `arm_step_until`
(`arm_cpu.c:4734–4767`) single-step (`steps = 1`) once fewer than 10
cycles remain, i.e. for essentially every active-mote slice (4 cycles at
4 MHz). The interpreter already stops at `cycle_limit` per instruction
(`msp430_cpu.c:1019–1020`), so the outer batching is redundant with it; the
ARM plan's §5.7b measured 79.8% of `arm_step` calls arriving with a budget
of 1. `msp430_step_until` self + `msp430_step` = 7% of Sky samples.

### F13 (design) — MSP430 JIT is switched off in multinode

`msp430_elf_mote.c:327–335` frees the JIT cache "because the scheduler
steps in ~1 µs increments". True today; it is a consequence of the slicing
model, not a fixed fact, and it means the only accelerated ISA in the tree
runs unaccelerated in every multinode simulation.

### F14 (correctness, minor) — `now_ns` pinned to the horizon before the pump

`runner :3140` sets `sim_rt.now_ns = sim_ns` (the horizon) before actions,
JS gen-msgs and service polls run, then the pump moves it back to the first
event. Headless the horizon *is* the next event; with the UI (+100 ms),
serial/pacing (+1 ms), the shell (+1 s) or a Renode quantum, anything that
reads `sim_runtime_now_ns` in that window (`sim_control_send` wake,
`native_cooja_mote.c:345–348`, shell wakes `shell_commands.c:921, 1876`,
Renode UART inject `renode_mote.c:84–85`) stamps a time later than every
pending event, and `now_ns` is non-monotonic within an iteration. The
header documents it as intended (`sim_runtime.h:315–317`); the consequence
(injected input up to a slice late in interactive modes) is not.

### F15 (correctness, minor) — MSP430 serial injection with clock deviation

`msp_mote_serial_input` (`msp430_elf_mote.c:743–781`) steps the CPU and
then sets `last_execute_us` from raw cycle time. With `clock_deviation ≠ 1`
raw cycle time ≠ kernel time, so the next slice's `jump_us` is wrong and
the mote over-steps. Only MSP430 + serial injection + deviation.

### F16 (labels) — `CSIM_PHASE_TIMING` mislabels

`time_step` brackets the whole `sim_runtime_run_until` including dispatch,
bus delivery and the O(N) loops (`runner :3295–3318`), but prints as "step
(CPU)"; "kernel/other" is only the outer loop. The grid reads 99% "CPU"
while 9% of samples are in the interpreter.

### F19 (correctness, B) — an ARM core woken from WFI runs past its slice's end

`arm_step_until` hands `arm_step` a budget of `remaining / 2`
*instructions* (`arm_cpu.c`, `steps`), and the interpreter loop stops only
on that count (`while (remaining > 0 && !cpu->stopping)`); the cycle
horizon (`cycle_limit`) is consulted by the WFI fast-forward and by the
JIT, not per instruction.  A core asleep for most of a slice wakes at an
event near its end with nearly the whole budget left, and runs it: a TSCH
slot's ISR on the CC2538 (16 MHz) busy-waiting on TX_ACTIVE ran ~250 µs of
cycles past the slice's end, measured with `CC2538_RF_TRACE=1` against the
pcap.  Anything a peripheral puts on the radio bus in that time is stamped
with the kernel's time, the slice's end, so it goes on the air early by
the firmware's own timers; the core then stalls until the kernel catches
up, and if it enters WFI first, issue #76's rewind sets its cycles (its
timers) back to the horizon.  The CC2538 RF core ends the batch at its
FSM strobes (Decisions Log), so its calibrations run on the bus's clock;
the nRF52840's PSDU read is timed on the bus's clock too, and its TXEN,
RXEN, START and CCASTART end the batch (Decisions Log; a Contiki-NG TSCH
slot ISR was ~45 µs past the slice's end at TXEN).
Bounding the batch at `cycle_limit` in the core would fix it for every
ARM radio and moves every ARM workload in `check-baseline`; not done.

## 4. Where the time goes

### 4.1 Event counts (instrumented build, stock semantics)

| Workload | Nodes × sim s | Wall | Kernel events | of which RX_BYTE | Wakeups / node·s | Instr / wakeup | Peripheral events fired |
|---|---|---|---|---|---|---|---|
| `chain-4node-sky` | 4 × 180 | 0.68 s | 11.77 M | 30.6 k | **16.3 k** | **1.9** | 115 k (95 k Timer A CCR) |
| `chain-3node-nrf52840-dk` | 3 × 240 | 0.92 s | 2.31 M | 18.0 k | 3.2 k | 66 | 92 k (RTC) |
| `chain-4node-cc2538dk` | 4 × 180 | 0.68 s | 3.75 M | 30.1 k | 5.2 k | 21 | 92 k (SysTick) |
| `test-tsch-nrf52840-dk` | 2 × 90 | 1.36 s | 3.75 M | 1.0 k | 20.9 k | 49 | — |
| `udgm-100node-grid-sky`, 20 s | 100 × 20 | 12.4 s | 24.0 M | 53.6 k | 12.0 k | 3.2 | — |

Reading: peripheral events are cheap and rare (a 128 Hz clock tick per
node). The kernel event count is set by the **1 µs active-mote slicing**:
a Sky node that is active 1.6% of the time produces 16 k wakeups per second
and executes under two instructions per wakeup; an nRF52840 executes ~64
per wakeup (64 MHz × 1 µs). Every such wakeup is a heap pop, a dispatch
with four O(N) loops, an execute slice with a single-instruction
`step_until`, a heap insert, and an outer-loop iteration: **~58 ns per
event on the 4-node chain, ~515 ns on the 100-node grid.**

### 4.2 Profiles (`sample`, self time, share of samples)

`chain-4node-sky` (3416 samples): `msp430_step_interpreter` 32% ·
`mixed_dispatch_event` self 16% · `run_mixed_multinode_test` self 11% ·
`sim_eq_schedule_gen` 6% · `msp430_step_until` 6% · `sim_eq_pop` 6% ·
`sim_radio_bus_drain_rx` 6% · `msp_mote_execute` 5% · `sim_runtime_run_until`
4% · `sim_schedule_mote_wakeup_if_earlier` 2% · `srh_trace_cb` 1%.
**Interpreter 32%, per-event overhead ~62%.**

`udgm-100node-grid-sky` (4300 samples): `mixed_dispatch_event` self **52%** ·
`sim_radio_bus_drain_rx` **16%** · `msp430_step_interpreter` 9% ·
`sim_eq_pop` 7% · `sim_eq_schedule_gen` 6%. **Interpreter 9%.** ~1.6x real
time; `CSIM_PHASE_TIMING` reports 99% "step (CPU)" (F16).

`chain-3node-nrf52840-dk` (1100 samples): `arm_step_interpreter` 75% ·
`arm_step` 6% · `arm_jit_run` 3.5% · everything kernel-side < 8%.
`test-tsch-nrf52840-dk`: `arm_step_interpreter` 77%. **ARM is
interpreter-bound; see the ARM plan.**

### 4.3 What that means for prioritisation

- For MSP430 and for any simulation with more than a handful of nodes, the
  kernel/runner overhead is the problem and the wins are large and cheap.
- For ARM at 2–4 nodes the kernel is ≤ 10% and only the interpreter matters.
- Both meet in one place: the 1 µs slicing model sets the event count on
  every ISA (F12/F13/T3).

## 5. Optimisation plan

Ordered by measured evidence, then confidence, then cost. Every tier is
gated by `tools/check-determinism.sh` and `tools/check-baseline.sh` (all
nine workloads byte-identical except the lines its `FILTER` drops: wall-clock,
speed ratio, throughput and the opt-in PC-trace lines) plus the
radio-bus/radio-medium unit suites. Tiers 0 and 2 are semantics-preserving by
construction. Tier 1 is only once its prerequisite has been shown not to move
output (see there). Tier 3 is not, and gets its own gate.

### Tier 0 — remove O(N) work from the wakeup path (**DONE: 5.78x on 100 nodes, 1.27x on Sky chain, byte-identical**)

The prototype combined items 1–3 at `252fc0e`. Item 3 alone has since
measured no change, so the gain is items 1–2.

Measured on PR #64 (items 1, 2 and 4) against `main` at
`521d32c` (#60 and #61 in both), min of three sequential runs, stock `make`,
Apple Silicon. stdout (minus the wall-clock lines), stderr and the exit code
were identical in every run:

| Workload | `main` | Tier 0 | |
|---|---|---|---|
| `udgm-100node-grid-sky`, 180 s sim (the config's own `timeout_ms`) | 160.7 s | **27.8 s** | **5.78x** |
| `chain-4node-sky.yaml` | 687 ms | 542 ms | 1.27x |
| `chain-4node-cc2538dk.json` | 678 ms | 644 ms | 1.05x |

Also byte-identical: `check-baseline.sh 521d32c` (all nine workloads),
`test-tsch-cc2538dk`, `mixed-sky-native` (Sky + native motes), and the
Contiki-NG Cooja suite (85/93 pass, 0 fail; the 8 skips are the TUN cases).

1. **DONE** `dispatch_mote_wakeup`: skip the snapshot and got-frame loops
   unless the run has a native, and delete the `mixed_deliver_rf_bytes`
   loop outright (dead — natives are `SYNC`); `mixed_deliver_rf_bytes`
   itself, its one other (equally dead) call in `native_yield_callback` and
   the `emu_deliver_bytes` forwarder went with it.  As built, `have_native`
   is set by `init_node` whenever it boots a native and cleared only on a
   restart, so a removal can only leave the loops running, never skip one
   that matters.
   **Not done:** for the native case, replace the all-nodes snapshot with
   the sender's neighbour list from the medium — only native-heavy
   workloads would gain, and it changes which nodes are inspected, so it
   wants its own gate.
2. **DONE** (the `count == 0` test inline, as the prototype did; a bus-side
   count would need every one of the queue's six writers kept in step, for
   a loop R1 deletes) Drain only receivers with a non-empty `emu_rx_queue`.
   Once F1 lands, this loop disappears entirely.
3. ~~Make the PC trace opt-in~~ **DONE (PR #61)**: `CSIM_PC_TRACE=1`;
   the `FW cc2420_transmit=…` line goes with it. Not a measurable speedup
   alone (685 vs 689 ms Sky chain; 13.4 vs 13.1 s grid). The same PR made
   `check-baseline.sh` capture stdout and stderr separately, since the
   merged capture reported spurious diffs whenever an early stdout line
   moved a buffer-flush boundary.
4. **DONE** Drop the redundant **first** `if_earlier` in `deliver_rx_byte`, the one
   with `next_ns` (F7). Not the second: that is the same-time wakeup after
   `receive_byte`, and without it every receiver reacts to a radio byte one
   slice late, which moves every radio workload.  Pop order cannot change:
   the lead is never negative, so the first request was never earlier than
   `t`, and with the if-earlier semantics the pair leaves the same queue
   entry as the second alone (`min(existing, t)`); nothing is scheduled
   between them, so the first only spent a sequence number.

Cost: a day. Risk: low. The prototype of items 1–3 diffed clean on 2686 + 173
output lines; item 4 was not in it and is gated like the rest.

### Tier 1 — pump-internal same-mote slice batching (est. 1.3–1.6x on MSP430 workloads, ~1.05–1.1x on ARM; needs F9 first)

An active mote reschedules itself to `now + 1 µs`; when that is the earliest
event in the heap, popping it is pure overhead. In `dispatch_mote_wakeup`
(or better, inside the pump with a "continue with the same mote" return
value), after `execute()` returns `next_ns`, loop `while (next_ns <
sim_eq_peek_time(q) && next_ns <= horizon && no stop/pause requested) {
now_ns = next_ns; next_ns = execute(m, next_ns); }` before touching the
heap. This is identical to pushing and immediately popping, because nothing
else can be scheduled between (single thread, and any event scheduled by
the slice itself is visible to `peek_time` on the next check — RX bytes for
other motes go through the heap and end the batch if earlier). It removes
a heap insert + pop, a generation check, an outer-loop iteration and a
dispatch per active microsecond. Expected to cut `sim_eq_*` +
`run_mixed_multinode_test` + `sim_runtime_run_until` self time (~25% on the
Sky chain) to a few percent.

**Prerequisite: a horizon past the next event (F9).** Headless, the runner
caps the pump's horizon at the next event (`runner :3129`), so inside the
pump the horizon *is* the time of the event being dispatched, and
`next_ns <= horizon` never holds for a mote's own next slice. Built as
above, the batch is a no-op and measures 0%. It only pays once the outer loop
lets the pump run past the next event, and that changes how often the
per-iteration duties run:

- timed actions (`runner :3146–3192`) and `CSIM_NODE*_INPUT` injection
  (`:3320–3349`) have due times and can bound the horizon exactly;
- the JS engine drain (`:3198`), `sim_service_poll_all` (`:3274`),
  radio-state tracking (`:3351–3361`) and the progress tick (`:3415`) run
  once per iteration, and output that depends on their cadence can move.

So Tier 1 is two steps. First F9: bound the horizon by the next due duty
rather than the next event, with a written list of the outputs it can move
and a `check-baseline.sh` run showing whether they do. Then the batch
itself, which is byte-identical relative to that. Until the first step has
been through the baseline, "byte-identical" for Tier 1 is a target, not an
argument.

Also required: `sim_control_note_event` (the `step N` budget) still counted
per slice.

### Tier 2 — cheaper primitives (est. 5–10% on MSP430 workloads; byte-identical)

1. ~~`sim_eq_schedule_gen` replace-in-place~~ **DONE (PR #60), and the
   queue is then left alone.** Reschedule rewrites the entry and sifts once;
   both sifts use a hole. Measured 685 → 674 ms on the Sky chain and
   13.4 → 12.5 s on the 100-node grid. The hope was 5–10% (pop + schedule
   were 12–13% of samples); the grid got 7% because its heap is deep
   enough for the per-level saving to matter, the chain got 1.6% because
   its heap is a handful of entries and the rest of the 12% is fixed
   per-call cost (wrapper, generation lookup, clamp, branches). No
   container fixes per-call cost; only calling it less does — that is
   Tier 1. **Decision (2026-09-26): no further event-queue work.** Not a
   radix heap (monotone keys would fit, change-key would not), not a
   calendar queue (bimodal times: active motes within µs of now, sleepers
   tens of ms out), not a 4-ary layout — unless a profile taken *after*
   Tier 1 puts `sim_eq_*` back above a few percent.
2. `msp430_step_until` / `arm_step_until`: call the interpreter once with a
   large count and let the existing per-instruction `cycle_limit` check
   stop it, instead of `steps = 1`. Keep the boundary event drain. Gate on
   the TSCH workloads (the comment cites a real desync the single-step
   fixed; the per-instruction limit check was added later and should make
   the outer batching redundant — verify, don't assume).
3. Per-CPU event queue (`include/common/event_queue.h`): the singly-linked
   insert is O(queue length). Queue lengths are small (< 10), so this is
   not measurable today; leave it, but note `cpu_set_frequency`'s full
   re-sort runs on every DCO write.
4. Runner outer loop: hoist the `getenv`/`snprintf` console-injection scan
   (`runner :3324–3349`) to a one-time table; skip the JSON `has_test` block when
   `action_count == 0`.

### Tier 3 — longer execute slices (the real ceiling; NOT byte-identical, needs its own gate)

The 1 µs slice is Cooja's, and it is what makes every MSP430 simulation a
kernel benchmark. The conservative alternative is to let an active mote A run
until `min(its next CPU event, the next kernel event of any kind, its first
radio emission, now + cap)`.

Single-threaded, nothing can influence A before the next kernel event
*except* reactions to A's own transmissions, and the kernel does not bound
how fast those come. The bus schedules a receiver's `RX_BYTE` at the byte's
own emission stamp, clamped to now (`sim_radio_bus.c:351–357`), so a
receiver can react at the instant A emitted. A chip with no modelled TX
ramp-up (the nRF54L15 puts the whole frame on the air at TXEN) or a channel
switch changes what A's in-slice CCA/RSSI reads should see with zero delay,
so a byte-period lookahead is not safe. The safe rule is to **end A's slice
at its first radio emission** (any call into the bus's TX path): until then
no other mote can have reacted to anything of A's. The cap is then only a
performance knob, not a causality argument. Running on past an emission
would need the smallest modelled TX ramp-up of any receiver in range as the
bound, and that is zero today.

Events A schedules inside the slice (TX bytes) must be stamped with A's
in-slice time (`anchor + cycles − anchor_cycles`), not the slice start. That
last point
changes byte timestamps by up to the slice length and therefore changes
every simulation's output — it is *more* accurate than Cooja, not less, but
it must be validated the way the ARM JIT was (paired runs, TSCH association
+ held sync, RPL convergence, `check-baseline.sh` with an explicit
"expected to move" sign-off).

Payoff: the Sky chain's 11.8 M events would drop toward the ~150 k
peripheral events plus radio bytes; the MSP430 JIT (F13) becomes worth
re-enabling; the ARM `arm_step` budget-of-1 tail (§5.7b of the ARM plan)
disappears. Realistically 3–10x on MSP430 multinode, 1.1–1.3x on ARM.
This is a project (a week plus validation), not a fix, and it should be
done after Tiers 0–2 have taken the cheap wins and after F1 has removed the
synchronous delivery code that would otherwise interact with it.

### What is *not* on this list

- Idle-loop skipping ([`../idle-skip.md`](../idle-skip.md), opt-in): for
  ARM firmware that polls (OpenThread), a slice that would only repeat a
  proven idle pass does not run the CPU.  It is not Tier 3: the kernel still
  wakes the node every microsecond, so the event queue, the outer loop and
  the other nodes see the same run, and the output is byte-identical by
  construction (`tools/check-idle-skip.sh`).  What it leaves is this plan's
  per-wakeup cost: a scratch build whose ARM slices did nothing ran the
  3-node CC2538 Thread setup's 75 M slices (25 s) in 1.34 s, about 18 ns a
  slice, against 2.2 s for the run with idle_skip.
- The ARM interpreter: covered by `arm-performance-plan.md`; nothing here
  changes its conclusions.
- Multithreading: the whole point of Phase 5 was retiring `--threads`; the
  gains above come from doing less work, not from parallelism.
- The radio medium filter: `radio_medium_filter_byte_radio` does not
  appear in any profile above 0.1%.
- The event queue's data structure, after PR #60. The heap is small and
  cache-resident, the remaining cost is per-call, and Tier 1 removes the
  calls. See Tier 2 item 1 for the measurement and the decision.

## 6. Refactoring plan

Ordered so each step is reviewable on its own and leaves the tree
byte-identical on `check-baseline.sh` unless stated.

### R1 — delete the synchronous chip-delivery subsystem (F1)

Remove from `sim_radio_bus.c`: `sim_radio_bus_deliver_bytes`,
`sim_radio_bus_queue_frame`, `sim_radio_bus_drain_rx`,
`sim_radio_bus_set_executing`, `emu_rx_queue`, `emu_rx_end_ns`, the
RXFIFO-full `step_until`, `rx_pre_sync`, the ACK flush loop and the
`frame_snap` statics; from the mote ops: `rx_byte_sync`, `rx_pre_sync`,
`rx_busy`, `rxfifo_available` (chip side), `RX_TICKING_STEP`,
`DRAIN_MINI_STEP`; from the chips: CC2420 `rx_incoming[]` replay, CC2538
ISTXON-time RXFIFO injection. Keep `BATCH` only as "stage bytes; hand the
whole frame to a `FRAME_CONSUMER` at frame-complete" and **assert at
registration** that a `BATCH` receiver has `receive_frame` and no
`rx_byte_sync`. Fix the header comment about nrf52840. Retire the
`SIM_RADIO_RX_DIRECT`/`QUEUED` outcomes and the "Emu RX frames … direct
/ queued / drained" stats line. Expected: `sim_radio_bus.c` loses ~350
lines and the runner's per-wakeup drain loop with them (this subsumes half
of Tier 0).

Gate: byte-identical on all nine baseline workloads (the code is dead for
them) **except** the end-of-run `Emu RX frames: … direct, … queued, …
drained, … dropped, … collided` line (`runner :3534`). R1 retires three of
its counters, the runner prints it on every run, and `check-baseline.sh`
does not filter it, so "byte-identical" cannot pass as written. Land the
line change as its own first commit (drop the retired counters, or move the
line behind `-v`) with the baseline diff shown to be that line only; then
the deletion is byte-identical against it. Plus `test_radio_bus` rewritten
to the surviving surface.

### R2 — JS mote as a frame consumer (F2)

Register JS with `SIM_RADIO_CAP_FRAME_CONSUMER`; deliver at frame end (the
bus's `accurate_tx_end`) rather than `now`, and make the same choice for
external motes explicit in `external-nodes-plan.md`. Add
`cross-level-demo.json` to the checked scenarios with a validator that the
JS node prints a received frame. Not byte-identical for JS scenarios (they
are currently broken); byte-identical elsewhere.

### R3 — chip ACK and PHY timing (F3, F4)

- CC2538: ~~stage the ACK, emit from a chip event at +192 µs with a proper
  TX state for its air time~~ done by PR #91 (F3; Sky senders still miss
  some, see there); ~~TX and RX calibration, the TX/RX transit, the SFD at
  its end~~ done (the TSCH unicast fix, Decisions Log); make `ISRFOFF`
  actually stop the parser (behind a compatibility flag if any test depends
  on it); the 12-symbol SFD-search pause after a received frame
  (`FSMCTRL.RX2RX_TIME_OFF`, SWRU319C §23.9.2) is not modelled.
- nRF52840: ~~96 → 192 µs~~ (gone with the stand-in ACK, #88); ~~ramp-up
  by `MODECNF0.RU`, the RX/TX turnaround, a 128 µs CCA, ADDRESS/FRAMESTART
  /END at the SFD's, the PHR's and the frame's end, derived from
  `cpu_freq_hz`~~ done (the nRF52840 timing fix, Decisions Log).  TX
  disable (tTXDIS, 21 µs in 802.15.4 mode, PS v1.1 §6.20.15.8) is still
  the 10-cycle `NRF_RADIO_DISABLE_CYCLES`, energy detection (EDSTART) still
  ends at once (the PS: 8 symbols), and the TX side raises ADDRESS and
  FRAMESTART at START, not after the SFD and the PHR have gone out.
- nRF54L15: PHYEND at real air time; retire the 100 µs constant. If
  `nrf54l15-ack-gap.md`'s ordering problem reappears, model ramp-up/down
  durations as that document proposes.
- CC1200 / bus: per-(node, radio) `tx_asm`, or a byte period declared at
  registration.
- ~~Native `SYNC`: wake at true frame end (F5)~~ done by PR #56.

Each moves the simulation and needs the `check-baseline.sh` "expected to
move" sign-off with the TSCH, RPL-UDP and TrustZone-RPL scenarios as the
functional gate (all must still associate/converge/round-trip).

### R4 — runner hygiene

- `now_ns` handling around the pump (F14): keep `now_ns` at the last
  dispatched event and pass the horizon as a parameter; audit the readers
  listed under F14.
- `msp_mote_serial_input` (F15): re-pin `last_execute_us` from kernel time.
- Remove `native_yield_callback` / `native_step_until_ns`.
- Fix `CSIM_PHASE_TIMING` labels (F16), or replace it with the event-kind
  counters from §4.1 (cheap, always-on, far more informative than a
  wall-clock split).
- Move per-process statics (F7) onto the owning structs.

### R5 — kernel API for Tier 1/3

Give `sim_runtime_run_until` a dispatch return value ("next wakeup for the
same mote, or none") so slice batching lives in the kernel rather than the
runner, and give the bus a `stamp_ns` argument so Tier 3 can hand it the
in-slice time. Both are additive.

## 7. Sequencing

State on 2026-10-07: Tier 0 is done — item 3 in PR #61, items 1, 2 and 4 in
PR #64 (5.78x on the grid, byte-identical) — and Tier 2
item 1 in PR #60. R1 is next; it came after Tier 0, not in parallel with
it, since both change the per-wakeup drain loop.

1. ~~Tier 0 (a day)~~ **DONE** — 5.78x on the grid, byte-identical.
2. R1 (two to three days) — deletes the hazard and most of the remaining
   per-wakeup runner work; rewrites `test_radio_bus`.
3. R2 (half a day) — fixes a broken supported scenario.
4. Tier 1 + R5 (two days) — F9's horizon change first, gated on its own,
   then the batch; the second-largest win.
5. Tier 2 (one to two days, item by item, each gated).
6. R3 + R4 (a week, each item gated separately; these move the simulation).
7. Tier 3 (a week plus validation) — only after 1–6, with its own gate and
   a written "expected to move" argument.

## 8. Reproducing the measurements

```sh
make
# phase split (mislabelled, see F16) and per-run summary
CSIM_PHASE_TIMING=1 ./build/test_runner test configs/chain-4node-sky.yaml -q
# profiles: run long, sample 4 s
./build/test_runner test configs/chain-4node-sky.yaml -t 1500000 -q & sample $! 4 1 -mayDie -file sky.txt
./build/test_runner test configs/udgm-100node-grid-sky.json -q & sample $! 5 1 -mayDie -file grid.txt
# event counts: add a per-kind counter in sim_runtime_run_until and a
# callback histogram (dladdr) in execute_events — the scratch patch used
# for §4.1 is ~30 lines and should become a permanent CSIM_EVENT_STATS=1
# (Tier 2 item 4 / R4).
# determinism gates for every change
tools/check-determinism.sh test configs/chain-4node-sky.yaml
tools/check-baseline.sh
```
