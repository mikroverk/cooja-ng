#!/usr/bin/env python3
"""
Per-node energest numbers from a --ui-record replay document (or a frames:
file), over the whole run or a window of it: the figures the site's energy
cards quote, generated rather than typed.

  tools/energest-summary.py demos/ladder.json                 # whole run
  tools/energest-summary.py demos/ladder.json --from 90 --to 120
  tools/energest-summary.py demos/ladder.json --json          # machine-readable

A window is whole frames: those whose end lies in (FROM, TO].  The numbers are
differences of the frames' cumulative totals (docs/energest.md, "UI frames"),
so time in state is exact to the microsecond and energy is the plugin's
datasheet estimate.  "radio on" is listen + rx + tx + interfered.
"""

import argparse
import json
import sys


def frames_of(path):
    with open(path, encoding='utf-8') as f:
        text = f.read()
    try:
        doc = json.loads(text)
    except ValueError:
        doc = None
    if isinstance(doc, dict) and 'deltas' in doc:          # a replay document
        return [e['data'] for d in doc['deltas'] for e in d.get('f', [])
                if e.get('id') == 'energest']
    return [json.loads(line) for line in text.splitlines() if line.strip()]   # frames:


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('file')
    ap.add_argument('--from', dest='t_from', type=float, default=None, help='window start, s')
    ap.add_argument('--to', dest='t_to', type=float, default=None, help='window end, s')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()

    frames = frames_of(a.file)
    if not frames:
        sys.exit(f'{a.file}: no energest frames')
    # A restarted run starts its totals over: keep the last run only.
    starts = [i for i, fr in enumerate(frames) if fr['t0'] == 0]
    frames = frames[starts[-1]:] if starts else frames

    def end_s(fr):
        return (fr['t0'] + fr['n'] * fr['bin']) / 1e9

    t_from = a.t_from if a.t_from is not None else 0.0
    t_to = a.t_to if a.t_to is not None else end_s(frames[-1])
    before = [fr for fr in frames if end_s(fr) <= t_from + 1e-9]
    inside = [fr for fr in frames if t_from + 1e-9 < end_s(fr) <= t_to + 1e-9]
    if not inside:
        sys.exit(f'{a.file}: no frame ends in ({t_from}, {t_to}] s')
    last = {n['id']: n for n in inside[-1]['nodes']}
    first = {n['id']: n for n in before[-1]['nodes']} if before else {}
    span_s = end_s(inside[-1]) - (end_s(before[-1]) if before else 0.0)

    out = []
    for nid in sorted(last):
        n, p = last[nid], first.get(nid, {'us': {}, 'uj': {}})

        def us(lane, state):
            return n['us'].get(lane, {}).get(state, 0) - p['us'].get(lane, {}).get(state, 0)

        def lane_total(lane):
            return sum(us(lane, s) for s in n['us'].get(lane, {}))

        cpu_t = lane_total('cpu')
        radio_t = lane_total('radio')
        uj = {c: n['uj'].get(c, 0.0) - p['uj'].get(c, 0.0) for c in n['uj']}
        total_uj = sum(uj.values())
        vcc = n.get('vcc', 0.0)
        life_s = cpu_t / 1e6 if cpu_t else span_s
        row = {
            'id': nid,
            'model': n.get('model', ''),
            'unverified': bool(n.get('unverified')),
            'seconds': round(life_s, 6),
            'cpu_active_pct': 100.0 * us('cpu', 'active') / cpu_t if cpu_t else None,
            'radio_on_pct': (100.0 * sum(us('radio', s) for s in ('listen', 'rx', 'tx', 'interfered'))
                             / radio_t) if radio_t else None,
            'radio_tx_ms': us('radio', 'tx') / 1e3,
            'radio_rx_ms': us('radio', 'rx') / 1e3,
            'mj': total_uj / 1e3,
            'mj_cpu': uj.get('cpu', 0.0) / 1e3,
            'mj_radio': uj.get('radio', 0.0) / 1e3,
            'mw': total_uj / 1e3 / life_s if life_s else None,
            'ma': (total_uj / 1e3 / life_s / vcc) if life_s and vcc else None,
        }
        out.append(row)

    if a.json:
        json.dump({'file': a.file, 'from_s': t_from, 'to_s': end_s(inside[-1]), 'nodes': out},
                  sys.stdout, indent=1)
        print()
        return
    print(f'{a.file}: {t_from:g}..{end_s(inside[-1]):g} s  (time in state exact; energy a datasheet estimate)')
    print(f"{'node':>4} {'cpu active':>10} {'radio on':>9} {'tx ms':>8} {'rx ms':>8} "
          f"{'mJ':>9} {'cpu mJ':>8} {'radio mJ':>9} {'mW':>7} {'mA':>7}")
    for r in out:
        f = lambda v, w, d: f'{v:{w}.{d}f}' if v is not None else ' ' * (w - 1) + '-'
        print(f"{r['id']:>4} {f(r['cpu_active_pct'], 9, 2)}% {f(r['radio_on_pct'], 8, 2)}% "
              f"{f(r['radio_tx_ms'], 8, 1)} {f(r['radio_rx_ms'], 8, 1)} {f(r['mj'], 9, 1)} "
              f"{f(r['mj_cpu'], 8, 1)} {f(r['mj_radio'], 9, 1)} {f(r['mw'], 7, 2)} {f(r['ma'], 7, 3)}"
              + ('  (unverified model)' if r['unverified'] else ''))


if __name__ == '__main__':
    main()
