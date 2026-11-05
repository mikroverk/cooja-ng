#!/usr/bin/env python3
"""check-energest-summary.py — does an energest run's summary say what it should?
(docs/energest.md "Validation"; used by tools/check-energest.sh)

Reads a run's stdout (the energest summary the plugin prints at the end)
and checks, exiting 0 when all hold, 1 otherwise:

  always: no CHECK FAILED / NOT equal line, no stamp before its lane's
          last, every node's CPU low-power time equal to its core's own
          lpm_ns to under 1 us per wake;
  and each EXPECT given:
    N:lifetime=S        node N lived S seconds (to the microsecond)
    N:lane.state=PCT    lane in that state PCT% of its time (to 0.005)
    N:lane.state>PCT    ... more than PCT%
    N:ma~M              node N's average current within 2% of node M's

  tools/check-energest-summary.py out.txt 9:lifetime=9.0 3:rtc0.on=100 9:ma~1
"""
import re
import sys


def parse(out):
    nodes, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"energest: node (\d+) .* lifetime ([0-9.]+) s", line)
        if m:
            cur = nodes.setdefault(int(m[1]), {"life": float(m[2]), "lanes": {}, "wakes": []})
            continue
        if cur is None:
            continue
        m = re.match(r"energest:   (\S+) +((?: \S+ [0-9.]+ s \([0-9.]+%\))+)  = ", line)
        if m:
            cur["lanes"][m[1]] = {s: float(p) for s, p in
                                  re.findall(r" (\S+) [0-9.]+ s \(([0-9.]+)%\)", m[2])}
        m = re.search(r"average ([0-9.]+) mA", line)
        if m:
            cur["ma"] = float(m[1])
        m = re.search(r"cpu low-power .* \(([+-][0-9.]+) us each\)", line)
        if m:
            cur["wakes"].append(float(m[1]))
    return nodes


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    out = open(argv[1]).read()
    nodes = parse(out)
    bad = 0
    for text in ("CHECK FAILED", "NOT equal", "stamps before their lane"):
        if text in out:
            print("summary:", text)
            bad = 1
    if not nodes:
        print("summary: no energest node in", argv[1])
        bad = 1
    for n, d in nodes.items():
        if not d["wakes"] or any(abs(x) >= 1.0 for x in d["wakes"]):
            print(f"node {n}: cpu low-power vs lpm_ns per wake {d['wakes']} us")
            bad = 1
    for x in argv[2:]:
        m = re.match(r"(\d+):(\S+?)([=>~])(\S+)$", x)
        if not m:
            print("bad expectation:", x)
            return 2
        n, key, op, want = int(m[1]), m[2], m[3], m[4]
        d = nodes.get(n)
        if d is None:
            print(f"node {n}: not in the summary")
            bad = 1
            continue
        if key == "lifetime":
            got, ok = d["life"], abs(d["life"] - float(want)) < 5e-7
        elif key == "ma":
            ref = nodes.get(int(want), {}).get("ma", 0.0)
            got = d.get("ma", 0.0)
            ok = ref > 0 and abs(got / ref - 1) < 0.02
        else:
            lane, st = key.rsplit(".", 1)
            got = d["lanes"].get(lane, {}).get(st, 0.0)
            ok = abs(got - float(want)) < 0.005 if op == "=" else got > float(want)
        if not ok:
            print(f"node {n} {key}: {got}, want {op}{want}")
            bad = 1
    print(f"summary: {len(nodes)} nodes" + ("" if bad else ", as expected"))
    return bad


if __name__ == "__main__":
    sys.exit(main(sys.argv))
