# Running Zephyr OS on Cooja-NG

Cooja-NG runs the ELF `west build -b nrf52840dk/nrf52840` writes, on an
emulated nRF52840: the Cortex-M4F, the RADIO, TIMER, RTC, PPI and UARTE.
`nrf_802154` drives the radio as it would on silicon, ACKs included. Unlike
native_sim and nrf52_bsim, nothing is rebuilt for the host: the image is the
one you would flash. The 802.15.4 medium delivers each frame byte by byte, as
it is on the air, and Zephyr nodes share it with Contiki-NG, RIOT and
OpenThread nodes. Renode can also drive Cooja-NG's clock and join its network
(`examples/renode/`).

```
*** Booting Zephyr OS build v4.4.0 ... ***
Hello World! nrf52840dk/nrf52840
```

| You use | In Cooja-NG |
|---|---|
| `west build -b nrf52840dk/nrf52840` | the same build: `cngsim run build/zephyr/zephyr.elf` (the board from `build/zephyr/.config`), or `--board nrf52840dk/nrf52840` |
| native_sim, or nrf52_bsim and BabbleSim | the board's image on an emulated CPU and radio; 802.15.4 only, no BLE |
| `west flash` and a serial terminal | the node's console in the web UI (`--ui`), or `console <node>` in `--shell` |
| a Renode `.resc` | a YAML setup file, or Renode as Cooja-NG's clock master |
| twister | not yet; `cngsim run` exits 0 when the checks pass and 1 when one fails, for CI |

> **Experimental.** Contiki-NG is Cooja-NG's validated target (it passes the
> upstream Cooja test suite). Zephyr is newer: the samples below work end to
> end on the nRF52840 DK, the echo pair also on the CC1352R1 LaunchPad
> (`cc1352r1_launchxl`), and hello_world on the CC1312R7; the gaps are under
> *Limitations*.

## Quick start

No Zephyr toolchain needed: stock samples are bundled in
`firmware/nrf52840-dk/` (`PROVENANCE.md` there says how they were built),
and two examples run them.

```sh
# 802.15.4: the stock echo_server and echo_client (UDP over 6LoWPAN/IPv6),
# one node each; passes when the server has echoed twenty datagrams
cngsim run zephyr-echo

# Zephyr's echo_server answering a Contiki-NG node
# (passes when all seven datagram sizes come back)
cngsim run contiki-zephyr

# The same in the browser (click a node for its console)
cngsim run contiki-zephyr --ui
```

Example names work from a checkout (`./build/cngsim`) and from an install
alike; `cngsim examples` lists them all. The console lines name the board
and the OS:

```
    0.952 [Node 1/nrf52840-dk Zephyr] [00:00:00.314,941] <inf> net_echo_server_sample: Waiting for UDP packets on port 4242 (IPv6)...
```

## Run the bundled samples

These name the images by path: run them from a checkout or from an
install's `share/cooja-ng`.

```sh
# One node: the hello_world banner
cngsim run firmware/nrf52840-dk/zephyr-hello-world.nrf52840-dk -t 2s

# Two threads and timers (samples/synchronization): they alternate every ~565 ms
cngsim run firmware/nrf52840-dk/zephyr-synchronization.nrf52840-dk -t 3s
```

The echo pair is built for the CC1352R1 LaunchPad too (2.4 GHz IEEE mode);
`cngsim new zephyr-echo` writes a setup file for either board, and takes
Zephyr's board names:

```sh
cngsim new zephyr-echo --board cc1352r1_launchxl -o echo-cc1352.yaml
cngsim run echo-cc1352.yaml
```

The synchronization sample exercises the system timer (`k_msleep`) and thread
context switching (PendSV, MSP and PSP banking). The echo samples
(`samples/net/sockets/echo_*` with only the sample's own
`overlay-802154.conf`) exercise the on-chip 802.15.4 radio and the full
6LoWPAN/IPv6 stack end to end.

## Your own build

`west build` leaves the board in `build/zephyr/.config`, and `cngsim run`
reads it there, so a build runs with no `--board`:

```sh
cngsim run build/zephyr/zephyr.elf -t 2s
# cngsim: note: board nrf52840dk/nrf52840, from build/zephyr/.config (CONFIG_BOARD_TARGET): runs as nrf52840-dk
```

Two builds on the command line are two nodes
(`cngsim run echo_server/build/zephyr/zephyr.elf
echo_client/build/zephyr/zephyr.elf -t 40s`). An ELF copied out of its build
directory needs `--board`, which takes the name you gave `west build -b`
(`nrf52840dk/nrf52840`, `cc1352r1_launchxl`, or the pre-3.7
`nrf52840dk_nrf52840`); `cngsim boards --os` lists them for every board.

## Your own builds in a setup file

For a network with checks that make it a test, name your builds in a setup
file. This one passes when the server has echoed twenty datagrams, and fails
at once if either node cannot bring its network up:

```yaml
# echo.yaml: Zephyr's echo_server and echo_client on two nRF52840 DKs
version: 2
title: "Zephyr echo over 802.15.4"
timeout_ms: 40000
mote_types:
  - { name: server, board: nrf52840dk/nrf52840, firmware: echo_server/build/zephyr/zephyr.elf }
  - { name: client, board: nrf52840dk/nrf52840, firmware: echo_client/build/zephyr/zephyr.elf }
nodes:
  - { type: server, id: 1, x: 0, y: 0 }
  - { type: client, id: 2, x: 20, y: 0 }
test:
  steps:
    - { wait: "Received and replied", node: 1, count: 20 }
  fail_on: ["Network init failed"]
```

```sh
cngsim check echo.yaml       # validate it without running
cngsim run echo.yaml         # consoles, the check, PASSED or FAILED (exit 0 or 1)
cngsim run echo.yaml --ui    # watch it
```

Firmware paths are relative to the setup file, and a setup file names the
board with `board:` (it does not read `.config`). The setup format is in
[test-format.md](test-format.md); scripted console sessions (`sendln`,
`expect`, `capture`) for shell samples are in [shell.md](shell.md).

## How it was built

No `sudo` and no Zephyr SDK are needed — a Python venv plus the system
`arm-none-eabi-gcc` (`gnuarmemb` toolchain variant) is enough:

```sh
python3 -m venv --without-pip ~/zephyr-venv          # PEP 668: build pip inside a venv
~/zephyr-venv/bin/python get-pip.py                  # from bootstrap.pypa.io
~/zephyr-venv/bin/pip install west cmake ninja pyelftools
~/zephyr-venv/bin/west init ~/zephyrproject && cd ~/zephyrproject && west update
~/zephyr-venv/bin/pip install -r zephyr/scripts/requirements-base.txt

export PATH=~/zephyr-venv/bin:$PATH
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb GNUARMEMB_TOOLCHAIN_PATH=/usr
west build -b nrf52840dk/nrf52840 zephyr/samples/hello_world
```

No board overlay or config changes are needed — the stock image (default
**UARTE** EasyDMA console) runs as-is. Then run the resulting
`build/zephyr/zephyr.elf` as it is (the board comes from the build's
`.config`), or name it `board: nrf52840dk/nrf52840` in a setup file (an
image named `<app>.nrf52840-dk` needs neither).

## What the emulator provides

Booting Zephyr needed three additions to `src/arm/nrf52840_soc.c` beyond what
Contiki used (see commit history):

- **CLOCK** `HFCLKSTAT`/`LFCLKSTAT`/`LFCLKSRC` — Zephyr's `lfclk_spinwait()`
  polls `LFCLKSTAT.STATE`; csim previously modelled only the `*_STARTED`
  events, so the spin never exited (the actual boot blocker).
- **RTC1** (`0x40011000`, IRQ 17) — Zephyr's `nrf_rtc_timer` system clock
  (Contiki uses RTC0).
- **UARTE EasyDMA TX** (`TASKS_STARTTX` + `TXD.PTR`/`TXD.MAXCNT` →
  `EVENTS_ENDTX`/`TXSTOPPED`) — the default Zephyr console. csim's transfer is
  instantaneous, so it latches `TXSTOPPED` immediately (what the driver's
  `ENDTX→STOPTX` link would produce; the UARTE takes that shortcut itself,
  while the radio's PPI links, used by `nrf_802154`, are modelled). The legacy
  non-DMA UART (Contiki's path, and Zephyr with a `nordic,nrf-uart` overlay)
  also still works.
- **MRS IPSR** (SYSm=5) now returns just the exception number, so Zephyr's
  `_isr_wrapper` dispatches the right ISR — without this every interrupt
  mis-dispatched and the tickless RTC1 clock never advanced.
- **Banked MSP/PSP** across exception entry/return + `CONTROL.SPSEL`, so the
  PendSV context switch works and threads actually run (Contiki used MSP only,
  so it never exercised this).

For the 802.15.4 echo, two more were needed:

- **TEMP sensor** (`0x4000C000`) — `nrf_802154` periodically measures die
  temperature for radio calibration and *blocks the system work-queue* on
  `device_sync_sem` until the `DATARDY` interrupt. With no TEMP model that read
  hung forever, so DAD never completed (`Network init failed -116`). csim
  completes it instantly: `TASKS_START` → `EVENTS_DATARDY` + IRQ, `TEMP` ≈ 25 °C.
- **UARTE EasyDMA RX** (`STARTRX`/`RXD.PTR`/`ENDRX`) — the input half of the
  console, for shell-driven samples (shared with the RIOT path, `docs/riot.md`).

### Bring-up debugging toolkit

These env-gated scopes (behaviour-neutral; in `src/arm/`) are what made the
above tractable — reach for them when porting a new OS or peripheral:

| Env var | What it shows |
|---|---|
| `ARM_MMIO_TRACE=1` | first access to each **unmapped peripheral page** (find missing peripherals) |
| `ARM_EXC_TRACE=1` | **exception entry/return** (exc/EXC_RETURN, SP, PC) — ISR dispatch + context switches |
| `NRF_RX_TRACE=1` | nRF radio RX framing |

Plus the end-of-run per-node `PC=0x…` (the ARM `program_counter` op) →
`arm-none-eabi-addr2line -e <elf> <pc>` resolves a boot spin to a source line.
The per-mote GDB stub is also available for interactive inspection.

## Limitations

- The echo samples' console carries Zephyr's shell: each log line arrives
  after a coloured `uart:~$` prompt and its ANSI escapes, as a serial
  terminal would show them. `cngsim run` renders them away in what it
  prints; `-v`, `--shell`, `--script`, test_runner, the web UI and
  recordings show the raw line. A setup file's checks and a script's
  `expect` and `capture` see the raw line too, so match on the text
  (`Received and replied`), not on the start of the line (`^`). `cmd`
  finds the prompt through its escapes.
- Zephyr nodes acknowledge frames themselves: `nrf_802154` times its ACK from
  the frame's END on TIMER0 and starts it through PPI, and the model runs that
  path (Phase 3 of `docs/design/zephyr-802154-plan.md`). In the bundled echo
  pair every frame that asks for an ACK gets one. Up to 0.4.0 the model
  missed it and Zephyr never acknowledged a frame.
- OpenThread on Zephyr is a further step — see
  `docs/design/zephyr-802154-plan.md` (needs the QSPI settings backend).
  OpenThread's own bare-metal nRF52840 port (`ot-nrf528xx`) runs and forms
  a Thread network: [openthread.md](openthread.md).
