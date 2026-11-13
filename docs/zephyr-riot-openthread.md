# Zephyr, RIOT and OpenThread: a quick start

Cooja-NG runs the same firmware image you would flash on a board. This page
gets you from nothing to a running Zephyr, RIOT or OpenThread network in a
few minutes. Each section has three steps:

1. **Try it:** run an example. The firmware is included, so you don't need a
   toolchain.
2. **Watch it:** add `--ui` and the network opens in your browser.
3. **Use your own build:** point `cngsim` at the image you built.

You need `cngsim` first: see [quick-start.md](quick-start.md) (download a
release and put its `bin/` on your PATH). `cngsim examples` lists every
example.

**Board names.** `--board` and a setup file's `board:` take your OS's own
board name as it is: `nrf52840dk/nrf52840` for Zephyr, `nrf52840dk` for
RIOT. Cooja-NG's own name for the same board is `nrf52840-dk`. All three
mean the same board; `cngsim boards --os` lists them all.

**In CI.** Every run ends with `PASSED` or `FAILED` and exits 0 or 1. A
ready-to-copy GitHub Actions job is in
[quick-start.md](quick-start.md#7-put-it-in-ci).

Support for these three is newer than for Contiki-NG. What works and what
doesn't is in each OS's full guide, linked at the end of its section.

## Zephyr

**1. Try it.** Zephyr's own `echo_server` and `echo_client` samples, one node
each, talking over 802.15.4:

```sh
cngsim run zephyr-echo
```

The run ends with `PASSED` once the server has echoed twenty messages.
Another example has Zephyr answer a Contiki-NG node:

```sh
cngsim run contiki-zephyr
```

**2. Watch it.**

```sh
cngsim run contiki-zephyr --ui
```

Your browser opens with the network paused. Press Play, then click a node to
see its console.

**3. Use your own build.** Build for the nRF52840 DK as usual, then run the
ELF. `cngsim` reads the board from your build directory:

```sh
west build -b nrf52840dk/nrf52840 samples/hello_world
cngsim run build/zephyr/zephyr.elf -t 2s
```

You should see the banner on node 1's console:

```
    0.000 [Node 1/nrf52840-dk Zephyr] Hello World! nrf52840dk/nrf52840
```

`-t 2s` stops after two simulated seconds; nothing is checked, so the run
ends with `DONE`. To get `PASSED` or `FAILED`, write a setup file with a
check: `cngsim new zephyr-echo -o echo.yaml` writes one for the echo
samples, and you point its `firmware:` lines at your builds.

Two ELFs on the command line give two nodes on the same radio:

```sh
cngsim run server/build/zephyr/zephyr.elf client/build/zephyr/zephyr.elf --ui
```

The CC1352R1 LaunchPad (`cc1352r1_launchxl`) also works.

**Coming from native_sim?** Use Cooja-NG where you would use native_sim,
but with the real nRF52840 image and radio driver instead of a host
build. There is no twister integration yet; a setup file with a check is
the test.

**Full guide:** [zephyr.md](zephyr.md).

## RIOT

**1. Try it.** Three RIOT nodes in a line build an RPL network from RIOT's
shell, and the last one pings the first through the middle one:

```sh
cngsim run riot-rpl
```

Or a RIOT node joining a network that a Contiki-NG node runs:

```sh
cngsim run contiki-riot
```

**2. Watch it.**

```sh
cngsim run riot-rpl --ui
```

Click a node to see RIOT's shell, and type into it as you would on a board.

**3. Use your own build.** RIOT builds with `make` and `arm-none-eabi-gcc`.
From the top of a RIOT checkout:

```sh
make -C examples/networking/gnrc/networking BOARD=nrf52840dk
cngsim run examples/networking/gnrc/networking/bin/nrf52840dk/gnrc_networking.elf -n 3 --shell
```

`cngsim` reads the board from the `bin/nrf52840dk/` directory, and `-n 3`
starts three nodes. `--shell` is your `make term`: type `run` to start
the simulation, then `console 3` to talk to node 3's RIOT shell (`ifconfig`,
`ping`, ...). `~.` on its own line takes you back.

Networking is tested on `nrf52840dk` only, for now.

To get a network of your own size, write a setup file and run it:

```sh
cngsim new riot-rpl --board nrf52840dk -n 6 --layout line -o dodag.yaml
cngsim run dodag.yaml --ui
```

**Full guide:** [riot.md](riot.md). It also covers rehearsing a FIT IoT-LAB
experiment before you book the testbed.

## OpenThread

**1. Try it.** OpenThread's CLI example on three CC2538 boards. They form a
Thread network: the first node becomes the leader, the others join as its
children, and a ping to all nodes is answered.

```sh
cngsim run thread-cc2538
```

`thread-nrf52840` does the same on nRF52840 DKs. It runs slower, about half
real time.

**2. Watch it, and type into it.**

```sh
cngsim run thread-overlay --ui
```

The map draws the Thread roles: leader, routers and a sleepy child. Halfway
through, part of the network moves out of range, forms its own partition,
and later merges back.

Each node runs the OpenThread CLI. Click a node and type `state`,
`router table` or `ipaddr` into its console, as you would on a board. From a
terminal instead:

```sh
cngsim run thread-cc2538 --shell
> run 12s
> console 1
state
leader
```

Coming from OTNS? [openthread.md](openthread.md#coming-from-otns) maps its
commands to Cooja-NG's.

**3. Use your own network or build.** Write a setup file for a network of
your size:

```sh
cngsim new thread --board nrf52840-dk -n 5 --layout line -o thread.yaml
cngsim run thread.yaml --ui
```

Or build `ot-cli-ftd` yourself. On the nRF52840, use the software-crypto
build, because the CryptoCell is not emulated:

```sh
git clone --recurse-submodules https://github.com/openthread/ot-nrf528xx
cd ot-nrf528xx && OT_CMAKE_NINJA_TARGET=ot-cli-ftd ./script/build nrf52840 soft_crypto -DOT_TCP=OFF
cngsim run build/bin/ot-cli-ftd --board nrf52840-dk -n 3 --ui
```

Each node runs the OpenThread CLI, so you set up the network by typing the
same commands you would on real boards.

**Full guide:** [openthread.md](openthread.md). It covers the CC2538 build,
the dataset commands, and what changes if you come from OTNS.

## Next steps

- `cngsim run <setup> --shell` gives you a command line for the running
  network: cut links, add loss, drift clocks ([shell.md](shell.md)).
- A setup file can wait for a line on a node's console and pass or fail,
  which makes it a test for CI ([test-format.md](test-format.md)).
- `cngsim boards --os` shows the board names each OS uses.
