#!/usr/bin/env python3
"""Qualify native publishing through the compiled job worker on a matching host.

The selected SDK and native linker must already be configured. Environment
values, request files and logs are private build products under a new directory.
This is a trusted qualification tool, not an agent execution authorization API.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time
import traceback

from native_gameplay_sdk_reference import ROOT, Runner, executable, need, sha


class JobRunner(Runner):
    def __init__(self, args, evidence):
        super().__init__(args, evidence)
        # Explicit host-selected OS/linker paths. Never forward all environment
        # variables or a prefix wildcard that might include account credentials.
        selected = {
            'PATH', 'SYSTEMROOT', 'WINDIR', 'SYSTEMDRIVE', 'COMSPEC', 'OS',
            'PROGRAMFILES', 'PROGRAMFILES(X86)', 'PROGRAMW6432',
            'PROGRAMDATA', 'ALLUSERSPROFILE',
            'INCLUDE', 'LIB', 'LIBPATH', 'VCTOOLSINSTALLDIR', 'VCINSTALLDIR',
            'VSINSTALLDIR', 'WINDOWSSDKDIR', 'WINDOWSSDKVERSION',
            'UNIVERSALCRTSDKDIR', 'UCRTVERSION', 'WINDOWSSDKBINPATH',
            'WINDOWSSDKVERBINPATH', 'FRAMEWORKDIR', 'FRAMEWORKVERSION',
            'FRAMEWORKVERSION64', 'FRAMEWORKDIR64', 'NETFXSDKDIR',
            'TMP', 'TEMP', 'TMPDIR', 'DOTNET_CLI_HOME', 'NUGET_PACKAGES',
        }
        self.env = {key: value for key, value in self.env.items() if key.upper() in selected}
        self.env.update(DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1',
                        DOTNET_GENERATE_ASPNET_CERTIFICATE='false',
                        DOTNET_PROCESSOR_COUNT='2', MSBUILDDISABLENODEREUSE='1',
                        UseSharedCompilation='false', BuildInParallel='false',
                        IlcSingleThreaded='true', LC_ALL='C.UTF-8',
                        POIMA_TOOLCHAIN_VALUE='literal $(no-shell); & "wine" \u2603')
        home = Path(self.env['DOTNET_CLI_HOME'])
        for name in ('nuget-http-cache', 'nuget-plugins-cache'):
            (home / name).mkdir()
        self.env.update(NUGET_HTTP_CACHE_PATH=str(home / 'nuget-http-cache'),
                        NUGET_PLUGINS_CACHE_PATH=str(home / 'nuget-plugins-cache'))
        if os.name == 'nt':
            # NuGet requires profile/configuration directories as well as its
            # package cache. Supply job-owned paths, never the personal profile.
            for name in ('appdata', 'localappdata'):
                (home / name).mkdir()
            self.env.update(USERPROFILE=str(home), APPDATA=str(home / 'appdata'),
                            LOCALAPPDATA=str(home / 'localappdata'))
        else:
            # Avoid .NET falling back to a personal home/cache under Linux.
            self.env['HOME'] = self.env['DOTNET_CLI_HOME']
        self.owner_env = dict(os.environ)
        self.owner_env.update(POIMA_TOOLCHAIN_SENTINEL_A='owner-only-a',
                              POIMA_TOOLCHAIN_SENTINEL_B='owner-only-b',
                              DOTNET_STARTUP_HOOKS='poima-invalid-owner-hook.dll')
        self.evidence['environment'] = dict(
            replacement=True, names=sorted(self.env), inherited_owner_sentinels=False,
            inherited_startup_hook=False, configured_cache=True,
            values_retained_privately=True)

    def cleanup(self, process, row):
        if os.name == 'nt':
            # Closing the killed worker also closes its kill-on-close Job Object.
            result = subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20)
            row['cleanup'] = dict(method='taskkill tree and worker Job Object', exit_code=result.returncode)
        else:
            # The worker creates a separate group for its child. Include observed
            # descendants rather than assuming the worker's own group covers it.
            children = {process.pid}
            for _ in range(8):
                before = len(children)
                for path in Path('/proc').glob('[0-9]*/stat'):
                    try:
                        fields = path.read_text().rsplit(')', 1)[1].split()
                        if int(fields[1]) in children:
                            children.add(int(path.parent.name))
                    except (OSError, ValueError, IndexError):
                        pass
                if len(children) == before:
                    break
            for pid in sorted(children, reverse=True):
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            row['cleanup'] = dict(method='observed owned descendants SIGKILL', pids=sorted(children))
        process.wait(timeout=20)

    def run(self, label, command, *, success=True):
        spec = dict(executable=str(command[0]), working_directory=str(self.args.output),
                    arguments=[str(value) for value in command[1:]],
                    environment=sorted(self.env.items()), timeout_ms=900000)
        request = self.args.output / (label + '.request.json')
        request.write_text(json.dumps(spec, ensure_ascii=False) + '\n', encoding='utf-8')
        stdout, stderr = self.args.output / (label + '.stdout.log'), self.args.output / (label + '.stderr.log')
        row = dict(label=label, command=[str(value) for value in command],
                   expected_success=success, timeout_seconds=900,
                   stdout=stdout.name, stderr=stderr.name, request_sha256=sha(request))
        self.evidence['commands'].append(row)
        started = time.monotonic()
        with (self.args.output / (label + '.worker.json')).open('xb') as out, \
                (self.args.output / (label + '.worker.stderr.log')).open('xb') as err:
            flags = dict(creationflags=subprocess.CREATE_NEW_PROCESS_GROUP) if os.name == 'nt' else dict(start_new_session=True)
            process = subprocess.Popen([self.args.worker, request], stdin=subprocess.DEVNULL,
                                       stdout=out, stderr=err, env=self.owner_env, **flags)
            row['pid'] = process.pid
            try:
                # The compiled worker's own deadline fires first, leaving time
                # for process-tree cleanup and its terminal receipt.
                process.wait(timeout=930)
            except BaseException:
                row['interrupted_or_timed_out'] = True
                try:
                    self.cleanup(process, row)
                except BaseException:
                    row['cleanup_error'] = traceback.format_exc()
                    # The Windows worker's Job Object closes when it dies.
                    # Linux descendant cleanup is not claimed after a failed
                    # emergency scan/kill; preserve that failure explicitly.
                    try:
                        if process.poll() is None:
                            process.kill()
                        process.wait(timeout=20)
                    except BaseException:
                        row['worker_reap_error'] = traceback.format_exc()
                raise
            finally:
                row.update(worker_exit_code=process.poll(), duration_seconds=time.monotonic() - started)
        result = json.loads((self.args.output / (label + '.worker.json')).read_text(encoding='utf-8'))
        stdout.write_text(result['stdout'], encoding='utf-8')
        stderr.write_text(result['stderr'], encoding='utf-8')
        row.update(state=result['state'], exit_code=result['exit_code'], elapsed_ms=result['elapsed_ms'],
                   stdout_sha256=sha(stdout), stderr_sha256=sha(stderr),
                   stdout_bytes=result['stdout_bytes'], stderr_bytes=result['stderr_bytes'],
                   stdout_truncated=result['stdout_truncated'], stderr_truncated=result['stderr_truncated'],
                   unchanged_owner_environment=result['unchanged_environment'], error=result['error'])
        need(result['unchanged_environment'], 'Worker modified its owner environment')
        need(row['state'] == ('succeeded' if success else 'failed') and
             row['exit_code'] == (0 if success else 1) and not row['error'] and
             row['worker_exit_code'] == (0 if success else 2),
             label + ' returned an unexpected worker result; inspect retained logs')
        return row

    def verify_environment(self):
        # Compare actual child variables/values to the explicit replacement,
        # including literal metacharacters and Unicode. Print no values.
        code = ('import json,os,sys; e=dict(json.load(open(sys.argv[1],encoding="utf-8"))["environment"]); '
                'a=dict(os.environ); '
                'norm=lambda d:{k.upper():v for k,v in d.items()} if os.name=="nt" else d; '
                'assert norm(a)==norm(e),("replacement differs",sorted(a),sorted(e)); '
                'print("exact replacement environment verified")')
        probe = self.args.output / 'probe-environment.request.json'
        result = self.run('probe-environment', [sys.executable, '-c', code, probe])
        need('exact replacement environment verified' in
             (self.args.output / result['stdout']).read_text(encoding='utf-8'), 'Missing real environment probe')
        self.evidence['environment']['exact_child_probe'] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--worker', required=True)
    parser.add_argument('--dotnet', required=True)
    parser.add_argument('--compatibility-test', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--engine-version', required=True)
    args = parser.parse_args()
    need(platform.machine().lower() in ('x86_64', 'amd64') and sys.platform in ('linux', 'win32'),
         'Use a matching Windows or Linux x64 host')
    for name in ('worker', 'dotnet', 'compatibility_test'):
        setattr(args, name, executable(getattr(args, name)))
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    evidence = dict(passed=False, commands=[], host_os=sys.platform, engine_version=args.engine_version,
                    worker_sha256=sha(args.worker), runner_sha256=sha(__file__),
                    compatibility_test_sha256=sha(args.compatibility_test))
    original = dict(os.environ)
    try:
        runner = JobRunner(args, evidence)
        runner.verify_environment()
        runner.verify()
        need(dict(os.environ) == original, 'Qualification changed its own environment')
        evidence['unchanged_qualification_environment'] = True
    except BaseException:
        evidence['passed'] = False
        evidence['error'] = traceback.format_exc()
    finally:
        (args.output / 'evidence.json').write_text(json.dumps(evidence, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
