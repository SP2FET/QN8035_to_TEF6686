"""Run with: python -B -m unittest discover -s test/scripts -v"""
import contextlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import ota_upload as ota


WANT_SHA = "9da3c1eb35f643ff"
OLD_SHA = "1234567890abcdef"


def status(sha, state="pending", uptime=1):
    return {"uptime_s": uptime, "fw": {"elf_sha256": sha, "ota_state": state}}


class OtaBuildTests(unittest.TestCase):
    def test_full_hash_and_legacy_sdk_hash_match(self):
        for sha in (WANT_SHA, WANT_SHA[:9]):
            with self.subTest(sha=sha):
                self.assertTrue(ota.same_build({"elf_sha256": sha}, WANT_SHA))

    def test_incorrect_or_malformed_hashes_do_not_match(self):
        for sha in (OLD_SHA, OLD_SHA[:9], WANT_SHA[:9] + "0000000", "", WANT_SHA[:8], WANT_SHA[:10],
                    WANT_SHA + "0", "z" * 9, None, 123):
            with self.subTest(sha=sha):
                self.assertFalse(ota.same_build({"elf_sha256": sha}, WANT_SHA))
        self.assertFalse(ota.same_build({}, WANT_SHA))

    def test_reboot_with_legacy_hash_is_not_another_build(self):
        with patch.object(ota, "get_status", return_value=status(WANT_SHA[:9])), \
                patch.object(ota.time, "sleep"), contextlib.redirect_stdout(io.StringIO()):
            ota.wait_for_new_build("bridge", WANT_SHA, 100)

    def test_old_server_then_wifi_disconnect_then_new_server(self):
        replies = [status(OLD_SHA, "valid", 101), OSError("Wi-Fi reconnecting"),
                   status(WANT_SHA, uptime=2)]
        with patch.object(ota, "get_status", side_effect=replies) as get, \
                patch.object(ota.time, "sleep"), contextlib.redirect_stdout(io.StringIO()):
            ota.wait_for_new_build("bridge", WANT_SHA, 100)
        self.assertEqual(3, get.call_count)

    def test_reboot_into_different_build_still_fails(self):
        with patch.object(ota, "get_status", return_value=status(OLD_SHA)), \
                patch.object(ota.time, "sleep"), self.assertRaisesRegex(ota.OtaError, "another build"):
            ota.wait_for_new_build("bridge", WANT_SHA, 100)

    def test_confirmation_handles_pending_legacy_hash_and_reconnection(self):
        replies = [OSError("Wi-Fi reconnecting"), status(WANT_SHA[:9]),
                   status(WANT_SHA[:9], "valid")]
        with patch.object(ota, "get_status", side_effect=replies) as get, \
                patch.object(ota.time, "sleep"), contextlib.redirect_stdout(io.StringIO()):
            ota.wait_for_confirmation("bridge", WANT_SHA)
        self.assertEqual(3, get.call_count)

    def test_rollback_after_restart_still_fails(self):
        replies = [status(WANT_SHA), status(OLD_SHA, "valid")]
        with patch.object(ota, "get_status", side_effect=replies), \
                patch.object(ota.time, "sleep"), self.assertRaisesRegex(ota.OtaError, "rolled back"):
            ota.wait_for_confirmation("bridge", WANT_SHA)

    def test_same_legacy_build_is_not_uploaded_again(self):
        with patch.object(ota.sys, "argv", ["ota_upload.py", "firmware.bin"]), \
                patch.object(ota.sys, "stdout"), \
                patch.object(ota, "read_image", return_value=(b"image", {
                    "version": "0.1.0", "project": "bridge", "built": "today",
                    "elf_sha256": WANT_SHA})), \
                patch.object(ota, "get_status", return_value=status(WANT_SHA[:9], "valid")), \
                patch.object(ota, "upload") as upload:
            self.assertEqual(0, ota.main())
        upload.assert_not_called()


class ArchiveStepTests(unittest.TestCase):
    def run_step(self, elf_bytes, want_sha, side_effect=None):
        with tempfile.TemporaryDirectory() as tmp:
            if elf_bytes is not None:
                with open(os.path.join(tmp, "firmware.elf"), "wb") as f:
                    f.write(elf_bytes)
            out = io.StringIO()
            with patch.object(ota.elf_archive, "archive", side_effect=side_effect,
                              return_value="id") as arch,                     contextlib.redirect_stdout(out):
                ota.archive_elf(os.path.join(tmp, "firmware.bin"), want_sha)
            return arch, out.getvalue()

    def test_matching_elf_is_archived(self):
        elf = b"elf"
        sha = ota.elf_archive.hashlib.sha256(elf).hexdigest()[:16]
        arch, _ = self.run_step(elf, sha)
        arch.assert_called_once()
        self.assertEqual("ota", arch.call_args[0][2])

    def test_other_or_missing_elf_is_skipped_with_a_note(self):
        for elf in (b"elf", None):
            with self.subTest(elf=elf):
                arch, out = self.run_step(elf, WANT_SHA)
                arch.assert_not_called()
                self.assertIn("note:", out)

    def test_archive_error_does_not_raise(self):
        elf = b"elf"
        sha = ota.elf_archive.hashlib.sha256(elf).hexdigest()[:16]
        arch, out = self.run_step(elf, sha, side_effect=OSError("disk full"))
        self.assertIn("WARNING", out)


if __name__ == "__main__":
    unittest.main()
