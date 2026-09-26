"""Explicit renderer failure-path tests; may briefly create windows."""
# SPDX-License-Identifier: Apache-2.0
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv[1]).resolve())
sys.argv = [sys.argv[0]]


class RenderFailures(unittest.TestCase):
    def invoke_failure(self, *args, environment=None):
        process = subprocess.run([BINARY, "render-smoke", "--frames", "1", *args],
                                 capture_output=True, text=True, encoding="utf-8", env=environment, timeout=30)
        self.assertEqual(process.returncode, 4, process.stdout + process.stderr)
        reply = json.loads(process.stdout)
        self.assertEqual(reply["status"], "error")
        self.assertEqual(reply["diagnostics"][0]["code"], "render_smoke_failed")
        self.assertFalse(reply["result"]["capture_written"])
        self.assertIsNone(reply["result"]["capture_path"])
        self.assertFalse(reply["result"]["renderer_qualified"])
        return reply["result"]

    def test_unavailable_selected_device(self):
        result = self.invoke_failure("--gpu", "4095")
        self.assertEqual(result["frames_presented"], 0)
        self.assertIn("No selected device", result["detail"])

    def test_capture_to_missing_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "missing-parent" / "frame.bmp"
            result = self.invoke_failure("--capture", str(target))
            self.assertEqual(result["frames_presented"], 1)
            self.assertIn("BMP capture", result["detail"])
            self.assertFalse(target.exists())

    def test_missing_driver(self):
        with tempfile.TemporaryDirectory() as directory:
            environment = os.environ.copy()
            missing = str(Path(directory) / "missing-driver.json")
            environment.update(VK_DRIVER_FILES=missing, VK_ICD_FILENAMES=missing)
            result = self.invoke_failure(environment=environment)
            self.assertEqual(result["frames_presented"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
