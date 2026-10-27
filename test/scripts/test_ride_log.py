"""Run with: python -B -m unittest discover -s test/scripts -v"""
import csv
import http.server
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
import zlib

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
    lines.append({"s": 903, "t": 5000 + 9500, "k": "h", "f": 92000, "ev": "lost",
                  "why": "RDS flush failed", "op": 3, "io_err": 1, "retries": 2, "rec": 1,
                  "pc": base + 9.5, "boot": 1})
    lines.append({"s": 904, "t": 5000 + 10700, "k": "h", "f": 92000, "ev": "ready", "why": "mock",
                  "op": 3, "io_err": 1, "retries": 2, "rec": 1, "pc": base + 10.7, "boot": 1})
    lines.append({"pc": base + 3, "k": "gap", "boot": 1, "lost": 5})
    statuses = [
        {"pc": base + 1, "status": {"uptime_s": 100, "heap_free": 60000, "heap_min": 50000,
                                    "tef": {"ready": True, "diag": {"io_errors": 0, "recoveries": 0}},
                                    "qn": {"dropped": 0}, "stacks": {"radio": 900}}},
        {"pc": base + 10, "status": {"uptime_s": 109, "heap_free": 58000, "heap_min": 49000,
                                     "tef": {"ready": False, "diag": {"io_errors": 1, "recoveries": 1}},
                                     "qn": {"dropped": 3}, "stacks": {"radio": 850}}},
        {"pc": base + 101, "status": {"uptime_s": 2, "heap_free": 61000, "heap_min": 60000,
                                      "sys": {"reset": "panic", "warm": True, "crashes": 1},
                                      "tef": {"ready": True, "diag": {"io_errors": 0, "recoveries": 0}},
                                      "qn": {"dropped": 0}, "stacks": {"radio": 1000}}},
    ]
    (d / "status.jsonl").write_text("\n".join(json.dumps(x) for x in statuses) + "\n", encoding="utf-8")
    tunings = [{"pc": base, "saved": True, "v": {"af_up": 60, "af_gain": 20},
                "def": {"af_up": 60, "af_gain": 30}},
               {"pc": base + 50, "saved": False, "v": {"af_up": 40, "af_gain": 20},
                "def": {"af_up": 60, "af_gain": 30}}]
    (d / "tuning.jsonl").write_text("\n".join(json.dumps(x) for x in tunings) + "\n", encoding="utf-8")
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
            self.assertEqual(text.count("<svg"), 7)
            self.assertIn("Device health", text)
            self.assertIn("tuner recoveries", text)
            self.assertIn("RDS flush failed", text)
            self.assertIn("panic", text)
            self.assertIn("af_gain", text)
            self.assertEqual(len(rows(paths["health"])), 3)
            self.assertIn("AF_Update calibration", text)
            self.assertIn("novel", text)
            self.assertEqual(len(rows(paths["quality"])), sum(1 for x in lines if x.get("k") == "q"))
            self.assertEqual(len(rows(paths["af"])), sum(1 for x in lines if x.get("k") == "a"))
            self.assertEqual(len(rows(paths["events"])), 3 + 3 + 2)

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


class HealthTests(unittest.TestCase):
    def test_watch_reports_increases_restart_and_margins(self):
        w = rl.HealthWatch()
        base = {"uptime_s": 10, "tef": {"ready": True, "diag": {"recoveries": 0, "io_errors": 0}},
                "qn": {"command_drops": 0}, "stacks": {"radio": 2000}, "heap_min": 50000}
        self.assertEqual(w.check(base), [])
        worse = json.loads(json.dumps(base))
        worse["uptime_s"] = 12
        worse["tef"] = {"ready": False, "error": "I2C errors", "diag": {"recoveries": 1, "io_errors": 5}}
        worse["qn"]["command_drops"] = 2
        worse["stacks"]["radio"] = 300
        text = "\n".join(w.check(worse))
        self.assertIn("tuner recoveries +1", text)
        self.assertIn("tuner I/O errors +5", text)
        self.assertIn("head-unit commands dropped +2", text)
        self.assertIn("tuner not ready: I2C errors", text)
        self.assertIn("task radio: stack margin down to 300", text)
        self.assertEqual(w.check(worse), [])          # nothing new, stack warned once
        self.assertIn("device restarted", w.check(dict(worse, uptime_s=1))[0])

    def test_tuning_values_and_dump_key(self):
        resp = {"saved": True, "params": [{"k": "af_up", "v": 60, "def": 60}, {"k": "x"}, "junk"]}
        self.assertEqual(rl.tuning_values(resp), {"saved": True, "v": {"af_up": 60, "x": None},
                                                  "def": {"af_up": 60, "x": None}})
        self.assertIsNone(rl.dump_key({"sys": {"dump": None}}))
        self.assertEqual(rl.dump_key({"sys": {"dump": {"elf": "ab", "pc": "0x1"}}}), "ab-0x1")

    def test_counter_increase_across_restart(self):
        st = [{"time": float(i), "status": {"qn": {"dropped": v}}} for i, v in enumerate([0, 2, 5, 1, 4])]
        self.assertEqual(rr.counter_increase(st, "qn.dropped"), (8, 1.0, 4.0))
        hs = [{"time": 1.0, "ev": "lost", "why": "x"}, {"time": 2.5, "ev": "ready"},
              {"time": 3.0, "ev": "lost", "why": "y"}]
        self.assertEqual([r[2] for r in rr.outages(hs)], [1.5, None])


DUMP = bytes(range(256)) * 5


class DumpTests(unittest.TestCase):
    def serve(self, erase_status=None):
        """Device stand-in; erase_status forces the answer, else 200 on a matching crc, else 409."""
        posts = []

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, code, body=b'{"ok":true}'):
                self.send_response(code)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                self.reply(200, DUMP) if self.path == "/api/coredump" else self.reply(404)

            def do_POST(self):
                posts.append(self.path)
                crc = self.path.partition("crc=")[2]
                ok = crc == f"{zlib.crc32(DUMP) & 0xffffffff:08x}"
                self.reply(erase_status or (200 if ok else 409))

        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        return f"127.0.0.1:{srv.server_port}", posts

    def fetch(self, host):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        name, outcome = rl.fetch_coredump(host, tmp.name, "abc-0x400")
        self.assertEqual((Path(tmp.name) / name).read_bytes(), DUMP)
        return name, outcome

    def test_saved_then_erased_with_crc(self):
        host, posts = self.serve()
        name, outcome = self.fetch(host)
        self.assertEqual(name, "coredump-abc-0x400.bin")
        self.assertEqual(posts, [f"/api/coredump/erase?crc={zlib.crc32(DUMP):08x}"])
        self.assertIn("erased on the device", outcome)

    def test_crc_mismatch_keeps_file_and_dump(self):
        host, posts = self.serve(409)
        self.assertIn("kept on the device (HTTP 409)", self.fetch(host)[1])

    def test_erase_404_keeps_file(self):
        host, posts = self.serve(404)
        self.assertIn("kept on the device (HTTP 404)", self.fetch(host)[1])

    def test_erase_connection_failure_keeps_file(self):
        host, posts = self.serve()
        real = rl.urllib.request.urlopen

        def flaky(req, timeout=None):
            if getattr(req, "method", None) == "POST":
                raise OSError("connection closed")
            return real(req, timeout=timeout)

        rl.urllib.request.urlopen = flaky
        self.addCleanup(setattr, rl.urllib.request, "urlopen", real)
        self.assertIn("kept on the device (connection closed)", self.fetch(host)[1])


if __name__ == "__main__":
    unittest.main()
