#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check verifier failure paths without a USB device or decoder subprocess."""

import hashlib
import importlib.util
import io
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("verify_mkv", ROOT / "scripts/verify-mkv.py")
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class VerifierTests(unittest.TestCase):
    def test_missing_samples_is_readable(self):
        with self.assertRaisesRegex(SystemExit, "no finalized audio and video"):
            verify.check_live(Path("capture"), {})

    def test_rejects_unknown_profile_and_missing_or_external_media(self):
        """A malformed report must fail before it can launch a decoder or skip hash checks."""
        report = dict(finalized=True, video_frames_submitted=1, audio_samples_submitted=1,
                      stop_reason="duration_reached")
        for update in ({"profile": "typo"}, {"media": None}, {"media": "../capture.mkv"},
                       {"media": "/tmp/capture.mkv"}):
            with self.subTest(update=update), patch.object(verify.subprocess, "Popen") as process:
                with self.assertRaises(SystemExit):
                    verify.check_live(Path("capture"), report | update)
                process.assert_not_called()

    def test_checks_stderr_written_during_process_exit(self):
        """A decoder may close stdout before writing its final CRC diagnostic."""
        class Decoder:
            def __init__(self, *args, stderr, **kwargs):
                self.stdout = io.BytesIO(b"pcm")
                self.errors = stderr

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return False

            def wait(self):
                self.errors.write(b"CRC mismatch\n")
                return 0

        with patch.object(verify.subprocess, "Popen", Decoder):
            with self.assertRaisesRegex(SystemExit, "slice CRC failure"):
                verify.check_stream_hash(Path("capture.mkv"), "0:a:0", [], 3,
                                         hashlib.sha256(b"pcm").hexdigest())


if __name__ == "__main__":
    unittest.main()
