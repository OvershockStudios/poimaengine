#!/usr/bin/env python3
"""Run the post-effect compile and admission contract against pinned DXC."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/project_post_effect.py"
SAMPLE = ROOT / "tests/fixtures/project_post_effect"
DEFAULT_DXC = ROOT / ".cache/toolchains/dxc-v1.8.2505.1"


def digest(payload):
    return hashlib.sha256(payload).hexdigest()


class ProjectPostEffectContract(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory(prefix="poima-post-contract-")
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        self.project = shutil.copytree(SAMPLE, self.root / "First project")
        self.manifest = self.project / "effect.json"
        self.source = self.project / "Vignette.hlsl"
        self.output = self.root / "example.poimapost"

    def run_compile(self, *, project=None, output=None, dxc=None, expected=None):
        path = (project or self.project) / self.manifest.name
        arguments = [sys.executable, str(SCRIPT), str(path), "--dxc-root", str(dxc or DXC), "--output", str(output or self.output)]
        if expected is not None:
            arguments.extend(["--expected-source-sha256", expected])
        return subprocess.run(arguments, capture_output=True, timeout=20)

    def compile(self, *, project=None, output=None, expected=None):
        result = self.run_compile(project=project, output=output, expected=expected)
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(result.stdout)
        self.assertEqual(receipt["status"], "ok")
        artifact = (output or self.output).read_bytes()
        self.assertEqual(receipt["artifact_sha256"], digest(artifact))
        data = json.loads(artifact)
        self.assertEqual(receipt["source_sha256"], data["source_sha256"])
        shader = base64.b64decode(data["spirv"]["data"], validate=True)
        self.assertEqual(len(shader), data["spirv"]["bytes"])
        self.assertEqual(digest(shader), data["spirv"]["sha256"])
        self.assertEqual(digest(shader), receipt["spirv_sha256"])
        return data

    def refused(self, text, *, project=None, output=None, dxc=None, expected=None):
        result = self.run_compile(project=project, output=output, dxc=dxc, expected=expected)
        self.assertEqual(result.returncode, 1, (result.stdout, result.stderr))
        body = json.loads(result.stderr)
        self.assertEqual(body["status"], "error")
        self.assertIn(text, body["diagnostic"])
        self.assertFalse((output or self.output).exists())
        return result.stderr

    def test_relocated_source_and_output_are_byte_identical(self):
        first = self.compile()
        relocated = shutil.copytree(self.project, self.root / "Moved source")
        second = self.root / "second.poimapost"
        self.assertEqual(first, self.compile(project=relocated, output=second))
        self.assertEqual(second.read_bytes(), self.output.read_bytes())
        self.assertEqual(first["manifest_sha256"], digest(self.manifest.read_bytes()))
        self.assertEqual(first["manifest"]["inputs"], ["scene_hdr"])
        self.assertEqual(first["interface"], {"entry": "poima_project_post_v1", "stage": "fragment",
                                               "descriptor_set": 0, "binding": 0, "color_location": 0})
        self.assertEqual([row["path"] for row in first["sources"]], ["Feather.hlsli", "Vignette.hlsl"])
        for row in first["sources"]:
            payload = (self.project / row["path"]).read_bytes()
            self.assertEqual(row["sha256"], digest(payload))
            self.assertEqual(row["bytes"], len(payload))

        original = self.output.read_bytes()
        failure = self.run_compile()
        self.assertEqual(failure.returncode, 1, failure.stderr)
        self.assertIn(b"output already exists", failure.stderr)
        self.assertEqual(self.output.read_bytes(), original)

    def test_edited_include_changes_artifact(self):
        inspection = subprocess.run([sys.executable, str(SCRIPT), str(self.manifest), "--inspect",
                                     "--dxc-root", str(self.root / "No compiler")], capture_output=True, timeout=10)
        self.assertEqual(inspection.returncode, 0, inspection.stderr)
        observed = json.loads(inspection.stdout)
        pinned = observed["source_sha256"]
        first = self.compile(expected=pinned)
        self.assertEqual(pinned, first["source_sha256"])
        self.assertEqual(observed["sources"], first["sources"])
        helper = self.project / "Feather.hlsli"
        helper.write_text(helper.read_text().replace("0.72", "0.56"))
        changed = self.root / "changed.poimapost"
        self.refused("Post-effect source SHA-256 conflict", output=changed, expected=pinned,
                     dxc=self.root / "No compiler")
        second = self.compile(output=changed)
        self.assertEqual(first["manifest_sha256"], second["manifest_sha256"])
        self.assertNotEqual(first["source_sha256"], second["source_sha256"])
        self.assertNotEqual(first["sources"][0]["sha256"], second["sources"][0]["sha256"])
        self.assertNotEqual(first["spirv"]["sha256"], second["spirv"]["sha256"])

    def test_dependency_scope_and_budget(self):
        source = self.source.read_text()
        self.source.write_text(source.replace('"Feather.hlsli"', '"../Feather.hlsli"'))
        self.refused("portable root-relative")
        self.source.write_text('#define HIDDEN "Feather.hlsli"\n#include HIDDEN\n' + source)
        self.refused("only literal quoted include directives")
        self.source.write_text(source)
        helper = self.project / "Feather.hlsli"
        helper.write_text('#include "Feather.hlsli"\n')
        self.refused("Cyclic post-effect include")
        helper.write_bytes(b"x" * (128 * 1024))
        self.refused("exceeds its")
        helper.unlink()
        helper.symlink_to(SAMPLE / helper.name)
        self.refused("Too many levels of symbolic links")

    def test_rejects_resources_and_operations_outside_the_declared_abi(self):
        manifest = self.manifest.read_text()
        self.manifest.write_text(manifest.replace('"scene_hdr"', '"scene_depth"'))
        self.refused("one scene_hdr input")
        self.manifest.write_text(manifest.replace('"version": 1', '"version": 1, "version": 1'))
        self.refused("Duplicate post-effect manifest field")
        self.manifest.write_text(manifest)
        self.source.write_text("""
[[vk::binding(0, 0)]] Texture2D<float4> extra;
float3 poima_post_effect(float3 hdr, float2 uv) {
    return extra.Load(int3(int2(uv * 8.0), 0)).rgb;
}
""")
        self.refused("one source image")
        self.source.write_text("""
[[vk::binding(1, 0)]] Texture2D<float4> extra;
float3 poima_post_effect(float3 hdr, float2 uv) {
    return hdr + extra.Load(int3(int2(uv * 8.0), 0)).rgb;
}
""")
        self.refused("one source image")
        self.source.write_text("""
float3 poima_post_effect(float3 hdr, float2 uv) {
    [loop] while (hdr.r > 0.5) { hdr *= 0.75; }
    return hdr;
}
""")
        self.refused("opcode 246")

    def test_relocated_compiler_diagnostics_and_replaced_compiler(self):
        self.source.write_text(self.source.read_text().replace("edge_gain(", "bad_function("))
        first = self.refused("Vignette.hlsl:")
        relocated = shutil.copytree(self.project, self.root / "Moved source")
        second = self.refused("Vignette.hlsl:", project=relocated)
        self.assertEqual(first, second)
        self.assertIn(b"bad_function", first)
        self.assertNotIn(str(self.root).encode(), first)
        unpinned = self.root / "Unpinned"
        (unpinned / "bin").mkdir(parents=True)
        (unpinned / "bin/dxc").write_bytes(b"Different compiler")
        self.refused("DXC hash mismatch for bin/dxc", dxc=unpinned)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxc-root", type=Path, default=DEFAULT_DXC)
    options, other = parser.parse_known_args()
    DXC = options.dxc_root.absolute()
    unittest.main(argv=[sys.argv[0], *other])
