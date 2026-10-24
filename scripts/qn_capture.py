"""Decode QN8035 bus captures (Saleae Logic 2 digital CSV export).

The CSV holds one row per transition with columns `Time [s]`, `SCL`, `SDA` (others, such as
Vcc and INT, are reported as edges). Views:

  ops       one line per register access: `rd 13 = 90`, `wr 00 = 1B` (default)
  timeline  ops with each QF30 tune macro folded into one TUNE line, RDS group reads into
            one RDS line, STATUS2 polls and BD37534 (0x40) writes collapsed
  timing    SCL high/low times and the read turnaround a slave must meet
  replay    the QN8035 register accesses as C initialisers {'R'|'W', reg, value}, for host tests

Usage: python scripts/qn_capture.py captures/seek.csv [--view timeline] [--from S] [--to S]
"""
import argparse
import csv
import sys

QN_ADDR = 0x10


def transactions(path):
    """Yield (t_start, t_stop, bytes, acks, first_data_rise, extra_edges) per START..STOP.

    Bytes include the address byte. A repeated START ends a transaction too (t_stop is None).
    Edges of other channels come through as ('edge', t, name, value) tuples.
    """
    with open(path, newline="") as f:
        rows = csv.reader(f)
        hdr = next(rows)
        i_scl, i_sda = hdr.index("SCL"), hdr.index("SDA")
        others = [(i, n) for i, n in enumerate(hdr) if i not in (0, i_scl, i_sda)]
        first = next(rows)
        p_scl, p_sda = int(first[i_scl]), int(first[i_sda])
        p_oth = {i: first[i] for i, _ in others}
        cur = None
        for row in rows:
            t = float(row[0])
            scl, sda = int(row[i_scl]), int(row[i_sda])
            for i, n in others:
                if row[i] != p_oth[i]:
                    yield ("edge", t, n, row[i])
                    p_oth[i] = row[i]
            if scl and p_scl and sda != p_sda:
                if cur is not None:
                    yield ("xfer", cur["t0"], t if sda else None, cur["bytes"], cur["acks"],
                           cur["data_rise"])
                cur = {"t0": t, "bits": [], "bytes": [], "acks": [], "data_rise": None} if not sda \
                    else None
            elif scl and not p_scl and cur is not None:
                if len(cur["bytes"]) == 1 and not cur["bits"] and cur["data_rise"] is None:
                    cur["data_rise"] = t
                cur["bits"].append(sda)
                if len(cur["bits"]) == 9:
                    v = 0
                    for b in cur["bits"][:8]:
                        v = (v << 1) | b
                    cur["bytes"].append(v)
                    cur["acks"].append(cur["bits"][8] == 0)
                    cur["bits"] = []
            p_scl, p_sda = scl, sda


def register_ops(path):
    """Pair `W reg` + `R val` into reads. Yields (t, kind, reg, val, note).

    kind: 'rd', 'wr', 'edge' (reg = channel name), 'other' (reg = address byte, val = data)."""
    pending = None
    for x in transactions(path):
        if x[0] == "edge":
            yield (x[1], "edge", x[2], x[3], "")
            continue
        _, t, t_stop, data, acks, _ = x
        if not data:
            continue
        addr, is_read = data[0] >> 1, data[0] & 1
        note = "" if acks[0] else "NACK"
        if t_stop is None:
            note += " Sr"
        if addr != QN_ADDR:
            yield (t, "other", data[0], data[1:], note)
            continue
        if not is_read and len(data) == 2:
            pending = (t, data[1], note)
        elif not is_read and len(data) == 3:
            yield (t, "wr", data[1], data[2], note)
        elif is_read and pending and len(data) == 2:
            yield (pending[0], "rd", pending[1], data[1], (pending[2] + note).strip())
            pending = None
        else:
            yield (t, "other", data[0], data[1:], note + " unpaired")


def fmt_op(t, kind, reg, val, note):
    if kind in ("rd", "wr"):
        s = f"{kind} {reg:02X} = {val:02X}"
    elif kind == "edge":
        s = f"{reg} -> {val}"
    else:
        dev = "BD" if reg >> 1 == 0x40 else f"{reg >> 1:02X}{'R' if reg & 1 else 'W'}"
        s = f"{dev} " + " ".join(f"{v:02X}" for v in val)
    return f"{t:10.4f} {s} {note}".rstrip()


def _is(op, kind, reg):
    return op[1] == kind and op[2] == reg


def _tune_line(blk):
    """One QF30 tune macro (rmw 0x49 ... unmute) as text."""
    w, r = {}, {}
    for o in blk:
        (w if o[1] == "wr" else r).setdefault(o[2], []).append(o[3])
    ch_step = w.get(0x0A, [0])[-1]
    ch = w[0x07][-1] | ((ch_step & 3) << 8)
    start = "PULSE55" if 0x55 in w else "CHSC"
    pll = "/".join(f"{w.get(k, [0])[-1]:02X}" for k in (0x16, 0x17, 0x46))
    cca = w.get(0x01, [0])[-1]
    f4f = f"{r.get(0x4F, [0])[0]:02X}->{w.get(0x4F, [0])[-1]:02X}"
    mute = " ".join(f"{o[2]:02X}={o[3]:02X}" for o in blk if o[1] == "wr" and o[2] in (0x4A, 0x4C))
    return (f"TUNE {60 + ch * 0.05:6.2f} MHz {start} CCA={cca:02X} PLL={pll} "
            f"SYS1={'/'.join(f'{v:02X}' for v in w.get(0x00, []))} 4F={f4f} mute[{mute}] "
            f"{(blk[-1][0] - blk[0][0]) * 1000:.1f} ms")


def timeline(ops):
    """Fold the QF30 driver's macros. Yields (t, text)."""
    n = len(ops)
    i = 0
    while i < n:
        o = ops[i]
        # Tune macro: starts with rmw(0x49, 0x0F, 0x0A), CH written to 0x07, ends at the unmute
        # after the 0x4F write.
        if _is(o, "rd", 0x49) and i + 1 < n and _is(ops[i + 1], "wr", 0x49) \
                and ops[i + 1][3] & 0x0F == 0x0A:
            j = next((k for k in range(i, min(i + 30, n)) if _is(ops[k], "wr", 0x07)), None)
            if j is not None:
                e, seen_4f = j, False
                while e + 1 < n and e - j < 30:
                    e += 1
                    seen_4f |= _is(ops[e], "wr", 0x4F)
                    if seen_4f and ops[e][1] == "wr" and ops[e][2] in (0x4A, 0x4C):
                        break
                yield o[0], _tune_line(ops[i:e + 1])
                i = e + 1
                continue
        if _is(o, "rd", 0x13):
            if i + 8 < n and all(_is(ops[i + 1 + k], "rd", 0x0B + k) for k in range(8)):
                d = [ops[i + 1 + k][3] for k in range(8)]
                blocks = " ".join(f"{d[2 * k]:02X}{d[2 * k + 1]:02X}" for k in range(4))
                yield o[0], f"RDS 13={o[3]:02X} {blocks}"
                i += 9
                continue
            j = i
            while j < n and _is(ops[j], "rd", 0x13) and not (
                    j + 8 < n and all(_is(ops[j + 1 + k], "rd", 0x0B + k) for k in range(8))):
                j += 1
            vals = sorted({ops[k][3] for k in range(i, j)})
            yield o[0], f"poll 13 x{j - i} {{{','.join(f'{v:02X}' for v in vals)}}}"
            i = j
            continue
        if o[1] == "other" and o[2] >> 1 == 0x40:
            j = i
            while j < n and ops[j][1] == "other" and ops[j][2] >> 1 == 0x40 \
                    and ops[j][0] - ops[max(j - 1, i)][0] < 0.2:
                j += 1
            yield o[0], "BD " + ", ".join(" ".join(f"{v:02X}" for v in ops[k][3]) for k in range(i, j))
            i = j
            continue
        yield o[0], fmt_op(*o)[11:]
        i += 1


def timing(path):
    hi, lo = [], []
    turn_stop, turn_bit = [], []
    t_rise = t_fall = None
    with open(path, newline="") as f:
        rows = csv.reader(f)
        hdr = next(rows)
        i_scl = hdr.index("SCL")
        p = None
        for row in rows:
            t, scl = float(row[0]), int(row[i_scl])
            if p is not None and scl != p:
                if scl:
                    if t_fall is not None and t - t_fall < 1e-3:
                        lo.append(t - t_fall)
                    t_rise = t
                else:
                    if t_rise is not None and t - t_rise < 1e-3:
                        hi.append(t - t_rise)
                    t_fall = t
            p = scl
    last = None
    for x in transactions(path):
        if x[0] != "xfer" or not x[3]:
            continue
        _, t0, t_stop, data, _, rise = x
        if data[0] == (QN_ADDR << 1 | 1) and last and len(last[0]) == 2 and last[1] is not None \
                and rise is not None:
            turn_stop.append(t0 - last[1])
            turn_bit.append(rise - last[1])
        last = (data, t_stop) if data[0] == QN_ADDR << 1 else None

    def line(name, v):
        v = sorted(v)
        if not v:
            return f"{name}: none"
        return (f"{name}: n={len(v)} min {v[0] * 1e6:.1f} median {v[len(v) // 2] * 1e6:.1f} "
                f"max {v[-1] * 1e6:.1f} us")

    return [line("SCL high", hi), line("SCL low", lo),
            line("read: STOP after `20 reg` -> START `21`", turn_stop),
            line("read: STOP after `20 reg` -> first data SCL rise", turn_bit)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("csv")
    ap.add_argument("--view", choices=("ops", "timeline", "timing", "replay"), default="ops")
    ap.add_argument("--from", dest="t0", type=float, default=0.0)
    ap.add_argument("--to", dest="t1", type=float, default=float("inf"))
    a = ap.parse_args()
    if a.view == "timing":
        print("\n".join(timing(a.csv)))
        return
    ops = [o for o in register_ops(a.csv) if a.t0 <= o[0] <= a.t1]
    if a.view == "ops":
        lines = (fmt_op(*o) for o in ops)
    elif a.view == "replay":
        lines = (f"    {{'{'R' if k == 'rd' else 'W'}', 0x{reg:02X}, 0x{val:02X}}}, /* {t:.4f} s */"
                 for t, k, reg, val, _ in ops if k in ("rd", "wr"))
    else:
        lines = (f"{t:10.4f} {s}" for t, s in timeline(ops))
    try:
        for line in lines:
            print(line)
    except BrokenPipeError:
        sys.stderr.close()


if __name__ == "__main__":
    main()
