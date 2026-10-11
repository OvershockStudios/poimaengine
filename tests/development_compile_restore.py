#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Opt-in real SDK compile/restore qualification through the public WorldClient.

Use --binary, --dotnet and a fresh --output on the executable's native platform.
Windows execution requires an external gated process-tree owner. This fixture
builds one package-free net10.0 library; it does not execute gameplay or measure
iteration performance. Failed attempts and native responses remain in output.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time
import traceback
import uuid

# The ignored draft and eventual tests/ location use the same checkout SDK.
ROOT = next(parent for parent in Path(__file__).resolve().parents
            if (parent/'tools/python/poima_client').is_dir() and (parent/'CMakeLists.txt').is_file())


def need(value, message):
    if not value:
        raise AssertionError(message)


def sha(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            result.update(chunk)
    return result.hexdigest()


class RecordedTransport:
    """Keep SDK requests/results losslessly; delegate all transport behavior."""
    def __init__(self, transport, record):
        self.transport, self.record = transport, record

    @property
    def closed(self):
        return self.transport.closed

    def request(self, method, params=None, *, timeout=30.0):
        need(len(self.record['calls']) < 8192, 'Native request inventory exceeds bound')
        row = dict(method=method, params=copy.deepcopy(params))
        self.record['calls'].append(row)
        try:
            result = self.transport.request(method, params, timeout=timeout)
            row['result'] = copy.deepcopy(result)
            return result
        except BaseException as error:
            row['error'] = dict(type=type(error).__name__, message=str(error))
            if hasattr(error, 'code'):
                row['error'].update(code=error.code, data=copy.deepcopy(error.data))
            raise

    def close(self):
        self.transport.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'dotnet', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=900.0)
    args = parser.parse_args()
    need(math.isfinite(args.timeout) and 300 <= args.timeout <= 1800, 'Whole fixture timeout must be 300..1800 seconds')
    binary, dotnet = args.binary.resolve(strict=True), args.dotnet.resolve(strict=True)
    need(binary.is_file() and dotnet.is_file(), 'Use explicit matching-platform engine and SDK executables')
    output = args.output.resolve()
    need(not output.exists(), 'Output must be a fresh directory')
    need(not binary.is_relative_to(output) and not dotnet.is_relative_to(output), 'Output overlaps selected tools')
    output.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(ROOT/'tools/python'))
    from poima_client import WorldClient, RpcError

    record = dict(format='poima.development-compile-restore.v1', passed=False,
                  scope='Actual native World stdio and package-free SDK compilation; no gameplay execution, reload or timing-budget claim.',
                  calls=[], jobs=[], checks=[], cleanup_errors=[], binary_sha256=sha(binary), dotnet_sha256=sha(dotnet))
    pins = {str(path):sha(path) for path in [Path(__file__).resolve(),
            *sorted((ROOT/'tools/python/poima_client').glob('*.py')),
            *sorted((ROOT/'tools/python/poima_client').glob('*.json'))]}
    record['source_sha256'] = pins
    deadline = time.monotonic()+args.timeout
    project_root = output/'source'; project_root.mkdir()
    products = output/'compiled'
    project = project_root/'Probe.csproj'
    source = project_root/'Probe.cs'
    project.write_text('''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<TargetFramework>net10.0</TargetFramework><AssemblyName>CompileRestoreProbe</AssemblyName>
<EnableDefaultCompileItems>false</EnableDefaultCompileItems><Deterministic>true</Deterministic>
<GenerateAssemblyInfo>false</GenerateAssemblyInfo><Nullable>enable</Nullable>
</PropertyGroup><ItemGroup><Compile Include="Probe.cs"/></ItemGroup></Project>''', encoding='utf-8')
    (project_root/'NuGet.Config').write_text('<configuration><packageSources><clear/></packageSources></configuration>\n', encoding='utf-8')
    def write_source(value):
        source.write_text('public static class Probe\n{ public static int Value => '+value+'; }\n', encoding='utf-8')
        return sha(source)
    initial_source = write_source('101')
    project_pin = sha(project)
    config_pin = sha(project_root/'NuGet.Config')
    env = dict(DOTNET_CLI_HOME=str(output/'dotnet-home'), NUGET_PACKAGES=str(output/'nuget/packages'),
               NUGET_HTTP_CACHE_PATH=str(output/'nuget/http'), NUGET_PLUGINS_CACHE_PATH=str(output/'nuget/plugins'),
               DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', DOTNET_CLI_UI_LANGUAGE='en',
               DOTNET_PROCESSOR_COUNT='2', MSBUILDDISABLENODEREUSE='1', UseSharedCompilation='false', BuildInParallel='false')
    for name in ('DOTNET_CLI_HOME','NUGET_PACKAGES','NUGET_HTTP_CACHE_PATH','NUGET_PLUGINS_CACHE_PATH'):
        Path(env[name]).mkdir(parents=True)
    client = None
    transport = None
    accepted = []
    failure = None
    def call(method, params=None):
        left = deadline-time.monotonic()
        need(left > 0, 'Whole native fixture deadline expired')
        result = client.call(method, params, timeout=min(5.0, left))
        need(time.monotonic() < deadline, 'Response exceeded whole fixture deadline')
        return result
    def error(method, params, code):
        try:
            call(method, params)
        except RpcError as failure:
            need(failure.code==code, 'Unexpected native rejection code: '+str(failure.code))
        else:
            raise AssertionError('Expected native rejection: '+method)
    def wait(job, *, cleanup=False):
        until = time.monotonic()+(15.0 if cleanup else 150.0)
        while True:
            state = (client.call('development.inspect', dict(job_id=job['job_id']), timeout=3.0)
                     if cleanup else call('development.inspect', dict(job_id=job['job_id'])))
            if state['terminal']:
                return state
            need(time.monotonic() < until, 'Accepted SDK job did not become terminal')
            if not cleanup:
                need(call('world.inspect')==record['world_before'], 'SDK work changed authoritative authoring state')
            time.sleep(.05)
    def submit(label, *, restore='omitted'):
        request = dict(request_id=uuid.uuid4().hex, executable=str(dotnet), project=str(project),
                       output=str(products), configuration='Release', timeout_ms=120000)
        if restore!='omitted':
            request['restore'] = restore
        job = call('development.compile', request)
        accepted.append(job)
        row = dict(label=label, request=request, job=job, source_sha256=sha(source))
        record['jobs'].append(row)
        terminal = wait(job)
        row['terminal'] = terminal
        row['diagnostics'] = call('development.diagnostics', dict(job_id=job['job_id']))
        retry = call('development.compile', request)
        need(retry==dict(job_id=job['job_id'], replayed=True, receipt_scope='session'), 'Exact terminal retry duplicated SDK work')
        return row
    def assets():
        return {p.relative_to(products).as_posix():sha(p) for p in products.rglob('project.assets.json')} if products.exists() else {}
    dll = products/'managed/CompileRestoreProbe.dll'
    try:
        # SDK transport inherits its host environment; scope only this process's
        # variables around host creation, then restore them immediately. Native
        # build jobs inherit the already-created host's isolated snapshot.
        saved = {name:os.environ.get(name) for name in env}
        try:
            os.environ.update(env)
            client = WorldClient.open(binary, output/'world.json', cwd=output, close_timeout=15.0)
        finally:
            for name,value in saved.items():
                if value is None:
                    os.environ.pop(name,None)
                else:
                    os.environ[name] = value
        need(all(os.environ.get(name)==value for name,value in saved.items()), 'Host setup changed fixture environment')
        transport = client.transport
        client.transport = RecordedTransport(transport, record)
        record['native_owner_pid'] = transport.process_id
        schema = call('world.describe', dict(view='method', name='development.compile'))['methods']['development.compile']
        need(schema['properties']['restore']==dict(type='boolean',default=True) and 'restore' not in schema['required'], 'Restore discovery/default is incorrect')
        record['world_before'] = call('world.inspect')
        before = call('development.jobs')
        malformed = dict(request_id=uuid.uuid4().hex, executable=str(dotnet), project=str(project), output=str(products))
        for value in (None,0,1,0.0,1.0,'false',[],{}):
            error('development.compile',dict(malformed,restore=value),-32602)
            need(call('development.jobs')==before, 'Invalid restore consumed work or retry authority')
        record['checks'].append('optional_schema_and_strict_boolean_no_work')
        cold = submit('cold-no-restore', restore=False)
        need(cold['terminal']['state']=='failed' and cold['terminal']['exit_code']!=0 and not dll.exists() and not assets(), 'Cold no-restore did not fail without a new artifact/cache')
        need(any(d['severity']=='error' and d['code']=='NETSDK1004' for d in cold['diagnostics']['diagnostics']), 'Cold failure is not the missing assets/no-restore boundary')
        need(cold['terminal']['result'] is None, 'Failed cold build advertised a verified artifact')
        record['checks'].append('cold_no_restore_real_failure_no_fallback')
        restored = submit('default-restore')
        need(restored['terminal']['state']=='succeeded' and restored['terminal']['exit_code']==0 and dll.is_file(), 'Default restore did not compile the package-free project')
        cache = assets(); need(len(cache)==1, 'Default restore did not create one assets file')
        first = sha(dll)
        restored['dll_sha256'], restored['assets'] = first, cache
        normalized = dict(restored['request'],restore=True)
        need(call('development.compile',normalized)==dict(job_id=restored['job']['job_id'],replayed=True,receipt_scope='session'), 'Omitted and explicit true restore receipts differ')
        error('development.compile',dict(restored['request'],restore=False),-32009)
        record['checks'].append('default_restore_success_omitted_true_retry_false_conflict')
        write_source('202')
        changed = submit('source-edit-no-restore', restore=False)
        need(changed['terminal']['state']=='succeeded' and changed['terminal']['exit_code']==0 and sha(dll)!=first and assets()==cache, 'Source-only no-restore did not compile a distinct DLL with the same assets')
        second = sha(dll); changed['dll_sha256'], changed['assets'] = second, assets()
        for replacement in (True,'omitted'):
            changed_request = dict(changed['request'])
            if replacement=='omitted': changed_request.pop('restore')
            else: changed_request['restore'] = replacement
            error('development.compile',changed_request,-32009)
        record['checks'].append('source_edit_no_restore_changes_dll_and_preserves_assets')
        write_source('')
        syntax = submit('syntax-error-no-restore', restore=False)
        need(syntax['terminal']['state']=='failed' and syntax['terminal']['exit_code']!=0 and syntax['terminal']['result'] is None and sha(dll)==second and assets()==cache, 'Syntax failure changed the prior artifact or advertised success')
        need(any(d['severity']=='error' and d['code']=='CS1525' and d['location'] and d['location']['line']==2 and
                 d['origin'].replace('\\','/').rsplit('/',1)[-1]=='Probe.cs' for d in syntax['diagnostics']['diagnostics']), 'Failed SDK job lost exact source compiler diagnostics')
        syntax['retained_prior_dll_sha256'] = sha(dll)
        record['checks'].append('syntax_failure_retains_prior_dll_and_failed_receipt')
        write_source('303')
        repair = submit('repair-no-restore', restore=False)
        need(repair['terminal']['state']=='succeeded' and repair['terminal']['exit_code']==0 and sha(dll) not in (first,second) and assets()==cache, 'No-restore repair did not produce a distinct successful artifact')
        repair['dll_sha256'], repair['assets'] = sha(dll), assets()
        need(len(call('development.jobs')['jobs'])==5 and len(accepted)==5, 'Rejected/retried requests changed the actual SDK job inventory')
        need(call('world.inspect')==record['world_before'], 'Compilation changed authored World state')
        need(sha(project)==project_pin and sha(project_root/'NuGet.Config')==config_pin and
             all(sha(path)==pin for path,pin in pins.items()) and sha(binary)==record['binary_sha256'] and sha(dotnet)==record['dotnet_sha256'], 'Immutable source, client or selected tools changed')
        record['checks'].append('repair_success_five_real_jobs_unchanged_owner_and_inputs')
        record['initial_source_sha256'] = initial_source
        record['project_sha256'], record['nuget_config_sha256'] = project_pin, config_pin
        record['functional_passed'] = True
    except BaseException as error_value:
        failure = error_value
        record['error'] = traceback.format_exc()
    finally:
        if client is not None:
            for job in accepted:
                try:
                    observed = client.call('development.inspect',dict(job_id=job['job_id']),timeout=3.0)
                    if not observed['terminal']:
                        client.call('development.cancel',dict(job_id=job['job_id']),timeout=3.0)
                        wait(job,cleanup=True)
                except BaseException as error_value:
                    record['cleanup_errors'].append(str(error_value))
            try:
                need(client.call('session.close',timeout=3.0)==dict(closed=True), 'Native owner close receipt differs')
                until = time.monotonic()+15.0
                while transport.returncode is None and time.monotonic()<until:
                    time.sleep(.01)
                need(transport.returncode==0, 'Native owner did not exit normally')
            except BaseException as error_value:
                record['cleanup_errors'].append(str(error_value))
            try:
                client.close()
            except BaseException as error_value:
                record['cleanup_errors'].append(str(error_value))
            if transport is not None:
                record['native_exit_code'] = transport.returncode
                record['native_stderr'], record['native_stderr_truncated'] = transport.stderr_tail, transport.stderr_truncated
                if transport.returncode!=0 or transport.stderr_tail or transport.stderr_truncated:
                    record['cleanup_errors'].append('Native owner exit or diagnostics were not clean')
        record['passed'] = bool(record.get('functional_passed') and failure is None and not record['cleanup_errors'])
        data = (json.dumps(record,indent=2,ensure_ascii=False,allow_nan=False)+'\n').encode('utf-8')
        need(len(data)<=32*1024*1024, 'Native evidence exceeds fixture bound')
        (output/'evidence.json').write_bytes(data)
        print(json.dumps(dict(passed=record['passed'],sdk_jobs=len(record['jobs']),checks=len(record['checks']))))
    if failure is not None:
        raise failure
    need(record['passed'], 'Native cleanup failed; preserve the evidence before retrying')


if __name__=='__main__':
    main()
