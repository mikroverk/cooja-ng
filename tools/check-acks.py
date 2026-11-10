#!/usr/bin/env python3
"""Link-layer ACK timing and retransmissions in a csim pcap (--pcap).

For every unicast frame that asks for an ACK, per link (sender -> receiver):
how many times it went on the air, and for every Imm-Ack the gap between the
end of the frame it acknowledges and the ACK's first preamble byte, both on
the radio bus's clock (the pcap stamps each frame's preamble start, ns).

A node is named by the last two bytes of its address (the node id for the
Cooja-style addresses the emulated boards get; 257 for a Sky's 0x0101).
An ACK is matched to the last frame before it with its DSN that ended at
most 3 ms before it.  A copy of a frame's (src, dst, DSN) is a
retransmission when it follows the one before within 50 ms (CSMA: the
sender missed an ACK that is in the capture or never came) or follows an
unacknowledged copy within 5 s (TSCH retries at the link's next cell).

  tools/check-acks.py cap.pcap                       # the table
  tools/check-acks.py cap.pcap --min-gap-us 190      # no ACK before the
          sender's TX-to-RX turnaround is over (aTurnaroundTime, 12 symbols =
          192 us, IEEE 802.15.4-2015 Table 11-1; less a byte-clock margin)
  tools/check-acks.py cap.pcap --first-try 2:3       # every unicast 2 -> 3
          acknowledged at its first transmission

Exit status 1 when a check fails, 2 on a bad argument or file.
"""
import argparse
import collections
import struct
import sys

BYTE_NS = 32000          # 250 kbit/s O-QPSK: 2 symbols of 16 us
PHY_HDR = 6              # 4 preamble bytes, SFD, PHR (not in the pcap)


def frames(path):
    with open(path, 'rb') as f:
        hdr = f.read(24)
        if len(hdr) < 24:
            raise ValueError("not a pcap file")
        magic = struct.unpack('<I', hdr[:4])[0]
        if magic not in (0xa1b23c4d, 0xa1b2c3d4):
            raise ValueError("not a little-endian pcap file")
        nsec = magic == 0xa1b23c4d
        while True:
            h = f.read(16)
            if len(h) < 16:
                return
            sec, frac, incl, _ = struct.unpack('<IIII', h)
            data = f.read(incl)
            yield sec * 1_000_000_000 + (frac if nsec else frac * 1000), data


def node(b):
    return b[0] | (b[1] << 8)


def parse(buf):
    """FCF, DSN and the two addresses of an IEEE 802.15.4 MAC frame."""
    if len(buf) < 5:
        return None
    fcf = buf[0] | (buf[1] << 8)
    ft, ar, pc = fcf & 7, (fcf >> 5) & 1, (fcf >> 6) & 1
    dm, ver, sm = (fcf >> 10) & 3, (fcf >> 12) & 3, (fcf >> 14) & 3
    if ver == 2:                     # IEEE 802.15.4-2015 Table 7-2
        if dm == 3 and sm == 3:
            dpan, span = not pc, False
        elif dm and sm:
            dpan, span = True, not pc
        elif dm:
            dpan, span = not pc, False
        elif sm:
            dpan, span = False, not pc
        else:
            dpan, span = bool(pc), False
    else:
        dpan, span = bool(dm), bool(sm) and not pc
    off, dst, src = 3, None, None
    if dpan:
        off += 2
    if dm == 2:
        dst = node(buf[off:off + 2]); off += 2
        if dst == 0xffff:
            dst = 'bcast'
    elif dm == 3:
        dst = node(buf[off:off + 2]); off += 8
    if span:
        off += 2
    if sm == 2 and len(buf) >= off + 2:
        src = node(buf[off:off + 2])
    elif sm == 3 and len(buf) >= off + 8:
        src = node(buf[off:off + 2])
    return dict(type=ft, ar=ar, seq=buf[2], src=src, dst=dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('pcap')
    ap.add_argument('--min-gap-us', type=float, default=None)
    ap.add_argument('--first-try', action='append', default=[], metavar='SRC:DST')
    a = ap.parse_args()
    try:
        fl = []
        for ts, data in frames(a.pcap):
            p = parse(data)
            if p:
                p['ts'] = ts
                p['end'] = ts + (PHY_HDR + len(data)) * BYTE_NS
                fl.append(p)
    except (OSError, ValueError) as e:
        print(f"check-acks: {a.pcap}: {e}", file=sys.stderr)
        return 2

    gaps = collections.defaultdict(list)
    for j, ack in enumerate(fl):
        if ack['type'] != 2:
            continue
        for k in range(j - 1, -1, -1):
            d = fl[k]
            if ack['ts'] - d['end'] > 3_000_000:
                break
            if d['type'] != 2 and d['ar'] and d['seq'] == ack['seq']:
                d['acked'] = True
                gaps[(d['src'], d['dst'])].append((ack['ts'] - d['end']) / 1000.0)
                break
    copies = collections.defaultdict(list)     # link -> [copies per frame]
    last = {}
    for p in fl:
        if p['type'] == 2 or not p['ar'] or p['dst'] in (None, 'bcast'):
            continue
        k = (p['src'], p['dst'], p['seq'])
        prev = last.get(k)
        if prev and (p['ts'] - prev[0]['ts'] < 50_000_000 or
                     (not prev[0].get('acked') and
                      p['ts'] - prev[0]['ts'] < 5_000_000_000)):
            prev[1][0] += 1
            last[k] = (p, prev[1])
        else:
            c = [1]
            copies[(p['src'], p['dst'])].append(c)
            last[k] = (p, c)

    fail = []
    print(f"{'link':>12} {'frames':>6} {'sent':>5} {'retx':>5} {'acks':>5}"
          f"  ACK after the frame's end, us: min median max")
    for link in sorted(set(copies) | set(gaps), key=str):
        c = [n[0] for n in copies.get(link, [])]
        g = sorted(gaps.get(link, []))
        gs = f"{g[0]:7.1f} {g[len(g) // 2]:7.1f} {g[-1]:7.1f}" if g else "      -"
        print(f"{f'{link[0]}->{link[1]}':>12} {len(c):6d} {sum(c):5d} "
              f"{sum(c) - len(c):5d} {len(g):5d}  {gs}")
        if a.min_gap_us is not None and g and g[0] < a.min_gap_us:
            fail.append(f"{link[0]}->{link[1]}: an ACK {g[0]:.1f} us after the "
                        f"frame's end (< {a.min_gap_us:g})")
    for spec in a.first_try:
        try:
            s, d = (int(x) for x in spec.split(':'))
        except ValueError:
            print(f"check-acks: --first-try {spec}: want SRC:DST", file=sys.stderr)
            return 2
        c = [n[0] for n in copies.get((s, d), [])]
        if not c:
            fail.append(f"{s}->{d}: no unicast frames")
        elif max(c) > 1:
            fail.append(f"{s}->{d}: {sum(c) - len(c)} retransmissions of "
                        f"{len(c)} frames (up to {max(c)} copies)")
    for f in fail:
        print(f"FAIL: {f}")
    if not fail:
        print("OK")
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
