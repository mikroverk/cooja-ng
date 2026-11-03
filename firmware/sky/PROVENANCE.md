## nullnet-broadcast.sky

- **Source**: contiki-ng commit `9aa6ecb055182fa8511df68d0ff1f7edc31ed4a9`
- **Source path**: `examples/nullnet` (file: `nullnet-broadcast.c`)
- **TARGET**: `sky`
- **Toolchain**: host
- **Built**: 2026-06-09T22:21:15Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target sky --example examples/nullnet --output firmware/sky/nullnet-broadcast.sky --source-file nullnet-broadcast`

## cputest.sky / timertest.sky

- **Source**: prebuilt MSPSim-era fixtures copied verbatim from the
  Contiki-NG tree at `tools/cooja/firmware/sky/` (sources:
  `tools/cooja/tests/cputest.c`, `tools/cooja/tests/timertest.c`)
- **TARGET**: `sky`
- **Restored**: 2026-06-09 (removed in 1089e7d, restored for the
  `test_runner firmware` boot tests which otherwise SKIP them)
- Exercises instruction-level behavior (cputest) and timer A/B
  capture/compare paths (timertest); the boot test asserts clean
  execution, no expected-output substring.

## udp-server.sky

- **Source**: contiki-ng commit `f15d82e66` (the image prints `Contiki-NG-release/v5.1-164-gf15d82e66`)
- **Source path**: `examples/rpl-udp` (file: `udp-server.c`)
- **TARGET**: `sky`
- **Toolchain**: Docker contiker/contiki-ng, msp430-gcc 4.7.4 (mspgcc dev 20120911), the compiler the image's `.comment` names
- **Built**: 2026-10-31 by Joakim Eriksson, with DWARF: `tools/build-device-firmware.sh --target sky --example examples/rpl-udp --source-file udp-server --output firmware/sky/udp-server.sky --debug --relstr release/v5.1-164-gf15d82e66`
- **Why**: the RPL-UDP pair most configs use.  This rebuild replaced an image with no recorded provenance and no DWARF for Contiki's code; it is the same program — every allocated section, the entry point and every symbol (`tools/check-same-program.py`), and the same simulation output and PCAP — so overlays can read struct layouts from the image itself.

## udp-client.sky

- **Source**: contiki-ng commit `f15d82e66` (the image prints `Contiki-NG-release/v5.1-164-gf15d82e66`)
- **Source path**: `examples/rpl-udp` (file: `udp-client.c`)
- **TARGET**: `sky`
- **Toolchain**: Docker contiker/contiki-ng, msp430-gcc 4.7.4 (mspgcc dev 20120911), the compiler the image's `.comment` names
- **Built**: 2026-10-31 by Joakim Eriksson, with DWARF: `tools/build-device-firmware.sh --target sky --example examples/rpl-udp --source-file udp-client --output firmware/sky/udp-client.sky --debug --relstr release/v5.1-164-gf15d82e66`
- **Why**: the RPL-UDP pair most configs use.  This rebuild replaced an image with no recorded provenance and no DWARF for Contiki's code; it is the same program — every allocated section, the entry point and every symbol (`tools/check-same-program.py`), and the same simulation output and PCAP — so overlays can read struct layouts from the image itself.

## Overlay struct layouts (udp-client, udp-server)

`src/services/rpl_layout.h`'s layout `contiki-ng-msp430` holds for these two
images: `tools/gen-struct-layout.py` on the images themselves gives exactly
its rows (`sizeof(rpl_instance_t)` = 254).
