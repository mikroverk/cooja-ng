#!/usr/bin/env python3
"""check-energest-output.py — are an energest run's files consistent?
(docs/energest.md "Validation")

Takes the three files one run of the energest plugin wrote — the CSV
(energest.csv), the UI frames (energest.frames) and the trace
(energest.trace) — and checks, exiting 0 when all hold, 1 otherwise:

  1. every component's states sum to its node's lifetime in the CSV (the
     `total` row), to the nanosecond;
  2. the trace reproduces the CSV: summing each component's dwells from its
     state changes gives every CSV time_ns exactly;
  3. the frames are well formed (docs/energest.md "UI frames"): consecutive
     windows of `n` bins, every lane's bins decode (a negative entry is a
     run of zero bins) to `n` values of 0..1000 permille whose sum over the
     non-base states is at most 1000 (+ rounding), and the last frame's
     cumulative `us` totals equal the CSV's to the microsecond.

  tools/check-energest-output.py energest.csv energest.frames energest.trace
"""
import csv, json, sys
from collections import defaultdict


def fail(msg):
    print("FAIL:", msg)
    return 1


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    csv_path, frames_path, trace_path = argv[1:]
    bad = 0

    rows = list(csv.DictReader(open(csv_path, newline="")))
    times = defaultdict(lambda: defaultdict(dict))   # node -> comp -> state -> ns
    life = {}
    for r in rows:
        n = int(r["node"])
        if r["component"] == "total":
            life[n] = int(r["time_ns"])
        elif r["component"] != "battery":
            times[n][r["component"]][r["state"]] = int(r["time_ns"])
    for n, comps in times.items():
        for c, st in comps.items():
            s = sum(st.values())
            if s != life.get(n):
                bad += fail(f"node {n} {c}: states sum to {s} ns, lifetime {life.get(n)} ns")
    print(f"csv: {sum(len(c) for c in times.values())} components on {len(times)} nodes, "
          "each summing to its node's lifetime")

    # 2. the trace
    seqs = defaultdict(list)
    end = max(life.values()) if life else 0
    for r in csv.DictReader(open(trace_path, newline="")):
        if r["component"] == "log":
            continue
        seqs[(int(r["node"]), r["component"])].append((int(r["t_ns"]), r["state"]))
    # The trace has no end marker: a component's last state runs to the end
    # of its node's life, which the CSV's lifetime gives (start + lifetime).
    checked = 0
    for (n, c), seq in seqs.items():
        sums = defaultdict(int)
        start = seq[0][0]
        stop = start + life[n] if n in life else end
        for i, (t, st) in enumerate(seq):
            t1 = seq[i + 1][0] if i + 1 < len(seq) else stop
            sums[st] += t1 - t
        want = {k: v for k, v in times[n][c].items()}
        got = {k: v for k, v in sums.items() if v}
        if got != want:
            bad += fail(f"node {n} {c}: trace sums {got} != csv {want}")
        checked += 1
    print(f"trace: {checked} components re-summed from their state changes"
          + ("" if bad else ", equal to the csv"))

    # 3. the frames
    frames = [json.loads(l) for l in open(frames_path) if l.strip()]
    prev_end = None
    last = {}
    for f in frames:
        if f.get("v") != 1:
            bad += fail(f"frame at {f.get('t0')}: version {f.get('v')}")
        t0, b, nb = f["t0"], f["bin"], f["n"]
        if prev_end is not None and t0 != prev_end:
            bad += fail(f"frame at {t0}: the previous one ended at {prev_end}")
        prev_end = t0 + nb * b
        for node in f["nodes"]:
            for lane in node["lanes"]:
                tot = [0] * nb
                for st, arr in lane.get("f", {}).items():
                    vals = []
                    for v in arr:
                        vals.extend([0] * -v if v < 0 else [v])
                    if len(vals) != nb or any(v < 0 or v > 1000 for v in vals):
                        bad += fail(f"frame {t0} node {node['id']} {lane['c']}.{st}: "
                                    f"{len(vals)} bins, want {nb}")
                        continue
                    for k, v in enumerate(vals):
                        tot[k] += v
                if max(tot) > 1000 + len(lane.get("f", {})):
                    bad += fail(f"frame {t0} node {node['id']} {lane['c']}: a bin sums "
                                f"to {max(tot)} permille")
            last[node["id"]] = node
    for n, node in last.items():
        for c, st in node["us"].items():
            for s, us in st.items():
                want = times[n][c].get(s, 0) // 1000
                if us != want:
                    bad += fail(f"last frame node {n} {c}.{s}: {us} us, csv {want} us")
    print(f"frames: {len(frames)} consecutive frames" + ("" if bad else
          ", bins in range, the last frame's totals equal to the csv"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
