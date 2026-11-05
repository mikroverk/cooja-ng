#!/usr/bin/env python3
"""check-energest-output.py — are an energest run's files consistent?
(docs/energest.md "Validation")

Takes the three files one run of the energest plugin wrote — the CSV
(energest.csv), the UI frames (energest.frames) and the trace
(energest.trace) — and checks, exiting 0 when all hold, 1 otherwise:

  1. every component's states sum to its node's lifetime in the CSV (the
     `total` row), to the nanosecond;
  2. the trace reproduces the CSV: summing each component's dwells from its
     state changes gives every CSV time_ns exactly (a node's lives, its
     `life` rows `added` and `removed`, bound the dwells: a node removed,
     added or rebooted during the run has no time between two lives);
  3. the frames are well formed (docs/energest.md "UI frames"): consecutive
     windows of `n` bins, every lane's bins decode (a negative entry is a
     run of zero bins) to `n` values of 0..1000 permille whose sum over the
     non-base states is at most 1000 (+ rounding), every frame's cumulative
     `us` totals are the totals at its end (a lane alive through a whole
     frame gains exactly n x bin in it, to a microsecond per state: a frame
     is emitted a frame late, and its totals once ran on into the next
     one), and the last frame's totals equal the CSV's to the microsecond;
     every frame lists exactly the nodes alive in it, by the trace's lives,
     with `b0`/`b1` at the first and one past the last bin they lived in.

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
    lives = defaultdict(list)      # node -> [[start, end or None], ...]
    end = max(life.values()) if life else 0
    for r in csv.DictReader(open(trace_path, newline="")):
        if r["component"] in ("log", "air"):
            continue
        n, t = int(r["node"]), int(r["t_ns"])
        if r["component"] == "life":
            if r["state"] == "added":
                if lives[n] and lives[n][-1][1] is None:
                    bad += fail(f"node {n}: added at {t} while alive")
                lives[n].append([t, None])
            elif r["state"] == "removed":
                if not lives[n] or lives[n][-1][1] is not None:
                    bad += fail(f"node {n}: removed at {t} while not alive")
                else:
                    lives[n][-1][1] = t
            continue
        seqs[(n, r["component"])].append((t, r["state"]))
    # The trace has no end marker: the last life runs to the run's end, which
    # the CSV's lifetime gives (its start + the lifetime the closed lives
    # leave).
    # A trace with no life rows (one life per node): it begins at the
    # node's first row.
    for (n, c), seq in seqs.items():
        if n in life and not lives.get(n):
            first = min(q[0][0] for (m, _), q in seqs.items() if m == n)
            lives[n] = [[first, None]]
    for n, ls in lives.items():
        if ls and ls[-1][1] is None:
            ls[-1][1] = ls[-1][0] + life.get(n, 0) - sum(b - a for a, b in ls[:-1])
    def life_end(n, t):
        """The end of node n's life that holds t (the latest begun by t)."""
        e = None
        for a, b in lives.get(n, []):
            if a <= t:
                e = b
        return e
    checked = 0
    for (n, c), seq in seqs.items():
        sums = defaultdict(int)
        start = seq[0][0]
        stop = start + life[n] if n in life else end
        for i, (t, st) in enumerate(seq):
            t1 = seq[i + 1][0] if i + 1 < len(seq) else stop
            le = life_end(n, t)
            if le is not None:
                if t > le:
                    bad += fail(f"node {n} {c}: a state change at {t}, "
                                f"after its life ended at {le}")
                t1 = min(t1, le) if i + 1 < len(seq) else le
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
    sums = {}                  # (node, lane) -> its us summed over states, the frame before
    gains = 0
    for i, f in enumerate(frames):
        if f.get("v") != 1:
            bad += fail(f"frame at {f.get('t0')}: version {f.get('v')}")
        t0, b, nb = f["t0"], f["bin"], f["n"]
        if prev_end is not None and t0 != prev_end:
            bad += fail(f"frame at {t0}: the previous one ended at {prev_end}")
        prev_end = t0 + nb * b
        t1 = prev_end
        if lives:
            want = {}
            for n, ls in lives.items():
                cut = [(max(a, t0), min(e, t1)) for a, e in ls if a < t1 and e > t0]
                if cut:
                    b0 = (cut[0][0] - t0) // b
                    b1 = -(-(cut[-1][1] - t0) // b)
                    want[n] = (b0 if b0 > 0 else None, b1 if b1 < nb else None)
            got = {node["id"]: (node.get("b0"), node.get("b1")) for node in f["nodes"]}
            if set(got) != set(want):
                bad += fail(f"frame {t0}: nodes {sorted(got)}, alive in it {sorted(want)}")
            for n in set(got) & set(want):
                if got[n] != want[n]:
                    bad += fail(f"frame {t0} node {n}: b0/b1 {got[n]}, its lives give {want[n]}")
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
            whole = "b0" not in node and "b1" not in node and i + 1 < len(frames)
            for c, st in node["us"].items():
                tot_us, key = sum(st.values()), (node["id"], c)
                if whole and key in sums:
                    gains += 1
                    if abs(tot_us - sums[key] - nb * b // 1000) > len(st) + 1:
                        bad += fail(f"frame {t0} node {node['id']} {c}: its totals gained "
                                    f"{tot_us - sums[key]} us in a {nb * b // 1000} us frame")
                sums[key] = tot_us
    for n, node in last.items():
        for c, st in node["us"].items():
            for s, us in st.items():
                want = times[n][c].get(s, 0) // 1000
                if us != want:
                    bad += fail(f"last frame node {n} {c}.{s}: {us} us, csv {want} us")
    print(f"frames: {len(frames)} consecutive frames" + ("" if bad else
          f", bins in range, totals at each frame's end ({gains} lane-frames), "
          "the last frame's totals equal to the csv"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
