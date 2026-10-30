
## udp-client-v52.cc2538dk

- **Source**: contiki-ng commit `5968cdae931a68a1413b5b8feef1c0438c2ae2a1`
- **Source path**: `examples/rpl-udp` (file: `udp-client.c`)
- **TARGET**: `cc2538dk`
- **Toolchain**: host arm-none-eabi-gcc 15.2.Rel1
- **Built**: 2026-09-09T08:44:27Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target cc2538dk --example examples/rpl-udp --source-file udp-client --local --output firmware/cc2538dk/udp-client-v52.cc2538dk`
- **Why**: the Renode half of the cross-simulator demo (examples/renode/cc2538-csim-rpl.resc).
  Renode emulates this CC2538; csim emulates its RPL parent.

## udp-server-v52.cc2538dk

- **Source**: contiki-ng commit `5968cdae931a68a1413b5b8feef1c0438c2ae2a1`
- **Source path**: `examples/rpl-udp` (file: `udp-server.c`)
- **TARGET**: `cc2538dk`
- **Toolchain**: host arm-none-eabi-gcc 15.2.Rel1
- **Built**: 2026-09-09T19:52:28Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target cc2538dk --example examples/rpl-udp --source-file udp-server --local --output firmware/cc2538dk/udp-server-v52.cc2538dk`
- **Why**: the RPL root for the Renode co-simulation demo (examples/renode/cc2538-csim-rpl.resc):
  Renode emulates this CC2538 as the DAG root and UDP server; csim's nodes join it.

## ot-cli-ftd.cc2538dk

- **Source**: [openthread/ot-cc2538](https://github.com/openthread/ot-cc2538) commit `f542e4710743996e5f849dd062e584f3f8e0a678`, OpenThread submodule `53e8322140dde7b26827332100504507cec9cc4e` (thread-reference-20250612-1522-g53e832214)
- **Target**: `ot-cli-ftd`, OpenThread's CLI example as a full Thread device (BSD-3-Clause; mbedTLS Apache-2.0)
- **Toolchain**: host arm-none-eabi-gcc 15.2.Rel1
- **Built**: 2026-10-30T16:11Z by Joakim Eriksson; stripped with `--strip-debug`.
- **Build command**: `OT_CMAKE_NINJA_TARGET=ot-cli-ftd ./script/build`, after one local change to `src/cc2538.ld`: `} > FLASH= 0` written as `} > FLASH = 0` (binutils 2.45 no longer parses the fill without the space).
- **Why**: `configs/test-openthread-cc2538dk.yaml` — two nodes form a Thread network on the emulated CC2538.
