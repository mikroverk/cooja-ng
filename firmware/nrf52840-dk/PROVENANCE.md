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

## udp-client.nrf52840-dk

- **Source**: contiki-ng commit `ad0d073818684fd0c6cdce2f01c3aac9331f812e`
- **Source path**: `examples/rpl-udp` (file: `udp-client.c`)
- **TARGET**: `nrf52840`
- **BOARD**: `dk`
- **Toolchain**: Docker contiker/contiki-ng:latest
- **Built**: 2026-05-11T09:09:11Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target nrf52840 --board dk --example examples/rpl-udp --output firmware/nrf52840-dk/udp-client.nrf52840-dk --source-file udp-client`

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

## ot-cli-ftd.nrf52840-dk

- **Source**: [openthread/ot-nrf528xx](https://github.com/openthread/ot-nrf528xx) commit `382ea3da509242782e3511173a24eb29687bec7a`, OpenThread submodule `53e8322140dde7b26827332100504507cec9cc4e` (thread-reference-20250612-1522-g53e832214)
- **Target**: `ot-cli-ftd`, OpenThread's CLI example as a full Thread device, the `soft_crypto` build (OpenThread's mbedTLS; the CryptoCell CC310 is not modelled).  OpenThread BSD-3-Clause; mbedTLS Apache-2.0; the Nordic drivers it links under their own licences in `third_party/NordicSemiconductor`.
- **Toolchain**: host arm-none-eabi-gcc 15.2.Rel1
- **Built**: 2026-10-30T16:50Z by Joakim Eriksson; stripped with `--strip-debug`.
- **Build command**: `OT_CMAKE_BUILD_DIR=build-soft OT_CMAKE_NINJA_TARGET=ot-cli-ftd ./script/build nrf52840 soft_crypto -DOT_TCP=OFF` (without `-DOT_TCP=OFF` the soft-crypto build fails to link the CLI's TLS example).
- **Why**: `configs/test-openthread-nrf52840-dk.yaml` — two nodes form a Thread network on the emulated nRF52840.
