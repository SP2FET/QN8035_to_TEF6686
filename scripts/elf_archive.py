#!/usr/bin/env python3
"""Keep the ELF of every flashed build, so a core dump can always be decoded.

    python scripts/elf_archive.py .pio/build/lolin_s2_mini/firmware.elf --target usb
    python scripts/elf_archive.py --find d10cb3ee2        (a 9-character core dump id)

The build id is the first 16 hex characters of the ELF's SHA-256 (fw.elf_sha256 in
/api/status). The ELF is copied to builds/<id>.elf and builds/index.tsv gets one line per
flash. Called by scripts/pio_archive.py (USB) and scripts/ota_upload.py. Standard library only.
"""
import argparse
import datetime
import hashlib
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DEST = os.path.join(ROOT, "builds")
HEADER = "id\tversion\tflashed_utc\ttarget\tgit\n"
MIN_PREFIX = 8


def build_id(elf_path):
    h = hashlib.sha256()
    with open(elf_path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def project_version():
    try:
        with open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8") as f:
            m = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', f.read())
        return m.group(1) if m else ""
    except OSError:
        return ""


def git_describe():
    try:
        out = subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT, text=True,
                             capture_output=True, timeout=10)
        return out.stdout.strip() if out.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def archive(elf_path, dest_dir, target, version=None, git_rev=None, now=None):
    """Copy the ELF to <dest_dir>/<id>.elf, append an index line, return the build id."""
    bid = build_id(elf_path)
    version = project_version() if version is None else version
    git_rev = git_describe() if git_rev is None else git_rev
    now = now or datetime.datetime.now(datetime.timezone.utc)
    os.makedirs(dest_dir, exist_ok=True)
    copy = os.path.join(dest_dir, bid + ".elf")
    if not (os.path.exists(copy) and build_id(copy) == bid):
        shutil.copyfile(elf_path, copy)
    index = os.path.join(dest_dir, "index.tsv")
    new = not os.path.exists(index)
    with open(index, "a", encoding="utf-8", newline="") as f:
        if new:
            f.write(HEADER)
        f.write(f"{bid}\t{version}\t{now.strftime('%Y-%m-%dT%H:%M:%SZ')}\t{target}\t{git_rev}\n")
    return bid


def find(dest_dir, prefix):
    """Paths of archived ELFs whose id starts with prefix (at least 8 hex characters)."""
    prefix = prefix.lower()
    if len(prefix) < MIN_PREFIX or not re.fullmatch(r"[0-9a-f]+", prefix):
        return []
    try:
        names = sorted(os.listdir(dest_dir))
    except OSError:
        return []
    return [os.path.join(dest_dir, n) for n in names
            if n.endswith(".elf") and n.startswith(prefix)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("elf", nargs="?", help="firmware.elf to archive")
    ap.add_argument("--target", choices=("usb", "ota"), help="how it was flashed")
    ap.add_argument("--dest", default=DEFAULT_DEST, help="archive directory (default: builds/)")
    ap.add_argument("--find", metavar="ID", help="print archived ELFs whose id starts with ID")
    args = ap.parse_args()
    if args.find:
        paths = find(args.dest, args.find)
        print("\n".join(paths))
        return 0 if paths else 1
    if not args.elf or not args.target:
        ap.error("give an ELF and --target, or --find ID")
    print(f"archived {archive(args.elf, args.dest, args.target)} in {args.dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
