#!/usr/bin/env python3
"""Build real replacement libraries while a native host retains 1,000 entities."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import queue
import shutil
import statistics
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def native_path(path, windows):
    value = str(path.resolve())
    if windows:
        return subprocess.check_output(["wslpath", "-w", value], text=True).strip()
    return value


class Session:
    def __init__(self, executable, arguments=()):
        self.process = subprocess.Popen([str(executable.resolve()), *arguments], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, encoding="utf-8", bufsize=1)
        self.lines = queue.Queue()
        self.errors = []
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._stderr, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def _stderr(self):
        for line in self.process.stderr:
            self.errors.append(line)

    def call(self, command, success=True):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        try:
            line = self.lines.get(timeout=30)
        except queue.Empty as error:
            raise AssertionError(f"Host response timed out: {command}") from error
        if line is None:
            raise AssertionError(f"Host exited: {self.process.poll()}, {self.errors}")
        reply = json.loads(line)
        assert reply["status"] == ("ok" if success else "error"), reply
        return reply

    def close(self):
        if self.process.poll() is None:
            try:
                self.call("quit")
                self.process.wait(timeout=10)
            finally:
                if self.process.poll() is None:
                    self.process.kill()
                    self.process.wait()
        assert self.process.returncode == 0, self.errors
        assert not self.errors, self.errors


def summarize(values):
    ordered = sorted(values)
    return {"samples": len(values), "median_ms": statistics.median(values),
            "p95_ms": ordered[math.ceil(len(ordered) * 0.95) - 1], "max_ms": max(values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--windows-interop", action="store_true")
    parser.add_argument("--warm-edits", type=int, default=40)
    args = parser.parse_args()
    assert 1 <= args.warm_edits <= 100
    # Each run has its own source copy and build cache. Original source is untouched.
    args.output.mkdir(parents=True, exist_ok=True)
    run = args.output / f"run-{time.time_ns()}"
    source = run / "source"
    shutil.copytree(ROOT / "experiments/native_module", source)
    build = run / "build"
    source_file = source / "fixture.cpp"
    original = source_file.read_text()
    binary = build / ("libpoima_game_fixture.dll" if args.windows_interop else "libpoima_game_fixture.so")
    record = {"fixture": "native_module_v1", "language": "C++20", "entities": 1000,
              "windows_execution": args.windows_interop, "build_host": "Linux/WSL",
              "builds": [], "warm_edits": [], "reloads": [], "checks": {}}
    config = ["cmake", "-S", str(source.resolve()), "-B", str(build.resolve()), "-G", "Ninja",
              "-DCMAKE_BUILD_TYPE=RelWithDebInfo", f"-DPOIMA_INCLUDE_DIR={ROOT / 'include'}"]
    if args.windows_interop:
        config += [f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake/windows-llvm-mingw.cmake'}"]
    config_start = time.perf_counter()
    configured = subprocess.run(config, capture_output=True, text=True, timeout=60)
    record["configure"] = {"ms": (time.perf_counter() - config_start) * 1000,
                           "command": config, "exit_code": configured.returncode,
                           "log": configured.stdout + configured.stderr}
    assert configured.returncode == 0, record["configure"]

    host = Session(args.host)
    loaded = False

    def state():
        return host.call("inspect")["result"]["state"]

    def compile_variant(name, schema=1, scale=1, reject=False, bad_abi=False, fail_step=False, broken=False):
        text = original
        for key, value in [("SCHEMA", schema), ("SCALE", scale), ("BAD_ABI", int(bad_abi)),
                           ("REJECT_MIGRATION", int(reject)), ("FAIL_STEP", int(fail_step))]:
            text = text.replace(f"#define POIMA_FIXTURE_{key} " +
                                ("1" if key in ("SCHEMA", "SCALE") else "0"),
                                f"#define POIMA_FIXTURE_{key} {value}")
        # Definitions from CMake override #ifndef defaults, so edit the actual
        # expression/constants for this source-edit benchmark instead.
        text = text.replace("#ifndef POIMA_FIXTURE_SCHEMA", "#undef POIMA_FIXTURE_SCHEMA\n#ifndef POIMA_FIXTURE_SCHEMA")
        text = text.replace("#ifndef POIMA_FIXTURE_SCALE", "#undef POIMA_FIXTURE_SCALE\n#ifndef POIMA_FIXTURE_SCALE")
        for key in ["BAD_ABI", "REJECT_MIGRATION", "FAIL_STEP"]:
            text = text.replace(f"#ifndef POIMA_FIXTURE_{key}", f"#undef POIMA_FIXTURE_{key}\n#ifndef POIMA_FIXTURE_{key}")
        if broken:
            text += "\n#error Intentional compile failure for retention test\n"
        source_file.write_text(text)
        started = time.perf_counter()
        command = ["cmake", "--build", str(build.resolve()), "--parallel", "2"]
        compiler = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        # Pump the already-loaded implementation while the compiler runs.
        ticks_during_build = 0
        deadline = time.monotonic() + 60
        while compiler.poll() is None:
            if loaded:
                host.call("step 1")
                ticks_during_build += 1
            if time.monotonic() > deadline:
                compiler.kill()
                raise AssertionError("Fixture compilation timed out")
            time.sleep(0.005)
        log = compiler.communicate(timeout=5)[0]
        elapsed = (time.perf_counter() - started) * 1000
        info = {"name": name, "build_ms": elapsed, "exit_code": compiler.returncode,
                "ticks_during_build": ticks_during_build, "log": log}
        record["builds"].append(info)
        assert (compiler.returncode != 0) == broken, info
        if not broken:
            target = run / (name + binary.suffix)
            shutil.copy2(binary, target)
            info["bytes"] = target.stat().st_size
            info["sha256"] = hashlib.sha256(target.read_bytes()).hexdigest()
            return target, elapsed
        return None, elapsed

    def load(path, success=True):
        before = state()
        response = host.call("load " + native_path(path, args.windows_interop), success)
        after = response["result"]["state"]
        assert after["tick"] == before["tick"]
        assert after["shadow_files"] == 1
        if not success:
            assert (after["state_hash"], after["revision"]) == (before["state_hash"], before["revision"])
        return response

    try:
        initial, _ = compile_variant("initial")
        host.call("load " + native_path(initial, args.windows_interop))
        loaded = True
        first = state()
        assert first["entity_bytes"] == 32 and first["first"]["position_mm"] == 0
        stepped = host.call("step 100")["result"]["state"]
        assert stepped["first"]["position_mm"] == 100
        assert stepped["last"]["position_mm"] == 999 * 3 + 100 * (999 % 7 + 1)
        assert stepped["native_calls"] == 100000
        record["checks"]["deterministic_initial_update"] = True
        checkpoint = run / "state.bin"
        host.call("save " + native_path(checkpoint, args.windows_interop))
        saved = state()
        host.call("step 13")
        host.call("restore " + native_path(checkpoint, args.windows_interop))
        assert state()["state_hash"] == saved["state_hash"]
        roundtrip = run / "roundtrip.bin"
        host.call("save " + native_path(roundtrip, args.windows_interop))
        assert checkpoint.read_bytes() == roundtrip.read_bytes()
        record["checks"]["canonical_save_restore_save"] = True
        damaged = run / "damaged.bin"
        data = bytearray(checkpoint.read_bytes()); data[48] ^= 1; damaged.write_bytes(data)
        before = state()
        host.call("restore " + native_path(damaged, args.windows_interop), False)
        assert state()["state_hash"] == before["state_hash"]
        record["checks"]["corrupt_checkpoint_retains_state"] = True

        latest = initial
        for edit in range(args.warm_edits):
            scale = 2 if edit % 2 == 0 else 1
            start = time.perf_counter()
            latest, build_ms = compile_variant(f"edit-{edit}", scale=scale)
            before = state()
            response = load(latest)
            assert response["result"]["state"]["state_hash"] == before["state_hash"]
            after = host.call("step 1")["result"]["state"]
            assert after["first"]["position_mm"] == before["first"]["position_mm"] + scale
            record["warm_edits"].append({"build_ms": build_ms, "reload_ms": response["result"]["operation_ms"],
                                         "observed_total_ms": (time.perf_counter() - start) * 1000})
        record["checks"]["real_function_edits_preserve_state"] = True
        before = state()
        compile_variant("broken", broken=True)
        after = host.call("step 1")["result"]["state"]
        assert after["revision"] > before["revision"] and after["schema"] == before["schema"]
        record["checks"]["compile_failure_old_module_keeps_running"] = True
        for name, kwargs in [("bad-abi", {"bad_abi": True}), ("migration-failure", {"reject": True})]:
            bad, _ = compile_variant(name, **kwargs)
            load(bad, False)
            host.call("step 1")
            record["checks"][name + "_retains_state"] = True

        failing_step, _ = compile_variant("step-failure", fail_step=True)
        load(failing_step)
        before = state()
        host.call("step 3", False)
        after = state()
        assert (after["tick"], after["state_hash"], after["revision"]) == (before["tick"], before["state_hash"], before["revision"])
        record["checks"]["partial_update_failure_retains_state"] = True
        load(latest)
        for cycle in range(100):
            before = state()
            response = load(initial if cycle % 2 == 0 else latest)
            after = response["result"]["state"]
            assert after["state_hash"] == before["state_hash"]
            record["reloads"].append({"operation_ms": response["result"]["operation_ms"],
                                       "resident_bytes": after["resident_bytes"], "shadow_files": after["shadow_files"]})
            host.call("step 1")
        record["checks"]["100_reloads_preserve_state_and_retire_shadow_files"] = True
        upgraded, _ = compile_variant("schema-2", schema=2)
        before = state()
        response = load(upgraded)
        after = response["result"]["state"]
        assert after["schema"] == 2 and after["entity_bytes"] == 40
        assert after["first"] == {**before["first"], "energy": 100}
        host.call("step 1")
        assert state()["first"]["energy"] == 101
        load(initial, False)
        record["checks"]["layout_upgrade_and_rejected_downgrade"] = True
        host.call("restore " + native_path(checkpoint, args.windows_interop))
        after = state()
        assert after["tick"] == saved["tick"] and after["first"] == {**saved["first"], "energy": 100}
        record["checks"]["old_checkpoint_migrates"] = True
        record["final_state"] = after
        record["summary"] = {key: summarize([edit[key] for edit in record["warm_edits"]])
                             for key in ["build_ms", "reload_ms", "observed_total_ms"]}
        record["summary"]["repeated_reload_ms"] = summarize([row["operation_ms"] for row in record["reloads"]])
        record["passed"] = True
    finally:
        host.close()
        record["source_sha256"] = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in [ROOT / "experiments/native_module/host.cpp", ROOT / "experiments/native_module/fixture.cpp",
                      ROOT / "include/poima/experimental/module_abi.h", Path(__file__).resolve()]}
        record["host_sha256"] = hashlib.sha256(args.host.read_bytes()).hexdigest()
        (args.output / "results.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record["summary"], indent=2))


if __name__ == "__main__":
    main()
