#!/usr/bin/env python3
"""check-energest-agreement.py — does the simulator's time in state agree with
the firmware's own Energest?  (docs/energest.md "Validation")

Runs a config whose firmware is built with Contiki-NG's simple-energest
(firmware/src/rpl-udp-energest) and the energest plugin with a `trace:` file,
or reads such a trace.  The firmware prints a period summary every 10 s of
its rtimer:

    [INFO: Energest  ] --- Period summary #3 (10 seconds)
    [INFO: Energest  ] Total time  :     327680
    [INFO: Energest  ] CPU         :      12345/    327680 (37 permil)
    [INFO: Energest  ] LPM         : ...
    [INFO: Energest  ] Deep LPM    : ...
    [INFO: Energest  ] Radio Tx    : ...
    [INFO: Energest  ] Radio Rx    : ...
    [INFO: Energest  ] Radio total : ...

Each summary is the firmware's rtimer tick count per state over the period
that ended when energest_flush() ran, just before the header line was
printed.  The trace stamps that header with the cycle the CPU wrote its last
byte (the console clock: the UART model sends at once), a few microseconds
after the flush; the period is the `Total time` ticks before it, which the
simulation keeps exact.

For each period and node it sums the trace's state changes over that window
and prints both, in ticks, with the difference:

    firmware Radio Tx      vs  simulator radio tx, less the automatic ACKs
    firmware Radio Rx      vs  simulator radio listen + rx + interfered + ACKs
    firmware CPU           vs  simulator cpu active
    firmware LPM+Deep LPM  vs  simulator cpu low-power (lpm0..4, sleep, pm1..3)

An automatic ACK (a TX entered straight from RX: the radio answers a frame on
its own, CC2538 AUTOACK) is real transmit time — the plugin's energy charges
it — but Contiki's energest cannot see it: the driver never runs, so the
firmware counts that time as listening.  The comparison moves it there too.

The radio rows are the fidelity claim: exit 0 when every tx and listen
difference is within --tolerance ticks, 1 otherwise, 2 on unusable input.
The CPU rows measure something else: the firmware switches its energest from
CPU to LPM before its lpm_enter housekeeping and back only after lpm_exit, so
each wake moves a roughly fixed number of executing cycles from "CPU" to
"LPM".  The table prints the wakes per period and the CPU difference per wake
in core cycles; that number should be steady (--cpu-per-wake-spread), and its
size is the firmware's own bookkeeping, not a simulator error.

  tools/check-energest-agreement.py --run configs/energest-agreement-cc2538dk.yaml
  tools/check-energest-agreement.py TRACE.csv --tick-hz 32768
"""
import argparse, csv, os, re, subprocess, sys, tempfile
from collections import defaultdict

HEADER = re.compile(r"--- Period summary #(\d+) \((\d+) seconds\)")
ROW = re.compile(r"\]\s*(Total time|CPU|LPM|Deep LPM|Radio Tx|Radio Rx|Radio total)\s*:\s*(\d+)")
LOW_POWER = {"lpm0", "lpm1", "lpm2", "lpm3", "lpm4", "sleep", "pm1", "pm2", "pm3"}


def run(runner, config, extra):
    tmp = tempfile.mkdtemp(prefix="energest-agree-")
    cmd = [os.path.abspath(runner), "test", os.path.abspath(config), "-q"] + extra
    p = subprocess.run(cmd, cwd=tmp, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    trace = os.path.join(tmp, "energest-trace.csv")
    if not os.path.exists(trace):
        sys.stdout.write(p.stdout[-3000:])
        print(f"no {trace}: the config needs plugins: [energest] and "
              "energest: { trace: energest-trace.csv }")
        sys.exit(2)
    return trace


def load(path):
    """-> (changes[node][component] = [(t, state)], logs[node] = [(t, line)])"""
    changes = defaultdict(lambda: defaultdict(list))
    logs = defaultdict(list)
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            t, node = int(r["t_ns"]), int(r["node"])
            if r["component"] == "log":
                logs[node].append((t, r["state"]))
            else:
                changes[node][r["component"]].append((t, r["state"]))
    return changes, logs


def periods(logs):
    """node -> list of (period_end_ns, {'Total time': ticks, ...})"""
    out = defaultdict(list)
    for node, lines in logs.items():
        pending = None
        for t, line in lines:
            m = HEADER.search(line)
            if m:
                pending = (t, {})
                continue
            m = ROW.search(line)
            if m and pending:
                pending[1][m.group(1)] = int(m.group(2))
                if m.group(1) == "Radio total":
                    out[node].append(pending)
                    pending = None
    return out


def dwell_sums(seq, t0, t1, acks=False):
    """ns per state over [t0, t1) of one component's (t, state) changes.
    acks: a radio TX entered straight from RX is the chip's automatic ACK,
    summed as "ack" (Contiki's energest never sees it: no driver call)."""
    sums = defaultdict(int)
    for i, (t, st) in enumerate(seq):
        if acks and st == "tx" and i and seq[i - 1][1] == "rx":
            st = "ack"
        end = seq[i + 1][0] if i + 1 < len(seq) else t1
        a, b = max(t, t0), min(end, t1)
        if b > a:
            sums[st] += b - a
    return sums


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", nargs="?", help="an energest trace CSV (or use --run)")
    ap.add_argument("--run", metavar="CONFIG", help="run CONFIG with --runner and check its trace")
    ap.add_argument("--runner", default="build/test_runner")
    ap.add_argument("--tick-hz", type=int, default=32768, help="the firmware's RTIMER_SECOND")
    ap.add_argument("--cpu-hz", type=int, default=32_000_000, help="core clock for the per-wake column")
    ap.add_argument("--tolerance", type=float, default=2.0, help="max |radio difference| in ticks")
    ap.add_argument("--skip-first", type=int, default=1, help="periods to skip (boot)")
    ap.add_argument("--cpu-per-wake-spread", type=float, default=40.0,
                    help="max spread (max-min) of the CPU difference per wake, in core cycles")
    ap.add_argument("--quiet", action="store_true", help="the summary only")
    a = ap.parse_args()
    if not a.trace and not a.run:
        ap.error("give a trace or --run CONFIG")
    trace = run(a.runner, a.run, []) if a.run else a.trace
    changes, logs = load(trace)
    per = periods(logs)
    if not per:
        print("no Energest period summaries in the trace; is this simple-energest firmware?")
        return 2
    ns_per_tick = 1e9 / a.tick_hz
    rows = []
    worst = 0.0
    for node in sorted(per):
        cpu_seq = changes[node].get("cpu", [])
        radio_seq = changes[node].get("radio", [])
        for k, (end, fw) in enumerate(per[node]):
            if k < a.skip_first or "Total time" not in fw:
                continue
            t1 = end
            t0 = end - int(round(fw["Total time"] * ns_per_tick))
            radio = dwell_sums(radio_seq, t0, t1, acks=True)
            cpu = dwell_sums(cpu_seq, t0, t1)
            wakes = sum(1 for i, (t, st) in enumerate(cpu_seq)
                        if st == "active" and i and cpu_seq[i - 1][1] != "active" and t0 <= t < t1)
            cmp = [("tx", fw["Radio Tx"], radio.get("tx", 0)),
                   ("listen", fw["Radio Rx"],
                    radio.get("listen", 0) + radio.get("rx", 0) + radio.get("interfered", 0)
                    + radio.get("ack", 0)),
                   ("cpu", fw["CPU"], cpu.get("active", 0)),
                   ("sleep", fw["LPM"] + fw["Deep LPM"],
                    sum(v for s, v in cpu.items() if s in LOW_POWER))]
            for name, fw_ticks, sim_ns in cmp:
                sim_ticks = sim_ns / ns_per_tick
                d = sim_ticks - fw_ticks
                if name in ("tx", "listen"):
                    worst = max(worst, abs(d))
                pw = d * ns_per_tick * a.cpu_hz / 1e9 / wakes if name == "cpu" and wakes else None
                rows.append((node, k, name, fw_ticks, sim_ticks, d, wakes, pw))
    if not rows:
        print("no complete periods after --skip-first")
        return 2
    if not a.quiet:
        print(f"{'node':>4} {'period':>6} {'state':>7} {'firmware':>10} {'simulator':>12} "
              f"{'diff ticks':>10} {'diff us':>9} {'wakes':>6} {'cyc/wake':>9}")
        for node, k, name, fw_ticks, sim_ticks, d, wakes, pw in rows:
            extra = f" {wakes:>6} {pw:>+9.1f}" if pw is not None else ""
            print(f"{node:>4} {k:>6} {name:>7} {fw_ticks:>10} {sim_ticks:>12.2f} {d:>+10.2f} "
                  f"{d * ns_per_tick / 1000:>+9.1f}{extra}")
    by = defaultdict(list)
    for _, _, name, _, _, d, _, _ in rows:
        by[name].append(d)
    print("per state: mean / max |diff| in ticks (1 tick = %.2f us), %d periods"
          % (ns_per_tick / 1000, len(by["tx"])))
    for name, ds in by.items():
        print(f"  {name:>7}: {sum(ds) / len(ds):+8.2f} / {max(abs(x) for x in ds):8.2f}")
    ok = worst <= a.tolerance
    print(f"radio: worst |diff| {worst:.2f} ticks, tolerance {a.tolerance}: {'OK' if ok else 'FAIL'}")
    pws = [pw for *_, pw in rows if pw is not None]
    if pws:
        spread = max(pws) - min(pws)
        cpu_ok = spread <= a.cpu_per_wake_spread
        print(f"cpu: the firmware counts {sum(pws) / len(pws):+.0f} executing cycles per wake "
              f"differently (its lpm_enter/lpm_exit bookkeeping); spread {spread:.1f} cycles, "
              f"limit {a.cpu_per_wake_spread}: {'OK' if cpu_ok else 'FAIL'}")
        ok = ok and cpu_ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
