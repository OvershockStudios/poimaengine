#!/usr/bin/env python3
"""Fetch pinned, workspace-local build tools; never changes system packages/PATH."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
from pathlib import Path
import platform
import shutil
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
TOOLS = {
    "dotnet": {
        "url": "https://builds.dotnet.microsoft.com/dotnet/Sdk/10.0.401/dotnet-sdk-10.0.401-linux-x64.tar.gz",
        "sha512": "51c8b999af9e8dd9998c9edc5944e19a90788862068acd38694e098889054ce8c23d4f0c5cccfa16bf187d044562359e5ee69a9f8ad0bbe913ba90311fbce25b",
        "archive": "dotnet-sdk-10.0.401-linux-x64.tar.gz",
        "destination": ".cache/toolchains/dotnet-10.0.401",
        "ready": ".cache/toolchains/dotnet-10.0.401/dotnet",
    },
    "compiler": {
        "url": "https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64.tar.xz",
        "sha256": "bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21",
        "archive": "llvm-mingw-20260922.tar.xz",
        "destination": ".cache/toolchains",
        "ready": ".cache/toolchains/llvm-mingw-20260922-ucrt-ubuntu-22.04-x86_64/bin/x86_64-w64-mingw32-clang++",
    },
    "shaders": {
        "url": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/linux_dxc_2025_07_14.x86_64.tar.gz",
        "sha256": "f2213da1fc99dc8778c8823078e16ba97c7f80f86a1d4520ab1adf4b462bc48c",
        "archive": "dxc-v1.8.2505.1-linux.tar.gz",
        "destination": ".cache/toolchains/dxc-v1.8.2505.1",
        "ready": ".cache/toolchains/dxc-v1.8.2505.1/bin/dxc",
    },
}


def fetch(name):
    spec = TOOLS[name]
    algorithm = "sha512" if "sha512" in spec else "sha256"
    def checksum(path):
        digest = hashlib.new(algorithm)
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    archive = ROOT / ".cache/downloads" / spec["archive"]
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists() or checksum(archive) != spec[algorithm]:
        partial = archive.with_suffix(archive.suffix + ".part")
        print(f"Downloading {name} from {spec['url']}", flush=True)
        with urllib.request.urlopen(spec["url"], timeout=60) as source, partial.open("wb") as target:
            shutil.copyfileobj(source, target, length=1024 * 1024)
        if checksum(partial) != spec[algorithm]:
            raise RuntimeError(f"Publisher checksum mismatch: {name}")
        partial.replace(archive)
    if not (ROOT / spec["ready"]).exists():
        destination = ROOT / spec["destination"]
        destination.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive) as bundle:
            bundle.extractall(destination, filter="data")
    print(f"{name}: {ROOT / spec['ready']}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", choices=["compiler", "shaders", "dotnet", "graphics", "all"], default="graphics",
                        help="Default: graphics tools. dotnet is the optional Linux C# SDK; all includes it.")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() not in ["x86_64", "AMD64"]:
        parser.error("This bootstrap supplies Linux x64 host tools (including WSL), not native Windows host tools.")
    for name in TOOLS:
        if args.only in [name, "all"] or (args.only == "graphics" and name in ["compiler", "shaders"]):
            fetch(name)
