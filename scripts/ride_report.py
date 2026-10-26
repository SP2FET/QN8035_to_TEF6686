#!/usr/bin/env python3
"""Turn a ride recorded by ride_log.py into a self-contained HTML report and CSV files.

    python scripts/ride_report.py rides/20261026-101500 [--from 10:20:00] [--to 10:45:00]
                                  [--out report.html]

Reads trace.jsonl, status.jsonl and marks.jsonl from the ride directory (missing files are
fine) and writes report.html (or --out), quality.csv, af.csv and events.csv beside them.
Device time stamps are mapped to PC time per boot segment with the median offset of the
"clock" lines. Charts are inline SVG; no external resources. Standard library only.
"""
import argparse
import csv
import html
import json
import statistics
import sys
import time
from datetime import datetime
from pathlib import Path

SETTLED_AGE = 1000          # q.age in 0.1 ms
CAL_WINDOW_S = 10.0
CAL_AVG_S = 3.0
MAX_POINTS = 3000
RESULT_EVENTS = ("switched", "jumped", "worse", "other_pi", "no_pi", "abort")
EV_COLORS = {"switched": "#2a9d3a", "jumped": "#2a9d3a", "worse": "#e08a00",
             "other_pi": "#d62728", "no_pi": "#d62728", "check": "#888888",
             "trial": "#888888", "abort": "var(--abort)"}
PALETTE = ["#1f77b4", "#e377c2", "#17becf", "#bcbd22", "#9467bd", "#8c564b", "#7f7f7f", "#ff7f0e"]


# ---- loading ----

def read_jsonl(path):
    out = []
    path = Path(path)
    if not path.exists():
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if isinstance(obj, dict):
                out.append(obj)
    return out


def assign_times(trace):
    """Return (q, a, e) record lists with "time" (PC epoch seconds), sorted by time."""
    offs = {}
    for r in trace:
        if r.get("k") == "clock" and "now" in r and "pc" in r:
            offs.setdefault(r.get("boot", 1), []).append(r["pc"] - r["now"] / 1000.0)
    med = {b: statistics.median(v) for b, v in offs.items()}
    out = {"q": [], "a": [], "e": []}
    for r in trace:
        k = r.get("k")
        if k not in out:
            continue
        b = r.get("boot", 1)
        if b in med and isinstance(r.get("t"), (int, float)):
            r["time"] = r["t"] / 1000.0 + med[b]
        elif isinstance(r.get("pc"), (int, float)):
            r["time"] = r["pc"]
        else:
            continue
        out[k].append(r)
    for v in out.values():
        v.sort(key=lambda r: r["time"])
    return out["q"], out["a"], out["e"]


def day_time(first, hms):
    t = datetime.strptime(hms, "%H:%M:%S").time()
    return datetime.combine(datetime.fromtimestamp(first).date(), t).timestamp()


def window(recs, lo, hi):
    return [r for r in recs if lo <= r["time"] <= hi]


def iso(t):
    return datetime.fromtimestamp(t).isoformat(timespec="milliseconds")


# ---- small helpers ----

def mean(v):
    return statistics.fmean(v) if v else None


def median(v):
    return statistics.median(v) if v else None


def fmt(v, nd=1):
    return "" if v is None else f"{v:.{nd}f}"


def tenth(v):
    return None if v is None else v / 10.0


def mhz(v):
    return "" if v is None else f"{v / 1000:.2f}"


def score(r):
    if None in (r.get("lv"), r.get("usn"), r.get("wam")):
        return None
    return (r["lv"] - r["usn"] - r["wam"]) / 10.0


def is_trial(q):
    return bool(q.get("af", 0) & 8)


def is_settled(q):
    return q.get("age", 0) >= SETTLED_AGE


def tuned_settled(qs):
    return [q for q in qs if is_settled(q) and not is_trial(q)]


def decimate(pts, maxn=MAX_POINTS):
    """Keep min and max of each bucket so spikes survive."""
    if len(pts) <= maxn:
        return pts
    nb = max(1, maxn // 2)
    size = len(pts) / nb
    out = []
    for i in range(nb):
        chunk = pts[int(i * size):int((i + 1) * size)]
        if not chunk:
            continue
        lo = min(chunk, key=lambda p: p[1])
        hi = max(chunk, key=lambda p: p[1])
        out.extend(sorted({id(lo): lo, id(hi): hi}.values(), key=lambda p: p[0]))
    return out


# ---- CSV ----

def write_csvs(d, qs, as_, es, marks):
    paths = {}
    qcols = ["time", "boot", "khz", "age_ms", "level_dbuv", "usn_pct", "wam_pct", "offset_khz",
             "bw_khz", "mod_pct", "stereo", "softmute_pct", "highcut_pct", "stereo_blend_pct",
             "high_blend_pct", "rds_sync", "rds_groups", "rds_rejected", "af_flags",
             "cur_score_db", "need_dbuv"]
    p = d / "quality.csv"
    with open(p, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(qcols)
        for r in qs:
            g = r.get
            w.writerow([iso(r["time"]), g("boot", 1), g("f"), fmt(tenth(g("age")), 1),
                        *[fmt(tenth(g(k))) for k in ("lv", "usn", "wam", "off", "bw", "mod")],
                        g("st"), *[fmt(tenth(g(k))) for k in ("sm", "hc", "sb", "hb")],
                        g("sync"), g("g"), g("rej"), g("af"),
                        fmt(tenth(g("cur"))), fmt(tenth(g("need")))])
    paths["quality"] = p
    p = d / "af.csv"
    with open(p, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["time", "khz", "level_dbuv", "usn_pct", "wam_pct", "offset_khz", "score_db",
                    "score_avg_db", "level_avg_dbuv", "streak", "need_dbuv"])
        for r in as_:
            g = r.get
            w.writerow([iso(r["time"]), g("f"), *[fmt(tenth(g(k))) for k in ("lv", "usn", "wam", "off")],
                        fmt(tenth(g("sc"))), fmt(tenth(g("avg"))), fmt(tenth(g("lavg"))),
                        g("streak"), fmt(tenth(g("need")))])
    paths["af"] = p
    p = d / "events.csv"
    rows = [(e["time"], [iso(e["time"]), e.get("ev"), e.get("from"), e.get("to"),
                         fmt(tenth(e.get("sc"))), fmt(tenth(e.get("cur"))), e.get("pi") or "",
                         e.get("ban_s"), ""]) for e in es]
    rows += [(m["time"], [iso(m["time"]), "mark", "", "", "", "", "", "",
                          m.get("note", f"rating {m['rate']}" if "rate" in m else "")])
             for m in marks]
    rows.sort(key=lambda x: x[0])
    with open(p, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["time", "ev", "from_khz", "to_khz", "sc_db", "cur_db", "pi", "ban_s", "note"])
        for _, row in rows:
            w.writerow(row)
    paths["events"] = p
    return paths


# ---- SVG panels ----

def nice_step(span, target=8):
    for s in (1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600):
        if span / s <= target:
            return s
    return 43200


class Panel:
    W, H, L, R, T, B = 1400, 180, 64, 16, 14, 22

    def __init__(self, title, t0, t1, ylo, yhi, nd=1):
        self.title, self.t0, self.t1, self.nd = title, t0, max(t1, t0 + 1e-3), nd
        if yhi - ylo < 1e-9:
            ylo, yhi = ylo - 1, yhi + 1
        self.ylo, self.yhi = ylo, yhi
        self.parts, self.legend = [], []

    @staticmethod
    def autoy(vals, pad=0.08):
        vals = [v for v in vals if v is not None]
        if not vals:
            return 0.0, 1.0
        lo, hi = min(vals), max(vals)
        p = (hi - lo) * pad or 1.0
        return lo - p, hi + p

    def x(self, t):
        return self.L + (t - self.t0) / (self.t1 - self.t0) * (self.W - self.L - self.R)

    def y(self, v):
        v = min(max(v, self.ylo), self.yhi)
        return self.T + (1 - (v - self.ylo) / (self.yhi - self.ylo)) * (self.H - self.T - self.B)

    def line(self, pts, color, label, step=False, dash=None, width=1.4, gap=3.0):
        self.legend.append((color, label))
        pts = decimate(sorted(pts, key=lambda p: p[0]))
        segs, cur = [], []
        for p in pts:
            if cur and p[0] - cur[-1][0] > gap:
                segs.append(cur)
                cur = []
            cur.append(p)
        if cur:
            segs.append(cur)
        for seg in segs:
            d = f"M{self.x(seg[0][0]):.1f},{self.y(seg[0][1]):.1f}"
            for (t0, v0), (t1, v1) in zip(seg, seg[1:]):
                if step:
                    d += f"H{self.x(t1):.1f}"
                d += f"L{self.x(t1):.1f},{self.y(v1):.1f}"
            ds = f";stroke-dasharray:{dash}" if dash else ""
            self.parts.append(f'<path d="{d}" style="fill:none;stroke:{color};stroke-width:{width}{ds}"/>')

    def dots(self, pts, label=None, color=None, r=2.0):
        """pts: (t, v, color, tooltip)."""
        if label:
            self.legend.append((color or "#888", label))
        for t, v, c, tip in decimate(sorted(pts, key=lambda p: p[0])):
            title = f"<title>{html.escape(tip)}</title>" if tip else ""
            self.parts.append(f'<circle cx="{self.x(t):.1f}" cy="{self.y(v):.1f}" r="{r}" '
                              f'style="fill:{c or color};opacity:.8">{title}</circle>')

    def ticks(self, items):
        """items: (t, color, tooltip) full-height ticks."""
        for t, c, tip in items:
            x = self.x(t)
            self.parts.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{self.T}" y2="{self.H - self.B}" '
                              f'style="stroke:{c};stroke-width:1.2;opacity:.7">'
                              f'<title>{html.escape(tip)}</title></line>')

    def band(self, runs, color, label):
        self.legend.append((color, label))
        y1 = self.H - self.B
        for a, b in runs:
            xa, xb = self.x(a), max(self.x(b), self.x(a) + 1)
            self.parts.append(f'<rect x="{xa:.1f}" y="{y1 - 8}" width="{xb - xa:.1f}" height="8" '
                              f'style="fill:{color};opacity:.7"/>')

    def svg(self, marks):
        w, h = self.W, self.H
        out = [f'<svg viewBox="0 0 {w} {h}" width="{w}" height="{h}" xmlns="http://www.w3.org/2000/svg" '
               f'font-family="sans-serif" font-size="11">',
               f'<rect x="{self.L}" y="{self.T}" width="{w - self.L - self.R}" height="{h - self.T - self.B}" '
               f'style="fill:none;stroke:var(--grid)"/>']
        for i in range(5):
            v = self.ylo + (self.yhi - self.ylo) * i / 4
            y = self.y(v)
            out.append(f'<line x1="{self.L}" x2="{w - self.R}" y1="{y:.1f}" y2="{y:.1f}" style="stroke:var(--grid)"/>'
                       f'<text x="{self.L - 4}" y="{y + 4:.1f}" text-anchor="end" style="fill:var(--fg)">'
                       f'{v:.{self.nd}f}</text>')
        step = nice_step(self.t1 - self.t0)
        t = (int(self.t0 // step) + 1) * step
        while t < self.t1:
            x = self.x(t)
            out.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{self.T}" y2="{h - self.B}" style="stroke:var(--grid)"/>'
                       f'<text x="{x:.1f}" y="{h - 7}" text-anchor="middle" style="fill:var(--fg)">'
                       f'{time.strftime("%H:%M:%S", time.localtime(t))}</text>')
            t += step
        out.extend(self.parts)
        for m in marks:
            if not self.t0 <= m["time"] <= self.t1:
                continue
            x = self.x(m["time"])
            tip = m.get("note") or (f"rating {m['rate']}" if "rate" in m else "mark")
            lab = f'<text x="{x + 2:.1f}" y="{self.T + 10}" style="fill:var(--mark);font-weight:bold">{m["rate"]}</text>' \
                if "rate" in m else ""
            out.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{self.T}" y2="{h - self.B}" '
                       f'style="stroke:var(--mark);stroke-width:1.5"><title>{html.escape(tip)}</title></line>{lab}')
        lx = self.L + 6
        out.append(f'<text x="{lx}" y="{self.T + 11}" style="fill:var(--fg);font-weight:bold">{html.escape(self.title)}</text>')
        lx += 12 + 6.5 * len(self.title)
        for c, lab in self.legend:
            out.append(f'<rect x="{lx:.0f}" y="{self.T + 3}" width="10" height="8" style="fill:{c}"/>'
                       f'<text x="{lx + 13:.0f}" y="{self.T + 11}" style="fill:var(--fg)">{html.escape(lab)}</text>')
            lx += 24 + 6.2 * len(lab)
        out.append("</svg>")
        return "".join(out)


def rds_windows(qs, win=2.0):
    """(time, reject ratio %, sync runs) from deltas of g/rej per boot, reset on decrease."""
    acc, prev = {}, {}
    for q in qs:
        if "g" not in q or "rej" not in q:
            continue
        b = q.get("boot", 1)
        p = prev.get(b)
        prev[b] = (q["g"], q["rej"])
        if p is None or q["g"] < p[0] or q["rej"] < p[1]:
            continue
        w = int(q["time"] // win)
        a = acc.setdefault(w, [0, 0])
        a[0] += q["g"] - p[0]
        a[1] += q["rej"] - p[1]
    pts = [((w + 0.5) * win, 100.0 * r / (g + r)) for w, (g, r) in sorted(acc.items()) if g + r > 0]
    runs, start, last = [], None, None
    for q in qs:
        if q.get("sync"):
            if start is None:
                start = q["time"]
            last = q["time"]
        elif start is not None:
            runs.append((start, q["time"]))
            start = None
    if start is not None:
        runs.append((start, last))
    return pts, runs


def freq_color(f, freqs):
    return PALETTE[freqs.index(f) % len(PALETTE)]


def build_panels(qs, as_, es, t0, t1):
    ts = tuned_settled(qs)
    trial = [q for q in qs if is_trial(q) and is_settled(q)]
    panels = []

    p = Panel("Frequency MHz", t0, t1, *Panel.autoy([q["f"] / 1000 for q in qs if "f" in q]), nd=2)
    p.line([(q["time"], q["f"] / 1000) for q in qs if "f" in q], "#1f77b4", "q.f", step=True, gap=1e9)
    p.ticks([(e["time"], EV_COLORS.get(e.get("ev"), "#888"),
              f'{e.get("ev")} {mhz(e.get("from"))}->{mhz(e.get("to"))} sc {fmt(tenth(e.get("sc")))} '
              f'cur {fmt(tenth(e.get("cur")))} pi {e.get("pi") or "-"}') for e in es])
    panels.append(p)

    lv = [(q["time"], q["lv"] / 10) for q in ts if "lv" in q]
    tl = [(q["time"], q["lv"] / 10) for q in trial if "lv" in q]
    al = [a for a in as_ if a.get("lv") is not None]
    need = [(q["time"], q["need"] / 10) for q in qs if q.get("need") is not None]
    p = Panel("Level dBuV", t0, t1, *Panel.autoy([v for _, v in lv + tl + need] + [a["lv"] / 10 for a in al]))
    p.line(lv, "#1f77b4", "tuned")
    p.line(tl, "#d62728", "trial", gap=3.0)
    freqs = sorted({a["f"] for a in al})
    p.dots([(a["time"], a["lv"] / 10, freq_color(a["f"], freqs), f'{a["f"] / 1000:.2f} MHz') for a in al],
           "AF_Update (colour = frequency)", "#888", 2.2)
    p.line(need, "#555555", "need", dash="5,4", width=1.1, gap=1e9)
    panels.append(p)

    us = [(q["time"], q["usn"] / 10) for q in ts if "usn" in q]
    wm = [(q["time"], q["wam"] / 10) for q in ts if "wam" in q]
    p = Panel("USN / WAM %", t0, t1, *Panel.autoy([v for _, v in us + wm]))
    p.line(us, "#2ca02c", "USN")
    p.line(wm, "#ff7f0e", "WAM")
    panels.append(p)

    sc = [(q["time"], score(q)) for q in ts if score(q) is not None]
    cur = [(q["time"], q["cur"] / 10) for q in qs if q.get("cur") is not None]
    res = [e for e in es if e.get("ev") in ("switched", "jumped", "worse") and e.get("sc") is not None]
    p = Panel("Score dB", t0, t1, *Panel.autoy([v for _, v in sc + cur] + [e["sc"] / 10 for e in res]))
    p.line(sc, "#9aa5b1", "lv-usn-wam", width=0.8)
    p.line(cur, "#1f77b4", "cur")
    p.dots([(e["time"], e["sc"] / 10, EV_COLORS[e["ev"]], f'{e["ev"]} trial {e["sc"] / 10:.1f} dB') for e in res],
           "trial result", "#2a9d3a", 4.0)
    p.dots([(e["time"], e["cur"] / 10, "#1f77b4", f'reference {e["cur"] / 10:.1f} dB')
            for e in res if e.get("cur") is not None], "reference", "#1f77b4", 2.5)
    panels.append(p)

    p = Panel("DSP %", t0, t1, 0.0, 100.0, nd=0)
    for k, c, lab in (("sm", "#d62728", "soft mute"), ("hc", "#9467bd", "high cut"),
                      ("sb", "#2ca02c", "stereo blend"), ("hb", "#17becf", "high blend")):
        p.line([(q["time"], q[k] / 10) for q in ts if k in q], c, lab, width=1.1)
    panels.append(p)

    pts, runs = rds_windows(qs)
    p = Panel("RDS reject %", t0, t1, 0.0, 100.0, nd=0)
    p.line(pts, "#d62728", "rejected / all groups (2 s)", gap=6.0)
    p.band(runs, "#2a9d3a", "sync")
    panels.append(p)
    return panels


# ---- tables ----

def per_frequency(qs):
    rows = {}
    for i, q in enumerate(qs):
        if "f" not in q or not is_settled(q):
            continue
        nxt = qs[i + 1]["time"] if i + 1 < len(qs) else q["time"]
        r = rows.setdefault((q["f"], is_trial(q)), {"q": [], "t": 0.0, "g": 0, "rej": 0})
        r["q"].append(q)
        r["t"] += min(max(nxt - q["time"], 0.0), 1.0)
        if i + 1 < len(qs):
            n = qs[i + 1]
            if (n.get("boot", 1) == q.get("boot", 1) and n.get("g", 0) >= q.get("g", 0)
                    and n.get("rej", 0) >= q.get("rej", 0)):
                r["g"] += n.get("g", 0) - q.get("g", 0)
                r["rej"] += n.get("rej", 0) - q.get("rej", 0)
    out = []
    for (f, tr), r in sorted(rows.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        g = r["q"]
        lvs = [x["lv"] / 10 for x in g if "lv" in x]
        scs = [score(x) for x in g if score(x) is not None]
        ratio = 100.0 * r["rej"] / (r["g"] + r["rej"]) if r["g"] + r["rej"] else None
        out.append([f"{f / 1000:.2f}", "trial" if tr else "tuned", fmt(r["t"], 0), len(g),
                    fmt(mean(lvs)), fmt(median(lvs)),
                    fmt(mean([x["usn"] / 10 for x in g if "usn" in x])),
                    fmt(mean([x["wam"] / 10 for x in g if "wam" in x])),
                    fmt(mean(scs)), fmt(100.0 * sum(1 for x in g if x.get("st")) / len(g), 0),
                    fmt(ratio, 1)])
    return out


def calibration(qs, as_):
    """Per field: list of (AF_Update - settled mean of the same frequency)."""
    by_f = {}
    for q in tuned_settled(qs):
        if "f" in q:
            by_f.setdefault(q["f"], []).append(q)
    res = {"level": [], "usn": [], "wam": [], "score": []}
    for a in as_:
        samples = by_f.get(a.get("f"))
        if not samples:
            continue
        near = [q for q in samples if abs(q["time"] - a["time"]) <= CAL_WINDOW_S]
        if not near:
            continue
        nearest = min(near, key=lambda q: abs(q["time"] - a["time"]))
        if nearest["time"] <= a["time"]:
            use = [q for q in near if nearest["time"] - CAL_AVG_S <= q["time"] <= nearest["time"]]
        else:
            use = [q for q in near if nearest["time"] <= q["time"] <= nearest["time"] + CAL_AVG_S]
        for key, fld in (("level", "lv"), ("usn", "usn"), ("wam", "wam")):
            vals = [q[fld] / 10 for q in use if q.get(fld) is not None]
            if a.get(fld) is not None and vals:
                res[key].append(a[fld] / 10 - mean(vals))
        scs = [score(q) for q in use if score(q) is not None]
        if a.get("sc") is not None and scs:
            res["score"].append(a["sc"] / 10 - mean(scs))
    return res


def trial_calibration(qs, as_, es):
    rows = []
    for i, e in enumerate(es):
        if e.get("ev") != "trial" or e.get("to") is None:
            continue
        end = next((x["time"] for x in es[i + 1:] if x.get("ev") in RESULT_EVENTS), e["time"] + 30)
        samples = [q for q in qs if is_trial(q) and is_settled(q) and q.get("f") == e["to"]
                   and e["time"] <= q["time"] < end]
        prior = [a for a in as_ if a.get("f") == e["to"] and e["time"] - 60 <= a["time"] <= e["time"]]
        if not samples or not prior:
            continue
        a = prior[-1]
        lvm = mean([q["lv"] / 10 for q in samples if "lv" in q])
        scm = mean([score(q) for q in samples if score(q) is not None])
        rows.append({"time": e["time"], "to": e["to"], "n": len(samples),
                     "lavg": tenth(a.get("lavg")), "lv": lvm,
                     "avg": tenth(a.get("avg")), "sc": scm})
    return rows


def mark_rows(qs, marks):
    ts = tuned_settled(qs)
    out = []
    for m in marks:
        prev = [q for q in ts if m["time"] - 3.0 <= q["time"] <= m["time"]]
        what = (f"rating {m['rate']}" if "rate" in m else "") + (m.get("note") or "")
        out.append([time.strftime("%H:%M:%S", time.localtime(m["time"])), what or "mark",
                    *[fmt(mean([q[k] / 10 for q in prev if k in q])) for k in ("lv", "usn", "wam")],
                    fmt(mean([score(q) for q in prev if score(q) is not None])),
                    *[fmt(mean([q[k] / 10 for q in prev if k in q])) for k in ("sm", "hc")]])
    return out


# ---- HTML ----

CSS = """
:root{--bg:#fff;--fg:#222;--grid:#ddd;--mark:#b00020;--abort:#000;--th:#eee}
@media (prefers-color-scheme:dark){:root{--bg:#181a1b;--fg:#ddd;--grid:#3a3d40;--mark:#ff6b81;--abort:#fff;--th:#2a2d30}}
body{background:var(--bg);color:var(--fg);font:14px sans-serif;margin:12px;max-width:1420px}
svg{display:block;margin-bottom:6px}table{border-collapse:collapse;margin:6px 0 18px}
td,th{border:1px solid var(--grid);padding:2px 8px;text-align:right}th{background:var(--th)}
td:first-child,th:first-child{text-align:left}h2{margin:18px 0 4px}
"""


def table(headers, rows):
    if not rows:
        return "<p>No data.</p>"
    h = "".join(f"<th>{html.escape(str(x))}</th>" for x in headers)
    body = "".join("<tr>" + "".join(f"<td>{html.escape(str(c))}</td>" for c in r) + "</tr>" for r in rows)
    return f"<table><tr>{h}</tr>{body}</table>"


def render_html(d, qs, as_, es, marks, meta, t0, t1):
    parts = [f"<!DOCTYPE html><html><head><meta charset='utf-8'><title>Ride {html.escape(d.name)}</title>"
             f"<style>{CSS}</style></head><body><h1>Ride {html.escape(d.name)}</h1>"]
    fw = (meta or {}).get("fw") or {}
    parts.append(f"<p>{iso(t0)} to {iso(t1)}; {len(qs)} quality, {len(as_)} AF_Update, {len(es)} event "
                 f"records, {len(marks)} marks; firmware {html.escape(str(fw.get('version', '?')))}"
                 + (f"; note: {html.escape(str((meta or {}).get('note') or (meta or {}).get('args', {}).get('note') or ''))}"
                    if meta else "") + "</p>")
    for p in build_panels(qs, as_, es, t0, t1):
        parts.append(p.svg(marks))
    parts.append("<h2>Per tuned frequency</h2>")
    parts.append(table(["MHz", "kind", "time s", "samples", "level mean", "level median", "USN", "WAM",
                        "score", "% stereo", "RDS reject %"], per_frequency(qs)))
    parts.append("<h2>AF events</h2>")
    parts.append(table(["time", "event", "from -> to MHz", "sc dB", "cur dB", "pi", "ban s"],
                       [[time.strftime("%H:%M:%S", time.localtime(e["time"])), e.get("ev"),
                         f'{mhz(e.get("from"))} -> {mhz(e.get("to"))}',
                         fmt(tenth(e.get("sc"))), fmt(tenth(e.get("cur"))), e.get("pi") or "",
                         e.get("ban_s") if e.get("ban_s") is not None else ""] for e in es]))
    parts.append("<h2>AF_Update calibration (AF_Update minus settled tuned samples)</h2>")
    cal = calibration(qs, as_)
    parts.append(table(["field", "count", "mean", "median"],
                       [[k, len(v), fmt(mean(v), 2), fmt(median(v), 2)] for k, v in cal.items()]))
    parts.append("<h3>Trials: AF_Update before the trial vs trial samples</h3>")
    tc = trial_calibration(qs, as_, es)
    rows = [[time.strftime("%H:%M:%S", time.localtime(r["time"])), f'{r["to"] / 1000:.2f}', r["n"],
             fmt(r["lavg"]), fmt(r["lv"]), fmt(None if r["lavg"] is None else r["lv"] - r["lavg"], 2),
             fmt(r["avg"]), fmt(r["sc"]), fmt(None if r["avg"] is None or r["sc"] is None else r["sc"] - r["avg"], 2)]
            for r in tc]
    dl = [r["lv"] - r["lavg"] for r in tc if r["lavg"] is not None and r["lv"] is not None]
    ds = [r["sc"] - r["avg"] for r in tc if r["avg"] is not None and r["sc"] is not None]
    if tc:
        rows.append(["mean / median", "", "", "", "", f"{fmt(mean(dl), 2)} / {fmt(median(dl), 2)}",
                     "", "", f"{fmt(mean(ds), 2)} / {fmt(median(ds), 2)}"])
    parts.append(table(["time", "to MHz", "samples", "AF lavg", "trial level", "trial - AF",
                        "AF avg", "trial score", "trial - AF"], rows))
    parts.append("<h2>Marks (mean of the 3 s before)</h2>")
    parts.append(table(["time", "mark", "level", "USN", "WAM", "score", "soft mute %", "high cut %"],
                       mark_rows(qs, marks)))
    parts.append("</body></html>")
    return "".join(parts)


def build_report(d, tfrom=None, tto=None, out=None):
    d = Path(d)
    trace = read_jsonl(d / "trace.jsonl")
    qs, as_, es = assign_times(trace)
    marks = [dict(m, time=m["pc"]) for m in read_jsonl(d / "marks.jsonl")
             if m.get("k") == "mark" and isinstance(m.get("pc"), (int, float))]
    status_pcs = [s["pc"] for s in read_jsonl(d / "status.jsonl") if isinstance(s.get("pc"), (int, float))]
    meta = {}
    if (d / "meta.json").exists():
        try:
            meta = json.loads((d / "meta.json").read_text(encoding="utf-8"))
        except ValueError:
            pass
    alltimes = [r["time"] for r in qs + as_ + es + marks] + status_pcs
    if alltimes:
        first, last = min(alltimes), max(alltimes)
    else:
        first = last = time.time()
    lo = day_time(first, tfrom) if tfrom else first
    hi = day_time(first, tto) if tto else last
    qs, as_, es, marks = (window(x, lo, hi) for x in (qs, as_, es, marks))
    t0, t1 = (lo, hi) if (tfrom or tto) else (first, last)
    if t1 - t0 < 1.0:
        t1 = t0 + 1.0
    paths = write_csvs(d, qs, as_, es, marks)
    report = Path(out) if out else d / "report.html"
    report.write_text(render_html(d, qs, as_, es, marks, meta, t0, t1), encoding="utf-8")
    paths["report"] = report
    return paths


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Build an HTML report and CSVs from a ride directory.")
    ap.add_argument("ride_dir")
    ap.add_argument("--from", dest="tfrom", metavar="HH:MM:SS")
    ap.add_argument("--to", dest="tto", metavar="HH:MM:SS")
    ap.add_argument("--out", help="report file (default RIDE_DIR/report.html)")
    args = ap.parse_args(argv)
    if not Path(args.ride_dir).is_dir():
        print(f"{args.ride_dir}: not a directory", file=sys.stderr)
        return 1
    try:
        paths = build_report(args.ride_dir, args.tfrom, args.tto, args.out)
    except ValueError as e:
        print(f"bad time: {e}", file=sys.stderr)
        return 1
    for p in paths.values():
        print(p)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
