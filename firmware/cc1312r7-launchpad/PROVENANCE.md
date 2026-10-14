<!-- The Contiki-NG commit below is the CC1312R7 port on branch
     feature/cc1312r7-launchpad (Core SDK submodule 8162033), not yet merged
     upstream; rebuild with --contiki-dir pointing at that branch. -->

## hello-world-nullradio.cc1312r7-launchpad

- **Source**: contiki-ng commit `2a324403b45fa800df83a01990ee223cc2917c7b`
- **Source path**: `examples/hello-world` (file: `hello-world.c`)
- **TARGET**: `simplelink`
- **BOARD**: `launchpad/cc1312r7`
- **Make args**: `DEFINES=NETSTACK_CONF_RADIO=nullradio_driver`
- **Toolchain**: host
- **Built**: 2026-10-14T05:20:42Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target simplelink --board launchpad/cc1312r7 --example examples/hello-world --output firmware/cc1312r7-launchpad/hello-world-nullradio.cc1312r7-launchpad --make-args "DEFINES=NETSTACK_CONF_RADIO=nullradio_driver" --local`

## leds-nullradio.cc1312r7-launchpad

- **Source**: contiki-ng commit `2a324403b45fa800df83a01990ee223cc2917c7b`
- **Source path**: `examples/dev/leds` (file: `leds-example.c`)
- **TARGET**: `simplelink`
- **BOARD**: `launchpad/cc1312r7`
- **Make args**: `DEFINES=NETSTACK_CONF_RADIO=nullradio_driver`
- **Toolchain**: host
- **Built**: 2026-10-14T05:20:44Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target simplelink --board launchpad/cc1312r7 --example examples/dev/leds --output firmware/cc1312r7-launchpad/leds-nullradio.cc1312r7-launchpad --make-args "DEFINES=NETSTACK_CONF_RADIO=nullradio_driver" --local`

## button-hal-nullradio.cc1312r7-launchpad

- **Source**: contiki-ng commit `2a324403b45fa800df83a01990ee223cc2917c7b`
- **Source path**: `examples/dev/button-hal` (file: `button-hal-example.c`)
- **TARGET**: `simplelink`
- **BOARD**: `launchpad/cc1312r7`
- **Make args**: `DEFINES=NETSTACK_CONF_RADIO=nullradio_driver`
- **Toolchain**: host
- **Built**: 2026-10-14T05:20:46Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target simplelink --board launchpad/cc1312r7 --example examples/dev/button-hal --output firmware/cc1312r7-launchpad/button-hal-nullradio.cc1312r7-launchpad --make-args "DEFINES=NETSTACK_CONF_RADIO=nullradio_driver" --local`

## shell-nullradio.cc1312r7-launchpad

- **Source**: contiki-ng commit `2a324403b45fa800df83a01990ee223cc2917c7b`
- **Source path**: `examples/libs/shell` (file: `example.c`)
- **TARGET**: `simplelink`
- **BOARD**: `launchpad/cc1312r7`
- **Make args**: `DEFINES=NETSTACK_CONF_RADIO=nullradio_driver`
- **Toolchain**: host
- **Built**: 2026-10-14T05:20:47Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target simplelink --board launchpad/cc1312r7 --example examples/libs/shell --output firmware/cc1312r7-launchpad/shell-nullradio.cc1312r7-launchpad --make-args "DEFINES=NETSTACK_CONF_RADIO=nullradio_driver" --local`

## nullnet-broadcast.cc1312r7-launchpad

- **Source**: contiki-ng commit `2a324403b45fa800df83a01990ee223cc2917c7b`
- **Source path**: `examples/nullnet` (file: `nullnet-broadcast.c`)
- **TARGET**: `simplelink`
- **BOARD**: `launchpad/cc1312r7`
- **Toolchain**: host
- **Built**: 2026-10-14T05:20:49Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target simplelink --board launchpad/cc1312r7 --example examples/nullnet --output firmware/cc1312r7-launchpad/nullnet-broadcast.cc1312r7-launchpad --source-file nullnet-broadcast --local`

## udp-server.cc1312r7-launchpad

- **Source**: contiki-ng commit `2a324403b45fa800df83a01990ee223cc2917c7b`
- **Source path**: `examples/rpl-udp` (file: `udp-server.c`)
- **TARGET**: `simplelink`
- **BOARD**: `launchpad/cc1312r7`
- **Toolchain**: host
- **Built**: 2026-10-14T05:20:50Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target simplelink --board launchpad/cc1312r7 --example examples/rpl-udp --output firmware/cc1312r7-launchpad/udp-server.cc1312r7-launchpad --source-file udp-server --local`

## udp-client.cc1312r7-launchpad

- **Source**: contiki-ng commit `2a324403b45fa800df83a01990ee223cc2917c7b`
- **Source path**: `examples/rpl-udp` (file: `udp-client.c`)
- **TARGET**: `simplelink`
- **BOARD**: `launchpad/cc1312r7`
- **Toolchain**: host
- **Built**: 2026-10-14T05:20:51Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target simplelink --board launchpad/cc1312r7 --example examples/rpl-udp --output firmware/cc1312r7-launchpad/udp-client.cc1312r7-launchpad --source-file udp-client --local`

## tsch-node.cc1312r7-launchpad

- **Source**: contiki-ng commit `2a324403b45fa800df83a01990ee223cc2917c7b`
- **Source path**: `examples/6tisch/simple-node` (file: `node.c`)
- **TARGET**: `simplelink`
- **BOARD**: `launchpad/cc1312r7`
- **Make args**: `MAKE_MAC=MAKE_MAC_TSCH`
- **Local patch**: `node.c` coordinator guard removed — `if(node_id == 1) NETSTACK_ROUTING.root_start();` compiled on all targets (upstream gates it to COOJA/Z1), so node 1 self-selects TSCH coordinator in the emulator. Same patch as `firmware/cc1352r1-launchpad/tsch-node.cc1352r1-launchpad`; reverted after the build.
- **Toolchain**: host
- **Built**: 2026-10-14T05:20:53Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target simplelink --board launchpad/cc1312r7 --example examples/6tisch/simple-node --output firmware/cc1312r7-launchpad/tsch-node.cc1312r7-launchpad --source-file node --make-args "MAKE_MAC=MAKE_MAC_TSCH" --local`


## zephyr-hello-world.cc1312r7-launchpad

- **Source**: Zephyr (`zephyrproject/zephyr`) `v4.2.0-5538-g55473922756`, hal_ti `cc04902`
- **Source path**: `samples/hello_world`
- **BOARD**: `cc1312r7_lp`, out of tree: `tools/zephyr-boards/boards/ti/cc1312r7_lp` (selects `SOC_CC1352R7`)
- **Config**: STOCK sample, board defconfig only
- **Toolchain**: gnuarmemb (Arm GNU Toolchain 15.2.rel1)
- **Built**: 2026-10-14 by Joakim Eriksson; stripped with `--strip-debug`.
- **Build command**:
  `west build -b cc1312r7_lp samples/hello_world -- -DBOARD_ROOT=<csim>/tools/zephyr-boards`
