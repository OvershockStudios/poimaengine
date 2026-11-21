#!/usr/bin/env python3
"""Check compiled post-effect authoring, closure and frozen runtime ownership."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/python"))
from poima_client import RpcError, WorldClient


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(args, root):
    binary = str(args.binary.resolve())
    original = args.artifact.read_bytes()
    expected = digest(original)
    metadata = json.loads(original)

    def native(path):
        name = str(path.resolve())
        if args.windows_interop:
            return subprocess.check_output(["wslpath", "-w", name], text=True, timeout=10).strip()
        return name

    def cli(*arguments, exit_code=0):
        result = subprocess.run([binary, *arguments], capture_output=True, timeout=30)
        value = json.loads(result.stdout)
        assert result.returncode == exit_code, (value, result.stderr)
        return value["result"] if exit_code == 0 else value["diagnostics"]

    def refuses(owner, code, method, **params):
        try:
            owner.call(method, params, timeout=30)
        except RpcError as error:
            assert error.code == code, error
        else:
            raise AssertionError(f"{method} unexpectedly succeeded")

    project = root / "Post effect project"
    cli("project", "create", native(project), "--name", "Post effect contract")
    manifest = project / "project.poimaproject"
    world = project / json.loads(manifest.read_bytes())["entry"]["world"]
    source = world.parent / "Vignette.poimapost"
    source.write_bytes(original)
    environment, initial = next((key, entry["components"]["LightingEnvironment"])
                                for key, entry in json.loads(world.read_bytes())["entities"].items()
                                if "LightingEnvironment" in entry["components"])
    store = world.with_name(world.name + ".assets")
    package = store / (expected + ".poimapost")
    owner = WorldClient.open(binary, native(world))
    with owner:
        def request(method, **params):
            return owner.call(method, params, timeout=30)

        def select(value):
            active = request("world.inspect")["revision"]
            return request("world.transact", request_id=uuid.uuid4().hex, base_revision=active,
                           ops=[dict(op="component.set", id=environment, type="LightingEnvironment", value=value)])

        before = request("world.inspect")
        lighting = request("world.lighting")
        assert "post_effect" not in lighting and request("world.dependencies")["assets"] == []
        refuses(owner, -32602, "asset.post-effect.import", source=source.name)
        refuses(owner, -32009, "asset.post-effect.import", source=source.name, expected_sha256="0" * 64)
        broken = world.parent / "Broken.poimapost"
        malformed = b'{"format":"unrecognized"}'
        broken.write_bytes(malformed)
        refuses(owner, -32050, "asset.post-effect.import", source=broken.name, expected_sha256=digest(malformed))
        assert not store.exists()

        receipt = request("asset.post-effect.import", source=source.name, expected_sha256=expected)
        assert receipt == dict(asset=expected, id=metadata["manifest"]["id"],
                               bytes=len(original), words=metadata["spirv"]["bytes"] // 4), receipt
        assert package.read_bytes() == original
        assert request("asset.post-effect.import", source=source.name, expected_sha256=expected) == receipt
        assert request("world.inspect") == before
        assert request("world.dependencies")["assets"] == []

        # An existing invalid destination must survive an attempted reimport.
        package.write_bytes(malformed)
        refuses(owner, -32050, "asset.post-effect.import", source=source.name, expected_sha256=expected)
        assert package.read_bytes() == malformed and list(store.iterdir()) == [package]
        package.write_bytes(original)

        transaction = dict(request_id=uuid.uuid4().hex, base_revision=before["revision"],
                           ops=[dict(op="component.set", id=environment, type="LightingEnvironment",
                                     value={**initial, "post_effect": None})])
        refuses(owner, -32602, "world.transact", **transaction)
        selected = select({**initial, "post_effect": expected})
        dependency = dict(filename=package.name, sha256=expected, bytes=len(original))
        assert request("world.lighting") == {**lighting, "revision": selected["revision"], "post_effect": expected}
        assert request("world.dependencies")["assets"] == [dependency]
        assert cli("project", "inspect", native(manifest))["assets"] == [dependency]
        source.unlink()

        session = uuid.uuid4().hex
        if args.runtime:
            request("runtime.start", session_id=session, revision=selected["revision"])
            assert request("runtime.lighting", session_id=session)["post_effect"] == expected
        package.unlink()
        refuses(owner, -32050, "world.dependencies")
        package.write_bytes(malformed)
        refuses(owner, -32050, "world.dependencies")
        if args.runtime:
            request("runtime.step", session_id=session, request_id=uuid.uuid4().hex, expected_tick=0, ticks=1)
            assert request("runtime.lighting", session_id=session, tick=1)["post_effect"] == expected
            request("runtime.stop", session_id=session)
            refuses(owner, -32050, "runtime.start", session_id=uuid.uuid4().hex, revision=selected["revision"])
        removed = select(initial)
        assert request("world.lighting") == {**lighting, "revision": removed["revision"]}
        assert request("world.dependencies")["assets"] == []
        if args.runtime:
            request("runtime.start", session_id=uuid.uuid4().hex, revision=removed["revision"])
    assert owner.transport.returncode == 0
    assert cli("project", "inspect", native(manifest))["assets"] == []
    return {"artifact_sha256": expected, "selected_revision": selected["revision"], "runtime_checked": args.runtime == 1}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--runtime", type=int, choices=(0, 1), default=1)
    parser.add_argument("--windows-interop", action="store_true")
    args = parser.parse_args()
    directory = ROOT / "build" if args.windows_interop or os.name == "nt" else None
    with tempfile.TemporaryDirectory(prefix="post-effect-world-", dir=directory) as location:
        print(json.dumps(run(args, Path(location))))


if __name__ == "__main__":
    main()
