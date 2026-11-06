# Running RIOT OS on Cooja-NG

Cooja-NG runs the ELF `BOARD=nrf52840dk make` writes, on an emulated nRF52840:
the Cortex-M4F, its timers and UARTE, and the on-chip 802.15.4 radio that
RIOT's `nrf802154` driver drives. A stock `gnrc_networking` image boots to its
shell, and nodes form an **RPL DODAG** over the air, with each other or with
Contiki-NG nodes:

```
main(): This is RIOT! (Version: ...)
RIOT network stack example application
All up, running the shell now
...
dodag [2001:db8::1 | R: 512 | OP: Router | PIO: on | ...]
  parent [addr: fe80::ec6a:a59c:c7d:467f | rank: 256]
```

| You use | In Cooja-NG |
|---|---|
| `BOARD=native` with a tap interface or ZEP | `BOARD=nrf52840dk`: the image runs on an emulated nRF52840, `nrf802154` and all |
| `make term` | the node's console in the web UI (`--ui`), or `console <node>` in `--shell` |
| `tests/*/tests/*.py` (pexpect, through RIOT's testrunner) | a `--script` with `sendln`, `expect` and `capture`, exit 0 or 1 |
| FIT IoT-LAB `nrf52840dk` nodes | the same board model, on a unit-disk medium, before you book the testbed ([below](#rehearse-before-you-book-the-testbed)) |

> **Experimental.** Contiki-NG is Cooja-NG's validated target (it passes the
> upstream Cooja test suite). RIOT is newer: coverage is `gnrc_networking` on
> the nRF52840 DK (and hello-world on the CC1352R1 LaunchPad), and the gaps are
> under *Limitations*.

## Quick start

No RIOT checkout needed: a stock `gnrc_networking` image is bundled as
`firmware/nrf52840-dk/riot-gnrc-networking.nrf52840-dk`
(`firmware/nrf52840-dk/PROVENANCE.md` says how it was built), and two
examples run it.

```sh
# Three RIOT nodes in a line form a DODAG from RIOT's shell, and node 3
# pings the root through node 2
cngsim run riot-rpl

# RIOT joins a Contiki-NG RPL network, driven by timed shell commands
# (passes when it has pinged the root and traded UDP with it)
cngsim run contiki-riot

# The same in the browser (click a node for its console)
cngsim run contiki-riot --ui
```

Example names work from a checkout (`./build/cngsim`) and from an install
alike; `cngsim examples` lists them all. The console lines name the board
and the OS (`[Node 3/nrf52840-dk RIOT]`). `cngsim new riot-rpl --board
nrf52840dk -n 6 --layout line -o dodag.yaml` writes a DODAG of your own
size.

## Build a firmware

No SDK is needed: RIOT builds with `make` and the system `arm-none-eabi-gcc`:

```sh
git clone --depth 1 https://github.com/RIOT-OS/RIOT.git
cd RIOT/examples/networking/gnrc/networking
BOARD=nrf52840dk make            # -> bin/nrf52840dk/gnrc_networking.elf
```

The board is `nrf52840dk` (the on-chip `nrf802154` radio is the default
netdev). Run the ELF as it is; `cngsim` takes the board from the
`bin/nrf52840dk/` directory it is in:

```sh
cngsim run bin/nrf52840dk/gnrc_networking.elf -t 3s
# cngsim: note: board nrf52840dk, from the RIOT build directory bin/nrf52840dk: runs as nrf52840-dk
```

A copy elsewhere needs `--board nrf52840dk` (RIOT's `BOARD=` names work:
`cngsim boards --os` lists them), or a name like `<app>.nrf52840-dk`.
`arm-none-eabi-strip --strip-debug` keeps a copy small.

## Two RIOT nodes, one DODAG, from a script

`gnrc_networking` is shell-driven: a DODAG needs `rpl root` on one node and
`rpl init` on the others (they then join on the root's DIOs). A command script
types into each node's shell and checks what it prints, the way RIOT's own
pexpect tests do. Save this as `rpl.cnsh`:

```
sleep 1s                       # let both boot to the shell
sendln 1 ifconfig 5 add 2001:db8::1
sendln 1 rpl root 1 2001:db8::1
sleep 3s
sendln 2 rpl init 5
sleep 15s
sendln 2 rpl
expect 2 "parent [addr:" 5s
pass
```

```sh
cngsim run bin/nrf52840dk/gnrc_networking.elf -n 2 -t 40s --script rpl.cnsh
```

Node 1 becomes the root and sends DIOs; node 2 joins as a router, sends a DAO
and gets a DAO-ACK, and its `rpl` output names node 1 as its parent. (`5` is
the gnrc interface number; `ifconfig` shows it.) Without a script, `--ui`
lets you type the same commands into each node's console, and a setup file's
timed `send` actions do it at exact times (`examples/riot-rpl.yaml`,
`examples/contiki-riot.yaml`, [test-format.md](test-format.md)). The script commands are in
[shell.md](shell.md).

## Rehearse before you book the testbed

FIT IoT-LAB has nRF52840 DK, Zolertia Firefly and ESP32-C6-DevKitC-1 nodes.
Cooja-NG models the first two, and runs esp32sim's ESP32-C6 as an external
node, so the image you will flash on the testbed can boot, join and exchange
traffic here first, in CI, with the same seed giving the same run. RIOT has
been run here on the nRF52840 DK only (and boots on the CC1352R1 LaunchPad);
its Firefly build is untested.

The caveat is the radio. The medium here is unit-disk: within range a frame
arrives, unless the success ratio or a collision drops it; there is no path
loss, fading, multipath or external interference. A link that works here can
be lossy or absent on the testbed. Rehearse boot, shell scripts, addressing
and stack logic here; measure link quality there.

## RIOT in a Contiki-NG network

`cngsim run contiki-riot` (`examples/contiki-riot.yaml`, the same run as
`configs/interop-contiki-riot.json`) puts the bundled
`firmware/nrf52840-dk/riot-gnrc-networking.nrf52840-dk` next to a Contiki-NG RPL
root (`examples/interop/riot-root`). RIOT joins the root's DODAG, configures
`fd00::` from its prefix, pings it and trades UDP messages with it. What it takes,
all found on this emulator and each a property of the two stacks:

- **PAN ID.** RIOT defaults to 0x23, Contiki-NG and Zephyr to 0xabcd; the config
  sets it with `ifconfig 5 set pan_id 0xabcd`.
- **Storing mode.** RIOT's RPL builds storing-mode DAOs only (a Transit option
  without a parent address), and Contiki-NG's default RPL Lite is non-storing, so
  the root runs RPL Classic in storing mode.
- **Link-local traffic.** Contiki-NG adds the RPL Option (RFC 6553, type 0x63) to
  packets for global addresses. RIOT has no handler for it, and the type's top
  bits say "discard if unrecognised", so RIOT drops them; RFC 9008's type 0x23
  would be skipped instead. The demo's ping and UDP use link-local addresses.

## What the emulator provides

RIOT exercises the nRF52840 / Cortex-M4F differently from Contiki and Zephyr, and
each gap was a one-line-class fix in `src/arm/` (all general, all
regression-clean — see commit history). The notable ones:

- **PLD/PLI preload hint** — a byte/halfword load with `Rt=PC` is a NOP on
  Cortex-M, not a load-into-PC; newlib's optimised `strlen`/`memcpy` emit them.
- **M4 parallel `UADD8`/`SADD8` + `SEL`** with the `APSR.GE` flags — newlib's
  `strlen`/`strcmp` use them to find a zero byte in a word.
- **TIMER compare reschedule on `INTENSET`** — RIOT's nrf5x `timer_set` (and thus
  `ZTIMER_USEC`) writes `CC` then `INTENSET`; without rescheduling, the compare
  IRQ never fired and the ieee802154 CSMA backoff hung → no TX.
- **UARTE EasyDMA RX** (`STARTRX`/`RXD.PTR`/`ENDRX` + the `ENDRX_STARTRX` short)
  — the missing half of the console, which the shell scripts above type into.
- **Clear latched `EVENTS_CRCOK` on `EVENTS_END`** — RIOT's `nrf802154` driver
  consumes a frame via `END`+`CRCSTATUS` and never clears `CRCOK`, which kept the
  radio's RX re-arm deferred forever (the node went deaf after its first frame).

The radio TX/RX path, the RADIO state machine, the TEMP sensor, and TIMER/RTC
peripherals are shared with the Zephyr/Contiki nRF52840 model (`docs/zephyr.md`,
`src/arm/nrf52840_soc.c`).

### Bring-up debugging toolkit

Env-gated, behaviour-neutral scopes (in `src/arm/`) that made this tractable:

| Env var | What it shows |
|---|---|
| `ARM_WILD_TRAP=1` | first jump to a wild PC (SRAM or `>=0x40000000`) + registers — catches a corrupted PC |
| `ARM_PC_WATCH=0xa,0xb` | hit-counts for specific PCs (is an instruction reached?) |
| `NRF_RADIO_TRACE=1` | RADIO tasks, `STATE` value, ramp start/complete |
| `NRF_RXBYTE_TRACE=1` | radio state on each received air byte (RX vs DISABLED) |
| `NRF_UART_TRACE=1` | UART TX/RX register traffic + the RX-ring delivery |

Plus the end-of-run per-node `PC=0x…` → `arm-none-eabi-addr2line`, the per-mote
GDB stub, and a one-shot Zephyr/RIOT thread-list dumper pattern (resolve a stuck
thread's `pended_on` via DWARF — see `docs/design/zephyr-dad-test-plan.md`).

## Limitations

- Global-address traffic from Contiki-NG to RIOT is dropped by RIOT (the RPL
  Option, above); RIOT to RIOT is tested as far as `riot-rpl` goes: the
  DODAG and a ping over two hops.
- Contiki-NG sometimes misses RIOT's 802.15.4 ACK (RIOT sends it about 240 µs
  after the frame, from software) and resends, so a reply can arrive twice.
  Whether real hardware does the same is not checked.
- `gnrc_networking` is shell-driven; an app that configures itself needs no
  script.
- RIOT builds for the CC2538DK, the Z1 and the Firefly are untested here.
