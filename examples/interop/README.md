# Contiki-NG with other IoT OSes

The Contiki-NG side of two demos where Contiki-NG shares one emulated 802.15.4
channel with another OS. Both run on the nRF52840 DK, and the other OS's image is
stock.

| Demo | Config | Contiki-NG side | Other side |
|---|---|---|---|
| UDP echo over 6LoWPAN | `configs/interop-contiki-zephyr.json` | `echo-zephyr/` | Zephyr `echo_server` sample with `overlay-802154.conf` |
| One RPL network | `configs/interop-contiki-riot.json` | `riot-root/` | RIOT `gnrc_networking`, driven from its shell |

```sh
./build/cngsim run configs/interop-contiki-zephyr.json
./build/cngsim run configs/interop-contiki-riot.json        # --ui to watch either
```

Build either Contiki-NG image with

```sh
tools/build-device-firmware.sh --local --contiki-dir <clone> --target nrf52840 --board dk \
    --example examples/interop/<name> --output firmware/nrf52840-dk/<name>.nrf52840-dk \
    --make-args "CONTIKI=<clone>"
```

The RIOT config's shell actions name the root by its link-local address,
`fe80::f6ce:3601:f1f4:1`. Contiki-NG builds it from the nRF52840's FICR device
ID, which the simulator seeds from the node id (`src/motes/arm_elf_mote.c`).
Give the root another id, or change that seeding, and the address changes: the
ping and UDP actions then fail, and the config's `wait` for the parent line
times out. The root prints `Tentative link-local IPv6 address: ...` at boot,
so the run's console shows the new one.

What each pairing needs, and why, is in the comment at the top of each source
file, in `docs/riot.md` (PAN ID, RPL storing mode, link-local traffic) and in
`docs/zephyr.md` (Zephyr's `nrf_802154` driver, which sends its own 802.15.4
ACKs).  Contiki-NG sends its datagrams to the Zephyr server as link-layer
broadcast: its neighbour cache keeps one IPv6 address per MAC address, and
Zephyr's link-local address takes it.
