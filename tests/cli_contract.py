"""Black-box tests of the compiled CLI; no Python is used by the engine."""
# SPDX-License-Identifier: Apache-2.0
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv[1]).resolve())
PROBE_ENABLED = sys.argv[2] == "1"
RENDER_ENABLED = len(sys.argv) > 3 and sys.argv[3] == "1"
EDITOR_ENABLED = len(sys.argv) > 4 and sys.argv[4] == "1"
sys.argv = [sys.argv[0]]


class CliContract(unittest.TestCase):
    def invoke(self, *args, expected=0, environment=None, cwd=None):
        process = subprocess.run(
            [BINARY, *args], capture_output=True, text=True, encoding="utf-8",
            timeout=15, env=environment, cwd=cwd,
        )
        self.assertEqual(process.returncode, expected, process.stdout + process.stderr)
        # A single JSON document on stdout, even when drivers log to stderr.
        document = json.loads(process.stdout)
        self.assertEqual(document["protocol_version"], 1)
        self.assertIsNone(document["request_id"])
        self.assertEqual(document["status"], "ok" if expected == 0 else "error")
        self.assertIsInstance(document["diagnostics"], list)
        return document

    def test_discovery_is_repeatable_and_has_no_fake_features(self):
        first = self.invoke("capabilities")
        self.assertEqual(first, self.invoke("capabilities"))
        self.assertEqual(first["result"]["qualification"], "bootstrap_with_authored_world")
        features = first["result"]["features"]
        self.assertTrue(features["scene_editing"])
        self.assertEqual(features["vulkan_device_inspection"], PROBE_ENABLED)
        self.assertEqual(features["render_smoke"], RENDER_ENABLED)
        self.assertEqual(features["scene_capture"], RENDER_ENABLED)
        self.assertEqual(features["editor"], EDITOR_ENABLED)
        self.assertTrue(features["mcp"])
        for name in ["renderer", "animation", "vfx", "hot_reload"]:
            self.assertFalse(features[name], name)
        for operation in first["result"]["commands"]:
            with self.subTest(command=operation["name"]):
                schema = self.invoke("schema", operation["name"])["result"]
                self.assertEqual(schema["type"], "object")
                self.assertFalse(schema["additionalProperties"])
                self.assertEqual(schema["title"], operation["name"])

    def test_help_and_version_aliases(self):
        self.assertEqual(self.invoke(), self.invoke("help"))
        self.assertEqual(self.invoke("--help"), self.invoke("help"))
        self.assertEqual(self.invoke("--version"), self.invoke("version"))

    def test_invalid_requests_are_machine_readable(self):
        cases = [("missing",), ("version", "extra"), ("schema",), ("schema", "x", "y"),
                 ("doctor", "--unknown"), ("doctor", "--graphics", "--graphics"),
                 ("doctor", "--require-hardware", "--require-hardware")]
        cases += [("editor",), ("editor","--frames"), ("editor","w.json","--frames","0"),
                  ("editor","w.json","--width","20"), ("editor","w.json","--frames","x"),
                  ("editor","w.json","--gpu","1","--gpu","0"), ("editor","w.json","--report")]
        cases += [("serve",), ("serve","w.json"), ("serve","w.json","--endpoint","../bad"),
                  ("connect",), ("connect","../bad"), ("connect","good","--timeout-ms","99"),
                  ("connect","good","--timeout-ms","600001"), ("editor","w.json","--endpoint","bad.name")]
        for args in cases:
            with self.subTest(args=args):
                self.assertTrue(self.invoke(*args, expected=2)["diagnostics"])

    def test_diagnostics_escape_user_strings(self):
        name = 'unknown"\\\n\t\x01'
        result = self.invoke("schema", name, expected=2)
        self.assertEqual(result["diagnostics"][0]["message"], "Unknown schema command: " + name)

    def test_render_arguments_fail_before_opening_a_window(self):
        for args in [("--frames", "0"), ("--frames", "10001"), ("--frames", "-1"),
                     ("--width", "127"), ("--height", "4097"), ("--gpu", "4096"),
                     ("--frames", "1x"), ("--frames", "4294967296"), ("--capture", ""),
                     ("--frames",), ("--allow-software", "--allow-software"), ("--bogus",),
                     ("--width", "128", "--width", "128")]:
            with self.subTest(args=args):
                self.invoke("render-smoke", *args, expected=2)

    @unittest.skipIf(RENDER_ENABLED, "Actual GPU execution is a separate integration test")
    def test_render_not_built_is_explicit(self):
        reply = self.invoke("render-smoke", "--frames", "1", expected=3)
        self.assertFalse(reply["result"]["available"])
        self.assertFalse(reply["result"]["capture_written"])

    @unittest.skipIf(EDITOR_ENABLED, "Native editor execution is a separate integration test")
    def test_editor_not_built_preserves_project(self):
        with tempfile.TemporaryDirectory() as directory:
            world = Path(directory) / "world.json"
            reply = self.invoke("editor", str(world), "--frames", "1", expected=3)
            self.assertEqual(reply["diagnostics"][0]["code"], "unavailable")
            self.assertFalse(world.exists())

    def test_headless_inspection_from_unrelated_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.invoke("doctor", cwd=directory)["result"]
        self.assertIn(result["host"]["os"], ["Windows", "Linux"])
        self.assertGreater(result["host"]["logical_cpus"], 0)
        self.assertGreater(result["host"]["physical_memory_bytes"], 0)
        self.assertEqual(result["graphics"]["status"], "not_requested")
        self.assertFalse(result["renderer_qualified"])

    def test_absent_driver_or_disabled_probe_is_explicit(self):
        # Loader overrides are process-local and point to a nonexistent manifest.
        with tempfile.TemporaryDirectory() as directory:
            environment = os.environ.copy()
            missing = str(Path(directory) / "no-vulkan-driver.json")
            environment["VK_DRIVER_FILES"] = missing
            environment["VK_ICD_FILENAMES"] = missing
            result = self.invoke("doctor", "--graphics", environment=environment)["result"]
            self.assertEqual(result["graphics"]["status"], "unavailable" if PROBE_ENABLED else "not_built")
            self.assertEqual(result["graphics"]["devices"], [])
            self.assertFalse(result["hardware_vulkan13_found"])
            self.invoke("doctor", "--require-hardware", expected=3, environment=environment)
            # Driver failure cannot prevent headless commands from working.
            self.invoke("capabilities", environment=environment)


if __name__ == "__main__":
    unittest.main(verbosity=2)
