
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

## Overlay struct layouts (udp-client, udp-server)

`udp-client.cc2538dk` and `udp-server.cc2538dk` print
`Contiki-NG-release/v5.1` and were built with Arm GCC 9-2019-q4 (their
`.comment`); they carry no DWARF for Contiki's code, and that toolchain is
not at hand to rebuild them as the same program.  `src/services/rpl_layout.h`
has rows for them (layout `contiki-ng-cortex-m`) from DWARF builds of
`examples/rpl-udp` at contiki-ng `52f1b1e7d66605c30e3441ae87ab00328aa561f2`
(host Arm GCC 15.2, `--layout src/services/rpl_layout.h`); the overlay picks
them by `sizeof(rpl_instance_t)` (352 bytes, which these images'
`curr_instance` symbols match), the same layout the nRF52840 images' own
DWARF gives.

## ot-cli-ftd.cc2538dk

- **Source**: [openthread/ot-cc2538](https://github.com/openthread/ot-cc2538) commit `f542e4710743996e5f849dd062e584f3f8e0a678`, OpenThread submodule `53e8322140dde7b26827332100504507cec9cc4e` (thread-reference-20250612-1522-g53e832214)
- **Target**: `ot-cli-ftd`, OpenThread's CLI example as a full Thread device (BSD-3-Clause; mbedTLS Apache-2.0)
- **Toolchain**: host arm-none-eabi-gcc 15.2.Rel1
- **Built**: 2026-10-30T16:11Z by Joakim Eriksson; stripped with `--strip-debug`.
- **Build command**: `OT_CMAKE_NINJA_TARGET=ot-cli-ftd ./script/build`, after one local change to `src/cc2538.ld`: `} > FLASH= 0` written as `} > FLASH = 0` (binutils 2.45 no longer parses the fill without the space).
- **Why**: `configs/test-openthread-cc2538dk.yaml` — three nodes form a Thread network on the emulated CC2538.

## Overlay struct layouts (ot-cli-ftd.cc2538dk)

`src/services/thread_layout.h` has this image's rows, from a DWARF build that
is the same program (`tools/check-same-program.py`: every allocated section,
the entry point and every symbol).  With DWARF the image is 28 MB, so it is
not the shipped one.  The build, in the same ot-cc2538 checkout:
`CFLAGS=-g CXXFLAGS=-g OT_CMAKE_BUILD_DIR=build-dbg OT_CMAKE_NINJA_TARGET=ot-cli-ftd ./script/build`, then `instance_api.cpp` rebuilt with
`SOURCE_DATE_EPOCH` set to 2026-10-30 17:11:41 UTC (the build time the image prints in
its version string, the only other difference).  The overlay picks the rows
by `sizeof(ot::Instance)`, which the image's `ot::gInstanceRaw` matches.


## udp-client-tsch.cc2538dk, udp-server-tsch.cc2538dk

- **Source**: contiki-ng commit `52f1b1e7d66605c30e3441ae87ab00328aa561f2` (it prints
  `Contiki-NG-develop/v5.2-39-g52f1b1e7d-dirty`: the checkout had local changes outside
  `arch/cpu/cc2538`, `arch/platform/cc2538dk`, `arch/cpu/arm` and `os/` — gecko files, docs
  and tests, and deletions in the CMSIS submodule's DSP examples — and untracked directories
  no build here uses)
- **Source path**: `firmware/src/rpl-udp-mac` in this repository (from commit 9ab3ff2 on
  `feat/energest-graphs`, unchanged): verbatim copies of `examples/rpl-udp/udp-client.c` and
  `udp-server.c` (a request every 10 s), with a Makefile whose only setting is `MAKE_MAC`
  (TSCH by default: the 6TiSCH minimal schedule, the server as RPL root and TSCH
  coordinator); no project-conf.h
- **TARGET**: `cc2538dk`
- **Toolchain**: Docker `contiker/contiki-ng:latest` (image
  `sha256:b14d5afb0a2a…f4b609`, created 2026-07-10), its GNU Arm Embedded Toolchain
  10.3-2021.10 (`arm-none-eabi-gcc 10.3.1 20210824`), under x86-64 emulation on an arm64 Mac
- **Built**: 2026-11-05T21:23Z by Joakim Eriksson; built twice, byte-identical (`cmp`)
- **Build command**: the sources streamed into the container (the scratch directory the
  checkout lived in is not shared with Docker), the Contiki-NG checkout mounted read-only:
  `tar -cf - Makefile udp-client.c udp-server.c | docker run -i --rm -v ~/work/contiki-ng:/home/user/contiki-ng:ro contiker/contiki-ng:latest bash -c 'mkdir -p /tmp/app && cd /tmp/app && tar xf - && make TARGET=cc2538dk CONTIKI=/home/user/contiki-ng WERROR=0 RELSTR=develop/v5.2-39-g52f1b1e7d-dirty -j4 udp-client udp-server >&2 && tar cf - build/cc2538dk/udp-client.cc2538dk build/cc2538dk/udp-server.cc2538dk' | tar -xf -`
  (images: `build/cc2538dk/udp-*.cc2538dk`).  The emulated toolchain segfaults now and
  then; `make` was re-run in the same container until it finished, and the two builds
  that did so after different numbers of retries are the same bytes.
- **sha256**: udp-client-tsch `f3e0de1e…0043`, udp-server-tsch `a6444f9b…0f36`
- **Why**: `configs/test-tsch-unicast-cc2538dk.yaml` and `configs/test-tsch-chain-cc2538dk.yaml`,
  TSCH unicast on the CC2538 model.  Before the RF core's TX calibration, RX calibration
  and SFD timing were modelled (CHANGELOG, "TSCH unicast on the CC2538"), every enhanced
  ACK of these images came ~0.54 ms after the frame, ahead of the sender's ACK window, so
  each unicast was sent eight times and the client never joined the DAG.

## test-idle-rssi-valid.cc2538dk

- **Source**: this repository, `test/firmware/idle-rssi-valid/main.s` (hand-written Thumb assembly, no libraries) and its `link.ld` (the CCA at 0x0027FFD4).
- **Toolchain**: host Arm GNU Toolchain 15.2.Rel1 (`arm-none-eabi-as`/`arm-none-eabi-ld` 2.45.1).
- **Built**: 2026-11-09 by Joakim Eriksson.
- **Build command**: the two lines at the top of `main.s`, run in its directory (the object's name is part of the image).
- **Why**: idle-skip regression `configs/test-idle-rssi-valid-cc2538dk.yaml` (a busy-wait on RSSISTAT.RSSI_VALID), run with skipping off and on by `tools/check-idle-skip.sh`.
