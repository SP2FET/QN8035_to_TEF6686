#!/usr/bin/env python3
"""Flash the bridge over Wi-Fi (web OTA) and wait until the new firmware is confirmed.

    python scripts/ota_upload.py [--host 192.168.4.1] .pio/build/lolin_s2_mini/firmware.bin
    pio run -e lolin_s2_mini -t ota          (builds first, see scripts/pio_ota.py)

1. Reads the app descriptor from the image and asks the device what it runs.
2. POSTs the image to /api/ota. The device verifies it, switches the boot slot and restarts.
3. Waits until the device answers with the new build, then until it leaves probation
   (main.c confirms after 30 s; a reset before that rolls back to the old image).

The host defaults to $OTA_HOST, else the access point address. The password, needed only
when BRIDGE_OTA_PASSWORD is set, comes from --password or $OTA_PASSWORD. Uses the Python
standard library only, so PlatformIO's interpreter runs it as is.
"""
import argparse
import http.client
import json
import os
import struct
import sys
import time

DEFAULT_HOST = "192.168.4.1"
DESC_OFFSET = 24 + 8          # esp_image_header_t + first esp_image_segment_header_t
DESC_MAGIC = 0xABCD5432       # ESP_APP_DESC_MAGIC_WORD
IMAGE_MAGIC = 0xE9
CHUNK = 4096
REBOOT_TIMEOUT_S = 60         # Wi-Fi AP back up and /api/status answering
CONFIRM_TIMEOUT_S = 75        # probation is 30 s after boot


class OtaError(Exception):
    pass


def read_image(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < DESC_OFFSET + 256 or data[0] != IMAGE_MAGIC:
        raise OtaError(f"{path}: not an ESP app image")
    (magic,) = struct.unpack_from("<I", data, DESC_OFFSET)
    if magic != DESC_MAGIC:
        raise OtaError(f"{path}: no app descriptor (not an ESP-IDF app image)")

    def text(off, size):
        raw = data[DESC_OFFSET + off:DESC_OFFSET + off + size]
        return raw.split(b"\0")[0].decode("ascii", "replace")

    info = {
        "version": text(16, 32),
        "project": text(48, 32),
        "built": f"{text(96, 16)} {text(80, 16)}",
        "idf": text(112, 32),
        # Status reports the first 8 descriptor bytes, independent of SDK log settings.
        "elf_sha256": data[DESC_OFFSET + 144:DESC_OFFSET + 152].hex(),
    }
    return data, info


def get_status(host, timeout=3.0):
    conn = http.client.HTTPConnection(host, timeout=timeout)
    try:
        conn.request("GET", "/api/status")
        resp = conn.getresponse()
        body = resp.read()
        if resp.status != 200:
            raise OSError(f"HTTP {resp.status}")
        return json.loads(body)
    finally:
        conn.close()


def describe(fw):
    return (f"{fw.get('version', '?')} built {fw.get('built', '?')}, "
            f"elf {fw.get('elf_sha256', '?')}, {fw.get('partition', '?')}, "
            f"state {fw.get('ota_state', '?')}")


def same_build(fw, want_sha):
    reported = fw.get("elf_sha256")
    # 0.1.0 used the SDK log-hash helper, whose default cap is 9 hex characters.
    # Accept that exact legacy length as well as the corrected 16-character API.
    return (isinstance(reported, str) and len(reported) in (9, 16)
            and len(want_sha) == 16
            and all(c in "0123456789abcdef" for c in reported)
            and want_sha.startswith(reported))


def upload(host, data, password):
    conn = http.client.HTTPConnection(host, timeout=60)
    conn.putrequest("POST", "/api/ota")
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Content-Length", str(len(data)))
    if password:
        conn.putheader("X-OTA-Password", password)
    conn.endheaders()
    t0 = time.monotonic()
    send_error = None
    tty = sys.stdout.isatty()
    shown = -1
    try:
        for off in range(0, len(data), CHUNK):
            conn.send(data[off:off + CHUNK])
            done = min(off + CHUNK, len(data))
            pct = done * 100 // len(data)
            if pct != shown and (tty or pct % 10 == 0):
                shown = pct
                rate = done / 1024 / max(time.monotonic() - t0, 1e-3)
                line = f"  sent {pct:3d} %  ({rate:4.0f} KiB/s)"
                print(f"\r{line}" if tty else line, end="" if tty else "\n", flush=True)
    except OSError as e:
        send_error = e  # the device may have refused early; its reply says why
    if tty:
        print()
    try:
        resp = conn.getresponse()
        body = resp.read().decode("utf-8", "replace")
    except OSError as e:
        raise OtaError(f"upload failed: {send_error or e}")
    finally:
        conn.close()
    try:
        msg = json.loads(body).get("msg", body)
    except ValueError:
        msg = body.strip() or f"HTTP {resp.status}"
    if resp.status != 200:
        hint = " (set OTA_PASSWORD or --password)" if resp.status == 403 else ""
        raise OtaError(f"device refused the image: HTTP {resp.status}: {msg}{hint}")
    print(f"  device: {msg} ({time.monotonic() - t0:.1f} s)")


def wait_for_new_build(host, want_sha, old_uptime):
    print("Waiting for the restart ...", flush=True)
    time.sleep(3)  # the device restarts 0.5 s after replying; do not catch the old one
    deadline = time.monotonic() + REBOOT_TIMEOUT_S
    while time.monotonic() < deadline:
        try:
            st = get_status(host)
        except (OSError, ValueError):
            time.sleep(1)
            continue
        fw = st.get("fw", {})
        if same_build(fw, want_sha):
            print(f"  running: {describe(fw)}")
            return
        if st.get("uptime_s", 1 << 30) < old_uptime:
            raise OtaError(f"device restarted into another build: {describe(fw)}")
        time.sleep(1)
    raise OtaError(f"no answer from {host} within {REBOOT_TIMEOUT_S} s; is this computer "
                   "still on the bridge's Wi-Fi?")


def wait_for_confirmation(host, want_sha):
    print("Waiting for the probation period to end ...", flush=True)
    deadline = time.monotonic() + CONFIRM_TIMEOUT_S
    state = "?"
    while time.monotonic() < deadline:
        try:
            st = get_status(host)
        except (OSError, ValueError):
            time.sleep(2)
            continue
        fw = st.get("fw", {})
        if not same_build(fw, want_sha):
            raise OtaError(f"device rolled back to the previous build: {describe(fw)}")
        state = fw.get("ota_state", "?")
        if state == "valid":
            print("  confirmed")
            return
        if state not in ("pending", "new"):
            # no rollback support in the running bootloader: nothing to wait for
            print(f"  OTA state '{state}': no probation (bootloader without rollback?)")
            return
        time.sleep(2)
    raise OtaError(f"still '{state}' after {CONFIRM_TIMEOUT_S} s: check the log (web UI)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", help="firmware.bin (the app image, not the merged flash image)")
    ap.add_argument("--host", default=os.environ.get("OTA_HOST") or DEFAULT_HOST,
                    help=f"device address (default: $OTA_HOST or {DEFAULT_HOST})")
    ap.add_argument("--password", default=os.environ.get("OTA_PASSWORD", ""),
                    help="BRIDGE_OTA_PASSWORD, if set (default: $OTA_PASSWORD)")
    ap.add_argument("--force", action="store_true",
                    help="upload even if the device already runs this build")
    ap.add_argument("--no-wait", action="store_true",
                    help="return once the device has accepted the image")
    ap.add_argument("--info", action="store_true",
                    help="show image and device versions, upload nothing")
    args = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)  # keeps order with stderr under PlatformIO

    try:
        data, img = read_image(args.image)
        print(f"Image : {img['project']} {img['version']} built {img['built']}, "
              f"elf {img['elf_sha256']}, {len(data)} bytes")
        try:
            st = get_status(args.host)
        except (OSError, ValueError) as e:
            raise OtaError(f"cannot reach http://{args.host}/api/status ({e}); join the "
                           "bridge's Wi-Fi or pass --host / OTA_HOST")
        fw = st.get("fw")
        if fw is None:
            raise OtaError("the device firmware has no web OTA yet: flash it once over USB "
                           "(pio run -e lolin_s2_mini -t upload)")
        print(f"Device: {describe(fw)}")
        if args.info:
            return 0
        if same_build(fw, img["elf_sha256"]) and not args.force:
            print("The device already runs this build; use --force to upload anyway.")
            return 0
        if fw.get("ota_password") and not args.password:
            raise OtaError("the device requires a password: set OTA_PASSWORD or --password")

        print(f"Uploading to http://{args.host}/api/ota ...")
        upload(args.host, data, args.password)
        if args.no_wait:
            return 0
        wait_for_new_build(args.host, img["elf_sha256"], st.get("uptime_s", 1 << 30))
        wait_for_confirmation(args.host, img["elf_sha256"])
        print("OTA update done.")
        return 0
    except OtaError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
