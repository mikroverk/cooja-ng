# CC1312R7 LaunchPad Port — Status & Direction

> For the device contract see [`SPEC.md`](SPEC.md).

## Current state — sub-GHz through L6, Zephyr boots

| Level | Evidence |
|-------|----------|
| L0 ELF loads | 704 KB flash image, CCFG at `0xAFFA8` |
| L1 reset → `main()` | through `SetupTrimDevice()` in the CC13x2x7 stand-in ROM (wafer ID check passes) |
| L2 console | banner on UART0 |
| L3 buttons | `configs/test-button-cc1312r7-launchpad.yaml` + `test/scripts/button-cc1312r7-launchpad.cnsh` (BTN-1 DIO13, BTN-2 DIO14) |
| L3 console input | `configs/test-shell-cc1312r7-launchpad.yaml` |
| L4 Contiki timers | `configs/test-boot-cc1312r7-launchpad.yaml` |
| L5 radio, prop | `configs/test-nullnet-cc1312r7-launchpad.yaml` |
| L6 full stack, prop | `configs/test-rpl-udp-cc1312r7-launchpad.yaml` |
| TSCH, prop | `configs/test-tsch-cc1312r7-launchpad.yaml` — association + ≥3 drift reports |
| Interop, prop | `configs/test-mixed-subghz-cc1312r7-cc1352r1.yaml` — two CC1312R7 clients in a CC1352R1-rooted DAG |
| Zephyr | `zephyr-hello-world.cc1312r7-launchpad` — boots (`SOC_CC1352R7` init, same ROM) and prints |

## Firmware

Contiki-NG has no CC1312R7 support upstream. The images here come from a
local port (Contiki-NG branch `feature/cc1312r7-launchpad`): a
`cc13x2x7-cc26x2x7` CPU subfamily that builds TI's drivers from source
against the SDK 7.10.01.24 CC13x2x7 driverlib, and a `launchpad/cc1312r7`
board. Until that is merged, rebuilding needs that branch
(`--contiki-dir`).

## Open

- **Silicon identity.** `ICEPICK_DEVICE_ID`, `USER_ID` and `RFCMODEHWOPT`
  are derived from TI's driverlib, the trims are the CC1352R1's. Dump FCFG1
  and `RFCMODEHWOPT` from a real LP-CC1312R7 (as `fcfg1.bin` was for the
  CC1352R1, identifying words blanked) and replace them.
- **Hardware comparison.** Run `tools/cc13x2-hwcheck` on an LP-CC1312R7
  for the same table as `devices/cc1352r1-launchpad/HARDWARE-COMPARISON.md`.
- **On hardware, the Contiki-NG port itself** is untested: the RF settings
  (TI's CC1312R7 overrides and PA table) and the TI drivers compiled from
  source need a real board before the port goes upstream.
