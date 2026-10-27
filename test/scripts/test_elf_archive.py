"""Run with: python -B -m unittest discover -s test/scripts -v"""
import datetime
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import elf_archive as ea

NOW = datetime.datetime(2026, 10, 27, 12, 30, 5, tzinfo=datetime.timezone.utc)


class ElfArchiveTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        self.elf = self.dir / "firmware.elf"
        self.elf.write_bytes(b"\x7fELF fake image")
        self.sha = hashlib.sha256(b"\x7fELF fake image").hexdigest()[:16]
        self.dest = self.dir / "builds"

    def run_archive(self, target="usb"):
        return ea.archive(str(self.elf), str(self.dest), target, "0.5.1", "abc123", NOW)

    def test_id_is_sha256_prefix(self):
        self.assertEqual(self.sha, ea.build_id(str(self.elf)))
        self.assertEqual(self.sha, self.run_archive())

    def test_copy_and_index(self):
        self.run_archive()
        self.assertEqual(self.elf.read_bytes(), (self.dest / f"{self.sha}.elf").read_bytes())
        lines = (self.dest / "index.tsv").read_text().splitlines()
        self.assertEqual(["id\tversion\tflashed_utc\ttarget\tgit",
                          f"{self.sha}\t0.5.1\t2026-10-27T12:30:05Z\tusb\tabc123"], lines)

    def test_second_archive_adds_index_line_only(self):
        self.run_archive()
        copy = self.dest / f"{self.sha}.elf"
        mtime = copy.stat().st_mtime_ns
        self.run_archive("ota")
        self.assertEqual(mtime, copy.stat().st_mtime_ns)
        self.assertEqual(1, len(list(self.dest.glob("*.elf"))))
        lines = (self.dest / "index.tsv").read_text().splitlines()
        self.assertEqual(3, len(lines))
        self.assertTrue(lines[2].split("\t")[3] == "ota")

    def test_find_by_nine_char_prefix(self):
        self.run_archive()
        self.assertEqual([str(self.dest / f"{self.sha}.elf")],
                         ea.find(str(self.dest), self.sha[:9]))
        self.assertEqual([], ea.find(str(self.dest), "0" * 9 if self.sha[0] != "0" else "f" * 9))

    def test_find_rejects_short_prefix(self):
        self.run_archive()
        self.assertEqual([], ea.find(str(self.dest), self.sha[:7]))
        self.assertEqual([], ea.find(str(self.dest), ""))


if __name__ == "__main__":
    unittest.main()
