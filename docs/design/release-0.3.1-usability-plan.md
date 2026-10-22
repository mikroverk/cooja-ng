# Release 0.3.1: Cooja-NG for people

Status: **implemented (2026-10-21)**, on branch `feat/cngsim`: U1–U9, with
the deviations below.  Decisions in §10 taken 2026-10-21; the binding ones
are in the refactor plan's Decisions Log.

**Where the implementation differs from this plan:**
- **The runner stays in `test/`.**  `cngsim` links
  `test/test_mixed_multinode.o` beside the library objects instead of the
  runner moving into the library (§3.3).  It is the same object either way;
  moving it is a separate refactor.
- **The quiet output is a filter in cngsim, not gates in the runner (§5).**
  `cngsim` runs the simulation with its stdout and stderr through a filter
  thread, so the runner's prints, and `test_runner`'s output, are
  untouched.  The format coupling is guarded by `tools/check-cngsim.sh`.
- **`--save-config` paths are unchanged (§4.1).**  It still writes paths as
  the config gave them; rewriting them relative to the output would change
  what existing scripts compare.  Config-relative lookup makes the common
  case (saving next to the original) work.
- **Not done:** the radio panel in the UI (§6.3 extras; the shell's `radio`
  covers it), and the nRF52840 console timestamps that run backwards (an
  engine timing change, left for its own PR with a baseline review).
- **Added on the way:** a `--ui` run holds open when its checks finish
  (pause plus a message) instead of ending under the browser, and the
  first Ctrl-C ends a `cngsim` run the normal way, with its verdict.

Cooja-NG works well for the people who built it: from a repo checkout, with
the right working directory, and with the mode names and flags already in
their heads. 0.3.1 is about everyone else: a Contiki-NG developer who used
Cooja classic and wants to run a simulation without reading the source.

## 0. Goal, bar and non-goals

**Bar for 0.3.1.** Someone who has never seen the repo should get from a
download to a running simulation they can watch in **three commands and
under five minutes**, and to their own Contiki-NG firmware in **one more
step**. The release adds `docs/quick-start.md`, and that document is the
acceptance test: CI runs every command in it from a fresh unpacked tarball,
outside any checkout.

**The simulation does not change.**
- No change to the kernel, the chips or the radio bus.
- `tools/check-baseline.sh` and the release workflow's byte-identity checks
  must stay green.
- `cngsim` (below) is a new frontend over the same `run_sim` path. Its
  *presentation* differs from `test_runner`; what it simulates does not.

**Which binary is for whom.** Both binaries run the same simulation code.
- `cngsim` is for **users**: running, watching, creating and checking
  simulations. That includes a user's own CI (`cngsim run sim.yaml`
  headless, with the exit codes).
- `test_runner` is for **developers and Cooja-NG's own CI**:
  - the emulator's unit and differential suites (`correctness`,
    `arm-decode`, `arm-jit`, `radio-medium`, …);
  - Contiki-NG's Cooja suite through `tools/run-cooja-tests.sh` and the
    proposed `SIMULATOR=cooja-ng` patch;
  - the determinism gates, which diff its full output.

  It may later become a hidden `cngsim dev …` subcommand. That is not
  needed for 0.3.1.

**`test_runner` stays** as the developer and CI binary, and its output stays
unchanged: the baselines diff that output, and the Contiki-NG suite and the
docs use it. The fixes in §4 that are plain bugs (silent Sky fallback,
`--help`, unknown modes returning 0) apply to it as well.

**Not in 0.3.1:** a desktop GUI, compiling firmware from inside the
simulator, a complete web-UI "File > New" (that is 0.4, see §6.4), Homebrew,
and Windows.

## 1. Where a newcomer hits trouble today

The survey was run on 2026-10-20, on main at `245dc04` plus the open PRs.

| # | Problem | Where |
|---|---|---|
| F1 | The command is `build/test_runner test cfg.yaml`. "test" is a mode name, and most of the other modes are unit-test suites. | `test/test_main.c:417-424` |
| F2 | `--help` prints nothing and exits 0. An unknown mode (`test_runner frobnicate`) also prints nothing and exits 0. | `test/test_main.c:494-495` |
| F3 | **Firmware paths are relative to the current directory, not the config file.** Every shipped config only works from the repo root: from anywhere else you get `Cannot open ELF file: firmware/sky/udp-server.sky`. | `src/sim/sim_config.c:217-224` |
| F4 | **The web UI page is read from `ui/index.html` relative to the current directory.** Outside the repo, `--ui` serves "No UI loaded". | `src/services/websocket_ui_service.c:25,37` |
| F5 | **The release tarball contains only the binary and four scripts**: no UI page, firmware, configs or docs. Its README says "Own simulations: build/test_runner test my-sim.yaml", which cannot work with what the tarball contains. | `.github/workflows/release.yml:244-260` |
| F6 | Default output for a 4-node Sky run is about 1600 lines: about 1200 `[PKT]`/`[HEX]`/`[RF]` lines on stderr, plus ELF patching details and per-chip counters on stdout (MSP430 byte queues are printed even for ARM-only runs). The node console lines are in there, but buried. | `configs/chain-4node-sky.yaml` |
| F7 | `--ui` runs at 10x and **keeps running past the duration** until Ctrl-C. It doesn't open a browser, and it doesn't start paused. | `test/test_mixed_multinode.c:2788,3086` |
| F8 | **An unknown firmware extension, including `.elf`, silently boots as a Tmote Sky.** The `board:` key in `mote_types` is parsed and then ignored. | `src/sim/sim_board.c:48-64`, `test_mixed_multinode.c:1462` |
| F9 | Contiki-NG builds produce `<app>.<TARGET>`. For `nrf`, `simplelink` and `zoul` that is not a csim extension, so users must rename (the Zephyr and RIOT docs say so). | `docs/zephyr.md:54`, `docs/riot.md:29` |
| F10 | **There is no way to create a setup except writing YAML by hand.** There are no templates, no layout generators (a 100-node grid lists 100 nodes), and no `new` command. | `configs/udgm-100node-grid.json` |
| F11 | **The web UI cannot add or remove nodes, send serial input, or save the setup.** Of its six commands, only node drag changes anything. `--save-config` and the shell's `save-config` exist, but the UI can't reach them. | `src/services/websocket_ui_service.c:82-125` |
| F12 | **Docs have drifted from the code.** The README advertises `--threads N`, which the parser rejects. `.cc430` is in the README table but not in the registry. `.nrf54l15-xiao` is in the registry but not in the README. `--pcap`, `--plugin` and `-d` are undocumented. `scripts/csc2json.py` is a stale copy of `tools/csc2json.py`. | `README.md:115,158`, `README.md:84` |
| F13 | There is no `make install`, PREFIX, package or install script. | `Makefile` |

None of these are engine problems. They are all in the frontend, the
packaging and the docs.

## 2. Cooja classic, side by side

The aim is to do the same jobs as Cooja classic, not to clone it. The input
is YAML instead of `.csc`, and the commands differ from Cooja's.

| A Cooja classic user… | Cooja classic | Cooja-NG today | Cooja-NG 0.3.1 |
|---|---|---|---|
| starts the simulator | `cd tools/cooja && ./gradlew run` | `make`, then `./build/test_runner …` from the repo root | `cngsim` on PATH, from any directory |
| opens a saved simulation | File > Open `.csc` | `test_runner test cfg.yaml --ui` (repo root only) | `cngsim run sim.yaml --ui` (opens the browser, starts paused) |
| runs it headless (CI) | `--no-gui sim.csc` | `test_runner test cfg.yaml` | `cngsim run sim.yaml` |
| creates a new simulation | File > New, Add motes (N, random/linear/ellipse) | write YAML by hand | `cngsim new` (templates, layouts in the schema), or edit live in the UI and **Save** |
| picks firmware | Create mote type > browse `.c` > Compile | build it yourself, rename it to the board extension | build it yourself. `board:` takes any `.elf`/`.nrf`/`.simplelink` file. `cngsim boards` shows the Contiki-NG TARGET/BOARD for each board. |
| watches it | Network, Mote output, Timeline plugins | web UI (network, consoles, packets, timeline, plugin panels) | same, plus serial input, add/remove/reboot and Save |
| imports an existing `.csc` | — | `python3 tools/csc2json.py` (Sky, Z1 and Cooja mote types) | `cngsim convert sim.csc sim.yaml` (same converter, via the command) |
| writes a test script | Script editor (JS) | `test.js_script` / `steps` / `validators` | same, documented in the quick-start |

Compiling from source at load time is Cooja classic's one real convenience
we don't have. §6.5 explains why it waits.

## 3. The command: `cngsim`

### 3.1 Name

**Decided (2026-10-21):** the command is **`cngsim`**, short and with no
hyphen. Before release, check that no common package already installs a
`cngsim`. It doesn't collide with `cooja`, which the Contiki-NG Docker image (`contiker cooja`) uses for
Cooja classic. The tarball stays `cooja-ng-vX.Y.Z-<target>.tar.gz` and the
project name stays Cooja-NG. The name is defined once, as a Makefile
variable, so renaming later is one line.

### 3.2 Subcommands

```
cngsim run <sim.yaml | example-name | firmware...> [options]   run a simulation
cngsim new [template] [-o sim.yaml] [options]                    write a new setup file
cngsim check <sim.yaml>                                          validate a setup and resolve its files, no run
cngsim examples                                                  list the bundled example simulations
cngsim boards                                                    list boards: extension, CPU, radio, Contiki-NG TARGET/BOARD
cngsim convert <sim.csc> [-o sim.yaml]                           Cooja .csc to YAML (needs python3)
cngsim help [command]  |  cngsim --help  |  cngsim --version
```

**`run` options.** The engine already supports all of these; this list
only gives them friendly names.

| Option | Meaning | Today |
|---|---|---|
| `--ui [PORT]` | Web UI: loopback only, prints the URL, opens the browser (`--no-open` to skip), starts **paused** at 1x. Starting must be one action: a big Play button in the page (or Space), or **Enter in the terminal**, which prints `Paused — press Play in the browser, or Enter here to start`. `--start` skips the pause. | `--ui` (no open, 10x, running) |
| `--duration 60s` / `-t` | simulated duration, with its unit (`60s`, `500ms`, `2m`); a bare number is refused (`-t` is ms in `test_runner`, `--wall-timeout` is s) | `-t ms` |
| `--seed N`, `--speed N\|max\|realtime` | unchanged | same |
| `--trace radio\|packets\|boot\|stats\|all` | turns the developer output back on, one kind at a time | always on |
| `-v` / `--verbose` | `test_runner`'s full output, unchanged | default |
| `-q` / `--quiet` | the verdict and errors only | — |
| `--pcap FILE`, `--save-config FILE`, `--shell`, `--script FILE`, `--gdb …`, `--plugin …` | unchanged, now listed in `--help` | partly hidden |

**`cngsim run` with firmware files and no config** keeps the quickest
one-off: `cngsim run hello-world.sky`. Several files get one node each,
placed on the default circle, like `mixed-multinode` today.

**Bare example names.** `cngsim run rpl-udp-sky` looks the name up in the
bundled example library (§4.3) when no file by that name exists. That makes
the quick-start's first command work before the user has any files at all.

**Exit codes** are the existing table (`test/test_main.c:70-75`): 0 pass,
1 assertion, 2 invalid request, 6 wall timeout, 7 cancelled; plus 130 when
Ctrl-C ends a run before it is decided (`INTERRUPTED`, with how far it
got). `cngsim help` documents them.

### 3.3 How it is built

`cngsim` is a second `main` (`src/cli/cngsim_main.c`, about 400 lines). It:
1. parses the subcommand;
2. rewrites friendly options into the existing `run_sim` argv;
3. sets an output profile (§5);
4. calls the same entry point `test_runner test` calls.

That needs `run_sim` and `run_mixed_multinode_test` to move out of `test/`.
`make lib` is already the boundary (`lib-link-check`), so the runner moves
into the library, or at least into an object both binaries link. One
simulation path means `cngsim run X` and `test_runner test X` cannot drift
apart, and CI checks that directly (§9 gate G2).

## 4. Making it work outside the repo

### 4.1 Paths are relative to the file that names them

- A relative `firmware:`, `secure_firmware:` or `js_script:` path in a
  config resolves against **the config file's directory**.
- For compatibility it falls back to the current directory, so every
  existing invocation keeps working:
  - `cngsim` prints a one-line deprecation note when it uses the fallback;
  - `test_runner` stays silent.
- Shipped configs move to `../firmware/...` so they work both ways.
- `--save-config` and the UI's Save (§6.3) write paths relative to the
  *output* file's directory. That keeps `config-roundtrip` lossless when the
  output goes to a different directory.
- Bare names are not searched for anywhere.
- No simulation output changes: this only affects which file is opened.

### 4.2 The web UI is built into the binary

- `ui/index.html` (about 70 KB, one file) is turned into a C array at build
  time by a small host tool built by the Makefile. Using `xxd` would add a
  vim dependency; `.incbin` is not portable across our compilers.
- `--ui` serves that copy, so the page works in any directory and from the
  tarball.
- For UI development, `CNGSIM_UI_PAGE=path/index.html` or `--ui-page FILE`
  serves a file from disk instead (reloaded on each GET). The existing
  regular-file and size checks (`load_page`) still apply.
- The Pages site and `?replay=` keep using `ui/index.html` directly; nothing
  changes there.

### 4.3 Data directory: bundled examples and firmware

- An installed `cngsim` finds its data at `<bindir>/../share/cooja-ng/`. In
  a checkout it uses the repo root, found the same way, by the binary's own
  path. `CNGSIM_DATA` overrides both.
- The data directory holds:
  - `examples/`: a curated set of about 10 commented YAML simulations.
    **Decided (2026-10-21):** these are new files written for teaching, with
    comments on every key a newcomer would change. The test configs are not
    moved or reused, so teaching text never depends on test fixtures.
    The `--ui` examples are tuned to look good in the UI.
    - hello-world (1 Sky)
    - nullnet broadcast
    - rpl-udp on Sky, nRF52840 and CC2538
    - a 4-node chain
    - a 25-node grid using a layout (§6.2)
    - the mixed-platform DAG (3 architectures)
    - TSCH
    - sub-GHz on the CC1352R1
    - the shell example (`--shell`)
  - `firmware/`: the tracked images, about 18 MB uncompressed (`firmware/cooja`
    is gitignored and stays out).
- `cngsim examples` lists the examples with their one-line `title`.

### 4.4 Choosing the board

- **An unknown extension is an error** that lists the known boards and says
  to add `board:` to the mote type. It no longer silently becomes a Sky.
  - It is an error in both `cngsim` and `test_runner`, starting with 0.3.1.
    Contiki-NG's suite only uses `.sky`, `.z1` and `.cooja`, so nothing in
    CI depends on the fallback. The release notes list it under "Changed —
    check these before upgrading".
- **`mote_types[].board` is honoured.**
  - It selects the board by its registry name (`sim_board_find`, which
    exists today but has no callers).
  - It is required when the extension is not a csim board extension:
    `.elf`, `.nrf`, `.simplelink`, `.zoul`, `.hex` is refused. This fixes F9
    without renaming files.
    ```yaml
    mote_types:
      - name: node
        board: nrf52840-dk
        firmware: ~/contiki-ng/examples/hello-world/build/nrf52840/dk/hello-world.nrf52840
    ```
  - If `board:` contradicts a known extension, that is an error.
  - The YAML writer emits `board:` whenever the extension doesn't imply it.
- `--board NAME` does the same on the command line for `cngsim run firmware…`.
- `cngsim boards` prints the table from the registry: extension, CPU, radio,
  and the Contiki-NG `TARGET`/`BOARD` that builds for it (from the
  PROVENANCE files). That replaces the hand-kept README table (F12): the
  README links to `cngsim boards` and CI checks the two agree.
- A Decisions Log entry records the end of the Sky fallback.

## 5. Output for people

`cngsim run` defaults to the **human** profile.
- **While running:** node console lines as `test_runner` already prints
  them (`   12.345 [Node 2/MSP430] …`), plus one header line:
  `cooja-ng 0.3.1 · 4 nodes · seed 42 · 180 s`.
- **When there is a test:** each step and validator is reported as it
  passes or fails.
- **At the end:** the verdict, simulated and wall time, and the speed ratio.
  Nothing else.
- **On a failure:** the last 20 console lines of the failing node. This is
  the one piece of debug context that is almost always useful.

The other output moves behind `--trace` (§3.2): RF/PKT/HEX traces, ELF
patch details, per-chip counters and the performance block. `-v` restores
`test_runner`'s output exactly.

**How it works.** The output profile is a flag the existing print sites
check. Most of them already sit behind `verbose`/`quiet`
(`test_mixed_multinode.c`); the `[PKT]`/`[HEX]`/`[RF]` stderr traces need a
gate. `test_runner`'s default profile is "everything", so its output, and
therefore every baseline, is byte-identical (gate G1).

## 6. Creating setup files

This is the question of whether the web UI can create setups. Today it can't
(F11). But everything an editor needs already exists in `sim_control`:
`add`, `remove`, `move`, `reboot`, `send`, `save_config`. The shell already
builds setups interactively (`add`, `move`, `radio`, `save-config`). So the
answer is yes, in two steps, with two simpler tools beside it for people who
prefer a file.

### 6.1 `cngsim new`: templates (0.3.1)

```sh
cngsim new                                   # asks: template, board, node count, layout (TTY only)
cngsim new rpl-udp --board nrf52840-dk -n 6 --layout grid -o my-sim.yaml
cngsim new blank --firmware build/hello-world.sky -o hello.yaml
```

- **Templates** are the bundled examples (§4.3) with parameters filled in.
  There is no separate template language: a template is an example YAML plus
  a few substitutions (board, firmware, count, layout, duration, seed).
- **The output is commented** so it reads like the docs. For example:
  `# tx_range: radio range in metres (UDGM)` and
  `# Each node needs a type from mote_types`.
- **The written file passes `cngsim check`.** Every template × board
  combination is tested in CI.
- **The interactive questions** are plain stdin prompts with defaults, only
  when stdin is a TTY. A pipe or flags never prompt.

### 6.2 Layouts in the schema (0.3.1)

A node entry may describe several nodes:

```yaml
nodes:
  - { type: server, id: 1, x: 0, y: 0 }
  - { type: client, count: 24, layout: grid, columns: 5, spacing: 20, origin: [20, 0] }
  - { type: client, count: 10, layout: random, area: [0, 0, 200, 200] }
  - { type: client, count: 8, layout: line, spacing: 30 }       # also: circle, radius
```

- These are Cooja's Add-motes positioners (random, linear, ellipse) plus a
  grid.
- **Expansion happens at load time**, in `sim_config.c`, before
  `sim_normalized_config_t`. The runtime never sees a layout.
- **IDs** continue from the previous entry unless `id:` is given.
- **`random` is deterministic.** It draws from its own generator, seeded
  from the config's `seed` and the entry index, never from the simulation's
  RNG, so adding a random layout doesn't perturb anything else.
- **`--save-config` writes the expanded nodes** (positions can have moved).
  `config-roundtrip` stays lossless (load → YAML → load gives the same
  normalized config).
- **New `config-reject` fixtures:**
  - `count` without `layout`
  - `layout: grid` without `columns`
  - an unknown layout
  - more than `MAX_SIM_NODES` after expansion
- **Decisions Log entry:** "layouts expand at load; the normalized config
  and the writer only know nodes".

Separately, raise or remove the silent truncation of `mote_types` at 8
(`sim_config.c:560`): make it an error, or grow the array.

### 6.3 Web UI: edit a running simulation, then Save (0.3.1)

**Decided (2026-10-21): in 0.3.1.** The core is **Add node**, **Save** and
the existing **drag to move**. Remove, Reboot, serial input and the radio
panel come with them if they stay as thin as the table suggests; otherwise
they move to 0.3.2 without holding up the release.

New WebSocket commands, each a thin wrapper over a `sim_control` call that
already exists:

| UI | ws command | Calls |
|---|---|---|
| "Add node" button: pick a **mote type from the config**, then click to place it | `{cmd:"add", type, x, y}` | `sim_control_add` |
| Node context menu: Remove / Reboot | `remove`, `reboot` | `sim_control_remove`, `_reboot` |
| A text input under each node's console | `{cmd:"send", node, data}` | `sim_control_send` |
| Radio panel: range, success ratios | `{cmd:"radio", …}` | **new** `sim_control_set_medium` (the shell writes `sim->radio_medium` directly today; this gives both one path) |
| **Save** button | `{cmd:"save"}` → a `{"type":"config","yaml":…}` reply, which the browser downloads as `sim.yaml` | `ops.save_config`, refactored to also write into a buffer |

**The full-state JSON** gains `mote_types[{name, board, firmware-basename}]`
so the UI can offer them.

**Security rules for this API.** It must not become a way to run code:
- **The UI can add only mote types the config declares.** It never takes a
  firmware path, because adding a `.cooja` node `dlopen`s it, which is
  native code execution. A browser on the loopback port must not get that.
- **Save returns YAML text and never writes a path the browser chose.** The
  file lands wherever the browser saves downloads.
- **Read-only mode.** With `--ui-bind` (any non-loopback address) the
  mutating commands are off unless `--ui-edit` is given. A UI shared on a
  LAN is read-only by default.
- `tools/check-ui-access.py` gains a case for each rule.

**Restart behaviour.** Restart today re-runs the in-memory config and drops
live edits (`test_mixed_multinode.c:2588-2614`). It stays that way, and the
button's tooltip says so: "Save first to keep your changes".

**Replay files.** `--ui-record` records the new commands' effects like any
other state change. Replay stays read-only.

### 6.4 Web UI: a full "File > New" (0.4, not 0.3.1)

This needs runner changes that are too big for a point release:
- **Start with zero nodes.** Today `firmware_count < 1` exits
  (`test_mixed_multinode.c:2390`), and v2 requires `nodes`.
- **Open or reload a config from the UI.** Restart jumps to `sim_restart`,
  after the config is loaded (`:2522`); it would have to re-run loading.
- **Choose firmware from the data directory's library** (§4.3): a
  server-side list of the bundled images plus the files under the config's
  directory. Never a free path, for the reason in §6.3.
- **Create mote types in the UI** from that list.

With 6.3 in place this is mostly runner restructuring. The UI parts are
small. `cngsim ui` (no file) becomes the entry point.

### 6.5 Why not compile firmware from the simulator (later)

Cooja classic compiles the mote type's `.c` at load time. Doing that needs:
- a Contiki-NG checkout;
- a cross toolchain per board, or Docker;
- the `build:` metadata the schema already carries (runtime-inert today,
  read only by `tools/build-test-firmware.sh --from-json`).

Behind an explicit `cngsim build sim.yaml` it would be a thin wrapper over
the existing build scripts, so it is a good 0.4 candidate. It never runs
implicitly from `run`: a simulation that starts a compiler is not
reproducible. For 0.3.1 the quick-start shows the two `make` lines and the
`board:` key instead.

## 7. Packaging and install

**The release tarball** (release.yml Package step):

```
cooja-ng-v0.3.1-<target>/
  bin/cngsim
  bin/test_runner          # kept for the Contiki-NG suite and existing scripts
  share/cooja-ng/examples/  share/cooja-ng/firmware/
  share/cooja-ng/tools/    # run-cooja-tests.sh, csc2json.py, build-*.sh
  share/doc/cooja-ng/      # quick-start.md, shell.md, test-format.md, ui-replay.md, CHANGELOG.md
  README.md                # points to the quick-start
```

- `tools/run-cooja-tests.sh` and `docs/contiki-ng-testing.md` reference
  `build/test_runner` in the old layout. Keep a `build/test_runner` symlink
  in 0.3.1 and update the doc's paths. The proposed Contiki-NG
  `SIMULATOR=cooja-ng` patch (`docs/contiki-ng-testing.md` §6) pins v0.3.0,
  so it is unaffected until it moves to 0.3.1.
- Firmware adds roughly 5 MB compressed. The one-line `curl … | tar xz`
  stays.

**Install from source**

```
make install PREFIX=~/.local     # or /usr/local; DESTDIR honoured
```

This installs the same layout. It lets people who build from source run
`cngsim` from anywhere.

**Release workflow additions**
- **The quick-start test** (gate G3): unpack the tarball into an empty
  directory, then run every command in `docs/quick-start.md` from another
  directory. That includes `cngsim run rpl-udp-sky`, `cngsim new … && cngsim
  check`, and `cngsim run --ui --no-open` with a curl for `GET /` (it must
  return the real page, not "No UI loaded").
- **The existing firmware smoke test** then runs from the tarball, not the
  checkout, which also proves F5 is fixed.

**Later (0.4):** a Homebrew tap (`brew install mikroverk/tap/cooja-ng`) and
a Docker image with `cngsim` next to the Contiki-NG toolchain, so the
§6.5 build path works without a local toolchain.

## 8. Docs

### 8.1 `docs/quick-start.md` (new)

The quick-start is written for the Cooja classic user and the newcomer. It
stays short, and every command in it runs in CI (G3). A first draft is in
[`../quick-start.md`](../quick-start.md); it describes the planned commands
and is marked as a draft.

1. **Install** (1 command): `curl … | tar xz`, and add `bin/` to PATH.
   Building from source is the alternative: `make && make install`.
2. **Run your first simulation:** `cngsim run rpl-udp-sky`. It explains what
   the output lines mean, and what pass/fail and the exit code are.
3. **Watch it:** `cngsim run rpl-udp-sky --ui`. Then a tour: Play, drag a
   node out of range and watch the route break, the console, the Packets
   tab, the timeline.
4. **Make your own setup:** `cngsim new rpl-udp -n 6 --layout grid -o
   my.yaml`, then open the YAML and change the range, the duration and a
   node position. Or edit in the UI and press Save.
5. **Use your own Contiki-NG firmware:**
   - `make TARGET=sky` for Sky. `cngsim boards` shows the TARGET/BOARD for
     the other boards.
   - Point `firmware:` at the output and add `board:` when the extension
     isn't a board name.
6. **Add a check:** `test.steps` / `validators`, and a Cooja-style JS
   script, with the exit code for CI.
7. **Coming from Cooja classic:** the table from §2, plus `cngsim convert
   sim.csc`.
8. **Next:** `docs/shell.md`, `docs/test-format.md`, the boards list, and
   running Contiki-NG's own test suite (`docs/contiki-ng-testing.md`).

### 8.2 README and other fixes

- **README Quick start:** replace the 10-command block with the three
  quick-start commands and a link. Move the unit-test commands to
  CONTRIBUTING.md.
- **Fix F12:**
  - remove `--threads`;
  - document `--pcap`, `--plugin` and `-d` (in `--help` and `docs/shell.md`);
  - make the README boards table match the registry, checked in CI from
    `cngsim boards`;
  - delete the stale `scripts/csc2json.py`.
- **Site:** add an "Install" card with the three commands, linking to the
  quick-start.
- **Release tarball README:** the real commands, not the `my-sim.yaml` line.

## 9. Milestones and gates

Each milestone is one PR, in this order. Later ones depend on earlier ones.

| | Milestone | Done when |
|---|---|---|
| U1 | **Bugs first.** `--help` and unknown mode exit 2 with usage text (both binaries). Unknown firmware extension is an error (both binaries). `mote_types` truncation becomes an error. README `--threads` / boards drift (F12). | `config-reject` fixtures; `test_runner frobnicate` exits 2 |
| U2 | **Paths relative to the config** (§4.1), shipped configs moved to `../firmware`, the writer writes relative to its output | run every `configs/*` from `$TMPDIR` and from the repo root: identical output; `config-roundtrip` green |
| U3 | **Embedded UI page** (§4.2) | `--ui` from `$TMPDIR` serves the real page; `check-ui-access.py` green |
| U4 | **`board:` honoured** (§4.4), `cngsim boards` data in the registry | `.elf` + `board:` boots each ARM board; contradiction rejected |
| U5 | **`cngsim` binary**: `run` / `check` / `examples` / `boards` / `help`, the human output profile (§5), `--ui` opens paused and pauses at the end | G1, G2 |
| U6 | **Layouts in the schema** (§6.2) and **`cngsim new`** (§6.1) | every template × board passes `check` and runs; random layout identical across runs |
| U7 | **UI Add node and Save** next to the existing move (§6.3); Remove/Reboot/serial/radio if cheap, with `sim_control_set_medium` | save → reload → same normalized config; read-only on `--ui-bind`; access-check cases |
| U8 | **Packaging**: tarball layout, `make install`, data-dir lookup, examples library, `cngsim convert` | G3 |
| U9 | **Docs**: quick-start, README, site card, CHANGELOG `[0.3.1]`, `CSIM_VERSION` 0.3.1 | G3 runs the quick-start verbatim |

**Gates**
- **G1, determinism.**
  - `tools/check-baseline.sh` is green against the merge-base after every
    milestone.
  - `test_runner`'s output is byte-identical on all workloads.
  - The release workflow's byte-identity across platforms, with each JIT
    on and off, is unchanged.
- **G2, one simulation path.** For every bundled example and every
  `configs/*`, the console lines of `cngsim run X -v` equal
  `test_runner test X`'s.
- **G3, the quick-start test** (§7). It runs in CI on the tarball.

**Cut line.** All of U1–U9 are in 0.3.1. U1–U6, U8 and U9 make it "easy to
run"; U7 makes it "easy to create". If time runs short, only U7's extras
(Remove, Reboot, serial input, radio panel) move to 0.3.2. Add node, Save
and move stay.

**What else ships in 0.3.1.** The CHANGELOG's Unreleased section already
holds the CC1352R1 work (#75) and its fixes. The CC1312R7 (#77) joins if it
merges first. Those are engine additions, so 0.3.1 is "0.3.0 + boards +
usability". The 0.x SemVer note allows that.

## 10. Decisions

All decided on 2026-10-21:

1. ~~**Name.**~~ **Decided:** `cngsim` (§3.1).
2. ~~**`run --ui`: start paused or running?**~~ **Decided (2026-10-21):**
   start paused, as Cooja classic does, provided starting is one action
   (Play/Space in the browser, Enter in the terminal, `--start` to skip the
   pause).
3. ~~**The examples library.**~~ **Decided:** new, commented teaching files
   (§4.3).
4. ~~**Unknown extension in `test_runner`.**~~ **Decided:** an error in both
   binaries from 0.3.1 (§4.4).
5. ~~**U7 (UI editing).**~~ **Decided:** in 0.3.1. Add node, Save and move
   are the core (§6.3).

## 11. Decisions Log entries this plan adds

These go into `refactor-plan.md`'s Decisions Log when each lands:
- **`cngsim` is a frontend.** It shares the simulation path with
  `test_runner` (one `run_sim`). Presentation differs, simulation never does.
- **Config-relative paths:** paths in a config resolve against the config's
  directory, with a cwd fallback.
- **No silent Sky fallback:** an unknown firmware extension is an error, and
  `board:` names the board.
- **Layouts expand at load:** the normalized config and the writer only know
  nodes.
- **The UI never names files:** it adds only declared mote types, and Save
  returns text. Mutation is off on a non-loopback bind unless `--ui-edit`.
