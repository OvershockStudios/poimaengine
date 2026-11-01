#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the pinned patch on temporary copies only; never edits the supplied SDK."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import tempfile


FILES = (
    ("Jolt/Physics/Body/BodyManager.h", "151d8d31a120137c4696a81bccbeff832e40b9b53e24edd802e29c207a6c31e6", "9b48e53c3e4262b3da83a99e68b4e3719a473e07af3cd0d3bf7521476a5dca7b"),
    ("Jolt/Physics/Body/BodyManager.cpp", "d52f17bc72f96dbaaef5188758021d8feb83b47eb803e6b42340e3046c570dd0", "9759a588fc4e7e5af7bfd3967e051941a649437d3da54b0888c0498856259bf0"),
    ("Jolt/Physics/PhysicsSystem.h", "54e188496125baa8f802e36374f24cbdb125c28babe931f4c24d335b140a9671", "75bc2080736dc48467b3f2b04655ba92bc1cc72236d5dfa73e9caa80010f8df9"),
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def original_bytes(source, fragments, row):
    relative, original, patched = row
    data = (source / relative).read_bytes()
    if digest(data) == original:
        return data
    if digest(data) != patched:
        raise AssertionError(f"Unsupported test input: {relative}")
    text = data.decode()
    if relative.endswith("BodyManager.h"):
        for name in ("owner_state_type.inc", "owner_state_public.inc"):
            text = text.replace((fragments / name).read_text(), "")
        text = text.replace("\tbool ValidatePoimaOwnerStateLocked(const PoimaBodyOwnerState &inState, Array<uint8> &active_seen, Array<uint8> &flagged_seen, Array<uint8> &queued_live_seen) const;\n\n", "")
    elif relative.endswith("BodyManager.cpp"):
        text = text.replace("\n#include <Jolt/Core/ScopeExit.h>", "")
        text = text.replace((fragments / "owner_state_methods.inc").read_text(), "")
    else:
        text = text.replace((fragments / "owner_state_forward.inc").read_text(), "")
    data = text.encode()
    if digest(data) != original:
        raise AssertionError(f"Patch does not reconstruct byte-identical original source: {relative}")
    return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--jolt-source", type=pathlib.Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    fragments = root / "cmake/patches/jolt_owner_state"
    originals = {row[0]: original_bytes(args.jolt_source, fragments, row) for row in FILES}
    with tempfile.TemporaryDirectory(prefix="poima-jolt-owner-patch-") as directory:
        temp = pathlib.Path(directory)
        sdk = temp / "sdk"
        package = temp / "package/cmake"
        shutil.copytree(root / "cmake/patches/jolt_owner_state", package / "patches/jolt_owner_state")
        package.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / "cmake/jolt_owner_state.cmake", package / "jolt_owner_state.cmake")
        driver = temp / "apply.cmake"
        driver.write_text(f'include("{(package / "jolt_owner_state.cmake").as_posix()}")\npoima_patch_jolt_owner_state("{sdk.as_posix()}")\n')

        def reset():
            for relative, data in originals.items():
                target = sdk / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)

        def run(success):
            result = subprocess.run([args.cmake, "-P", str(driver)], capture_output=True, text=True)
            if (result.returncode == 0) != success:
                raise AssertionError(result.stdout + result.stderr)

        def all_bytes():
            return {relative: (sdk / relative).read_bytes() for relative, _, _ in FILES}

        reset()
        run(True)
        for relative, _, expected in FILES:
            assert digest((sdk / relative).read_bytes()) == expected
        stamps = {(sdk / relative): (sdk / relative).stat().st_mtime_ns for relative, _, _ in FILES}
        run(True)
        assert all(path.stat().st_mtime_ns == stamp for path, stamp in stamps.items()), "Idempotent reapply rewrote source mtimes"
        methods = package / "patches/jolt_owner_state/owner_state_methods.inc"
        reviewed_methods = methods.read_bytes()
        methods.write_bytes(reviewed_methods + b"\n// changed unreviewed fragment\n")
        before = all_bytes()
        run(False)
        assert all_bytes() == before, "Patched cache ignored an unreviewed fragment"
        methods.write_bytes(reviewed_methods)
        # Supported partially applied cache is completed without touching patched files.
        (sdk / FILES[1][0]).write_bytes(originals[FILES[1][0]])
        run(True)
        assert (sdk / FILES[0][0]).stat().st_mtime_ns == stamps[sdk / FILES[0][0]]
        reset()
        altered = sdk / FILES[1][0]
        altered.write_bytes(altered.read_bytes() + b"\n// unknown source\n")
        before = all_bytes()
        run(False)
        assert all_bytes() == before, "Unknown source caused a partial patch"
        reset()
        methods = package / "patches/jolt_owner_state/owner_state_methods.inc"
        methods.write_bytes(methods.read_bytes() + b"\n// changed unreviewed fragment\n")
        before = all_bytes()
        run(False)
        assert all_bytes() == before, "Unreviewed fragment caused a partial patch"
    print(json.dumps({"status": "passed", "groups": 6, "scope": "temporary source copies only; no compile or SDK mutation"}))


if __name__ == "__main__":
    main()
