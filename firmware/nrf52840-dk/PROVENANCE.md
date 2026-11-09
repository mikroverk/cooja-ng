## hello-world.nrf52840-dk

- **Source**: contiki-ng commit `ad0d073818684fd0c6cdce2f01c3aac9331f812e`
- **Source path**: `examples/hello-world` (file: `hello-world.c`)
- **TARGET**: `nrf52840`
- **BOARD**: `dk`
- **Toolchain**: Docker contiker/contiki-ng:latest
- **Built**: 2026-05-11T09:08:39Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target nrf52840 --board dk --example examples/hello-world --output firmware/nrf52840-dk/hello-world.nrf52840-dk`

## udp-server.nrf52840-dk

- **Source**: contiki-ng commit `ad0d073818684fd0c6cdce2f01c3aac9331f812e`
- **Source path**: `examples/rpl-udp` (file: `udp-server.c`)
- **TARGET**: `nrf52840`
- **BOARD**: `dk`
- **Toolchain**: Docker contiker/contiki-ng:latest
- **Built**: 2026-05-11T09:09:05Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target nrf52840 --board dk --example examples/rpl-udp --output firmware/nrf52840-dk/udp-server.nrf52840-dk --source-file udp-server`
- **Rebuilt with DWARF**: 2026-10-31 by Joakim Eriksson, at the same commit and toolchain (Docker contiker/contiki-ng, Arm GCC 10.3-2021.10, the compiler the image's `.comment` names): `tools/build-device-firmware.sh --target nrf52840 --board dk --example examples/rpl-udp --source-file udp-server --output firmware/nrf52840-dk/udp-server.nrf52840-dk --debug --relstr release/v5.1-81-gad0d07381`.  The same program — every allocated section, the entry point and every symbol (`tools/check-same-program.py`), and the same simulation output and PCAP — now with DWARF, so overlays can read its struct layouts.

## udp-client.nrf52840-dk

- **Source**: contiki-ng commit `ad0d073818684fd0c6cdce2f01c3aac9331f812e`
- **Source path**: `examples/rpl-udp` (file: `udp-client.c`)
- **TARGET**: `nrf52840`
- **BOARD**: `dk`
- **Toolchain**: Docker contiker/contiki-ng:latest
- **Built**: 2026-05-11T09:09:11Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target nrf52840 --board dk --example examples/rpl-udp --output firmware/nrf52840-dk/udp-client.nrf52840-dk --source-file udp-client`
- **Rebuilt with DWARF**: 2026-10-31 by Joakim Eriksson, at the same commit and toolchain (Docker contiker/contiki-ng, Arm GCC 10.3-2021.10, the compiler the image's `.comment` names): `tools/build-device-firmware.sh --target nrf52840 --board dk --example examples/rpl-udp --source-file udp-client --output firmware/nrf52840-dk/udp-client.nrf52840-dk --debug --relstr release/v5.1-81-gad0d07381`.  The same program — every allocated section, the entry point and every symbol (`tools/check-same-program.py`), and the same simulation output and PCAP — now with DWARF, so overlays can read its struct layouts.

## zephyr-echo-server.nrf52840-dk / zephyr-echo-client.nrf52840-dk

- **Source**: Zephyr (`zephyrproject/zephyr`) `v4.4.0-5779-g32e0ab566c6`
- **Source path**: `samples/net/sockets/echo_server` and `.../echo_client`
- **BOARD**: `nrf52840dk/nrf52840`
- **Config**: STOCK — only the sample's own `overlay-802154.conf` (selects the
  on-chip 802.15.4 radio / `nrf_802154`); no other firmware changes.
- **Toolchain**: gnuarmemb (`arm-none-eabi-gcc`)
- **Built**: 2026-06-21 by Joakim Eriksson; stripped with `--strip-debug`.
- **Build command**:
  `west build -b nrf52840dk/nrf52840 samples/net/sockets/echo_{server,client} -- -DEXTRA_CONF_FILE=overlay-802154.conf`
- **Purpose**: regression test for stock-Zephyr 802.15.4 networking on csim's
  nRF52840 (DAD + two-node UDP echo). The server logs `Received and replied`.


## tsch-node.nrf52840-dk

- **Source**: contiki-ng commit `36a1ab5e3` (local tree, 2026-07-02)
- **Source path**: `examples/6tisch/simple-node` (file: `node.c`)
- **TARGET**: `nrf52840`
- **BOARD**: `dk`
- **Local patch**: `node.c` coordinator guard removed — `if(node_id == 1) NETSTACK_ROUTING.root_start();` compiled on ALL targets (upstream gates it to COOJA/Z1), so node 1 self-selects TSCH coordinator in the emulator
- **Toolchain**: arm-none-eabi-gcc 15.2.1 (host build)
- **Built**: 2026-07-02 by Joakim Eriksson / Claude
- **Build command**: `make TARGET=nrf52840 BOARD=dk` in examples/6tisch/simple-node
- **Purpose**: 2-node TSCH regression (association + held sync + EACK'd keepalives); node 1 becomes coordinator, node 2 associates

## echo-zephyr.nrf52840-dk / riot-root.nrf52840-dk

The Contiki-NG side of the two interop demos (`configs/interop-contiki-zephyr.json`,
`configs/interop-contiki-riot.json`).

- **Source**: `examples/interop/echo-zephyr` and `examples/interop/riot-root` in
  this repository (files: `echo-zephyr.c`, `riot-root.c`), against contiki-ng
  commit `52f1b1e7d66605c30e3441ae87ab00328aa561f2`.  The clone had local changes
  and untracked directories; none of them is compiled into these builds (no
  tracked file under `os/`, `arch/cpu/nrf52840/` or `arch/platform/nrf52840/`
  differs from the commit).
- **TARGET**: `nrf52840`
- **BOARD**: `dk`
- **Config**: each example's `project-conf.h` and `Makefile` (echo-zephyr:
  nullrouting, so standard Neighbor Discovery; riot-root: RPL Classic in storing
  mode).
- **Toolchain**: host (Arm GNU Toolchain 15.2.rel1)
- **Built**: 2026-10-26 by Joakim Eriksson; echo-zephyr rebuilt 2026-10-27 from
  the same clone after its datagram header was shortened (`fill()`), so the
  16 B datagram carries pattern bytes too.
- **Build command**: `tools/build-device-firmware.sh --local --contiki-dir <clone> --target nrf52840 --board dk --example examples/interop/<name> --output firmware/nrf52840-dk/<name>.nrf52840-dk --make-args "CONTIKI=<clone>"`

## riot-gnrc-networking.nrf52840-dk

- **Source**: RIOT `fa7ffa17756c73abc92bf267abe8e53e32c4adfe`
- **Source path**: `examples/networking/gnrc/networking`
- **BOARD**: `nrf52840dk`
- **Config**: STOCK.  The demo sets the PAN ID from the shell (RIOT's default is
  0x23, Contiki-NG's and Zephyr's 0xabcd).
- **Toolchain**: arm-none-eabi-gcc 13.2.1 (Ubuntu 24.04 package), on jftest4
- **Built**: 2026-10-26 by Joakim Eriksson; stripped with `--strip-debug`.
- **Build command**: `BOARD=nrf52840dk make -C examples/networking/gnrc/networking`

## Overlay struct layouts (udp-client, udp-server)

`src/services/rpl_layout.h`'s layout `contiki-ng-cortex-m` holds for these
two images: `tools/gen-struct-layout.py` on the images themselves (they carry
DWARF) gives exactly its rows (`sizeof(rpl_instance_t)` = 352).

## ot-cli-ftd.nrf52840-dk

- **Source**: [openthread/ot-nrf528xx](https://github.com/openthread/ot-nrf528xx) commit `382ea3da509242782e3511173a24eb29687bec7a`, OpenThread submodule `53e8322140dde7b26827332100504507cec9cc4e` (thread-reference-20250612-1522-g53e832214)
- **Target**: `ot-cli-ftd`, OpenThread's CLI example as a full Thread device, the `soft_crypto` build (OpenThread's mbedTLS; the CryptoCell CC310 is not modelled).  OpenThread BSD-3-Clause; mbedTLS Apache-2.0; the Nordic drivers it links under their own licences in `third_party/NordicSemiconductor`.
- **Toolchain**: host arm-none-eabi-gcc 15.2.Rel1
- **Built**: 2026-10-30T16:50Z by Joakim Eriksson; stripped with `--strip-debug`.
- **Build command**: `OT_CMAKE_BUILD_DIR=build-soft OT_CMAKE_NINJA_TARGET=ot-cli-ftd ./script/build nrf52840 soft_crypto -DOT_TCP=OFF` (without `-DOT_TCP=OFF` the soft-crypto build fails to link the CLI's TLS example).
- **Why**: `configs/test-openthread-nrf52840-dk.yaml` — two nodes form a Thread network on the emulated nRF52840.

## Overlay struct layouts (ot-cli-ftd.nrf52840-dk)

`src/services/thread_layout.h` has this image's rows, from a DWARF build that
is the same program (`tools/check-same-program.py`: every allocated section,
the entry point and every symbol).  With DWARF the image is 28 MB, so it is
not the shipped one.  The build, in the same ot-nrf528xx checkout:
`CFLAGS=-g CXXFLAGS=-g OT_CMAKE_BUILD_DIR=build-dbg OT_CMAKE_NINJA_TARGET=ot-cli-ftd ./script/build nrf52840 soft_crypto -DOT_TCP=OFF`, then `instance_api.cpp` rebuilt with
`SOURCE_DATE_EPOCH` set to 2026-10-30 17:50:13 UTC (the build time the image prints in
its version string, the only other difference).  The overlay picks the rows
by `sizeof(ot::Instance)`, which the image's `ot::gInstanceRaw` matches.


## udp-{client,server}-tsch.nrf52840-dk, udp-{client,server}-csma.nrf52840-dk

- **Source**: contiki-ng commit `52f1b1e7d66605c30e3441ae87ab00328aa561f2` (it prints
  `Contiki-NG-develop/v5.2-39-g52f1b1e7d-dirty`: the checkout had local changes outside
  `arch/cpu/nrf52840` and `os/` — gecko files, docs and tests, and edits in the CMSIS
  submodule — and untracked directories no build here uses)
- **Source path**: `firmware/src/rpl-udp-mac` in this repository: verbatim copies of
  `examples/rpl-udp/udp-client.c` and `udp-server.c` (a request every 10 s), with a Makefile
  whose only setting is `MAKE_MAC`; no project-conf.h.  The two pairs are the same application
  and differ only in the MAC: TSCH with the 6TiSCH minimal schedule, or CSMA.
- **TARGET**: `nrf52840`, **BOARD**: `dk`
- **Toolchain**: host arm-none-eabi-gcc 15.2.Rel1 (Arm GNU Toolchain 15.2.Rel1, Build arm-15.86)
- **Built**: 2026-11-05T19:09Z by Joakim Eriksson (images taken from commit `9ab3ff2` on
  `feat/energest-graphs`); rebuilt from `firmware/src/rpl-udp-mac` byte-identical (`cmp`),
  the `-tsch` pair again on 2026-11-05 for `configs/test-tsch-chain-nrf52840.yaml`
- **Build command**: `cd firmware/src/rpl-udp-mac && make TARGET=nrf52840 BOARD=dk CONTIKI=~/work/contiki-ng WERROR=0 udp-client udp-server`
  for the `-tsch` images, then `rm -rf build` and the same with `MAKE_MAC=MAKE_MAC_CSMA` for the
  `-csma` ones (images: `build/nrf52840/dk/udp-*.nrf52840`)
- **sha256**: udp-client-tsch `e6aeecf2…6b54`, udp-server-tsch `50093368…91bb`,
  udp-client-csma `eb92d572…03fa`, udp-server-csma `772fdc8a…5946`
- **Why**: one application over either MAC on one chip.  The `-tsch` pair is the firmware of
  `configs/test-tsch-chain-nrf52840.yaml` (a 7-node chain, answered six hops out: the RADIO
  power-cycle regression); the energy ladder demo on `feat/energest-graphs` runs both pairs.

## udp-{client,server}-orchestra.nrf52840-dk

- **Source**: the same checkout and sources as the `-tsch` pair above (contiki-ng
  `52f1b1e7d`, printing `Contiki-NG-develop/v5.2-39-g52f1b1e7d-dirty`; `firmware/src/rpl-udp-mac`,
  verbatim `examples/rpl-udp`), built with Orchestra: `MAKE_WITH_ORCHESTRA=1` adds
  `os/services/orchestra` and the rules `examples/6tisch/simple-node` builds by default
  (`ORCHESTRA_CONF_RULES={&eb_per_time_source,&unicast_per_neighbor_rpl_ns,&default_common}`),
  in place of the 6TiSCH minimal schedule.
- **TARGET**: `nrf52840`, **BOARD**: `dk`
- **Toolchain**: host arm-none-eabi-gcc 15.2.Rel1 (Arm GNU Toolchain 15.2.Rel1, Build arm-15.86)
- **Built**: 2026-11-06 by Joakim Eriksson; two builds byte-identical, and the `-tsch` pair
  rebuilt from the same checkout byte-identical to the images above.
- **Build command**: `cd firmware/src/rpl-udp-mac && make TARGET=nrf52840 BOARD=dk CONTIKI=~/work/contiki-ng WERROR=0 MAKE_WITH_ORCHESTRA=1 udp-client udp-server`
  (images: `build/nrf52840/dk/udp-*.nrf52840`)
- **sha256**: udp-client-orchestra `016f2345…40e7`, udp-server-orchestra `1aa3daab…4bea`
- **Why**: `configs/test-tsch-orchestra-nrf52840.yaml`, TSCH whose cells sit on several channel
  offsets, so a node hears its neighbour only on the channel both are tuned to: the regression
  for the nRF52840's channel on the radio medium.

## test-idle-rtc-ovrflw.nrf52840-dk / test-idle-blx-wild.nrf52840-dk

- **Source**: this repository, `test/firmware/idle-rtc-ovrflw/main.s` and `test/firmware/idle-blx-wild/main.s` (hand-written Thumb assembly, no libraries; the second links with the first's `link.ld`).
- **Toolchain**: host Arm GNU Toolchain 15.2.Rel1 (`arm-none-eabi-as`/`arm-none-eabi-ld` 2.45.1).
- **Built**: 2026-11-07 and 2026-11-08 by Joakim Eriksson; rebuilt byte-identical on 2026-11-09.
- **Build command**: the two lines at the top of each `main.s`, run in its directory (the object's name is part of the image: its symbol table names it).
- **Why**: idle-skip regressions (docs/idle-skip.md), run with skipping off and on by `tools/check-idle-skip.sh`: `configs/test-idle-rtc-ovrflw-nrf52840.yaml` (an RTC overflow interrupt unmasked part of each pass) and `configs/test-idle-blx-wild-nrf52840.yaml` (a branch to 0xFFFFFFFF after a poll loop; with `test/scripts/idle-skip-shell-writes.cnsh`, shell writes the loop reads).
