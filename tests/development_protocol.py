#!/usr/bin/env python3
"""Opt-in native authoring compile-job check using a real .NET 10 SDK."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
import queue
import subprocess
import threading
import time
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'sdk', 'output'):
        parser.add_argument('--'+name, required=True, type=Path)
    args = parser.parse_args()
    run = args.output.resolve()/uuid.uuid4().hex
    run.mkdir(parents=True)
    project = run/'Probe.csproj'
    project.write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net10.0</TargetFramework>'
                       '<EnableDefaultCompileItems>false</EnableDefaultCompileItems></PropertyGroup>'
                       '<ItemGroup><Compile Include="Probe.cs"/></ItemGroup></Project>')
    (run/'Probe.cs').write_text('public static class Probe { public static int Value => 42; }\n')
    calls = []
    lines = queue.Queue()
    stderr = (run/'stderr.log').open('w', encoding='utf-8')
    process = subprocess.Popen([str(args.binary.resolve()), 'world', str(run/'world.json')],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr,
                               text=True, encoding='utf-8', bufsize=1)
    def reader():
        try:
            for line in process.stdout: lines.put(line)
        finally: lines.put(None)
    thread = threading.Thread(target=reader, daemon=True); thread.start()
    record = dict(passed=False, qualification='Real native authoring protocol and SDK compilation; no gameplay execution.')
    work_complete = False
    def rpc(method, params=None, error=None):
        request = dict(jsonrpc='2.0', id=len(calls)+1, method=method, params=params or {})
        process.stdin.write(json.dumps(request)+'\n'); process.stdin.flush()
        try: line = lines.get(timeout=20)
        except queue.Empty: raise TimeoutError(method+' blocked the owner response.') from None
        if line is None: raise RuntimeError('Native service ended.')
        response = json.loads(line); calls.append(dict(request=request, response=response))
        if response.get('id') != request['id']: raise RuntimeError('Response ID mismatch.')
        if error is not None:
            if response.get('error', {}).get('code') != error: raise RuntimeError('Expected rejection: '+str(response))
            return response['error']
        if 'error' in response: raise RuntimeError(str(response['error']))
        return response['result']
    try:
        discovered = rpc('world.describe', dict(view='method', name='development.compile'))
        if 'development.compile' not in discovered['methods']: raise RuntimeError('Compile not discoverable.')
        before = rpc('world.inspect')['revision']
        params = dict(request_id=uuid.uuid4().hex, executable=str(args.sdk.resolve()), project=str(project),
                      output=str(run/'compiled'), configuration='Release', timeout_ms=120000)
        job = rpc('development.compile', params)
        retry = rpc('development.compile', params)
        if not retry['replayed'] or job['job_id'] != retry['job_id']: raise RuntimeError('Compile retry duplicated job.')
        rpc('development.compile', dict(params, configuration='Debug'), error=-32009)
        deadline = time.monotonic()+150
        polls = 0
        while True:
            current = rpc('development.inspect', dict(job_id=job['job_id'])); polls += 1
            # Ordinary authoring queries remain usable while the compiler runs.
            if rpc('world.inspect')['revision'] != before: raise RuntimeError('Compilation changed authored revision.')
            if current['terminal']: break
            if time.monotonic() >= deadline: raise TimeoutError('Compiler never reached terminal status.')
            time.sleep(.05)
        if current['state'] != 'succeeded' or not (run/'compiled/managed/Probe.dll').is_file():
            raise RuntimeError('Compilation failed: '+str(current))
        diagnostics = rpc('development.diagnostics', dict(job_id=job['job_id']))
        if diagnostics['state'] != current['state'] or 'stdout' in diagnostics or 'stderr' in diagnostics:
            raise RuntimeError('Structured diagnostics disagree with compile status or include full logs.')
        if any(item['severity'] == 'error' for item in diagnostics['diagnostics']):
            raise RuntimeError('Unexpected error diagnostic in successful probe build.')
        (run/'Probe.cs').write_text('#warning Structured feedback fixture\npublic static class Probe { public static int Value => ; }\n')
        broken_params = dict(params, request_id=uuid.uuid4().hex, output=str(run/'failed-compile'))
        broken_job = rpc('development.compile', broken_params)
        deadline = time.monotonic()+150
        while True:
            broken = rpc('development.inspect', dict(job_id=broken_job['job_id']))
            if rpc('world.inspect')['revision'] != before:
                raise RuntimeError('Failed compilation changed authored revision.')
            if broken['terminal']: break
            if time.monotonic() >= deadline: raise TimeoutError('Failed compile never reached terminal status.')
            time.sleep(.05)
        broken_diagnostics = rpc('development.diagnostics', dict(job_id=broken_job['job_id']))
        errors = [item for item in broken_diagnostics['diagnostics'] if item['severity'] == 'error']
        if broken['state'] != 'failed' or broken['exit_code'] != 1:
            raise RuntimeError('Invalid source did not fail compilation: '+str(broken))
        if len(errors) != 1 or errors[0]['code'] != 'CS1525' or errors[0]['location']['line'] != 2:
            raise RuntimeError('Real compiler error was not extracted: '+str(broken_diagnostics))
        if errors[0]['origin'].replace('\\', '/').rsplit('/', 1)[-1] != 'Probe.cs':
            raise RuntimeError('Compiler error lost its source filename.')
        if 'stdout' in broken_diagnostics or 'stderr' in broken_diagnostics:
            raise RuntimeError('Error feedback includes full logs.')
        record['failed_compile'] = broken_diagnostics
        rpc('development.forget', dict(job_id=job['job_id']))
        rpc('development.inspect', dict(job_id=job['job_id']), error=-32004)
        if not rpc('development.compile', params)['replayed']: raise RuntimeError('Forgetting erased receipt.')
        record.update(polls=polls, unchanged_world_revision=before,
                      state=current['state'], elapsed_ms=current['elapsed_ms'])
        work_complete = True
    except BaseException as failure:
        record['error'] = str(failure)
        raise
    finally:
        try: process.stdin.close()
        except BaseException as failure: record['cleanup_error'] = 'stdin close: '+str(failure)
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            record['cleanup_error'] = 'Owner needed forced termination.'
            process.kill(); process.wait(timeout=10)
        stderr.close()
        record['passed'] = work_complete and process.returncode == 0 and 'error' not in record and 'cleanup_error' not in record
        record.update(exit_code=process.returncode, calls=calls)
        (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n')
        print(run)
        if process.returncode != 0 or 'cleanup_error' in record: raise RuntimeError('Native owner cleanup failed.')


if __name__ == '__main__': main()
