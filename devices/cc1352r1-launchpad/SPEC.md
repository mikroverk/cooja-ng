# Device SPEC — `TI CC1352R1 LaunchPad`

## Identity

- **SoC**: TI CC1352R (CC13x2/CC26x2 family, silicon PG2.1)
- **Board name**: LAUNCHXL-CC1352R1
- **Contiki-NG `TARGET`**: `simplelink`
- **Contiki-NG `BOARD`**: `launchpad/cc1352r1`
- **csim platform string**: `cc1352r1-launchpad`
- **Reference docs**: CC13x2/CC26x2 Technical Reference Manual (SWCU185);
  CC1352R datasheet (SWAS063); SimpleLink core SDK 4.10.02 device headers
  `source/ti/devices/cc13x2_cc26x2/inc/hw_*.h` and driverlib; Contiki-NG
  `arch/platform/simplelink/cc13xx-cc26xx/launchpad/cc1352r1/`
  (`CC1352R1_LAUNCHXL.h` for pins).

## CPU

- **Architecture**: `arm-cortex-m4f` (the nRF52840's core model; CPUID
  `0x410FC241`)
- **Frequency**: 48 000 000 Hz (SCLK_HF from XOSC_HF after boot)
- **RAM**: 81 920 bytes @ `0x20000000`; GPRAM 8 KB @ `0x11000000`; RF core
  RAM 16 KB + 4 KB @ `0x21000000`
- **Flash**: 360 448 bytes (44 × 8 KB) @ `0x00000000`; CCFG is the last
  sector, also seen at `0x50003000`–`0x50004FFF`
- **ROM**: 256 KB on the chip @ `0x10000000`. csim maps its first 16 KB
  (`0x4000`) with a stand-in built from
  driverlib (`tools/ti-simplelink-rom/`, see the Decisions Log in
  `docs/design/refactor-plan.md`), never TI's image.
- **Reuses existing emulator?**: CPU and NVIC, yes; every peripheral is new
  (`src/arm/cc13x2_soc.c`).

## Console

- **Peripheral**: UART0 (TI's PL011-derived UART, with the CC13x2
  end-of-transmission interrupt)
- **Base address**: `0x40001000`, IRQ 5 (`INT_UART0_COMB` = 21)
- **Baud**: 115 200 (IBRD 26, FBRD 3 at 48 MHz); TX paced at the
  programmed rate through a 32-byte FIFO
- **TX pin**: DIO13
- **RX pin**: DIO12

## LEDs

| Index | Name  | Port | Pin | Polarity |
|-------|-------|------|-----|----------|
| 0     | Red   | 0    | 6   | active-high |
| 1     | Green | 0    | 7   | active-high |

The CC13x2 has one 32-bit GPIO port (DIO0..DIO30), so the port is always 0.

## Buttons

| Index | Name  | Port | Pin | Polarity |
|-------|-------|------|-----|----------|
| 0     | BTN-1 | 0    | 15  | active-low, pull-up |
| 1     | BTN-2 | 0    | 14  | active-low, pull-up |

## Off-SoC chips

### MX25R8035F — 8 Mbit SPI flash

- **Bus**: SSI0; CS DIO20, CLK DIO10, MOSI DIO9, MISO DIO8
- **Notes**: the board init puts it into deep power-down at boot (visible as
  DIO9/10/20 activity in `CSIM_GPIO_TRACE`). Not modelled; nothing in the
  csim firmware reads it.

## Clock tree

- **Source**: 48 MHz crystal (XOSC_HF) for SCLK_HF; 32.768 kHz crystal
  (XOSC_LF) for SCLK_LF. Boot runs on RCOSC_HF until TI's Power driver
  starts the crystal and commits the switch through the ROM's
  `HFSourceSafeSwitch`.
- **CPU clock divider**: 1
- **What the firmware actually configures**: `SetupTrimDevice()` copies the
  FCFG1 trims into the analog blocks (DDI0 OSC, ADI2/3/4); `Power_init()`
  selects XOSC_HF (start-up modelled as 300 µs) and the LF source from CCFG;
  `SetupSetAonRtcSubSecInc()` loads the RTC rate trim, which csim accepts
  and does not apply (the RTC counts at exactly 32 768 Hz).

## Known firmware quirks

- **Factory config (FCFG1) gates the boot.** `SetupTrimDevice()` halts
  unless `ICEPICK_DEVICE_ID`/`USER_ID` decode to a CC13x2 PG2.0+;
  `Power_init()` spins unless `TFW_FT % 10000 > 682`; the PIN driver sizes
  its pin range from `IOCONF.GPIO_CNT` (28 here). csim maps FCFG1 as dumped
  from a LaunchPad (`fcfg1_launchpad[]` in `src/arm/cc13x2_soc.c`), with
  that chip's MAC and die-ID words blanked and per-node MAC words
  overlaid.
- **CCFG overrides the IEEE address** when any byte of `CCFG:IEEE_MAC_0/1`
  is not `0xFF`; otherwise Contiki takes FCFG1 `MAC_15_4_0/1`, which csim
  seeds per node (`00:12:4b:00:<id>:<id>`).
- **Bit-band.** driverlib enables the RTC, its channels and the SUBSECINC
  handshake through the peripheral bit-band alias (`HWREGBITW`). This port
  found and fixed csim's bit-band word address (it was `byte * 4`).
- **AON RTC compare.** TI's TimerP needs the channel event to arrive at
  least one SCLK_LF edge after the counter reaches the compare value; csim
  registers it on the edge after the match. Not stated in the TRM;
  measured on the LaunchPad (HARDWARE-COMPARISON.md).
- **IEEE mode on CC13xx: Contiki's CMD_FS fails.** It carries the sub-GHz
  channel-table frequency, and silicon answers ERROR_SYNTH_PROG; the radio
  is tuned by CMD_IEEE_RX's channel field. The same table lets
  `RADIO_PARAM_CHANNEL` accept 27..33, which CMD_IEEE_RX rejects
  (IEEE_ERROR_PAR), after which CMD_IEEE_TX ends IEEE_ERROR_NO_FS and
  Contiki's `transmit()` still returns RADIO_TX_OK.
- **Debugging a sleeping image.** Standby powers the JTAG domain down;
  `tools/cc13x2-hwcheck/catch.tcl` halts the CPU in the boot window.
- **Watchdog caps idle.** Contiki sleeps at most until the watchdog's
  remaining time (`WDT.VALUE`), so the WDT has to count.

## Reference firmware

- `firmware/cc1352r1-launchpad/hello-world-nullradio.cc1352r1-launchpad` —
  hello-world with `NETSTACK_CONF_RADIO=nullradio_driver`: boot without the
  RF core (L0–L4).
- `firmware/cc1352r1-launchpad/leds-nullradio.cc1352r1-launchpad` —
  `examples/dev/leds`, the LED sequence (L3).
- `firmware/cc1352r1-launchpad/nullnet-broadcast.cc1352r1-launchpad` —
  `examples/nullnet`, prop mode (L5).
- `firmware/cc1352r1-launchpad/udp-server.cc1352r1-launchpad`,
  `udp-client.cc1352r1-launchpad` — `examples/rpl-udp`, prop mode (L6).
- `*-ieee.cc1352r1-launchpad` — the same three examples built with
  `RF_CONF_MODE=RF_MODE_2_4_GHZ` (IEEE 802.15.4, 2.4 GHz).

## Definition of done

- [x] `make clean && make` builds with no new warnings
- [x] `./build/test_runner test configs/test-boot-cc1352r1-launchpad.yaml` (L0–L2, L4)
- [x] L3: `CSIM_GPIO_TRACE=1 ./build/test_runner mixed-multinode firmware/cc1352r1-launchpad/leds-nullradio.cc1352r1-launchpad -t 10000`
      shows the `leds-example.c` sequence on DIO6/DIO7
- [x] L5: `./build/test_runner test configs/test-nullnet-cc1352r1-launchpad.yaml`
- [x] L6: `./build/test_runner test configs/test-rpl-udp-cc1352r1-launchpad.yaml`
- [x] IEEE (2.4 GHz) mode: `test-nullnet-ieee-` and `test-rpl-udp-ieee-cc1352r1-launchpad.yaml`,
      and `test-mixed-platform-cc1352r1.yaml` with Sky, CC2538 and nRF52840
- [x] `.github/workflows/test.yml` runs the boot, nullnet and RPL-UDP tests and two determinism guards
- [x] FCFG1 and the RTC compare lag checked against the LaunchPad (HARDWARE-COMPARISON.md)
