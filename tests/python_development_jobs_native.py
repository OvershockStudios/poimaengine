#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Explicit SDK job qualification; Windows requires an external process-tree owner.

Runs package-free success, syntax failure and repair through the public client
with SDK 10.0.401 selected explicitly by global.json.
No gameplay, rendering, reload or iteration-performance qualification is implied.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import traceback
import uuid


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class RecordingTransport:
    def __init__(self, transport, calls):
        self.transport, self.calls = transport, calls

    def request(self, method, params=None, *, timeout=30.0):
        need(len(self.calls) < 2048, "Request inventory exceeded bound")
        row = dict(method=method, params=copy.deepcopy(params), timeout=timeout)
        self.calls.append(row)
        try:
            result = self.transport.request(method, params, timeout=timeout)
            row["result"] = copy.deepcopy(result)
            return result
        except BaseException as error:
            row["error_type"] = type(error).__name__
            raise

    @property
    def closed(self):
        return self.transport.closed

    def close(self):
        self.transport.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "dotnet", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--installed", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    if not args.installed:
        sys.path.insert(0, str(root / "tools/python"))
    from poima_client import WorldClient, RpcError
    import poima_client
    import poima_client.client
    import poima_client.development
    modules = {name: Path(module.__file__).resolve() for name, module in
               (("package", poima_client), ("client", poima_client.client),
                ("development", poima_client.development))}
    if args.installed:
        need(all(not path.is_relative_to(root / "tools/python") for path in modules.values()),
             "Installed fixture imported checkout code")

    binary, dotnet = args.binary.resolve(strict=True), args.dotnet.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "global.json").write_text(json.dumps(dict(sdk=dict(
        version="10.0.401", rollForward="disable", allowPrerelease=False))) + "\n", encoding="utf-8")
    project = output / "Probe.csproj"
    source = output / "Probe.cs"
    products = output / "compiled"
    project.write_text('''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<TargetFramework>net10.0</TargetFramework><AssemblyName>JobObservationProbe</AssemblyName>
<EnableDefaultCompileItems>false</EnableDefaultCompileItems><Deterministic>true</Deterministic>
</PropertyGroup><ItemGroup><Compile Include="Probe.cs"/></ItemGroup></Project>''', encoding="utf-8")
    (output / "NuGet.Config").write_text(
        "<configuration><packageSources><clear/></packageSources></configuration>\n", encoding="utf-8")
    good = "public static class Probe { public static int Value => 7; }\n"
    env = dict(DOTNET_PROCESSOR_COUNT="2", MSBUILDDISABLENODEREUSE="1",
               DOTNET_CLI_TELEMETRY_OPTOUT="1", DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1",
               DOTNET_CLI_UI_LANGUAGE="en", UseSharedCompilation="false",
               DOTNET_CLI_HOME=str(output / "dotnet-home"),
               NUGET_PACKAGES=str(output / "nuget"))
    saved = {key: os.environ.get(key) for key in env}
    record = dict(passed=False, calls=[], cases=[], binary_sha256=sha(binary), dotnet_sha256=sha(dotnet))
    record["modules"] = {name: dict(path=str(path), sha256=sha(path)) for name, path in modules.items()}
    client = None
    transport = None
    try:
        try:
            os.environ.update(env)
            client = WorldClient.open(binary, output / "world.json", cwd=output, close_timeout=15)
        finally:
            for key, value in saved.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
        transport = client.transport
        record["native_process_id"] = transport.process_id
        client.transport = RecordingTransport(transport, record["calls"])
        discovery = client.discover("method", "development.inspect")
        need(discovery["methods"]["development.inspect"]["properties"]["include_output"] ==
             dict(type="boolean", default=True), "Compact inspection is unavailable")
        record["schema_revision"] = discovery["schema_revision"]
        before = client.inspect()
        image = products / "managed/JobObservationProbe.dll"
        previous = None
        for label, text in (("success", good), ("syntax_failure", good.replace("=> 7;", "=> ;")),
                            ("repair", good)):
            source.write_text(text, encoding="utf-8")
            request = dict(request_id=uuid.uuid4().hex, executable=str(dotnet), project=str(project),
                           output=str(products), configuration="Release", timeout_ms=60000)
            receipt = client.call("development.compile", request)
            index = len(record["calls"])
            compact = client.wait_for_development(receipt["job_id"], timeout=75, poll_interval=0.05)
            observations = record["calls"][index:]
            need(observations and all(row["method"] == "development.inspect" and
                 row["params"] == dict(job_id=receipt["job_id"], include_output=False)
                 for row in observations), "Wait made extra requests or fetched logs")
            full = client.development_status(receipt["job_id"])
            projection = {key: value for key, value in full.items() if key not in ("stdout", "stderr")}
            need(compact == projection and "stdout" not in compact and "stderr" not in compact,
                 "Native full/compact projection differs")
            diagnostics = client.call("development.diagnostics", dict(job_id=receipt["job_id"], limit=16))
            need(diagnostics["terminal"] and not diagnostics["incomplete"] and not diagnostics["more"],
                 "Terminal diagnostics are incomplete")
            expected = "failed" if label == "syntax_failure" else "succeeded"
            need(full["state"] == expected and full["terminal"], "Unexpected actual SDK outcome")
            need(image.is_file(), "Managed image is missing")
            image_pin = sha(image)
            if label == "syntax_failure":
                need(image_pin == previous and any(row["code"] == "CS1525"
                     for row in diagnostics["diagnostics"]), "Syntax failure lost its diagnostic or prior DLL")
            else:
                need(full["exit_code"] == 0 and not diagnostics["diagnostics"], "Successful build differs")
                previous = image_pin
            need(client.inspect() == before, "Build observation mutated the world")
            need(client.call("development.compile", request) ==
                 dict(job_id=receipt["job_id"], replayed=True, receipt_scope="session"),
                 "Exact receipt replay duplicated work")
            record["cases"].append(dict(label=label, job_id=receipt["job_id"], state=expected,
                polls=len(observations), full_compact_equal=True, image_sha256=image_pin,
                diagnostics_complete=True, source_sha256=sha(source), receipt_replayed=True))
        try:
            client.development_status(9007199254740991, include_output=False)
        except RpcError as error:
            need(error.code == -32004, "Unknown-job rejection changed")
            record["unknown_job_error"] = error.code
        else:
            raise AssertionError("Unknown job became a terminal record")
        need(len(client.call("development.jobs")["jobs"]) == 3, "Wait changed job inventory")
        record["passed"] = True
    except BaseException:
        record["error"] = traceback.format_exc()
    finally:
        if client is not None:
            try:
                client.close()
                need(client.closed, "Client did not close")
                record["native_exit_code"] = transport.returncode
                need(record["native_exit_code"] == 0, "Native world did not exit normally")
                record["client_closed"] = True
            except BaseException:
                record["cleanup_error"] = traceback.format_exc()
                record["passed"] = False
        data = json.dumps(record, indent=2, allow_nan=False) + "\n"
        need(len(data.encode("utf-8")) <= 16 * 1024 * 1024, "Evidence exceeds bound")
        (output / "result.json").write_text(data, encoding="utf-8")
    print(json.dumps(dict(passed=record["passed"], cases=len(record["cases"]))))
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
