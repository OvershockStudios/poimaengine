#!/usr/bin/env python3
"""Integration check of actual GPU readback. Opens a window; run explicitly."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def inspect_bmp(path):
    data = path.read_bytes()
    if data[:2] != b"BM":
        raise AssertionError("Capture is not a BMP")
    offset = struct.unpack_from("<I", data, 10)[0]
    header, width, height, planes, bits, compression = struct.unpack_from("<IiiHHI", data, 14)
    if header < 40 or width <= 0 or height == 0 or planes != 1 or bits not in (24, 32):
        raise AssertionError("Unsupported capture dimensions/pixel format")
    if compression not in (0, 3):
        raise AssertionError("Expected uncompressed or bitfield BMP")
    if compression == 3:
        masks = struct.unpack_from("<III", data, 54)
        if masks != (0xff0000, 0xff00, 0xff):
            raise AssertionError(f"Unexpected BMP channel masks: {masks}")
    height = abs(height)
    stride = ((width * bits + 31) // 32) * 4
    if offset + stride * height > len(data):
        raise AssertionError("Truncated capture")
    step = bits // 8
    background = tuple(data[offset:offset + 3])
    counts = {"red": 0, "green": 0, "blue": 0, "foreground": 0}
    # Sample the entire image on a regular grid, independent of BMP row direction.
    samples = 0
    for y in range(0, height, 3):
        for x in range(0, width, 3):
            i = offset + y * stride + x * step
            blue, green, red = data[i:i + 3]
            samples += 1
            if max(abs(a - b) for a, b in zip((blue, green, red), background)) > 12:
                counts["foreground"] += 1
            for name, value, others in (("red", red, (green, blue)),
                                        ("green", green, (red, blue)), ("blue", blue, (red, green))):
                if value > max(others) + 25 and value > 80:
                    counts[name] += 1
    fractions = {name: value / samples for name, value in counts.items()}
    if not 0.20 < fractions["foreground"] < 0.30:
        raise AssertionError(f"Triangle coverage outside expected range: {fractions}")
    if any(fractions[name] < 0.025 for name in ("red", "green", "blue")):
        raise AssertionError(f"Missing expected interpolated vertex colors: {fractions}")
    return {"width": width, "height": height, "sample_fractions": fractions,
            "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--windows-interop", action="store_true")
    parser.add_argument("--allow-software", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    capture = (args.output / f"gpu-{args.gpu}.bmp").resolve()
    # Prevent an old image from satisfying a failed or missing new readback.
    capture.unlink(missing_ok=True)
    capture_arg = str(capture)
    if args.windows_interop:
        capture_arg = subprocess.check_output(["wslpath", "-w", str(capture)], text=True).strip()
    command = [str(args.binary.resolve()), "render-smoke", "--frames", "12", "--gpu", str(args.gpu),
               "--width", "960", "--height", "540", "--capture", capture_arg]
    if args.allow_software:
        command += ["--allow-software"]
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=60)
    record = {"command": command, "exit_code": result.returncode, "stderr": result.stderr}
    evidence = args.output / f"gpu-{args.gpu}.json"
    try:
        record["response"] = json.loads(result.stdout)
        report = record["response"]["result"]
        assert result.returncode == 0, result.stdout + result.stderr
        assert record["response"]["status"] == "ok"
        assert report["capture_written"] and report["frames_presented"] == 12
        assert report["nvrhi_errors"] == 0
        assert report["hardware"] or args.allow_software
        record["image_check"] = inspect_bmp(capture)
        assert (record["image_check"]["width"], record["image_check"]["height"]) == (report["width"], report["height"])
        record["passed"] = True
    finally:
        evidence.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
