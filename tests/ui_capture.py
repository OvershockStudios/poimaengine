#!/usr/bin/env python3
"""Explicit GPU readback qualification of the common native UI composition pass."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def image(path):
    data = path.read_bytes()
    assert data[:2] == b"BM"
    offset = struct.unpack_from("<I", data, 10)[0]
    header, width, signed_height, planes, bits, compression = struct.unpack_from("<IiiHHI", data, 14)
    assert header >= 40 and width == 320 and abs(signed_height) == 240
    assert planes == 1 and bits in (24, 32) and compression in (0, 3)
    if compression == 3:
        assert struct.unpack_from("<III", data, 54) == (0xff0000, 0xff00, 0xff)
    height = abs(signed_height)
    stride = ((width * bits + 31) // 32) * 4
    assert offset + stride * height <= len(data)

    def pixel(x, y):
        row = height - y - 1 if signed_height > 0 else y
        start = offset + row * stride + x * (bits // 8)
        b, g, r = data[start:start + 3]
        return (r, g, b)

    return pixel, hashlib.sha256(data).hexdigest()


def encode(linear):
    return round(255 * (12.92 * linear if linear <= .0031308 else 1.055 * linear ** (1 / 2.4) - .055))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--samples", type=int, choices=(1, 4), default=4)
    parser.add_argument("--windows-interop", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    prefix = (args.output / f"ui-gpu{args.gpu}-msaa{args.samples}").resolve()
    for suffix in ("-baseline.bmp", "-ui.bmp"):
        Path(str(prefix) + suffix).unlink(missing_ok=True)
    argument = str(prefix)
    if args.windows_interop:
        argument = subprocess.check_output(["wslpath", "-w", argument], text=True).strip()
    result = subprocess.run([str(args.binary.resolve()), argument, str(args.gpu), str(args.samples)],
                            capture_output=True, text=True, encoding="utf-8", timeout=90)
    evidence = {"exit_code": result.returncode, "stderr": result.stderr, "gpu_index": args.gpu,
                "requested_samples": args.samples, "passed": False}
    try:
        evidence["response"] = json.loads(result.stdout)
        assert result.returncode == 0, result.stdout + result.stderr
        for report in evidence["response"].values():
            if isinstance(report, dict):
                assert report["success"] and report["capture_written"] and report["hardware"]
                assert report["validation_errors"] == 0 and report["samples"] == args.samples
        pixel, digest = image(Path(str(prefix) + "-ui.bmp"))
        _, baseline_digest = image(Path(str(prefix) + "-baseline.bmp"))
        assert digest != baseline_digest
        alpha = 128 / 255
        grey = encode((((.5 + .055) / 1.055) ** 2.4) * alpha)
        probes = {
            "opaque": ((40, 40), (255, 0, 0)),
            "linear_overlap": ((90, 60), (encode(1 - alpha), encode(alpha), 0)),
            "linear_alpha": ((140, 80), (0, encode(alpha), 0)),
            "clip_inside": ((200, 40), (0, 0, 255)),
            "clip_left": ((185, 40), (0, 0, 0)),
            "clip_bottom": ((210, 70), (0, 0, 0)),
            "transformed": ((40, 170), (255, 255, 0)),
            "transform_outside": ((65, 170), (0, 0, 0)),
            "premultiplied_texture": ((120, 175), (grey, grey, grey)),
            "background": ((300, 220), (0, 0, 0)),
        }
        evidence["probes"] = {}
        for name, ((x, y), expected) in probes.items():
            actual = pixel(x, y)
            evidence["probes"][name] = {"pixel": [x, y], "expected": expected, "actual": actual}
            assert all(abs(a - b) <= 2 for a, b in zip(actual, expected)), (name, actual, expected)
        evidence.update(passed=True, image_sha256=digest, baseline_sha256=baseline_digest)
    finally:
        Path(str(prefix) + ".json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    main()
