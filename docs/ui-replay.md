# Replaying a recorded run in the web UI

`ui/index.html?replay=FILE.json` plays a recorded run from a static file: no simulator, no
WebSocket, no server beyond whatever serves the two files. Play/Pause, the speed slider and a
scrubber in the header drive the player; Restart seeks to the start. Dragging nodes and the
live commands are inert in this mode. The player only loads files from its own origin (a path
relative to the page, or an absolute URL on the same host): a link cannot point it at a file
someone else controls.

The file is the UI's own wire protocol as one JSON document:

```json
{ "format": "cooja-ng-ui-replay/1",
  "run":    { "simulator": "cooja-ng", "version": "…", "scenario": "…", "title": "…", "seed": 42, "duration_ms": 90000 },
  "full":   { "type": "full", "sim_time_ms": 0, "nodes": [...], "radio": {...}, "stats": {...} },
  "deltas": [ { "0": 100, "1": [rf, uart, frames, collisions, speed_x10, paused],
                "2": {"3": 1}, "3": {"3": [1,0,0]}, "4": {"1": 4628},
                "5": {"3": ["[INFO: Main ] Starting Contiki-NG…"]},
                "6": [[615575, 3, 1, 0], [4624709, 1, 7, 1, "DATA N1 -> bcast seq 198 len 97 ch 26"]] }, ... ] }
```

A delta uses the numeric keys `handleCBORDelta` reads, as strings: `"0"` sim_time_ms, `"1"`
stats, `"2"` radio state per node (off=0 on=1 tx=2 rx=3 intf=4), `"3"` LEDs per node, `"4"`
last_tx_ms per node, `"5"` console lines per node, `"6"` timeline entries
`[t_us, node, type, value, summary?, frame?]` with type off, on, tx, rx, intf, led_on, led_off,
frame, log = 0..8. A frame entry may carry the frame itself after its summary (an empty summary
if it has none): the MAC frame and its FCS as a pcap records it, a CBOR byte string on the wire
and hex in a recording, which the Packets tab dissects. Recordings without it still play, with
the summary's columns. Deltas are applied in order; seeking backwards rebuilds from `full`.

Producing the file:

- `--ui-record FILE.json` on any `test_runner test` / `mixed-multinode` run records the stream the
  web UI would receive: the first full state, then every delta (100 ms of simulation time apart)
  transcoded from its CBOR, with the plugin panels of the same tick (energest, …) as key `"p"`.
  Without `--ui` the run stays headless and unpaced — a 90 s three-ISA RPL run records in well under
  a second — and the simulation is unchanged: stdout is byte-identical with and without the flag,
  and the recorder's two status lines (`--ui-record: recording …`, `UI recording: N deltas written
  …`) go to stderr. One exception: the packet-frame and radio TX/RX start/end observer events are
  produced only while a UI or a recording is watching (they cost a packet decode per frame), so a
  plugin that subscribes to them (`plugins/packet_sink.c`) sees them with `--ui-record` as it does
  with `--ui`. The radio-state stream an energy service uses does not depend on either. With
  `--ui` the browser and the file see the same stream.

  The path is checked (created if missing, not yet truncated) before the run starts, so one that
  cannot be written ends the run there, and a run that fails before recording begins leaves an
  existing recording at that path as it was. The recording ends when the run does — its end
  (`-t`/`timeout_ms`), a test's verdict, a wall timeout, the first restart (the document keeps the
  run before it, since a replay has no way to rewind simulation time) — with a last delta for what
  happened since the previous one, and a full state even for a run too short to have had a
  broadcast. A tick in which simulation time has not moved (a paused run, an idle shell prompt)
  adds nothing; a browser connecting, or the shell's `ui` command, loses nothing from the file.
  Console text is written as UTF-8 (invalid bytes become U+FFFD), and plugin panels that are not
  valid JSON are left out of the recording with a warning. `run.seed` is the run's seed from the
  config or `--seed`. If writing the file fails (a full disk) or a delta cannot be transcoded, the
  run says so and exits 1: the file is not the run. A run killed by a signal leaves the document
  unterminated.

  ```sh
  ./build/test_runner test configs/test-mixed-platform-rpl.yaml -q --ui-record mixed.json
  ```

  What a recording cannot show yet: node moves and nodes added or removed at run time (positions
  travel only in the full state, and the recording keeps the first one), and the per-node cycle
  counters, which are also full-state only.
- `tools/rundir2ui.py RUN_DIR` in the agent-sim-protocol repo converts a `--run-dir` directory
  (`events.ndjson`, `scenario.replay.yaml`, `result.json`) into the same document, one delta per
  100 ms of simulation time that had events.

The player plays at the speed slider's ratio while the tab is visible; a hidden tab pauses it (the
browser stops animation frames), and it carries on from where it stopped when shown. Play after the
end starts over.

Serving a demo: put `ui/index.html` and the replay file in one directory and serve it
statically (`python3 -m http.server`, GitHub Pages, any file host).
