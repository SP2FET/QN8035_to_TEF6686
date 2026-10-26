"""Run with: python -B -m unittest discover -s test/scripts -v"""
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import ride_log as rl
import ride_report as rr


class WsTests(unittest.TestCase):
    def test_client_frame_is_masked_and_round_trips(self):
        key = bytes([1, 2, 3, 4])
        frame = rl.ws_encode(10, b"hello", key)
        self.assertEqual(frame[0], 0x8A)
        self.assertEqual(frame[1], 0x80 | 5)
        self.assertEqual(frame[2:6], key)
        self.assertNotEqual(frame[6:], b"hello")
        self.assertEqual(rl.WsDecoder().feed(frame), [(10, b"hello")])

    def test_length_forms(self):
        for n in (125, 126, 300, 65535, 65536, 70000):
            payload = b"x" * n
            if n < 126:
                head = bytes([0x81, n])
            elif n < 65536:
                head = bytes([0x81, 126]) + n.to_bytes(2, "big")
            else:
                head = bytes([0x81, 127]) + n.to_bytes(8, "big")
            self.assertEqual(rl.WsDecoder().feed(head + payload), [(1, "x" * n)], n)

    def test_fragments_split_reads_and_ping(self):
        d = rl.WsDecoder()
        data = (bytes([0x01, 3]) + b"abc" + bytes([0x89, 2]) + b"pp" + bytes([0x80, 2]) + b"de")
        out = []
        for i in range(len(data)):            # one byte at a time
            out += d.feed(data[i:i + 1])
        self.assertEqual(out, [(9, b"pp"), (1, "abcde")])

    def test_accept_key_rfc_example(self):
        self.assertEqual(rl.ws_accept(b"dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")


class TextTests(unittest.TestCase):
    def test_ansi_and_partial_lines(self):
        s = rl.LineSplitter()
        self.assertEqual(s.feed("\x1b[0;32mI (1) tag: one\x1b[0m\r\nI (2) ta"), ["I (1) tag: one"])
        self.assertEqual(s.feed("g: two\n\x1b[0;31mE x"), ["I (2) tag: two"])
        self.assertEqual(s.feed("\x1b[0m\n"), ["E x"])

    def test_marks(self):
        self.assertEqual(rl.parse_mark("", 1.0), {"pc": 1.0, "k": "mark"})
        self.assertEqual(rl.parse_mark("4\n", 1.0)["rate"], 4)
        self.assertEqual(rl.parse_mark("tunnel", 1.0)["note"], "tunnel")
        self.assertEqual(rl.parse_mark("12", 1.0)["note"], "12")


class TraceTests(unittest.TestCase):
    def test_lost_boot_and_clock(self):
        t = rl.TraceTracker()
        lines = t.process({"now": 1000, "next": 12, "first": 10, "lost": 4,
                           "r": [{"s": 10, "k": "q"}, {"s": 11, "k": "q"}]}, 100.0)
        self.assertEqual([x["k"] for x in lines], ["gap", "q", "q", "clock"])
        self.assertEqual(lines[0]["lost"], 4)
        self.assertEqual(lines[1]["boot"], 1)
        self.assertEqual(t.since, 12)
        lines = t.process({"now": 1500, "next": 13, "first": 12, "lost": 0, "r": [{"s": 12, "k": "q"}]}, 100.5)
        self.assertEqual([x["k"] for x in lines], ["q"])        # clock at most every 10 s
        lines = t.process({"now": 90, "next": 3, "first": 3, "lost": 0, "r": []}, 101.0)
        self.assertEqual(lines, [{"pc": 101.0, "k": "boot", "boot": 2}])
        self.assertEqual(t.since, 0)
        lines = t.process({"now": 120, "next": 2, "first": 0, "lost": 0, "r": [{"s": 0, "k": "q"}]}, 101.5)
        self.assertEqual(lines[0]["boot"], 2)

    def test_boot_id_change(self):
        t = rl.TraceTracker()
        t.process({"boot": "aa", "now": 1000, "next": 5, "first": 0, "lost": 0, "r": []}, 100.0)
        # restarted and already past the old position: only the id tells
        lines = t.process({"boot": "bb", "now": 9000, "next": 50, "first": 5, "lost": 0,
                           "r": [{"s": 5, "k": "q"}]}, 110.0)
        self.assertEqual(lines, [{"pc": 110.0, "k": "boot", "boot": 2}])
        self.assertEqual(t.since, 0)
        lines = t.process({"boot": "bb", "now": 9100, "next": 51, "first": 0, "lost": 0,
                           "r": [{"s": 0, "k": "q"}]}, 110.5)
        self.assertEqual(lines[0]["boot"], 2)

    def test_live_line_handles_missing(self):
        self.assertIn("--", rl.format_live(0, None, None, 0, 0))
        q = {"f": 94400, "lv": 345, "usn": 123, "wam": 80, "af": 10, "cur": 182, "need": 305}
        s = rl.format_live(0, q, {"freq_khz": 92000, "rds": {"ps": "RMF FM"}}, 5, 1, 92000)
        self.assertIn("92.00 MHz RMF FM lvl 34.5", s)
        self.assertIn("trial 94.40", s)
        self.assertIn("AF poor cur 18.2 need 30.5", s)


def synthetic_ride(d: Path):
    base = 1_800_000_000.0
    lines = []
    for boot, (dev0, n) in enumerate(((5000, 200), (100, 150)), start=1):
        pc0 = base + (0 if boot == 1 else 100)
        if boot == 2:
            lines.append({"pc": pc0, "k": "boot", "boot": 2})
        for i in range(n):
            ms = dev0 + i * 100
            pc = pc0 + i * 0.1
            if i % 50 == 0:
                lines.append({"pc": pc, "k": "clock", "now": ms, "boot": boot})
            trial = 8 if 60 <= i < 80 else 0
            lines.append({"s": i, "t": ms, "k": "q", "f": 94400 if trial else 92000,
                          "age": 1000 if i % 10 else 50, "lv": 300 + i % 7, "usn": 100, "wam": 80,
                          "off": 5, "bw": 1500, "mod": 300, "st": i % 2, "sm": 0, "hc": 100,
                          "sb": 500, "hb": 0, "sync": 1 if i > 20 else 0, "g": i, "rej": i // 4,
                          "af": 2 | trial, "cur": 150, "need": 280, "pc": pc, "boot": boot})
            if i % 5 == 0:
                lines.append({"s": i, "t": ms + 1, "k": "a", "f": 92000 if i % 10 else 94400,
                              "lv": 310, "usn": 110, "wam": 90, "off": 0, "sc": 110, "avg": 100,
                              "lavg": 300, "streak": 1, "need": 280, "pc": pc, "boot": boot})
        if boot == 1:
            lines.append({"s": 900, "t": dev0 + 5900, "k": "e", "ev": "trial", "from": 92000, "to": 94400,
                          "sc": 300, "cur": 150, "pi": None, "ban_s": 0, "pc": pc0 + 5.9, "boot": 1})
            lines.append({"s": 901, "t": dev0 + 8100, "k": "e", "ev": "worse", "from": 92000, "to": 94400,
                          "sc": 100, "cur": 150, "pi": "3F44", "ban_s": 0, "pc": pc0 + 8.1, "boot": 1})
            lines.append({"s": 902, "t": dev0 + 9000, "k": "e", "ev": "novel", "from": 92000, "to": 94400,
                          "sc": None, "cur": None, "pi": None, "ban_s": 0, "pc": pc0 + 9.0, "boot": 1})
    lines.append({"pc": base + 3, "k": "gap", "boot": 1, "lost": 5})
    (d / "trace.jsonl").write_text("\n".join(json.dumps(x) for x in lines) + "\nnot json\n", encoding="utf-8")
    marks = [{"pc": base + 5, "k": "mark", "rate": 2}, {"pc": base + 12, "k": "mark", "note": "tunnel"},
             {"pc": base + 13, "k": "mark"}]
    (d / "marks.jsonl").write_text("\n".join(json.dumps(x) for x in marks) + "\n", encoding="utf-8")
    return lines


def rows(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.reader(f))[1:]


class ReportTests(unittest.TestCase):
    def test_synthetic_ride(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            lines = synthetic_ride(d)
            paths = rr.build_report(d)
            text = paths["report"].read_text(encoding="utf-8")
            self.assertIn("<svg", text)
            self.assertEqual(text.count("<svg"), 6)
            self.assertIn("AF_Update calibration", text)
            self.assertIn("novel", text)
            self.assertEqual(len(rows(paths["quality"])), sum(1 for x in lines if x.get("k") == "q"))
            self.assertEqual(len(rows(paths["af"])), sum(1 for x in lines if x.get("k") == "a"))
            self.assertEqual(len(rows(paths["events"])), 3 + 3)

    def test_empty_ride_and_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            paths = rr.build_report(d)
            self.assertIn("<svg", paths["report"].read_text(encoding="utf-8"))
            self.assertEqual(rows(paths["quality"]), [])
            synthetic_ride(d)
            self.assertEqual(rr.main([str(d), "--from", "00:00:00", "--to", "00:00:01",
                                      "--out", str(d / "x.html")]), 0)
            self.assertTrue((d / "x.html").exists())

    def test_calibration_and_decimate(self):
        q = [{"time": float(i), "f": 92000, "age": 1000, "lv": 300, "usn": 100, "wam": 50, "af": 0}
             for i in range(20)]
        a = [{"time": 10.5, "f": 92000, "lv": 320, "usn": 100, "wam": 50, "sc": 170}]
        cal = rr.calibration(q, a)
        self.assertEqual(cal["level"], [2.0])
        self.assertEqual(cal["usn"], [0.0])
        self.assertEqual(cal["score"], [2.0])
        pts = [(i, (i * 7) % 13) for i in range(10000)]
        self.assertLessEqual(len(rr.decimate(pts)), rr.MAX_POINTS)


if __name__ == "__main__":
    unittest.main()
