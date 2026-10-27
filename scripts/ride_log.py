#!/usr/bin/env python3
"""Record a car ride from the bridge's Wi-Fi access point, so the device does no logging work.

    python scripts/ride_log.py [--host 192.168.4.1] [--out rides/NAME] [--note TEXT]
    python scripts/ride_report.py rides/NAME            (afterwards)

Writes into the output directory (default rides/<YYYYmmdd-HHMMSS> under the repo root):
  meta.json      host, start time, arguments, script version, firmware info
  trace.jsonl    records of GET /api/trace, plus "pc" (arrival time), "boot" (segment) and
                 "gap" / "boot" / "clock" lines
  status.jsonl   {"pc":..., "status": GET /api/status}
  console.log    the WebSocket log, ANSI colours stripped, prefixed with the PC time
  marks.jsonl    marks typed on stdin: empty line = mark, 1-5 = audio rating (5 = clean),
                 other text = note, q = quit (or Ctrl+C)
  tuning.jsonl   the reception tuning (GET /api/tuning) at the start, after a restart and
                 whenever it changed (checked every 5 min)
  coredump-*.bin a core dump the device holds (GET /api/coredump), fetched once each; once the
                 file is saved the device erases it (POST /api/coredump/erase, CRC-checked)

Problems the device reports are printed as they happen: TUNER lines from the trace (the tuner
unavailable, ready again, a failed boot) and HEALTH lines from /api/status (I/O errors and
retries, head-unit commands dropped or replayed, head-unit tunes timed out, restarts, a core
dump, stack or heap running low).

The host defaults to $OTA_HOST, else the access point address. Standard library only.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
import zlib
from datetime import datetime
from pathlib import Path

VERSION = "ride_log 1.1"
DEFAULT_HOST = "192.168.4.1"
REPO_ROOT = Path(__file__).resolve().parent.parent
WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
CLOCK_EVERY_S = 10.0
LIVE_WIDTH = 100
TUNING_EVERY_S = 300.0
# /api/status counters whose increase gets a HEALTH line: (dotted path, label).
WATCH = (
    ("tef.diag.recoveries", "tuner recoveries"),
    ("tef.diag.boot_failures", "tuner boot failures"),
    ("tef.diag.io_errors", "tuner I/O errors"),
    ("tef.diag.io_retries", "tuner I/O retries"),
    ("qn.dropped", "head-unit bus messages dropped"),
    ("qn.command_drops", "head-unit commands dropped"),
    ("qn.reconciles", "head-unit control replays"),
    ("bridge.timeouts", "head-unit tunes without a tuner answer"),
    ("bridge.settle_timeouts", "head-unit tunes reported before settling"),
    ("bridge.refused", "head-unit tunes refused"),
    ("af.mem.errors", "AF memory write errors"),
    ("sys.crashes", "unexpected resets since power-on"),
    ("web.select_errors", "httpd select errors"),
)
STACK_WARN_BYTES = 512
HEAP_WARN_BYTES = 16 * 1024


# ---- WebSocket (RFC 6455), pure parts ----

def ws_encode(opcode, payload=b"", mask_key=None):
    """One masked client frame, FIN set."""
    key = os.urandom(4) if mask_key is None else mask_key
    n = len(payload)
    head = bytes([0x80 | opcode])
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 65536:
        head += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
    return head + key + masked


class WsDecoder:
    """Feed bytes from the server, get whole messages as (opcode, payload).

    Text messages (opcode 1) come back decoded to str, fragments joined. Control frames
    (close 8, ping 9, pong 10) are returned as they are.
    """

    def __init__(self):
        self.buf = bytearray()
        self.frag_op = None
        self.frag = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            buf = self.buf
            if len(buf) < 2:
                break
            fin = bool(buf[0] & 0x80)
            op = buf[0] & 0x0F
            masked = bool(buf[1] & 0x80)
            n = buf[1] & 0x7F
            pos = 2
            if n == 126:
                if len(buf) < 4:
                    break
                n = struct.unpack_from(">H", buf, 2)[0]
                pos = 4
            elif n == 127:
                if len(buf) < 10:
                    break
                n = struct.unpack_from(">Q", buf, 2)[0]
                pos = 10
            key = b""
            if masked:
                key = bytes(buf[pos:pos + 4])
                if len(key) < 4:
                    break
                pos += 4
            if len(buf) < pos + n:
                break
            payload = bytes(buf[pos:pos + n])
            del self.buf[:pos + n]
            if masked:
                payload = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
            if op >= 8:
                out.append((op, payload))
                continue
            if op != 0:
                self.frag_op, self.frag = op, bytearray()
            self.frag += payload
            if fin:
                msg = bytes(self.frag)
                if self.frag_op == 1:
                    out.append((1, msg.decode("utf-8", "replace")))
                elif self.frag_op == 2:
                    out.append((2, msg))
                self.frag_op, self.frag = None, bytearray()
        return out


def ws_accept(key):
    return base64.b64encode(hashlib.sha1(key + WS_GUID).digest()).decode()


def ws_connect(host, path="/ws", timeout=5.0):
    """Open the socket, do the handshake, return (socket, bytes already read after it)."""
    sock = socket.create_connection((host, 80), timeout=timeout)
    try:
        key = base64.b64encode(os.urandom(16))
        req = (f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key.decode()}\r\n"
               f"Sec-WebSocket-Version: 13\r\n\r\n")
        sock.sendall(req.encode("ascii"))
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(1024)
            if not chunk:
                raise ConnectionError("closed during handshake")
            data += chunk
            if len(data) > 8192:
                raise ConnectionError("handshake too long")
        head, rest = data.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        if " 101" not in lines[0]:
            raise ConnectionError(f"handshake refused: {lines[0]}")
        hdrs = {}
        for ln in lines[1:]:
            k, _, v = ln.partition(":")
            hdrs[k.strip().lower()] = v.strip()
        if hdrs.get("sec-websocket-accept") != ws_accept(key):
            raise ConnectionError("bad Sec-WebSocket-Accept")
        return sock, rest
    except BaseException:
        sock.close()
        raise


# ---- log text ----

def strip_ansi(text):
    return ANSI_RE.sub("", text)


class LineSplitter:
    """Strips ANSI codes and returns complete lines; the partial tail waits for more text."""

    def __init__(self):
        self.partial = ""

    def feed(self, text):
        self.partial += strip_ansi(text)
        *lines, self.partial = self.partial.split("\n")
        return [ln.rstrip("\r") for ln in lines]


# ---- trace bookkeeping ----

class TraceTracker:
    """Turns /api/trace responses into the lines written to trace.jsonl."""

    def __init__(self):
        self.since = 0
        self.boot = 1
        self.boot_id = None
        self.last_clock = None

    def process(self, resp, pc):
        pc = round(pc, 3)
        lines = []
        nxt = int(resp.get("next", 0))
        # The device's "boot" id changes with every start; sequence numbers alone miss a restart
        # whose new run has already passed the old position.
        bid = resp.get("boot")
        restarted = bid is not None and self.boot_id is not None and bid != self.boot_id
        if bid is not None:
            self.boot_id = bid
        if restarted or nxt < self.since:
            self.boot += 1
            self.since = 0
            return [{"pc": pc, "k": "boot", "boot": self.boot}]
        lost = int(resp.get("lost", 0) or 0)
        if lost > 0:
            lines.append({"pc": pc, "boot": self.boot, "k": "gap", "lost": lost})
        for r in resp.get("r", []):
            if isinstance(r, dict):
                lines.append(dict(r, pc=pc, boot=self.boot))
        if "now" in resp and (self.last_clock is None or pc - self.last_clock >= CLOCK_EVERY_S):
            self.last_clock = pc
            lines.append({"pc": pc, "k": "clock", "now": resp["now"], "boot": self.boot})
        self.since = nxt
        return lines


def parse_mark(text, pc):
    text = text.strip()
    m = {"pc": round(pc, 3), "k": "mark"}
    if re.fullmatch(r"[1-5]", text):
        m["rate"] = int(text)
    elif text:
        m["note"] = text
    return m


# ---- live line ----

def _f(v, scale=10.0, fmt="{:.1f}"):
    return "--" if v is None else fmt.format(v / scale)


def format_live(now, q, status, nrec, lost, tuned_khz=None):
    st = status or {}
    rds = st.get("rds") or {}
    khz = tuned_khz if tuned_khz is not None else st.get("freq_khz")
    flags = (q or {}).get("af", 0) or 0
    af = "poor" if flags & 2 else "on" if flags & 1 else "idle"
    if q is None:
        af = "--"
    trial = f"{q['f'] / 1000:.2f}" if q and flags & 8 and "f" in q else "--"
    down = "TUNER DOWN " if (st.get("tef") or {}).get("ready") is False else ""
    return (f"{time.strftime('%H:%M:%S', time.localtime(now))} {down}"
            f"{'--' if khz is None else format(khz / 1000, '.2f')} MHz {rds.get('ps') or '--'} "
            f"lvl {_f((q or {}).get('lv'))} usn {_f((q or {}).get('usn'))} "
            f"wam {_f((q or {}).get('wam'))} | AF {af} cur {_f((q or {}).get('cur'))} "
            f"need {_f((q or {}).get('need'))} | trial {trial} | rec {nrec} lost {lost}")


def format_event(e):
    def mhz(v):
        return "--" if v is None else f"{v / 1000:.2f}"
    return (f"EVENT {e.get('ev')} {mhz(e.get('from'))}->{mhz(e.get('to'))} sc {_f(e.get('sc'))} "
            f"cur {_f(e.get('cur'))} pi {e.get('pi') or '--'}"
            + (f" ban {e['ban_s']}s" if e.get("ban_s") else ""))


# ---- device health ----

def dig(obj, path):
    """obj["a"]["b"] for "a.b"; None where a level is missing."""
    for key in path.split("."):
        if not isinstance(obj, dict):
            return None
        obj = obj.get(key)
    return obj


class HealthWatch:
    """Compares consecutive /api/status replies and names what got worse."""

    def __init__(self):
        self.prev = None
        self.warned = set()

    def check(self, st):
        prev, self.prev = self.prev, st
        msgs = []
        up, pup = st.get("uptime_s"), (prev or {}).get("uptime_s")
        if isinstance(up, (int, float)) and isinstance(pup, (int, float)) and up < pup:
            reason = (st.get("sys") or {}).get("reset") or "?"
            self.warned.clear()
            return [f"device restarted (reset reason {reason}, it had run {pup} s)"]
        if prev is not None:
            for path, label in WATCH:
                a, b = dig(prev, path), dig(st, path)
                if isinstance(a, (int, float)) and isinstance(b, (int, float)) and b > a:
                    msgs.append(f"{label} +{b - a} (now {b})")
            tef = st.get("tef") or {}
            if (prev.get("tef") or {}).get("ready") and tef.get("ready") is False:
                msgs.append(f"tuner not ready: {tef.get('error') or '?'}")
        if dig(st, "sys.dump") and not dig(prev or {}, "sys.dump"):
            msgs.append("the device holds a core dump")
        heap = st.get("heap_min")
        if isinstance(heap, (int, float)) and heap < HEAP_WARN_BYTES and "heap" not in self.warned:
            self.warned.add("heap")
            msgs.append(f"free heap went down to {heap} bytes")
        for task, free in sorted((st.get("stacks") or {}).items()):
            if isinstance(free, (int, float)) and free < STACK_WARN_BYTES and task not in self.warned:
                self.warned.add(task)
                msgs.append(f"task {task}: stack margin down to {free} bytes")
        return msgs


def tuning_values(resp):
    """The values and defaults of GET /api/tuning, and whether the set is saved."""
    params = resp.get("params") if isinstance(resp, dict) else None
    ps = [p for p in params or [] if isinstance(p, dict) and "k" in p]
    return {"saved": resp.get("saved") if isinstance(resp, dict) else None,
            "v": {p["k"]: p.get("v") for p in ps}, "def": {p["k"]: p.get("def") for p in ps}}


def dump_key(st):
    """Identity of the core dump /api/status reports, None without one."""
    d = dig(st, "sys.dump")
    if not isinstance(d, dict):
        return None
    return f"{d.get('elf', '')}-{d.get('pc', '')}"


def fetch_coredump(host, directory, key):
    with urllib.request.urlopen(f"http://{host}/api/coredump", timeout=10.0) as r:
        data = r.read()
    name = "coredump-" + re.sub(r"[^0-9A-Za-z_-]", "_", key) + ".bin"
    (Path(directory) / name).write_bytes(data)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    req = urllib.request.Request(f"http://{host}/api/coredump/erase?crc={crc:08x}", data=b"",
                                 method="POST")
    try:
        with urllib.request.urlopen(req, timeout=10.0):
            pass
        return name, "erased on the device"
    except urllib.error.HTTPError as e:
        return name, f"kept on the device (HTTP {e.code})"
    except Exception as e:  # noqa: BLE001 - the saved file stays
        return name, f"kept on the device ({e})"


def format_health(h):
    return (f"TUNER {h.get('ev')} {h.get('why') or ''} (I/O errors {h.get('io_err')}, retries "
            f"{h.get('retries')}, recoveries {h.get('rec')})")


# ---- I/O ----

class Out:
    """Line-flushed output files and console, shared by the threads."""

    def __init__(self, directory):
        self.dir = Path(directory)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.files = {}
        self.state = {}
        self.nrec = 0
        self.lost = 0
        self.q = None
        self.tuned_khz = None
        self.status = None

    def _file(self, name):
        if name not in self.files:
            self.files[name] = open(self.dir / name, "a", encoding="utf-8")
        return self.files[name]

    def write(self, name, text):
        with self.lock:
            f = self._file(name)
            f.write(text)
            f.flush()

    def jsonl(self, name, obj):
        self.write(name, json.dumps(obj, separators=(",", ":")) + "\n")

    def say(self, text):
        with self.lock:
            sys.stdout.write("\r" + " " * LIVE_WIDTH + "\r" + text + "\n")
            sys.stdout.flush()

    def health(self, name, ok, err=None):
        with self.lock:
            if self.state.get(name) == ok:
                return
            was = name in self.state
            self.state[name] = ok
        if not ok:
            self.say(f"WARNING {name}: lost connection ({err})")
        elif was:
            self.say(f"{name}: connected again")


def http_get_json(host, path, timeout=2.0):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
        return json.loads(r.read())


def retry_loop(name, out, stop, period, step):
    """Call step() every period s; on any error back off 0.5 s -> 5 s."""
    delay = 0.5
    while not stop.is_set():
        try:
            step()
            out.health(name, True)
            delay = 0.5
            stop.wait(period)
        except Exception as e:  # noqa: BLE001 - never let a recorder thread die
            out.health(name, False, e)
            stop.wait(delay)
            delay = min(delay * 2, 5.0)


def trace_thread(host, out, stop, period):
    tracker = TraceTracker()

    def step():
        resp = http_get_json(host, f"/api/trace?since={tracker.since}")
        if not isinstance(resp, dict):
            raise ValueError("bad trace response")
        for line in tracker.process(resp, time.time()):
            out.jsonl("trace.jsonl", line)
            k = line.get("k")
            if k == "e":
                out.say(format_event(line))
            elif k == "h":
                out.say(format_health(line))
            elif k == "boot":
                out.say(f"device restarted, boot segment {line['boot']}")
            if k in ("q", "a", "e", "h"):
                out.nrec += 1
            if k == "q":
                out.q = line
                if not line.get("af", 0) & 8 and "f" in line:
                    out.tuned_khz = line["f"]
            elif k == "gap":
                out.lost += line["lost"]

    retry_loop("trace", out, stop, period, step)


def status_thread(host, out, stop, period, coredump=True):
    watch = HealthWatch()
    tuning = {"last": None, "at": None, "warned": False}
    dumps = set()

    def save_tuning(pc, force):
        if not force and tuning["at"] is not None and pc - tuning["at"] < TUNING_EVERY_S:
            return
        tuning["at"] = pc
        vals = tuning_values(http_get_json(host, "/api/tuning", timeout=5.0))
        last = tuning["last"]
        if vals == last:
            return
        if last is not None:
            changed = sorted(k for k in vals["v"] if vals["v"][k] != last["v"].get(k))
            out.say("HEALTH tuning changed: " + (", ".join(f"{k}={vals['v'][k]}" for k in changed)
                                                 or f"saved {vals['saved']}"))
        tuning["last"] = vals
        out.jsonl("tuning.jsonl", dict(vals, pc=round(pc, 3)))

    def step():
        st = http_get_json(host, "/api/status")
        if not isinstance(st, dict):
            raise ValueError("bad status response")
        pc = time.time()
        out.status = st
        out.jsonl("status.jsonl", {"pc": round(pc, 3), "status": st})
        msgs = watch.check(st)
        for msg in msgs:
            out.say("HEALTH " + msg)
        try:
            save_tuning(pc, any(m.startswith("device restarted") for m in msgs))
            tuning["warned"] = False
        except Exception as e:  # noqa: BLE001 - the status line is written already
            tuning["at"] = None
            if not tuning["warned"]:
                tuning["warned"] = True
                out.say(f"tuning not read ({e})")
        key = dump_key(st) if coredump else None
        if key and key not in dumps:
            dumps.add(key)
            try:
                name, erase = fetch_coredump(host, out.dir, key)
                out.say(f"HEALTH core dump saved as {name}, {erase}")
            except Exception as e:  # noqa: BLE001
                out.say(f"core dump not saved ({e}); GET /api/coredump after the ride")

    retry_loop("status", out, stop, period, step)


def ws_thread(host, out, stop):
    delay = 0.5
    while not stop.is_set():
        sock = None
        try:
            sock, rest = ws_connect(host)
            sock.settimeout(1.0)
            out.health("websocket", True)
            delay = 0.5
            dec, split = WsDecoder(), LineSplitter()
            data = rest
            while not stop.is_set():
                for op, payload in dec.feed(data):
                    if op == 1:
                        for ln in split.feed(payload):
                            ts = time.time()
                            stamp = time.strftime("%H:%M:%S", time.localtime(ts))
                            out.write("console.log", f"{stamp}.{int(ts * 1000) % 1000:03d} {ln}\n")
                    elif op == 9:
                        sock.sendall(ws_encode(10, payload))
                    elif op == 8:
                        raise ConnectionError("server closed the WebSocket")
                data = b""
                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    continue
                if not data:
                    raise ConnectionError("connection closed")
        except Exception as e:  # noqa: BLE001
            out.health("websocket", False, e)
            stop.wait(delay)
            delay = min(delay * 2, 5.0)
        finally:
            if sock is not None:
                try:
                    if stop.is_set():
                        sock.sendall(ws_encode(8, b"\x03\xe8"))
                except OSError:
                    pass
                sock.close()


def live_thread(out, stop):
    while not stop.wait(1.0):
        line = format_live(time.time(), out.q, out.status, out.nrec, out.lost, out.tuned_khz)
        with out.lock:
            sys.stdout.write("\r" + line[:LIVE_WIDTH].ljust(LIVE_WIDTH))
            sys.stdout.flush()


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description="Record a ride from the bridge's Wi-Fi AP.")
    ap.add_argument("--host", default=os.environ.get("OTA_HOST", DEFAULT_HOST))
    ap.add_argument("--out", default=None, help="output directory (default rides/<time>)")
    ap.add_argument("--trace-period", type=float, default=0.5)
    ap.add_argument("--status-period", type=float, default=2.0)
    ap.add_argument("--no-ws", action="store_true", help="do not record the WebSocket log")
    ap.add_argument("--no-coredump", action="store_true",
                    help="do not fetch a core dump the device holds")
    ap.add_argument("--note", default="", help="free text stored in meta.json")
    return ap.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    started = datetime.now()
    outdir = Path(args.out) if args.out else REPO_ROOT / "rides" / started.strftime("%Y%m%d-%H%M%S")
    out = Out(outdir)
    meta = {"host": args.host, "start": started.isoformat(timespec="seconds"),
            "args": vars(args), "script": VERSION}
    try:
        meta["fw"] = http_get_json(args.host, "/api/status").get("fw")
    except Exception as e:  # noqa: BLE001
        print(f"device not reachable yet ({e}); recording anyway")
    (outdir / "meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    print(f"recording to {outdir}\nEnter = mark, 1-5 = audio rating (5 clean), text = note, q = quit")

    stop = threading.Event()
    threads = [threading.Thread(target=trace_thread, args=(args.host, out, stop, args.trace_period)),
               threading.Thread(target=status_thread,
                                args=(args.host, out, stop, args.status_period, not args.no_coredump)),
               threading.Thread(target=live_thread, args=(out, stop))]
    if not args.no_ws:
        threads.append(threading.Thread(target=ws_thread, args=(args.host, out, stop)))
    for t in threads:
        t.daemon = True
        t.start()
    try:
        while True:
            line = sys.stdin.readline()
            if line == "":
                # stdin closed (Ctrl+Z, or started without a console): readline() would return ""
                # at once from now on, so just sleep. Ctrl+C interrupts the sleep and ends the ride.
                while True:
                    time.sleep(1)
            if line.strip().lower() == "q":
                break
            mark = parse_mark(line, time.time())
            out.jsonl("marks.jsonl", mark)
            out.say(f"MARK {mark.get('rate', '')} {mark.get('note', '')}".rstrip())
    except KeyboardInterrupt:
        pass
    stop.set()
    for t in threads:
        t.join(timeout=2)
    print(f"\nstopped; {out.nrec} records, {out.lost} lost")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
