# Changelog

All notable changes to Cooja-NG (`csim`) are documented here. The format is
loosely based on [Keep a Changelog](https://keepachangelog.com/); this project
uses [Semantic Versioning](https://semver.org/) once it reaches 1.0 — until
then, 0.x minor releases may adjust the CLI, config, and plugin ABI.

## [Unreleased]

## [0.3.0] — 2026-10-11

Cooja-NG becomes something you can drive, embed and watch: a command shell
and script engine for testing firmware through its own console, the simulator
as a library, Renode as a co-simulation clock master, and recorded runs that
replay in the browser. The nRF54L15's TrustZone-M is enforced the way a Seeed
XIAO nRF54L15 enforces it; three CPU architectures share one RPL network; and
the kernel's wakeup path no longer scales with the node count. Release
binaries now produce byte-identical simulation output on linux-x86_64,
macos-x64 and macos-arm64, and the release workflow checks that they do.

Contributors to this release: @nfi (shell, nRF54L15, TrustZone-M enforcement)
and @nvt (hardening, FreeBSD, the JIT divide-trap fix, and review of most of
the rest).

### Changed — check these before upgrading
- **The web UI listens on loopback only.** `--ui PORT` binds 127.0.0.1;
  `--ui-bind ADDR` exposes it deliberately. Cross-origin WebSocket handshakes
  and DNS-rebinding `Host` headers are refused, and a slow or hostile client
  can no longer hold up the run (#53).
- **The MSP430 PC-trace counters are opt-in.** The end-of-run
  `FW cc2420_transmit=… eb_process=… queue_add=…` line appears only with
  `CSIM_PC_TRACE=1`; the per-instruction hook it needed is gone from every
  other run (#61).
- **Off-SoC chip drivers moved to `src/chips/`** (CC2420, CC1200,
  MX25R6435F, ENC28J60): they are not architecture-specific (#33).

### Added — interactive shell and command scripts (#43, #45, #47, #54)
- **`--shell`** gives a live simulation a command line (line editing,
  history, completion; plain lines from a pipe). **`--script FILE`** runs the
  same commands with blocking `expect` / `sleep` / `wait-until`, `at` / `every`
  / `on` queues and a pass/fail verdict, so one Contiki-NG shell firmware can
  serve many tests: the shell types into a node's console and asserts on what
  it prints. The exit code says what failed: 0 pass, 1 assertion, 2 invalid
  request, 6 wall timeout, 7 cancelled.
- Node commands (`cmd`: send a line, wait for the prompt, check the output),
  per-node console masks and log files, environment control, breakpoints and
  watchpoints, register and MSP430 memory access, GPIO, and fault injection —
  a SecureFault injected with `reg pc =` and caught with `expect-fault`.
- **`--wall-timeout DUR`** bounds a run in wall-clock time without touching
  the simulation. See [`docs/shell.md`](docs/shell.md).

### Added — web UI recording and replay, and a project site (#65, #66)
- **`--ui-record FILE.json`** writes the stream the web UI would receive as a
  replay file. Without `--ui` the run stays headless and unpaced, and stdout
  is byte-identical with and without the flag (`tools/check-ui-record.sh`, in
  CI). `ui/index.html?replay=FILE.json` plays it from a static file: pause,
  speed, a scrubber. See [`docs/ui-replay.md`](docs/ui-replay.md).
- **<https://mikroverk.github.io/cooja-ng/>**: recorded runs to watch in the
  browser, recorded by CI from the commit the site is built from, each
  published only if it shows what its card says.

### Added — the simulator as a library (#63)
- `make lib` builds `build/libcsim.a` without `test/`; `make lib-link-check`
  (in CI) links it with a `main` of its own, so a kernel, chip or service
  object that needs a symbol only the runner defines fails the build. Closing
  that gap fixed three leaks.

### Added — Renode co-simulation: csim as a clock slave (#36, #39, #41)
- **Renode can drive csim's simulation clock.** New
  `src/services/renode_cosim_service.c` speaks Renode's `CoSimulationPlugin`
  protocol — the 24-byte binary message over two TCP sockets, the handshake,
  the fixed-quantum `tickClock`, bus reads and writes, and the asynchronous
  `interrupt` and `logMessage` channel. Every `tickClock` becomes the runner's
  next horizon, so Renode's virtual time and csim's advance together. From
  Renode's side this needs **no Renode-side code**: one
  `CoSimulated.CoSimulatedPeripheral` line in a `.repl`
  (`examples/renode/csim.repl`). Everywhere else in csim, csim owns the clock;
  this is the one inversion. See
  [`docs/design/renode-cosim-plan.md`](docs/design/renode-cosim-plan.md).
- **csim's whole 802.15.4 network as a memory-mapped device.** New
  `src/native/renode_dev.c` is the register window behind that peripheral:
  transmit a frame into csim's medium, read received frames back with their
  sender, channel, per-receiver RSSI and **on-air start time**, bridge one
  csim node's console, and take a level interrupt when something arrives. The
  guest-side mirror of the map is `examples/renode/csim_dev.h`.
- **New mote kind `renode-cosim`**, selected by a `.renode` firmware
  extension the way `.py` selects an external node (the path is never opened).
  It has no CPU and no clock of its own: it is the device's place in csim's
  medium. Without a master attached it is inert, so a config containing one
  still runs as a plain regression.
- **New runner flags** `--renode ADDR:MAIN:ASYNC` and `--renode-freq HZ`
  (`--renode-freq` must match the peripheral's `frequency`; the `tickClock`
  message carries only a tick count). Also reachable as `plugins: ["renode"]`
  with the connection in `CSIM_RENODE`. `tools/csim-renode-launch.sh` maps
  Renode's positional spawn arguments onto the flag.
- **Tests**: `./build/test_runner renode-cosim` covers the wire codec against
  a hand-written byte vector, the register window and its FIFOs, and the real
  protocol loop driven by a scripted mock master over a `socketpair`.
  `tools/renode-mock-master.py` plus `tools/check-renode-cosim.sh` gate it end
  to end: exact clock coupling, frames crossing in both directions, console
  lines forwarded, determinism across identical runs, and a clean exit when
  the master dies mid-run.
- The runner's loop is touched in exactly two gated places, so with no
  `--renode` the default path stays byte-identical.

### Added — Renode as the RPL root, and three emulators on one network (#37)
- **Renode's own CC2538 as the RPL root of a csim network**
  (`configs/test-renode-root-rpl.yaml`): csim's Sky and nRF52840 clients join
  its DAG and complete UDP round trips through it, with
  `examples/renode/bridge/CsimBridge.cs` joining Renode's medium to csim's.
  With esp32sim as well, three emulators and three ISAs share one network
  (`configs/test-threeway-cosim.yaml`). Examples, not CI gates: they need
  Renode (and esp32sim).

### Added — nRF54L15 (#30, #31, #32, #34, #38, #42, #44, #46, #51, #58, #59)
- **TrustZone-M under the simulation kernel.** A node can carry a Secure-world
  ELF (`secure_firmware`) beside its Non-secure one; boot starts in the Secure
  world, and the end-of-run report adds SG / BXNS / secure-exception counters.
- **Attribution and peripheral permissions enforced as on silicon.** The SPU's
  per-peripheral permissions refuse Non-secure accesses with a precise
  BusFault taken by the Secure world, and SecureFault AUVIOL is precise too
  (the frame names the access, and a refused load or store changes nothing).
  The security unit's behaviour was measured on a Seeed XIAO nRF54L15
  ([`devices/nrf54l15-xiao/HARDWARE-COMPARISON.md`](devices/nrf54l15-xiao/HARDWARE-COMPARISON.md)).
  New configs: TrustZone boot, a watchdog the Secure world owns (verified line
  for line against a XIAO), a peripheral-permission violation, and two-node
  RPL-UDP over TrustZone with the radio driven through SG veneers.
- **Two-node RPL-UDP on the nRF54L15**: timer deadlines, interconnect channel
  groups, and the Nordic 802.15.4 driver's own hardware ACK path, which
  Contiki-NG's port now relies on (#34).
- Console receive through UARTE20 EasyDMA, paced at the baud rate (#31);
  SPIM with the DK's MX25R6435F flash and an ENC28J60 (#32); FICR.DEVICEID
  seeded so `node_id` equals the configured mote id (#30).

### Added — platforms, tests and tools
- **FreeBSD** (tested on 15.1, amd64): build with `gmake`; the scripts under
  `tools/` pick it themselves (#68).
- **Three CPU architectures and three radio models on one RPL DAG**
  (`configs/test-mixed-platform-rpl.yaml`): Tmote Sky, CC2538DK and
  nRF52840-DK, in CI (#35).
- `tools/check-baseline.sh` shows both binaries' wall time per workload (#55).
- **The release workflow** publishes a tag only if it is on main with passing
  tests and matches `CSIM_VERSION` and a CHANGELOG.md section. It runs real
  firmware on each unpacked binary with the JIT on and off, requires the three
  platforms' simulation output to be byte-identical, and attaches a
  build-provenance attestation to every tarball
  (`gh attestation verify FILE -R mikroverk/cooja-ng`). A manual run is a dry
  run (#70).

### Performance
- **Tier 0: no O(N) loops on the wakeup path** — 5.78x on 100 nodes,
  byte-identical output (#64). The measurements and the tiers after it are in
  [`docs/design/kernel-radio-review-and-performance-plan.md`](docs/design/kernel-radio-review-and-performance-plan.md) (#62).
- The event queue reschedules a wakeup in place and sifts with a hole,
  instead of a remove plus an insert of three-copy swaps — the operation an
  active mote does once per simulated microsecond (#60).
- MSP430 runs no longer pay for the PC-trace hook (#61, above).

### Fixed
- **Zolertia Firefly sub-GHz chain** (`configs/chain-4node-firefly-subghz.json`,
  now in CI): the radio bus re-armed a frame's byte clock on every preamble
  byte, so a CC1200 soft ACK arrived before its sender was back in RX; CSMA
  saw no ACKs, ETX passed RPL's limit and the network fell apart. Listed as a
  known limitation in 0.1.0 (#67).
- **The ARM JIT ran the 64-bit divide helpers the interpreter traps**, in a
  different number of cycles, so `CSIM_ARM_JIT=0` and `=1` diverged on
  nRF52840 TSCH ~14 s in. Blocks now stop before a trap address (#69).
- **Energest results changed whenever the web UI was watching**: the UI's
  activity flashes overwrote the radio state the energy stream reads (#65).
- The web UI's full state read each node's console ring from the wrong slot,
  so a browser that connected early saw blank lines; and the sidebar
  re-appended the first full state's lines on every redraw (#65).
- A native mote's CCA is derived from on-air time, so a frame never read (a
  collision, a radio turned off) no longer leaves the channel busy (#56).
- `tools/run-cooja-tests.sh` never reuses another directory's firmware build,
  and `--clean` keeps shipped firmware (#57).
- Builds with gcc 14 (`nanosleep` needed `<time.h>`) (#41).

### Hardening
- Bounds and defined-behaviour fixes in shared infrastructure (#48); a
  malformed config fails cleanly (#49); the ELF loader and the MSP430
  symbol-driven patches are bounded to the image (#50).
- A stalled or faulty peer — the Renode master, an external-node process, a
  GDB client — can no longer hang or livelock the run (#52).
- The web UI: see *Changed* above (#53).

### Known limitations
- Renode as an RPL *client* of a csim root does not yet complete RPL; Renode
  as the root does.
- No FreeBSD binary is published: build from source there. Native Cooja
  motes need two fixes on Contiki-NG's side to build on FreeBSD.
- From 0.1.0, still standing: SVC is a no-op (no SVCall exception), MSP430 CS
  HFXT returns the DCO frequency, the energest ARM CPU current is
  MSP430-class (indicative), and the serial socket's deliberate 40 ms host-link
  latency.

## [0.2.3] — 2026-09-05

These 0.2.x sections were written for 0.3.0 from each release's GitHub notes,
which list every PR.

### Added
- **YAML is the primary config format**, with JSON still accepted: one strict
  schema and one validator for both (unknown or duplicate keys, wrong types
  and YAML-1.1 booleans are errors), and `--save-config FILE` writes the live
  setup back as canonical YAML (#27).
- **External motes for emulators that keep their own clock**: the ESP32-C6
  (esp32sim) runs as an external node, and a peer's transmission goes on the
  air at its timestamp (#24, #25). Plan for external data-driven nodes, with a
  worked disturber example (#22, #26).

### Fixed
- Serial bytes injected into a native mote woke it at its own stale clock
  instead of the kernel's, so the kernel accepted a past event and simulated
  time ran backwards (629 rewinds in one ping test); the kernel now refuses
  wakeups in the past. This was the Linux `17-tun-rpl-br` traceroute failure
  (#29).

## [0.2.2] — 2026-09-03

Includes 0.2.1, which was versioned but not tagged.

### Added
- `tools/run-cooja-tests.sh --seed N --logdir DIR`, so Contiki-NG's
  `BASESEED`/`RUNCOUNT` loop runs faithfully, and a Contiki-NG integration
  guide (#20, #21).

### Changed
- **The suite fails loudly**: an unknown `.csc` feature, a failed firmware
  build, a test without assertions or a script without a verdict fails the
  run instead of passing green (#15). The suite's firmware target defaults to
  `cooja` (no silent cc2538dk fallback on a fresh checkout) (#18).

### Fixed
- MSP430: the slice cycle budget is enforced, `RPT` applies to single-operand
  instructions, and the CC2420 footer RSSI is right (#17).
- Native motes boot at their scheduled start and receive frames the way
  Cooja's ContikiRadio does (#19).
- Release tarballs carry `tools/test-border-router.sh`, without which six of
  the eight `17-tun-rpl-br` tests failed at host start (#14), and fetch GNU
  Lightning verified and mirrored (#13).

## [0.2.0] — 2026-08-21

The first release shipped as **prebuilt binaries** —
`cooja-ng-v0.2.0-{linux-x86_64,macos-x64,macos-arm64}.tar.gz` plus
`SHA256SUMS`, GNU Lightning statically linked — and the first to pass the
complete Contiki-NG 5.2 Cooja simulation suite, **93/93**, on Linux/x86-64 and
macOS/arm64 (#10, #11, #12).

### Added — ARM performance: interpreter and JIT (#9)
- The ARM interpreter is 34% faster on the runner workload, and a **GNU
  Lightning JIT for Cortex-M** compiles hot Thumb-16 blocks (on by default
  when Lightning is present): `zephyr-synchronization` 2.05 s → 0.58 s on
  Apple Silicon. It is cycle-exact by gate — `CSIM_ARM_JIT=0` and `=1`
  produce byte-identical output — and tested by two new suites, `arm-decode`
  and `arm-jit`, the second of which runs the generated machine code.
- `make pgo` trains on ARM workloads too.

### Added — radio media and fixes (#6, #7, @highlunder)
- A Gilbert-Elliott two-state burst-loss medium plugin (#6).
- cc2538 `SYS_CTRL` clock fixes for TSCH slot timing and the PM1/2 wedge, and
  nRF52840/nRF54L15 aborted-RX handling, so multi-hop RPL chains on nRF pass
  (#7).

### Added — ARMv8-M TrustZone-M on the nRF54L15 Cortex-M33 (#8)
- **The M33 runs secure/non-secure partitioned firmware**, not just non-secure.
  New `src/arm/arm_trustzone.c` implements the security-attribution engine —
  SAU regions plus SPU-as-IDAU, `arm_security_attr()`, the memory-mapped SAU
  registers at `0xE000EDD0` (Secure-only; RAZ/WI from Non-secure), and
  `arm_tz_blocks()` hot-path access enforcement feeding `SecureFault`/SFSR.
  Enabled per MCU by `has_trustzone` — nRF54L15 only, so every other platform
  is untouched.
- **Security state and transitions** in `arm_cpu.c`: banked SP/CONTROL, the
  transition instruction surface (`SG`, `BXNS`, `BLXNS` + `FNC_RETURN`,
  `TT`/`TTT`/`TTA`/`TTAT`), and secure exception entry/return with the
  integrity signature (a tampered signature is recorded as `SFSR.INVIS`). `SG`
  from outside an NSC region raises SecureFault (`INVEP`).
- **NVIC target-security banking** (`NVIC_ITNS`) in `arm_nvic.c`, wired into
  exception entry so the secure world can route individual IRQs to Non-secure.
- **Per-node world-transition instrumentation** — SG, BXNS and secure-exception
  counters per mote, which is the point of the work: TEE transition cost becomes
  measurable network-wide rather than per-image.
- **`tz-boot` harness** (`test_runner tz-boot <secure.elf> <normal.elf>`): loads
  a split image into one nRF54L15 node, runs the handoff, and reports the
  counters. csim boots the **real** Contiki-NG `trustzone/` split image — the
  secure world initializes TrustZone, configures SAU/IDAU and the non-secure
  environment, validates the NS image permissions and reset vector, routes IRQs,
  and jumps to the non-secure reset handler. Verified on two different split
  apps (`{secure,normal}-world` → 389 SG + 390 BXNS; `trustzone/rpl-udp` → 16 SG
  + 17 BXNS), both ending in Non-secure state with 0 secure exceptions, so the
  pass is app-independent. The split images live in the `contiki-ng-nrf54l15`
  checkout, not this tree, so `tz-boot` is a manual harness and not part of the
  default gate.
- **71 new TrustZone tests** in `arm-correctness`, which goes 153 → **224**:
  SAU/IDAU attribution (14), SAU MMIO (9), access enforcement (9), TT (3),
  SG/BXNS (11), SecureFault (6), `NVIC_ITNS` (6), transition counters (3),
  BLXNS/FNC_RETURN (10).
- No regression on the non-TZ path with `has_trustzone` live on the same SoC the
  release gates: `correctness` PASS, `radio-medium` 241/241, `cc1200-mock-host`
  73/73, and `configs/test-2node-nrf54l15-dk.json` PASSED at an identical
  60013 ms simulated.

Plan, scope boundaries and the deferred (spec-completeness) list:
[`docs/design/trustzone-m-plan.md`](docs/design/trustzone-m-plan.md).

## [0.1.0] — 2026-07-25

First public release. A fast, multi-architecture C re-implementation of the
parts of Cooja and MSPSim needed to run the upstream Contiki-NG test suite
headlessly, with a focus on simulation speed, deterministic timing, and
faithful peripheral behaviour.

The feature set is described under *Emulation* / *Simulation kernel* /
*Plugins* / *Tooling & tests* below; the `Hardening` sections record a full
pre-release subsystem audit (correctness, memory-safety, robustness) whose
fixes are included in this release. See
[`docs/design/release-0.1.1-hardening.md`](docs/design/release-0.1.1-hardening.md)
for that audit and its plan.

### Emulation
- **MSP430 / MSP430X** CPU (computed-goto interpreter + optional GNU Lightning
  JIT), MCU configs F149 / F1611 / F2617 / F5437 / CC430F5137 / FR5969, with
  GPIO, USART/eUSCI, Timer A/B, BCS/UCS/CS clocks, ELF loader, and the CC2420
  radio.
- **ARM Cortex-M3/M4/M33** interpreter (Thumb-2 + M4 DSP/VFP) with CC2538
  (on-chip RF Core), Zolertia Firefly (+ CC1200 sub-GHz), and Nordic nRF52840 /
  nRF54L15 (on-chip 802.15.4) platforms.
- **RISC-V (RV32EMC)** via the nRF54L15 FLPR coprocessor: the M33 loads the FLPR
  blob into shared SRAM and releases it through the VPR `CPURUN` register, after
  which the RV32EMC core runs **unmodified Contiki-NG** dual-core alongside the
  M33 over one address space. ISA `rv32emc_zicsr_zifencei` (base + M + C + CSR +
  `fence.i`); both cores idle in WFI. See
  [`docs/design/riscv-vpr-plan.md`](docs/design/riscv-vpr-plan.md).
- **Native Cooja motes** (`dlopen`) and **JS app motes** (QuickJS).
- **Multi-RTOS**: Contiki-NG is the primary, fully-validated target; csim also
  boots stock **Zephyr OS** (incl. 802.15.4 `echo_server`/`echo_client` over
  UDP) and **RIOT OS** (`gnrc_networking` forming a 2-node RPL DODAG) on the
  nRF52840 — experimental / best-effort. The nRF52840 model grew the fidelity
  these need: UARTE EasyDMA RX, the TEMP sensor, RADIO/TIMER/RTC behaviour, and
  Cortex-M4 ops (PLD/PLI hint, parallel UADD8/SADD8 + SEL with APSR.GE). See
  [`docs/zephyr.md`](docs/zephyr.md) and [`docs/riot.md`](docs/riot.md).

### Simulation kernel
- Single-threaded, event-driven kernel (`sim_runtime_t`): ns-precise clock,
  unified `(time, seq)` event queue, per-radio multi-channel medium, per-byte
  RF delivery, and a Cooja-compatible execution model.
- **Ports-and-adapters architecture**: CPUs, radio chips, propagation policies,
  observation features, and plugins are adapters behind small vtable ports; the
  kernel never branches on a concrete node type.
- Pluggable **radio medium** policy (UDGM / NONE / plugin), and **power-aware
  range** matching Cooja UDGM (range scales with the transmitter's output
  power; byte-identical for firmware that holds max PA).

### Plugins
- `dlopen` plugin ABI (`csim_plugin.h`), additive/version-gated:
  v1 register a service, v2 register a radio medium, v3 draw a live web-UI panel
  (`publish_panel`).
- Plugins may also be **compiled in** as built-in services and selected by
  config name (`"plugins": ["energest"]`), Cooja's built-in-plugin style.
- Bundled examples: `packet_sink` (service `.so`), `lossy_medium` (medium
  `.so`), and **energest** — a compiled-in energy estimator (per-mote radio
  duty cycle + CPU/LPM/TX/LISTEN energy, with a live UI panel).

### Tooling & tests
- JSON simulation configs (v1 + v2), a live WebSocket UI, PCAP capture,
  activity timeline, per-mote GDB stub, and a JS/JSON test engine.
- Passes the upstream Contiki-NG Cooja test suite via `tools/run-cooja-tests.sh`
  — **93 / 93**, 0 failed / 0 skipped: all 85 headless tests plus all 8
  TUN/border-router cases (those need `--with-tun` and root). Plus instruction,
  firmware, radio-medium/bus, chip-driver, and plugin unit suites.
- CI on Linux (gcc) and macOS (clang) gating the unit suites plus nRF52840
  networking (Contiki RPL-UDP on DK + Dongle, stock Zephyr 802.15.4 echo) and
  determinism / config-equivalence guards.

### Hardening — memory safety
- **WebSocket server**: validate the frame payload length before the buffer
  arithmetic. A 64-bit attacker-controlled length folded into a signed `int`
  truncated negative, passed the "have we buffered the whole frame?" guard, and
  drove a `memmove` out of bounds — a crash triggerable by any connected
  client. Oversized/unmasked frames are now rejected.
- **ELF loader**: bound-check segment routing without adding, so a crafted
  `p_paddr` near `UINT32_MAX` can't wrap past the region end and return a wild
  destination pointer (heap corruption on load). `elf_find_symbol` validates
  `sh_link` and caps the strtab allocation against a malformed-symtab DoS.
- **Native Cooja motes**: clamp every firmware-controlled / medium-supplied
  frame length to the 128-byte radio buffers (the direct RX fast path had no
  clamp), and guard the firmware-set log length.
- **Config loader**: guard an unchecked `ftell` (a directory path gave
  `malloc(0)` + `fread(SIZE_MAX)`).
- **Native dlopen**: use `mkstemp` for the per-node temp copy instead of a
  predictable `/tmp` name opened without `O_EXCL` (symlink / planted-library
  vector on shared hosts); release the handle and temp file on load failure.

### Hardening — correctness
- **MSP430 `DADD`**: real per-nibble BCD addition with the carry flag, replacing
  a plain binary add that produced wrong sums and never set carry (shared helper
  so the interpreter and decoded paths can't diverge).
- **MSP430 `RRCM`**: carry-out now comes from the last bit rotated out (was off
  by two vs the sibling rotate instructions).
- **MSP430 Timer Up/Down (MC=3)**: compare and overflow events scheduled on the
  real 0→CCR0→0 triangle instead of the continuous-mode wrap.
- **MSP430 JIT**: exclude `ADDC` from inlining (its carry-in left no register to
  compute the overflow flag — a warm-block-only divergence); self-modifying-code
  cache invalidation now frees every block whose actual byte span covers the
  write, not just a fixed 6-byte window.
- **ARM NVIC**: PendSV and SysTick no longer share a single pending slot (a
  SysTick firing before a pended PendSV was taken silently dropped the context
  switch); NVIC IPR word reads are clamped at the array end.
- **CC2538 GPTimer**: timeouts raise the NVIC interrupt and periodic mode
  reloads (was poll-only, so a WFI waiting on a GPTimer IRQ wedged).
- **RISC-V (nRF54L15 FLPR)**: WFI resumes on a pending enabled interrupt
  regardless of `mstatus.MIE` (spec behaviour; the old code could deadlock the
  coprocessor); machine-interrupt priority corrected to MEI > MSI > MTI.
- **nRF54L15**: per-node DPPI/timer/EGU binding storage — three file-scope
  tables were shared across SoC instances, so in a multi-node run one node's
  callback could be routed to another's radio/timer.
- **Radio medium / event queue**: clamp `node_count` to the array bound; a
  full-queue reschedule now replaces the node's wakeup instead of dropping it.

### Hardening — robustness
- Service dispatch re-entrancy guard enforced in release builds (was
  assert-only); pcap writer checks every write and stops on a short write
  instead of emitting a corrupt capture; energy-panel JSON never truncates
  mid-structure; firmware→board detection keys on the basename's extension;
  external-command service cleans up on `fork` failure. The firmware test
  harness no longer reports success for a firmware that never self-reports —
  a hung or instruction-starved run printed `WARN` and returned 0, making it
  indistinguishable from a pass in the suite's exit status.

### Hardening — nRF54L15 radio (T3)
- **Two-node nRF54L15 802.15.4 now routes end-to-end.** The radio's TX-completion
  event was scheduled off the lagging `sim_time_ns`, so PHYEND fired ~1 cycle
  after START instead of ~100 µs later; the whole TX collapsed into one cycle and
  the driver's DPPI TXEN/START fan-out emitted each frame twice — two SFDs on air,
  so a per-byte receiver mis-latched the second SFD as the PHR and every frame
  failed CRC. Scheduling `tx_end_event` in cycles fixes it. Regression test:
  `configs/test-2node-nrf54l15-dk.json`.

### Hardening — nRF multi-hop 802.15.4 (nRF54L15 + nRF52840)
- **3+ node RPL-UDP chains now route end-to-end on both nRF radios.** The bug was
  in the radio model, not 6LoWPAN forwarding: when a reception was aborted
  mid-frame (routine on a multi-hop router that hears two neighbours and gets
  collision-truncated frames), the abort paths fired only a non-interrupting
  `PHYEND`, so the `nrf_802154` driver's `psdu_being_received` flag — set on
  ADDRESS/FRAMESTART, cleared only by a CRCOK/CRCERROR with its RX IRQ — stayed
  set forever. Every later `nrf_802154_transmit_raw` then returned
  `BUSY_CHANNEL` (`psdu_being_received_now`) and the router could never
  TX/ACK/forward again. Single-hop never hit this (no collisions → no aborts).
  - **nRF54L15** (`nrf54l15_soc.c`): the RX-stall watchdog now fires
    `END+PHYEND+CRCERROR` instead of bare `PHYEND`.
  - **nRF52840** (`nrf52840_soc.c`, `arm_elf_mote.c`): a `radio_abort_inflight_rx`
    helper fires the terminal CRCERROR on an invalid-PHR-after-SFD, on a
    STOP/DISABLE that interrupts a frame, and via a newly-wired `rx_stall` op.
    Additionally, the fabricated auto-ACK now checks the frame's extended
    destination address against this node's `FICR.DEVICEADDR0` — previously every
    neighbour ACKed every unicast, so two ACKs collided at the sender and it
    retransmitted until it gave up.
  - Regression tests: `configs/chain-3node-nrf54l15-dk.json`,
    `configs/chain-3node-nrf52840-dk.json`,
    `configs/chain-4node-nrf52840-dk.json` (+ `-dongle`; node 4 relays 3 hops).
    No regressions to nRF52840 2-node RPL, TSCH, Zephyr echo, nRF54L15 2-node,
    FLPR dual-core, or the cc2538/sky controls.

### Hardening — serial socket (native border router)
- **`17-tun-rpl-br/09-native-border-router-cooja-frag` now passes; the Cooja
  suite is 93/93.** A 1200-byte ping through `border-router.native` used to get
  5 transmitted / 0 received, with the router dying on `slip_send overflow`.
  The cause was a race against a 31 ms window that csim lost by being too fast.
  SLIP has no flow control, so Contiki substitutes a fixed
  `SLIP_DEV_CONF_SEND_DELAY` of `CLOCK_SECOND/32` = 31 ms and drains one SLIP
  packet per `slip_flushbuf()`. That timer is a passive `struct timer` posting
  no event, and the native platform gives `select()` a flat 1 s
  `SELECT_TIMEOUT` when idle — so inside the window nothing is scheduled to
  wake the router, and its only early wake-up is one of our bytes. A host-side
  1200-byte ping fragments into 13 SLIP packets queued in one unpaced burst; a
  reply landing inside the window consumed that wake-up while flushing was
  still forbidden, so the loop slept a full second per fragment and the
  2048-byte queue overflowed fatally. Measured flush→reply latency: csim
  0.2–26.5 ms (0 of 13 above the threshold) vs Cooja 0.0–47.3 ms (5 of 14
  above) — Cooja is not correct here, only lucky, and stalls a second whenever
  it loses. Protocol traffic is identical in both (one 7–8 byte `!R`
  confirmation per fragment), so this is not a throughput or framing
  difference. Fixed by modelling the USB-CDC host link a real slip-radio sits
  behind (default 40 ms wall-clock, `CSIM_SERIAL_TX_LATENCY_MS`, `0` disables)
  — the latency that makes Contiki's 31 ms constant work on hardware, and
  which neither simulator modelled. Result: 5/5 replies, 0% loss, versus
  Cooja's 3/5–5/5. Removal criteria are documented in `sim_serial_bridge.c`.

### Known limitations
See [README "Known issues"](README.md#known-issues). Notably: a default
circular topology with many nodes (`-n 16`) does not converge (one collision
domain, not a regression); the Firefly sub-GHz chain has an ACK-turnaround
residual after sustained traffic; native host-process scheduling is a
documented deferral; the energest ARM CPU current is MSP430-class (indicative).
**SVC is a no-op** (no SVCall exception) and **MSP430 CS HFXT** returns the DCO
frequency — unmodeled; only affects firmware that uses them. The serial socket
carries a deliberate 40 ms wall-clock host-link latency (see below) that a
future flow-controlled link should remove.

[Unreleased]: https://github.com/mikroverk/cooja-ng/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/mikroverk/cooja-ng/releases/tag/v0.3.0
[0.2.3]: https://github.com/mikroverk/cooja-ng/releases/tag/v0.2.3
[0.2.2]: https://github.com/mikroverk/cooja-ng/releases/tag/v0.2.2
[0.2.0]: https://github.com/mikroverk/cooja-ng/releases/tag/v0.2.0
[0.1.0]: https://github.com/mikroverk/cooja-ng/releases/tag/v0.1.0
