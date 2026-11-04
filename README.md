<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/cooja-ng-logo-dark.svg">
    <img src="docs/assets/cooja-ng-logo.svg" alt="Cooja-NG" width="420">
  </picture>
</p>
<p align="center">
  <a href="https://github.com/mikroverk/cooja-ng/releases/latest"><img src="https://img.shields.io/github/v/release/mikroverk/cooja-ng" alt="Latest release"></a>
  <a href="https://github.com/mikroverk/cooja-ng/actions/workflows/test.yml"><img src="https://github.com/mikroverk/cooja-ng/actions/workflows/test.yml/badge.svg?branch=main" alt="tests"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/mikroverk/cooja-ng" alt="License: BSD-3-Clause"></a>
  <a href="https://github.com/mikroverk/cooja-ng/stargazers"><img src="https://img.shields.io/github/stars/mikroverk/cooja-ng?style=social" alt="GitHub stars"></a>
</p>

# Cooja-NG — cross-level emulation and simulation for low-power wireless

Cooja-NG is a low-power wireless emulator and network simulator.  It runs unmodified Contiki-NG, Zephyr, RIOT and OpenThread firmware on emulated boards — Nordic nRF52840 and nRF54L15, TI CC2538 and CC13x2, the MSP430 motes — from the CPU and peripherals to the radio chip, and the boards share one simulated IEEE 802.15.4 radio medium, 2.4 GHz and sub-GHz.  It is deterministic, and it runs on your laptop, in CI, or in the browser.

### ▶ [Try it in your browser](https://mikroverk.github.io/cooja-ng/)

No install and no server: the simulator itself, compiled to WebAssembly ([`wasm/`](wasm/README.md)), runs the original firmware in the tab — a Thread network you can re-draw while it runs, Contiki-NG talking to Zephyr and to RIOT, three CPU architectures on one RPL network, TrustZone-M, TSCH, a 100-node grid.  Each demo is also a recorded run, replayed by the simulator's own web UI.

## At a glance

| | |
|---|---|
| **CPUs** | MSP430 and MSP430X; ARM Cortex-M3, M4F and M33 (with TrustZone-M); RISC-V RV32EMC (the nRF54L15's FLPR coprocessor).  Interpreters, with JITs for MSP430 and Thumb code |
| **Boards** | Tmote Sky, Zolertia Z1, MSP-EXP430FR5969; TI CC2538DK, Zolertia Firefly; Nordic nRF52840 DK and Dongle, nRF54L15 DK, Seeed XIAO nRF54L15; TI CC1352R1 and CC1312R7 LaunchPads ([table](#supported-platforms)) |
| **Firmware** | [Contiki-NG](docs/contiki-ng-testing.md) on every radio board (the CC1312R7's from a Contiki-NG pull request), and its Cooja test suite passes, 93 / 93; [Zephyr](docs/zephyr.md), [RIOT](docs/riot.md) and [OpenThread](docs/openthread.md) images, stock, on the boards their guides list (experimental) |
| **Radios** | CC2420, the CC2538 RF core, CC1200 sub-GHz, the Nordic 802.15.4 radio, the TI CC13x2 RF core (sub-GHz and 2.4 GHz); a frame reaches the receiving radio byte by byte as it is on the air |
| **The air** | One medium for every node, by band and channel: a unit-disk model with interference range, success ratios and TX power, plus plugin media (random and bursty loss).  BLE and LoRa are not modelled |
| **Networks** | Up to 128 nodes, mixed architectures; native Cooja motes, JavaScript motes, external emulators and Renode as nodes too |
| **Determinism** | One event queue, one nanosecond clock: a seed gives a byte-identical run, with the JIT on or off, on Linux, macOS and FreeBSD; the browser build is checked against the native one |
| **Tools** | `cngsim` (headless, exit codes for CI); a web UI that edits the running network; network overlays drawn from firmware memory, as YAML you edit live; packets dissected in the browser; pcap; recording and replay; a shell and command scripts; GDB on ARM nodes; energy estimation; plugins |

## Quick start

```sh
git clone https://github.com/mikroverk/cooja-ng.git && cd cooja-ng
make                                   # build/cngsim and build/test_runner
./build/cngsim examples                # bundled, commented simulations
./build/cngsim run rpl-udp-sky         # headless: node consoles, checks, verdict
./build/cngsim run rpl-udp-sky --ui    # watch it in the browser
./build/cngsim new rpl-udp --board nrf52840-dk -n 6 -o my-net.yaml   # your own setup
```

**[docs/quick-start.md](docs/quick-start.md)** is the guide: installing
(release tarballs, or `make install PREFIX=~/.local`), the web UI (add,
move, remove and reboot nodes, save the setup), your own Contiki-NG firmware,
checks, and a table for Cooja users.

`cngsim` is the command for running simulations.  `test_runner` is the
developer and CI binary: the emulator's unit suites, the Contiki-NG Cooja
suite, and the determinism gates, which diff its full output.  Both run the
same simulation.

## Where it comes from

**The cross-level simulator Cooja pioneered, generalised: more CPUs, more firmware stacks, more radios.**

[Cooja](https://github.com/contiki-ng/cooja) introduced *cross-level* simulation: one simulation at the network, operating-system and machine-code instruction-set levels at once (F. Österlind, A. Dunkels, J. Eriksson, N. Finne and T. Voigt, "Cross-Level Sensor Network Simulation with COOJA", IEEE LCN 2006, [doi:10.1109/LCN.2006.322172](https://doi.org/10.1109/LCN.2006.322172)).  Cooja-NG (codename `csim`) began as a clean-room re-implementation in C of Cooja and MSPSim, the network simulator and MSP430 emulator that [Contiki-NG](https://github.com/contiki-ng/contiki-ng) is tested with, and keeps that model: emulated MSP430, ARM and RISC-V devices run next to native Cooja motes, JavaScript motes, Renode and external nodes, on one simulated radio medium.  It still runs Contiki-NG's Cooja test suite headless — **93 / 93**, including all 8 TUN/border-router cases (those need `--with-tun` and root; [docs/contiki-ng-testing.md](docs/contiki-ng-testing.md)):

| | Cooja + MSPSim | Cooja-NG |
|---|---|---|
| CPUs | MSP430, through MSPSim; native Cooja motes; Java application motes | MSP430; ARM Cortex-M3/M4F/M33 with TrustZone-M; RISC-V (the nRF54L15's coprocessor); native Cooja motes; JavaScript motes |
| Firmware | Contiki-NG | Contiki-NG, Zephyr, RIOT and OpenThread, unmodified |
| Radios | the radios of its MSP430 motes | the CC2420, plus the CC2538, nRF52840, nRF54L15 and CC13x2 radios and the CC1200 sub-GHz |
| Where it runs | a Java application on the JVM, with a Swing GUI or headless | one C binary, headless; a web UI; a browser tab (WebAssembly) |
| Determinism | seeded runs | byte-identical for a seed, with the JIT on or off, across Linux, macOS and FreeBSD |
| Contiki-NG's Cooja tests | the reference | 93 / 93, roughly an order of magnitude faster than Cooja + MSPSim |

It is not a full Cooja replacement: there is no desktop GUI, no Java plugin ecosystem, no closed-source motes, and of Cooja's radio media only UDGM (the unit-disk model) is implemented.

## Building

| Target | Notes |
|---|---|
| `make` | Release build (`-O3 -flto`, tuned for the host CPU); GNU Lightning (the JITs) auto-detected via pkg-config |
| `make debug` | `-O0 -g -DDEBUG`, no LTO |
| `make WERROR=0` | Warnings are errors by default; this drops `-Werror` for a compiler that reports something new |
| `make pgo` | Profile-guided build — ~1.55× on clang, ~1.18× on gcc |
| `make install PREFIX=~/.local` | `cngsim` in `PREFIX/bin`, its examples, firmware, tools and guides in `PREFIX/share/cooja-ng` (`DESTDIR` honoured; `make uninstall`) |
| `make clean` | Remove `build/` |

A C compiler is all the build needs: GNU Lightning is optional, and QuickJS, cJSON, libyaml, CBOR and linenoise are bundled under `lib/`.  `cngsim convert` and the Cooja suite also need `python3`, and the TUN tests `iproute2` and `tunslip6`.  Pre-built firmware ships under `firmware/`, so Contiki-NG is needed only to build your own (`make configure CONTIKI_DIR=/abs/path` remembers where it is).

**FreeBSD** (tested on 15.1, amd64): build with `gmake` (`pkg install gmake`, plus `lightning` for the JIT) wherever this README says `make`; the scripts under `tools/` pick it themselves.  Simulation output is byte-identical to Linux.  Native Cooja motes (`.cooja`) don't build there yet, because Contiki-NG's Cooja platform doesn't (`pthread_getattr_np`, and `putchar` as a macro).

## Supported platforms

| Extension | Platform |
|---|---|
| `.sky` | MSP430F1611 (Tmote Sky) + CC2420 |
| `.z1` | MSP430F2617 (Zolertia Z1) + CC2420 |
| `.msp430fr5969` | MSP-EXP430FR5969 LaunchPad (FRAM, no radio) |
| `.cc2538dk` | ARM Cortex-M3 — TI CC2538 + on-chip 802.15.4 |
| `.zoul-firefly` | ARM Cortex-M3 — Zolertia Firefly (CC2538 + CC1200 sub-GHz) |
| `.nrf52840-dongle` | ARM Cortex-M4F — Nordic PCA10059 USB Dongle |
| `.nrf52840-dk`, `.nrf52840` | ARM Cortex-M4F — Nordic PCA10056 DK |
| `.nrf54l15-dk` | ARM Cortex-M33 — Nordic PCA10156 DK, with the RISC-V FLPR coprocessor and TrustZone-M |
| `.nrf54l15-xiao` | ARM Cortex-M33 — Seeed Studio XIAO nRF54L15 |
| `.cc1352r1-launchpad` | ARM Cortex-M4F — TI LAUNCHXL-CC1352R1 (RF core: sub-GHz prop and 2.4 GHz IEEE) |
| `.cc1312r7-launchpad` | ARM Cortex-M4F — TI LP-CC1312R7 (RF core: sub-GHz prop) |
| `.cooja` | Native Cooja shared library |

An image with any other name (`.nrf`, `.elf` ...) says its board with `board:` in the setup, or `--board` on the command line; `cngsim boards` lists them all.  Per-board status, specs and hardware comparisons are in [`devices/`](devices/).

## Features

- **MSP430** — the full MSP430 + MSP430X instruction set, cycle-accurate, with an optional JIT; CC2420 with ns byte timing and auto-ACK; Timer A/B, USART/USCI/eUSCI, GPIO, BCS/CS clocks, hardware multiplier.
- **ARM Cortex-M3/M4F/M33** — Thumb-2, FPv4 VFP, NVIC, SysTick, an optional cycle-exact JIT; SoC models of the TI CC2538 (and the off-SoC CC1200), Nordic nRF52840 and nRF54L15, and TI CC13x2 (the RF core as a command-level model).
- **nRF54L15 TrustZone-M** — Secure/Non-secure split images, SAU + SPU attribution, `SG`/`BXNS`/`BLXNS`, SecureFault, and per-node world-transition counters, booting the real Contiki-NG `trustzone/` images ([plan](docs/design/trustzone-m-plan.md)).
- **RISC-V** — the nRF54L15's FLPR, an RV32EMC coprocessor running unmodified Contiki-NG next to the M33 over shared SRAM ([plan](docs/design/riscv-vpr-plan.md)).
- **Zephyr and RIOT** images boot and network too, on the nRF52840 and, for Zephyr's echo pair and RIOT's hello-world, the CC1352R1 LaunchPad (experimental; Contiki-NG is the validated target): [zephyr.md](docs/zephyr.md), [riot.md](docs/riot.md), [CC1352R1 status](devices/cc1352r1-launchpad/STATUS.md).
- **OpenThread** — OpenThread's own CC2538 and nRF52840 ports form a Thread network (leader, routers, children, secured frames) on the bundled CLI images (experimental): [openthread.md](docs/openthread.md).
- **Other node kinds** — native Cooja motes (`.cooja`, mixable with emulated ones), JavaScript app motes, external processes, and Renode co-simulation ([plan](docs/design/renode-cosim-plan.md)).
- **One event-driven kernel** — single-threaded, every node on one ns clock; emulated radios receive a frame byte by byte as it is on the air (what TSCH and tight CSMA timing need), and 2.4 GHz and sub-GHz share one medium by `(band, channel)`.  The same seed gives a byte-identical run.
- **Tests** — YAML/JSON setups with steps, validators, `fail_on` and timed actions ([test-format.md](docs/test-format.md)); Cooja JavaScript test scripts run unchanged; an interactive shell and command scripts ([shell.md](docs/shell.md)).
- **Network overlays** — the web UI's canvas draws the network's own state, read out of each node's memory: RPL's DODAG, or OpenThread's roles, partitions and links in OTNS's style.  An overlay can be a YAML descriptor instead of C, edited in the running page ([overlay-descriptors.md](docs/design/overlay-descriptors.md)).
- **Observation** — a web UI with no build step (network, consoles, packets dissected by [dpkt-js](https://mikroverk.github.io/dpkt-js/), timeline, add/move/remove nodes), recordings replayed in the browser ([ui-replay.md](docs/ui-replay.md)), pcap capture, energy estimation, and plugins (`.so` services, radio media, UI panels).

## Running tests

```sh
./build/test_runner correctness         # MSP430 instructions
./build/test_runner arm-correctness     # Thumb-2, M4 DSP + VFP, ARMv8-M TrustZone
./build/test_runner radio-medium        # multi-channel routing
./build/test_runner all                 # every unit suite
./build/test_runner test configs/test-mixed-platform-rpl.yaml   # Sky + CC2538 + nRF52840, one DAG

# The Contiki-NG Cooja test suite: the 85 tests that need no root (~10–15 min warm)
git clone https://github.com/contiki-ng/contiki-ng.git ../contiki-ng
make configure CONTIKI_DIR=$(pwd)/../contiki-ng
make cooja-tests VERBOSE=1
```

The suite has 93 tests: the 85 above, and 8 TUN/border-router tests that run with `tools/run-cooja-tests.sh --with-tun` as root.

[docs/contiki-ng-testing.md](docs/contiki-ng-testing.md) covers the Cooja suite: seeds, logs, the TUN tests, running it from inside a Contiki-NG tree with `SIMULATOR=cooja-ng`, and CI.  It fails loudly: an unknown `.csc` feature, a failed firmware build, or a script that ends without a verdict fails the run, never a silent skip.  The full list of `test_runner` suites and determinism gates is in [`CLAUDE.md`](CLAUDE.md).

## Performance

Measured on Apple Silicon (PGO build) and Linux x86-64 (release); indicative.

| Scenario | Speed |
|---|---|
| MSP430 micro-benchmarks (JIT) | ~430 MIPS |
| MSP430 2-node RPL-UDP, 60 s simulated | ~1600× real time (Apple Silicon PGO), ~250× (x86-64) |
| CC2538DK 2-node RPL-UDP | ~300× real time |
| nRF52840 2-node RPL-UDP | ~360× real time |
| nRF54L15 M33 + RISC-V FLPR, both idle in `WFI` | ~714× real time |

Where the time goes per ISA and node count: [kernel and radio performance plan](docs/design/kernel-radio-review-and-performance-plan.md).

## Architecture

```
                          ┌──────────────────────────────────────────────┐
                          │       Cooja-NG (cngsim / test_runner)        │
                          │                                              │
firmware/*.sky / .z1 ────►│  MSP430 F1611/F2617/FR5969 + CC2420          │
firmware/*.cc2538dk    ──►│  ARM Cortex-M3 + CC2538 RF Core (on-chip)    │──► consoles, packets,
firmware/*.zoul-firefly──►│  ARM Cortex-M3 + CC2538 + CC1200 sub-GHz     │    pcap, timeline,
firmware/*.nrf52840-dk ──►│  ARM Cortex-M4F + Nordic 802.15.4 radio      │    web UI, overlays,
firmware/*.nrf54l15-dk ──►│  ARM Cortex-M33 + Nordic 802.15.4 + DPPI/GRTC│    recordings,
   └─ + RV32E FLPR  ─────►│  RISC-V coprocessor, dual-core, shared SRAM  │    COOJA.testlog
firmware/*.cc1352r1-…  ──►│  ARM Cortex-M4F + TI RF core (sub-GHz, 2.4)  │
firmware/*.cooja       ──►│  Native Cooja motes (dlopen)                 │
                          │                                              │
                          │  one event queue • one medium, by band and   │
                          │  channel • ns-precise time • JS assertions   │
                          └──────────────────────────────────────────────┘
```

A small simulation kernel (clock, one event queue, the radio bus, mote slots) talks only to narrow vtable ports: a mote, a service, a radio medium, the plugin ABI.  Every CPU, SoC, feature and plugin is an adapter behind one of them, so the kernel never branches on a node's type.  [docs/architecture.md](docs/architecture.md) has the diagrams, the ports, the time model and the environment variables; [docs/design/refactor-plan.md](docs/design/refactor-plan.md) has the design direction and its Decisions Log.

## Known issues

The known limitations of this release are listed under its version in [`CHANGELOG.md`](CHANGELOG.md).  The serial socket delays mote→host bytes by 40 ms of wall-clock time on purpose, for Contiki-NG's native border router: [contiki-ng-testing.md](docs/contiki-ng-testing.md#8-troubleshooting) explains why.

Release history is in [`CHANGELOG.md`](CHANGELOG.md).

## Citing

If you use Cooja-NG in your work, please cite it. [`CITATION.cff`](CITATION.cff) has the details, and GitHub's **Cite this repository** button (in the sidebar) gives them as BibTeX or APA:

```bibtex
@software{Eriksson_Cooja-NG_2026,
  author  = {Eriksson, Joakim and Tsiftes, Nicolas and Finne, Niclas and H{\"o}glund, Joel},
  license = {BSD-3-Clause},
  month   = oct,
  title   = {{Cooja-NG}},
  url     = {https://github.com/mikroverk/cooja-ng},
  version = {0.4.0},
  year    = {2026}
}
```

## License

3-clause BSD.  See [LICENSE](LICENSE).  Copyright © 2026 Joakim Eriksson, RISE Research Institutes of Sweden.

Bundled `lib/quickjs/` is © Fabrice Bellard and Charlie Gordon, MIT license.

The web UI inlines [dpkt-js](https://github.com/mikroverk/dpkt-js) (© 2026 Joakim Eriksson, © 2004 Dug Song), 3-clause BSD: [ui/dpkt-js.LICENSE](ui/dpkt-js.LICENSE), also in the page and installed as `docs/LICENSE.dpkt-js`.
