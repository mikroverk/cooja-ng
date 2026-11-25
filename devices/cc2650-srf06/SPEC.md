# Device SPEC — `TI CC2650 on SmartRF06EB`

## Identity

- **SoC**: TI CC2650 (CC26x0 family)
- **Board name**: TI SmartRF06EB with CC2650EM module
- **Contiki-NG `TARGET`**: `cc26x0-cc13x0`
- **Contiki-NG `BOARD`**: `srf06/cc26x0`
- **csim platform string**: `cc2650-srf06`
- **Reference docs**:
  - CC13x0/CC26x0 Technical Reference Manual (SWCU117I)
  - CC2650 datasheet (SWRS158B)
  - SmartRF06EB User's Guide (SWRU321B)
  - Contiki-NG `arch/platform/cc26x0-cc13x0/srf06/cc26x0/` (`board.h` for pins),
    `arch/cpu/cc26x0-cc13x0/` (CPU code)
  - driverlib (CC26xx Driver Library; CC26xxware)

Note: This is the same development board SmartRF06 on which `cc2538dk` hosts a CC2538EM.
The CC2650 radio is *on-chip* (2.4 GHz only), so there is no CC2420/CC1200 off-SoC radio.

## CPU

- **Architecture**: `arm-cortex-m3` (48 MHz, no FPU; CC2650 is the
  Cortex-M3 of the CC13xx/CC26xx family; CPUID `0x412FC231` for r2p1)
- **Frequency**: 48 000 000 Hz (SCLK_HF; 48 MHz HF RC or 24 MHz XTAL × internal doubler)
- **RAM**: 20 480 bytes @ `0x20000000`; GPRAM (flash cache; VIMS) 8 KB @
  `0x11000000`; RF core RAM 4KB @ `0x21000000`
- **Flash**: 128 KB @ `0x00000000`; the last 88 bytes at `0x1FFA8` are CCFG
  (also aliased at `0x50003000`); factory config FCFG1 @ `0x50001000`
- **ROM**: 115KB on the chip @ `0x10000000` (BROM). The ROM API tables
  are at `0x10000048` (HAPI) and `0x10000180` (ROM_API_TABLE), as named
  in `cc26xxware/driverlib/rom.h`. Not yet modelled in csim (Step 6.1
  skeleton has no ROM page); a stand-in built from driverlib (`tools/ti-simplelink-rom/`).
- **Reuses existing emulator?**: partly. Cortex-M3 same as `cc2538dk`.
  The same SmartRF06 development board.
  Chip architecture and on-chip peripherals similar to CC13x2.

## Console

- **Peripheral**: UART0
- **Base address**: `0x40001000`
- **Baud**: 115 200 (`TI_UART_CONF_BAUD_RATE` in Contiki-NG)
- **TX pin**: DIO3
- **RX pin**: DIO2

Note: RTS / CTS are wired but unused in Contiki-NG.

## LEDs

| Index | Name  | Port | Pin | Polarity |
|-------|-------|------|-----|----------|
| 0     | Red   (LED_1)  | 0 | 25  | active-high |
| 1     | Yellow (LED_2) | 0 | 27  | active-high |
| 2     | Green  (LED_3) | 0 | 7   | active-high |
| 3     | Orange (LED_4) | 0 | 6   | active-high |

The CC2650 has one 32-bit GPIO port (DIO0..DIO30), so the port is always 0.
Pins reported as DIO rather than pins of SmartRF06.

Note: `arm_platform_config_t` carries `leds[3]`, so only 3 LEDs
(red DIO25, yellow DIO27, green DIO7) are connected there.

## Buttons

| Index | Name  | Port | Pin | Polarity |
|-------|-------|------|-----|----------|
| 0     | Key Left   | 0 | 15 | active-low, pull-up |
| 1     | Key Right  | 0 | 18 | active-low, pull-up |
| 2     | Key Up     | 0 | 19 | active-low, pull-up |
| 3     | Key Down   | 0 | 12 | active-low, pull-up |
| 4     | Key Select | 0 | 11 | active-low, pull-up |

Note: `arm_platform_config_t` carries a single `button`, so only
1 button (Key Select DIO11) is connected there.

## Off-SoC chips

SmartRF06 hosts a few components – SFH5711 ambient light sensor,
BMA250 accelerometer sensor, Micro SD card storage and LCD (optional)
display – but we won't currently model them.
Only ALS is supported by Contiki-NG.

## Clock tree

- **Source**: SCLK_HF from 48-MHz RC oscillator (RCOSC_HF) or doubled 24-MHz XTAL oscillator (XOSC_HF).
  SCLK_LF from 32-kHz RC oscillator (RCOSC_LF) or 32.768-kHz XTAL oscillator (XOSC_LF).
- **CPU clock divider**: 1 (CPU runs on SCLK_HF)
- **What the firmware actually configures**:
  SCLK_HF at reset is from the 48-MHz RCOSC_HF, Contiki-NG switches to the 24-MHz XOSC_HF × internal doubler = 48-MHz
  for radio operations and back to RCOSC_HF.
  SCLK_LF at reset is from the 32-kHz RCOSC_LF, `SetupTrimDevice()` with
  Contiki-NG's CCFG switches to the 32.768-kHz XOSC_LF,
  Contiki-NG's `platform_init_stage_one()` explicilty switches to XOSC_LF.


## Known firmware quirks

- csim maps FCFG1 as dumped from the device (`fcfg1[]` in `src/arm/cc2650_soc.c`),
  with that chip's MAC and die-ID words blanked and per-node MAC words overlaid.
- CCFG overrides the IEEE address when any byte of `CCFG:IEEE_MAC_0/1` is not `0xFF`;
  otherwise FCFG1 `MAC_15_4_0/1` is used, which csim seeds per node (00:12:4b:00:<id>:<id>).

## Reference firmware

Pre-built ELFs that the test harness loads. **Each must have a
`PROVENANCE.md` next to it** (copy `firmware/PROVENANCE-template.md`).

- `firmware/cc2650-srf06/hello-world-nullradio.cc2650-srf06` — hello-world with
  `NETSTACK_CONF_RADIO=nullradio_driver`: boot without the RF core (L0–L4).
- `firmware/cc2650-srf06/button-hal-nullradio.cc2650-srf06` — button-hal with
  `NETSTACK_CONF_RADIO=nullradio_driver`: GPIO input model (all 5 buttons).
- `firmware/cc2650-srf06/shell-nullradio.cc2650-srf06` — shell with
  `NETSTACK_CONF_RADIO=nullradio_driver`: console input over UART0.

## Definition of done

- [x] `make clean && make` builds with no new warnings
- [x] `./build/test_runner test configs/test-boot-cc2650-srf06.yaml` (L0–L2, L4)
- [x] `CSIM_GPIO_TRACE=1 ./build/test_runner test configs/test-boot-cc2650-srf06.yaml -t 400` shows the `platform.c` sequence of fading LEDs: DIO25, DIO27, DIO7, DIO6 (L3)
- [x] `./build/test_runner test configs/test-button-cc2650-srf06.yaml --script test/scripts/button-cc2650-srf06.cnsh`
- [x] `./build/test_runner test configs/test-shell-cc2650-srf06.yaml` (console input over UART0)
- TODO levels > L4
