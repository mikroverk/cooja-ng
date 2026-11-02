# Quick start

Cooja-NG simulates networks of Contiki-NG nodes (also Zephyr and RIOT) on
emulated hardware: Tmote Sky, CC2538, nRF52840, nRF54L15, CC1352R and more.
It runs the real firmware images you would flash onto the boards, usually
hundreds of times faster than real time, and you can watch the network in
your browser.

If you have used Cooja before, see [Coming from Cooja](#coming-from-cooja).

## 1. Install

Download a release for your machine and put its `bin/` on your PATH. On
Linux (x86-64):

```sh
curl -L https://github.com/mikroverk/cooja-ng/releases/download/v0.4.0/cooja-ng-v0.4.0-linux-x86_64.tar.gz | tar xz
export PATH=$PWD/cooja-ng-v0.4.0-linux-x86_64/bin:$PATH
cngsim --version
```

On a Mac, use `macos-arm64` (Apple Silicon) or `macos-x64` (Intel) in
place of `linux-x86_64`. The package carries everything `cngsim` needs:
the examples, the firmware they run, and these guides (`docs/`).

**From source** (Linux, macOS, FreeBSD with `gmake`): a C compiler is
enough. GNU Lightning is optional and makes it faster.

```sh
git clone https://github.com/mikroverk/cooja-ng && cd cooja-ng
make && make install PREFIX=~/.local       # cngsim in ~/.local/bin
```

`build/cngsim` also runs straight from the checkout. Warnings are errors
in this build; if a compiler newer than the ones CI uses stops on one,
`make WERROR=0` builds anyway (and the warning is worth an issue).

## 2. Run your first simulation

```sh
cngsim examples              # what is there
cngsim run rpl-udp-sky
```

`rpl-udp-sky` has two Tmote Sky nodes. Node 2 joins node 1's RPL network
and sends it a UDP request every ten seconds or so; node 1 answers each one:

```
cooja-ng 0.4.0 · RPL-UDP on two Tmote Skys · 2 nodes · seed 42 · 120 s
    ...
    9.423 [Node 2/MSP430] [INFO: App       ] Not reachable yet
   19.000 [Node 2/MSP430] [INFO: App       ] Sending request 0 to fd00::212:7401:1:101
  ✓ "Sending request" on node 2 (19.000 s)
   19.019 [Node 1/MSP430] [INFO: App       ] Received request 'hello 0' from fd00::212:7402:2:202
  ✓ "Received request" on node 1 (19.019 s)
   19.063 [Node 2/MSP430] [INFO: App       ] Received response 'hello 0' from fd00::212:7401:1:101
    ...
  ✓ "Received response" ×3 on node 2 (38.203 s)
PASSED · 38.2 s simulated in 0.04 s (851x real time)
```

How to read it:
- Each line is something a node printed on its serial console. It starts
  with the **simulated** time in seconds, then the node it came from.
- The ✓ lines are the example's checks. The run ends as soon as all of
  them have passed.
- `cngsim` exits with 0 when every check passes and with 1 when one fails,
  so a run can go straight into a CI job. Ctrl-C ends a run early: it says
  `INTERRUPTED` and how far the simulation got, and exits with 130.
  `cngsim help` lists the other exit codes.

The same seed always gives the same run, down to the nanosecond.
`--seed 7` gives another run, just as reproducible. `-q` prints only the
verdict; `-v` prints everything the simulator knows (radio frames, chip
counters).

## 3. Watch it

```sh
cngsim run rpl-udp-sky --ui
```

Your browser opens at `http://127.0.0.1:8080/` with the simulation
paused. Press **Play** (or Space) in the browser, or Enter in the
terminal; add `--start` to skip the pause. It runs at real time; the speed
slider goes faster. Things to try:

- **Drag node 2** out of node 1's range (its circle). Its console reports
  `Not reachable yet` until you bring it back.
- **Consoles:** one per node, in the sidebar. Type a line in the box under
  a node's console to send it to that node.
- **Packets:** every radio frame, decoded (802.15.4, 6LoWPAN, IPv6, RPL,
  UDP).
- **Timeline:** radio activity per node over time. Scroll to zoom, drag
  to pan.

When the checks have passed, or the simulated time is up, the simulation
pauses and the terminal says so. Play runs on; Ctrl-C ends the run with its
verdict.

The UI takes commands, so it listens on loopback only and refuses WebSocket
connections from other sites' pages. `--ui-bind 0.0.0.0` (or one IPv4
address) makes it reachable from the network. It also turns off the Host
check that stops DNS rebinding, so any web page open in your own browser can
drive the run too, even with the port firewalled. Anyone who reaches such a
UI can pause, play, change the speed, restart the run and move nodes. Adding,
removing and rebooting nodes, sending a node a line, and Save also need
`--ui-edit` (on loopback they are always on). With `--ui-edit`, Save sends
the setup, file paths included, to every page connected at the time.

## 4. Make your own setup

A simulation is a YAML file. Start one from a template:

```sh
cngsim new rpl-udp --board sky -n 9 --layout grid -o my-net.yaml
cngsim run my-net.yaml --ui
```

(`cngsim new` with no arguments asks.) `my-net.yaml` comes out commented;
the parts you will want to change:

```yaml
version: 2
title: "My RPL network"
timeout_ms: 120000          # how long to simulate, in simulated milliseconds
seed: 42                    # the same seed gives the same run

medium:
  type: udgm                # unit-disk graph, as in Cooja: in range = received
  tx_range: 45              # metres

mote_types:                 # each firmware image, with a name
  - { name: server, firmware: udp-server.sky }
  - { name: client, firmware: udp-client.sky }

nodes:
  - { type: server, id: 1, x: 0, y: 0 }
  - { type: client, id: 2, count: 8, layout: grid, columns: 3, spacing: 30, x: 30, y: 0 }

test:
  steps:
    - { wait: "Received response", node: 4 }
```

- Paths are relative to the YAML file. `cngsim new` names the firmware
  that comes with cooja-ng by its absolute path, so the file still runs
  after you move it. Saving a setup (`--save-config`, or Save in the
  browser) keeps your own paths relative.
- One node entry can place many nodes: `count` plus a `layout`, starting
  at `x`, `y`:
  - `grid` takes `columns` and `spacing`;
  - `line` takes `spacing`;
  - `circle` takes `radius`;
  - `random` takes `width` and `height`. It follows `seed`, so it is the
    same every time.
- `cngsim check my-net.yaml` loads a file the way `run` would, without
  running it. A misspelt key is an error, not something silently ignored.

**In the browser:**
- **Add node** at the top: pick a mote type, then click where the node
  goes.
- **Right-click a node** to reboot or remove it. Drag nodes to move them.
- **Save** downloads the setup as it is now, as a YAML file that
  `cngsim run` takes.

The [examples](../examples/) are commented starting points too:
`cngsim examples` prints where they are. A copy runs from anywhere: the
`../firmware/...` it names is found among the bundled firmware, with a
note, until you point it at your own.

## 5. Use your own Contiki-NG firmware

Build the app as you would for the board. For a Tmote Sky:

```sh
cd contiki-ng/examples/hello-world
make TARGET=sky                 # writes hello-world.sky
```

and name the image in your setup:

```yaml
mote_types:
  - { name: hello, firmware: ~/contiki-ng/examples/hello-world/hello-world.sky }
```

An image named `<app>.<board>` runs as that board (`.nrf52840` runs as
the nRF52840 DK, unless `board: nrf52840-dongle` says otherwise). For any
other name (Contiki-NG's `.nrf`, `.simplelink` and `.zoul`, or a plain
`.elf`), say which board it is:

```sh
make TARGET=nrf BOARD=nrf54l15/dk         # writes hello-world.nrf
```

```yaml
mote_types:
  - { name: hello, board: nrf54l15-dk,
      firmware: ~/contiki-ng/examples/hello-world/hello-world.nrf }
```

An image run on its own, without a setup file, takes the board from the
command line: `cngsim run hello-world.nrf --board nrf54l15-dk`.

`cngsim boards` lists the boards and the `TARGET`/`BOARD` that builds for
each:

```
board                                         cpu                 radio                       build with (Contiki-NG)
sky                  Tmote Sky                MSP430F1611         CC2420 2.4 GHz              TARGET=sky
cc2538dk             TI CC2538DK              Cortex-M3           802.15.4 2.4 GHz            TARGET=cc2538dk
nrf52840-dk          nRF52840 DK              Cortex-M4F          802.15.4 2.4 GHz            TARGET=nrf52840 BOARD=dk
nrf54l15-dk          nRF54L15 DK              Cortex-M33 + RV32E  802.15.4 2.4 GHz            TARGET=nrf BOARD=nrf54l15/dk
cc1352r1-launchpad   TI LAUNCHXL-CC1352R1     Cortex-M4F          sub-GHz + 2.4 GHz RF core   TARGET=simplelink BOARD=launchpad/cc1352r1
...
```

Zephyr and RIOT images work the same way: see [zephyr.md](zephyr.md) and
[riot.md](riot.md); OpenThread, on the CC2538 and nRF52840:
[openthread.md](openthread.md). Native Cooja motes (`TARGET=cooja`, `.cooja`) run
too.

## 6. Add checks

A `test:` section turns a simulation into a test with a pass/fail exit
code:

```yaml
test:
  steps:                                  # in order; each waits for a console line
    - { wait: "Sending request", node: 2 }
    - { wait: "Received response", node: 2, count: 3, timeout_ms: 60000 }
  fail_on: ["ERROR", "assertion"]         # any node printing these fails the run
  validators:                             # counted over the whole run, judged at the end
    - { pattern: "Received request", node: 1, min_count: 2 }
```

Cooja test scripts (JavaScript with `YIELD`, `WAIT_UNTIL`,
`log.testOK()` ...) run too: `js_script: my-test.js`, or the script inline
as `js_script_inline:`. The whole format is in
[test-format.md](test-format.md).

For an interactive session with breakpoints, `expect` and scripted
console tests, use `cngsim run my-net.yaml --shell` or `--script
check.cnsh` (see [shell.md](shell.md)).

## Coming from Cooja

| In Cooja | In Cooja-NG |
|---|---|
| `./gradlew run`, File > Open `sim.csc` | `cngsim run sim.yaml --ui` |
| `--no-gui sim.csc` | `cngsim run sim.yaml` |
| File > New, Add motes (random / linear / ellipse) | `cngsim new`; `count:` + `layout:` in the YAML; **Add node** in the browser |
| Create mote type > Compile | build with `make TARGET=…`, name the image in `mote_types` |
| Network, Mote output, Timeline, Radio messages | the browser's network view, consoles, Timeline and Packets |
| Simulation > Save | **Save** in the browser, or `--save-config out.yaml` |
| Script editor | `test:` steps, or your Cooja JS script as `js_script:` |
| An existing `.csc` | `cngsim convert sim.csc` (needs python3; Sky, Z1 and Cooja motes) |

Contiki-NG's own Cooja test suite runs under Cooja-NG too:
[contiki-ng-testing.md](contiki-ng-testing.md).

## Next

- `cngsim help` and `cngsim help run`: every command and option.
- [test-format.md](test-format.md): the YAML format in full.
- [shell.md](shell.md): the interactive shell and command scripts.
- [ui-replay.md](ui-replay.md): record a run, replay it in a browser.
