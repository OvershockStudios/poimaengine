#!/usr/bin/env python3
"""Compile a project-owned scene-linear HDR effect with Poima's pinned DXC."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import base64
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat
import struct
import subprocess
import sys
from tempfile import NamedTemporaryFile, TemporaryDirectory


ROOT = Path(__file__).resolve().parents[1]
DXC_VERSION = "v1.8.2505.1"
DXC_ROOT = ROOT / ".cache/toolchains" / f"dxc-{DXC_VERSION}"
DXC_FILES = {
    "bin/dxc": "25927f0c2eb2eb22ebdf72ecf4798345a53b997ef1b05fea0280410d220427bb",
    "lib/libdxcompiler.so": "72832c39c5dfdf537586ca6500de94ef28cbf69d148ab3abe573da6ea209bad0",
    "lib/libdxil.so": "51f6a6eb5c77391a86c03b82a79affdb7fa5ea9a3a67be37f5f573ca112ad931",
}
ENTRY = "poima_project_post_v1"
DXC_ARGS = ("-spirv", "-T", "ps_6_0", "-E", ENTRY, "-fspv-target-env=vulkan1.3",
            "-Zpr", "-O3", "-Werror", "-Fo", "effect.spv", "effect.hlsl")
MAX_MANIFEST = 8192
MAX_SOURCE = 128 * 1024
MAX_FILES = 16
MAX_INCLUDE_DEPTH = 8
MAX_SPIRV = 128 * 1024
DXC_TIMEOUT = 12
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"[ \t]*$')
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
WRAPPER = """
[[vk::binding(0, 0)]] Texture2D<float4> poima_post_scene_hdr;
float4 poima_project_post_v1(float4 position : SV_Position) : SV_Target0 {
    uint width, height;
    poima_post_scene_hdr.GetDimensions(width, height);
    const int2 pixel = int2(position.xy);
    const float3 hdr = poima_post_scene_hdr.Load(int3(pixel, 0)).rgb;
    const float2 uv = (float2(pixel) + 0.5) / float2(width, height);
    const float3 color = poima_post_effect(hdr, uv);
    return float4(clamp(select(isfinite(color), color, 0.0), 0.0, 65504.0), 1.0);
}
"""


class EffectError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise EffectError(message)


def sha256(payload):
    return hashlib.sha256(payload).hexdigest()


def plain_directory(path):
    for ancestor in (*reversed(path.parents), path):
        require(stat.S_ISDIR(ancestor.lstat().st_mode), f"Expected an ordinary directory for {path.name}.")


def read_file(path, limit):
    """Check the entire ancestry, then read one bounded regular file."""
    plain_directory(path.parent)
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), "rb") as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1,
                f"Expected an ordinary file without hard links for {path.name}.")
        require(before.st_size <= limit, f"{path.name} exceeds its {limit}-byte limit.")
        payload = stream.read(limit + 1)
        after = os.fstat(stream.fileno())
    require(len(payload) == before.st_size and before.st_size == after.st_size and
            before.st_mtime_ns == after.st_mtime_ns and before.st_ctime_ns == after.st_ctime_ns,
            f"{path.name} changed during the read.")
    return payload


def portable_path(value, suffix):
    require(isinstance(value, str) and 0 < len(value) <= 256 and value.endswith(suffix),
            f"Expected a portable {suffix} path.")
    parts = value.split("/")
    require(len(parts) <= 12, "Source path has too many components.")
    for part in parts:
        require(re.fullmatch(r"[A-Za-z0-9_\-][A-Za-z0-9_ .\-]{0,63}", part) and not part.endswith((" ", ".")),
                "Only portable root-relative source paths are accepted.")
        require(not re.fullmatch(r"con|prn|aux|nul|com[1-9]|lpt[1-9]", part.split(".")[0], re.IGNORECASE),
                "Source path uses a reserved filename.")
    return value


def read_manifest(payload):
    def unique_fields(pairs):
        fields = {}
        for key, value in pairs:
            require(key not in fields, "Duplicate post-effect manifest field.")
            fields[key] = value
        return fields

    try:
        value = json.loads(payload.decode("utf-8"), object_pairs_hook=unique_fields)
    except EffectError:
        raise
    except (ValueError, RecursionError) as error:
        raise EffectError("Post-effect manifest must contain UTF-8 JSON.") from error
    require(isinstance(value, dict) and set(value) == {"format", "version", "id", "slot", "inputs", "source"},
            "Expected the exact v1 post-effect manifest fields.")
    require(value["format"] == "poima.project-post-effect" and type(value["version"]) is int and value["version"] == 1,
            "Unsupported post-effect manifest format or version.")
    require(isinstance(value["id"], str) and re.fullmatch("[a-z][a-z0-9_-]{0,63}", value["id"]),
            "Expected a lowercase portable effect ID.")
    require(value["slot"] == "scene-linear-pre-exposure" and value["inputs"] == ["scene_hdr"],
            "V1 requires the scene-linear-pre-exposure slot and one scene_hdr input.")
    portable_path(value["source"], ".hlsl")
    return value


def expand_source(root, source):
    """Snapshot and flatten literal includes before giving the bytes to DXC."""
    files = {}
    visited = set()
    stack = []
    byte_count = 0

    def expand(name):
        nonlocal byte_count
        require(name not in stack, f"Cyclic post-effect include at {name}.")
        require(name.casefold() not in visited, f"Repeated or case-colliding post-effect source at {name}.")
        require(len(stack) < MAX_INCLUDE_DEPTH and len(files) < MAX_FILES, "Post-effect include budget exceeded.")
        data = read_file(root / name, MAX_SOURCE - byte_count)
        byte_count += len(data)
        files[name] = {"path": name, "bytes": len(data), "sha256": sha256(data)}
        visited.add(name.casefold())
        stack.append(name)
        try:
            text = data.decode("ascii").replace("\r\n", "\n")
        except UnicodeDecodeError as error:
            raise EffectError(f"{name} must be ASCII HLSL.") from error
        require(all(char in "\t\n" or " " <= char <= "~" for char in text),
                f"{name} contains an unsupported control character.")
        # Backslashes, trigraphs and digraphs can change how a preprocessor
        # recognizes physical lines and directives. This profile excludes them.
        require(not any(token in text for token in ("\\", "??", "%:")),
                f"{name} contains an unsupported preprocessor token.")
        text = COMMENT.sub(lambda match: " " + "\n" * match[0].count("\n"), text)
        require("/*" not in text and "*/" not in text, f"{name} contains an unterminated comment.")
        require(not re.search(r"\b(poima_post_scene_hdr|poima_project_post_v1)\b", text),
                f"{name} uses an engine-owned shader symbol.")
        expanded = [f'#line 1 "{name}"']
        for number, line in enumerate(text.split("\n"), 1):
            match = INCLUDE.fullmatch(line)
            if match:
                relative = portable_path(match[1], ".hlsli")
                child = (Path(name).parent / relative).as_posix()
                portable_path(child, ".hlsli")
                expanded.extend([expand(child), f'#line {number + 1} "{name}"'])
            else:
                require(not any(char in line for char in "#\"'"),
                        f"{name}:{number} allows only literal quoted include directives.")
                expanded.append(line)
        stack.pop()
        return "\n".join(expanded)

    expanded = expand(source) + '\n#line 1 "poima_post_wrapper.hlsl"\n' + WRAPPER
    return expanded.encode("ascii"), [files[name] for name in sorted(files)]


def check_compiler(root):
    for name, expected in DXC_FILES.items():
        image = read_file(root / name, 64 * 1024 * 1024)
        require(sha256(image) == expected, f"DXC hash mismatch for {name}.")


def compiler_limits():
    import resource

    for limit, value in ((resource.RLIMIT_CPU, 8), (resource.RLIMIT_AS, 1024 * 1024 * 1024),
                         (resource.RLIMIT_FSIZE, 256 * 1024), (resource.RLIMIT_CORE, 0)):
        resource.setrlimit(limit, (value, value))


def compile_source(source, dxc_root):
    check_compiler(dxc_root)
    with TemporaryDirectory(prefix="poima-post-") as directory:
        work = Path(directory)
        (work / "effect.hlsl").write_bytes(source)
        log = work / "dxc.txt"
        try:
            with log.open("wb") as capture:
                result = subprocess.run([str(dxc_root / "bin/dxc"), *DXC_ARGS], cwd=work,
                                        env={"LC_ALL": "C", "LANG": "C", "LD_LIBRARY_PATH": str(dxc_root / "lib")},
                                        stdin=subprocess.DEVNULL, stdout=capture, stderr=subprocess.STDOUT,
                                        timeout=DXC_TIMEOUT, preexec_fn=compiler_limits)
        except subprocess.TimeoutExpired as error:
            raise EffectError("DXC exceeded the post-effect compilation time limit.") from error
        if result.returncode != 0:
            with log.open("rb") as capture:
                diagnostic = capture.read(8193)
            message = diagnostic[:8192].decode("utf-8", errors="replace").replace(directory, ".").strip()
            if len(diagnostic) > 8192:
                message += "\n[diagnostics truncated]"
            raise EffectError(f"DXC exited with status {result.returncode}." + (f"\n{message}" if message else ""))
        return read_file(work / "effect.spv", MAX_SPIRV)


# SPIR-V 1.6 opcodes for the fixed fullscreen fragment profile. Admit bounded
# generated shader operations, not arbitrary bytecode or the entire grammar.
NAME, CAPABILITY, MEMORY_MODEL, ENTRY_POINT, EXECUTION_MODE = 5, 17, 14, 15, 16
EXT_INST_IMPORT, EXT_INST, TYPE_FLOAT, TYPE_VECTOR, TYPE_IMAGE, TYPE_POINTER = 11, 12, 22, 23, 25, 32
FUNCTION, VARIABLE, STORE, IMAGE_FETCH, IMAGE_SIZE_LOD, DECORATE = 54, 59, 62, 95, 103, 71
PROFILE_OPS = frozenset({
    3, NAME, 6, EXT_INST_IMPORT, EXT_INST, MEMORY_MODEL, ENTRY_POINT, EXECUTION_MODE, CAPABILITY,
    19, 20, 21, TYPE_FLOAT, TYPE_VECTOR, 24, TYPE_IMAGE, TYPE_POINTER, 33,
    41, 42, 43, 44, 46, FUNCTION, 56, VARIABLE, 61, STORE, DECORATE,
    77, 78, 79, 80, 81, 82, 83, IMAGE_FETCH, IMAGE_SIZE_LOD,
    *range(109, 116), 124, *range(126, 149), *range(154, 192), *range(194, 200),
    245, 247, 248, 249, 250, 253, 255,
})


def spirv_string(words):
    data = struct.pack(f"<{len(words)}I", *words)
    stop = data.find(b"\0")
    require(stop >= 0, "Malformed SPIR-V string.")
    return data[:stop], stop // 4 + 1


def admit_spirv(data):
    require(20 <= len(data) <= MAX_SPIRV and len(data) % 4 == 0, "Invalid SPIR-V byte count.")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    require(words[0:2] == (0x07230203, 0x00010600) and 1 < words[3] <= 8192 and words[4] == 0,
            "Expected a bounded SPIR-V 1.6 module.")
    operations = defaultdict(list)
    offset, count = 5, 0
    while offset < len(words):
        width, opcode = words[offset] >> 16, words[offset] & 0xffff
        require(width > 0 and offset + width <= len(words), "Malformed SPIR-V instruction.")
        require(opcode in PROFILE_OPS, f"SPIR-V opcode {opcode} is outside the post-effect profile.")
        operations[opcode].append(words[offset + 1:offset + width])
        offset += width
        count += 1
        require(count <= 2048, "Post-effect shader exceeds its instruction budget.")

    require(operations[CAPABILITY] in ([(1,)], [(1,), (50,)]) and operations[MEMORY_MODEL] == [(0, 1)],
            "SPIR-V capabilities or memory model differ from the fullscreen fragment profile.")
    require(len(operations[ENTRY_POINT]) == 1 and len(operations[ENTRY_POINT][0]) >= 3,
            "Expected one SPIR-V entry point.")
    model, function, *signature = operations[ENTRY_POINT][0]
    name, name_words = spirv_string(signature)
    require(model == 4 and name == ENTRY.encode("ascii") and operations[EXECUTION_MODE] == [(function, 7)] and
            len(operations[FUNCTION]) == 1 and operations[FUNCTION][0][1:2] == (function,),
            "Expected the fixed fragment entry and upper-left screen coordinates.")
    require(len(operations[EXT_INST_IMPORT]) == 1 and len(operations[EXT_INST_IMPORT][0]) >= 2,
            "Expected the generated GLSL math import.")
    library, *library_name = operations[EXT_INST_IMPORT][0]
    require(spirv_string(library_name)[0] == b"GLSL.std.450" and
            all(instruction[2:3] == (library,) for instruction in operations[EXT_INST]),
            "Only the GLSL math instruction set is supported.")

    variables = {}
    for variable in operations[VARIABLE]:
        require(len(variable) == 3 and variable[2] in {0, 1, 3, 7}, "Unexpected SPIR-V variable or storage resource.")
        pointer, identifier, storage = variable
        require(identifier not in variables, "Duplicate SPIR-V resource ID.")
        variables[identifier] = (pointer, storage)
    globals_by_storage = {storage: identifier for identifier, (_, storage) in variables.items() if storage != 7}
    require(set(globals_by_storage) == {0, 1, 3} and sum(storage != 7 for _, storage in variables.values()) == 3,
            "Expected one source image, one fragment-coordinate input and one color output.")
    image_id, coordinate_id, target_id = (globals_by_storage[storage] for storage in (0, 1, 3))
    image_names = [spirv_string(record[1:])[0] for record in operations[NAME] if record[:1] == (image_id,)]
    require(image_names == [b"poima_post_scene_hdr"], "Expected the engine-owned HDR source image.")
    require(len(signature[name_words:]) == 3 and set(signature[name_words:]) == {image_id, coordinate_id, target_id},
            "Unexpected SPIR-V fragment interface.")
    decorations = operations[DECORATE]
    required = {(image_id, 34, 0), (image_id, 33, 0), (coordinate_id, 11, 15), (target_id, 30, 0)}
    require(set(decorations) == required and len(decorations) == len(required),
            "Expected set 0 binding 0, FragCoord, and one location 0 output.")
    require(len(operations[TYPE_IMAGE]) == 1 and len(operations[TYPE_IMAGE][0]) == 8,
            "Expected one sampled image type.")
    image_type, sampled_type, dim, depth, arrayed, multisampled, sampled, image_format = operations[TYPE_IMAGE][0]
    require((dim, arrayed, multisampled, sampled, image_format) == (1, 0, 0, 1, 0) and depth in {0, 2} and
            (sampled_type, 32) in operations[TYPE_FLOAT], "Expected one nonmultisampled read-only 2D float image.")
    float4_types = {vector[0] for vector in operations[TYPE_VECTOR] if len(vector) == 3 and
                    vector[2] == 4 and (vector[1], 32) in operations[TYPE_FLOAT]}
    pointers = set(operations[TYPE_POINTER])
    require((variables[image_id][0], 0, image_type) in pointers and
            all(any((variables[identifier][0], storage, vector) in pointers for vector in float4_types)
                for identifier, storage in ((coordinate_id, 1), (target_id, 3))),
            "SPIR-V resource types differ from the post-effect ABI.")
    require(operations[IMAGE_FETCH] and len(operations[IMAGE_FETCH]) == 1 and len(operations[IMAGE_SIZE_LOD]) <= 1 and
            sum(store[:1] == (target_id,) for store in operations[STORE]) == 1 and
            all(len(store) == 2 and (store[0] == target_id or variables.get(store[0], (0, 0))[1] == 7)
                for store in operations[STORE]), "Expected one image fetch and one color write.")
    return count


def encoded_json(record):
    return (json.dumps(record, sort_keys=True, ensure_ascii=True, separators=(",", ":")) + "\n").encode("ascii")


def publish(path, artifact):
    plain_directory(path.parent)
    data = encoded_json(artifact)
    with NamedTemporaryFile(dir=path.parent, prefix=".poima-post-", mode="wb") as stage:
        stage.write(data)
        stage.flush()
        os.fsync(stage.fileno())
        # A complete sibling file becomes visible at the final name only if
        # the target did not already exist.
        os.link(stage.name, path, follow_symlinks=False)
    return sha256(data)


def load_source(manifest_path, expected_source_sha256=None):
    require(platform.system() == "Linux" and platform.machine() == "x86_64",
            "Post-effect authoring requires Linux x64.")
    manifest_bytes = read_file(manifest_path, MAX_MANIFEST)
    manifest = read_manifest(manifest_bytes)
    source, files = expand_source(manifest_path.parent, manifest["source"])
    identity = {"manifest_sha256": sha256(manifest_bytes), "sources": files}
    identity["source_sha256"] = sha256(encoded_json(identity))
    if expected_source_sha256 is not None:
        require(re.fullmatch("[0-9a-f]{64}", expected_source_sha256), "Expected one lowercase source SHA-256.")
        require(expected_source_sha256 == identity["source_sha256"], "Post-effect source SHA-256 conflict.")
    return manifest, source, identity


def build(manifest_path, dxc_root, output, expected_source_sha256=None):
    require(output.suffix == ".poimapost", "Output must be a new .poimapost file.")
    require(not os.path.lexists(output), "Post-effect output already exists.")
    manifest, source, identity = load_source(manifest_path, expected_source_sha256)
    spirv = compile_source(source, dxc_root)
    instructions = admit_spirv(spirv)
    artifact = {
        "format": "poima.compiled-post-effect", "version": 1,
        "manifest": manifest, **identity, "generated_source_sha256": sha256(source),
        "compiler": {"release": DXC_VERSION, "files": DXC_FILES, "arguments": DXC_ARGS},
        "interface": {"entry": ENTRY, "stage": "fragment", "descriptor_set": 0, "binding": 0, "color_location": 0},
        "spirv": {"encoding": "base64", "bytes": len(spirv), "sha256": sha256(spirv),
                  "instructions": instructions, "data": base64.b64encode(spirv).decode("ascii")},
    }
    return {"artifact_sha256": publish(output, artifact), "spirv_sha256": artifact["spirv"]["sha256"],
            "source_sha256": identity["source_sha256"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--dxc-root", type=Path, default=DXC_ROOT)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--output", type=Path)
    mode.add_argument("--inspect", action="store_true", help="Report the source closure without running DXC.")
    parser.add_argument("--expected-source-sha256", help="Require this exact source closure.")
    args = parser.parse_args()
    try:
        if args.inspect:
            _, _, result = load_source(args.manifest.absolute(), args.expected_source_sha256)
        else:
            result = build(args.manifest.absolute(), args.dxc_root.absolute(), args.output.absolute(),
                           args.expected_source_sha256)
    except (EffectError, OSError, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "error", "diagnostic": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps({"status": "ok", **result}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
