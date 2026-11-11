# Device SPEC — `TI CC1312R7 LaunchPad`

## Identity

- **SoC**: TI CC1312R7 (CC13x2x7/CC26x2x7 family)
- **Board name**: LP-CC1312R7
- **Contiki-NG `TARGET`**: `simplelink`
- **Contiki-NG `BOARD`**: `launchpad/cc1312r7` (upstream since
  contiki-ng/contiki-ng#3285, see `firmware/cc1312r7-launchpad/PROVENANCE.md`)
- **Zephyr board**: `cc1312r7_lp`, out of tree in `tools/zephyr-boards/`
  (Zephyr has no CC1312R7 SoC; the board selects `SOC_CC1352R7`)
- **csim platform string**: `cc1312r7-launchpad`
- **Reference docs**: CC13x2/CC26x2 Technical Reference Manual (SWCU185);
  the CC1312R7 datasheet; SimpleLink CC13xx/CC26xx SDK 7.10.01.24
  `source/ti/devices/cc13x2x7_cc26x2x7/` (headers, driverlib, `chipinfo.c`),
  `boards/.meta/LP_CC1312R7.syscfg.json` (pins) and `devices/radioconfig`
  (RF settings, PA table).

## What differs from the CC1352R1 LaunchPad

The model is the CC13x2 one (`src/arm/cc13x2_soc.c`, `cc13x2_rfc.c`); the
part is chosen by `cc13x2_part_cc1312r7` (`arm_platform_config_t::soc_part`).

| | CC1352R1 LaunchPad | CC1312R7 LaunchPad |
|---|---|---|
| Flash | 352 KB (44 × 8 KB) | 704 KB (88 × 8 KB) |
| SRAM | 80 KB | 144 KB |
| CCFG | `0x57FA8` | `0xAFFA8` |
| ROM | stand-in from the CC13x2 driverlib | stand-in from the CC13x2x7 driverlib (`cc13x2x7_rom_image.c`) |
| FCFG1 `ICEPICK_DEVICE_ID` | `0x3BB4102F` (read) | `0x1BB7702F` (wafer `0xBB77`; read) |
| FCFG1 `USER_ID` | `0x3082F000` (read) | `0x10828000` (protocol `0x8`, CC13, no PA; read) |
| FCFG1 `IOCONF` | `0xFFFFFF1C`, GPIO_CNT 28 (read) | `0xFFFFFF1E`, GPIO_CNT 30 (read) |
| PRCM `RFCMODEHWOPT` | `0x2F` (read) | `0x21`, modes 0 and 5 (read) |
| Radio | sub-GHz prop + 2.4 GHz IEEE | sub-GHz prop only |
| UART0 | RX DIO12, TX DIO13 | RX DIO2, TX DIO3 |
| BTN-1 / BTN-2 | DIO15 / DIO14 | DIO13 / DIO14 |
| LEDs | DIO6 red, DIO7 green | same |

"Read" values come from an LP-CC1312R7 (FCFG1 and PRCM through the XDS110
and openocd). The rest of FCFG1 (trims) is still the CC1352R1's capture: 91
of the 268 words differ on the R7, mostly trims, and the R7's FCFG1 goes on
past `0x430`.

## CPU

- **Architecture**: `arm-cortex-m4f` (CPUID `0x410FC241`), 48 MHz
- **RAM**: 147 456 bytes @ `0x20000000`; GPRAM 8 KB @ `0x11000000`; RF core
  RAM @ `0x21000000`
- **Flash**: 720 896 bytes @ `0x00000000`; CCFG is the last sector, also seen
  at `0x50003000`
- **ROM**: 256 KB on the chip @ `0x10000000`; csim maps its first 16 KB
  (`0x4000`) with a stand-in built by
  `tools/cc13x2-rom/build-rom.sh --device cc13x2x7_cc26x2x7`

## Console, LEDs, buttons

UART0 at `0x40001000` (115 200 baud) on the XDS110 backchannel, RX DIO2 /
TX DIO3. Red LED DIO6, green LED DIO7, active-high. BTN-1 DIO13, BTN-2
DIO14, active-low with the IOC pull-up (no external pull on the board).
One 32-bit GPIO port, so the port is always 0.

## Radio

The RF core model of the CC1352R1 (`src/arm/cc13x2_rfc.c`), proprietary
mode only. Contiki-NG's CC1312R7 settings use TI's CC1312R7 overrides for
`setting_tc106` and `rf_patch_cpe_prop`; csim accepts the patch into RF core
RAM and does not execute it, as for the CC1352R1. On air the frames are the
same 802.15.4g layout (sync `0x55904E`), so the two parts share a network
(`configs/test-mixed-subghz-cc1312r7-cc1352r1.yaml`).
