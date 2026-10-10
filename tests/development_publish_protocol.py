#!/usr/bin/env python3
"""Opt-in genuine Native AOT publication/export through host-configured MCP jobs.

Requires an already configured matching-host SDK/linker, a copied gameplay SDK
and an installed runtime. Raw toolchain values and RPC logs remain in the chosen
new output directory. This exercises engine tools, not an external model session.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time
from types import SimpleNamespace
import uuid

from development_toolchain import JobRunner
from native_gameplay_sdk_reference import ROOT, GAME_TYPE, executable, need, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'dotnet', 'compatibility-test', 'sdk-dll', 'runtime', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--publish-only', action='store_true', help='Do not qualify bundle export (for a headless runtime without graphics).')
    args = parser.parse_args()
    for name in ('binary', 'dotnet', 'compatibility_test'):
        setattr(args, name, executable(str(getattr(args, name))))
    args.sdk_dll = args.sdk_dll.resolve(strict=True)
    args.runtime = args.runtime.resolve(strict=True)
    args.output = args.output.absolute()
    args.output.mkdir(parents=True, exist_ok=False)
    record = dict(passed=False, scope='Host-configured MCP native publishing/export; no external model, graphics or coherent source snapshot claim.',
                  commands=[], checks=[], export_requested=not args.publish_only, binary_sha256=sha(args.binary), sdk_sha256=sha(args.sdk_dll))
    runner = JobRunner(SimpleNamespace(output=args.output), record)
    source, products = args.output/'source', args.output/'products'
    source.mkdir(); products.mkdir()
    fixture = source/'game'; fixture.mkdir()
    shutil.copyfile(ROOT/'tests/managed_ui_gameplay/ManagedUiGame.cs', fixture/'Game.cs')
    shutil.copyfile(args.sdk_dll, fixture/'Poima.Gameplay.dll')
    project = fixture/'Game.csproj'
    project.write_text('''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<TargetFramework>net10.0</TargetFramework><LangVersion>14.0</LangVersion>
<Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings><AllowUnsafeBlocks>true</AllowUnsafeBlocks>
<TreatWarningsAsErrors>true</TreatWarningsAsErrors><Deterministic>true</Deterministic>
</PropertyGroup><ItemGroup><Reference Include="Poima.Gameplay"><HintPath>Poima.Gameplay.dll</HintPath></Reference></ItemGroup></Project>''')
    # Selected tools and environment belong to this owner-created configuration.
    runtime_spec = json.loads((args.runtime/'runtime.json').read_text(encoding='utf-8'))
    exporter = args.runtime/runtime_spec['executable']
    profile = dict(name='native', project_root=str(source), output_root=str(products),
                   python=str(Path(sys.executable).resolve()), publisher=str(ROOT/'scripts/publish_native_gameplay.py'),
                   dotnet=str(args.dotnet), exporter=str(exporter), runtime_root=str(args.runtime),
                   target_rid='win-x64' if os.name == 'nt' else 'linux-x64', environment=sorted(runner.env.items()))
    config = args.output/'profiles.json'
    config.write_text(json.dumps(dict(format='poima.development-profiles', version=1, profiles=[profile]), ensure_ascii=False)+'\n', encoding='utf-8')
    calls, lines = [], queue.Queue()
    stderr = (args.output/'host.stderr.log').open('w', encoding='utf-8')
    process = subprocess.Popen([args.binary, 'mcp', '--world', str(args.output/'owner-world.json'), '--development-profiles', config],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr, text=True, encoding='utf-8',
                               env=runner.owner_env, bufsize=1)
    def reader():
        try:
            for line in process.stdout: lines.put(line)
        finally: lines.put(None)
    threading.Thread(target=reader, daemon=True).start()
    complete = False
    def exchange(method, params=None):
        request = dict(jsonrpc='2.0', id=len(calls)+1, method=method, params=params or {})
        process.stdin.write(json.dumps(request)+'\n'); process.stdin.flush()
        line = lines.get(timeout=20)
        need(line is not None, 'Native host ended before its response.')
        reply = json.loads(line); calls.append(dict(request=request, response=reply))
        need(reply.get('id') == request['id'] and 'error' not in reply, 'Invalid MCP response: '+str(reply))
        return reply['result']
    def tool(method, params=None, *, error=None, build=False):
        result = exchange('tools/call', dict(name='poima_build' if build else 'poima_call',
                                           arguments=dict(method=method, params=params or {})))
        data = result['structuredContent']
        need(json.loads(result['content'][0]['text']) == data, 'Structured and textual result disagree.')
        if error is not None:
            need(result['isError'] and data.get('error', {}).get('code') == error, 'Expected tool rejection: '+str(data))
            return data['error']
        need(not result['isError'] and 'result' in data, 'Native operation failed: '+str(data))
        return data['result']
    def wait(job):
        deadline = time.monotonic()+930
        while True:
            state = tool('development.inspect', dict(job_id=job['job_id']))
            need(tool('world.inspect')['revision'] == 0, 'Build changed the authoritative owner world.')
            if state['terminal']: return state
            need(time.monotonic() < deadline, 'Native build never became terminal.')
            time.sleep(.25)
    def cli(label, command, *, success=True):
        stdout, err = args.output/(label+'.stdout.log'), args.output/(label+'.stderr.log')
        start = time.monotonic()
        with stdout.open('w', encoding='utf-8') as out, err.open('w', encoding='utf-8') as error:
            flags = dict(creationflags=subprocess.CREATE_NEW_PROCESS_GROUP) if os.name == 'nt' else dict(start_new_session=True)
            child = subprocess.Popen([str(v) for v in command], stdout=out, stderr=error, env=runner.env, **flags)
            try: child.wait(timeout=180)
            except BaseException:
                if os.name == 'nt': subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'], timeout=20, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                else: child.kill()
                child.wait(timeout=20); raise
        row = dict(label=label, exit_code=child.returncode, seconds=time.monotonic()-start,
                   stdout_sha256=sha(stdout), stderr_sha256=sha(err))
        record['commands'].append(row)
        need((child.returncode == 0) == success, 'CLI returned an unexpected exit status: '+label)
        return stdout.read_text(encoding='utf-8')
    try:
        outside = args.output/'outside'; outside.mkdir()
        alias = args.output/'source-alias'
        escape_link = products/'escape'
        def link(target, destination):
            if os.name == 'nt':
                # These owner-created names contain no cmd metacharacters. A
                # junction needs no global Developer Mode or privilege change.
                for value in (str(target), str(destination)):
                    need(not any(char in value for char in '&|<>^%\r\n"'), 'Unsafe junction fixture path.')
                linked = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(destination), str(target)],
                                        capture_output=True, text=True, timeout=20)
                need(linked.returncode == 0, 'Cannot create owned junction fixture: '+linked.stderr)
            else:
                destination.symlink_to(target, target_is_directory=True)
        link(source, alias); link(outside, escape_link)
        linked_profile = dict(profile, project_root=str(alias))
        invalid_config = args.output/'linked-profiles.json'
        invalid_config.write_text(json.dumps(dict(format='poima.development-profiles', version=1, profiles=[linked_profile]))+'\n')
        cli('linked-profile-rejected', [args.binary, 'world', args.output/'rejected-world.json', '--development-profiles', invalid_config], success=False)
        rejected_log = (args.output/'linked-profile-rejected.stderr.log').read_text(encoding='utf-8')
        need('reparse' in rejected_log if os.name == 'nt' else 'symlink' in rejected_log, 'Linked startup path failed for an unrelated reason.')
        record['checks'] += ['native_linked_profile_rejected']
        exchange('initialize', dict(protocolVersion='2025-11-25', capabilities={}, clientInfo=dict(name='native-publish-check', version='1')))
        process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n'); process.stdin.flush()
        names = {entry['name'] for entry in exchange('tools/list')['tools']}
        need('poima_build' in names, 'Separate build authorization tool was not advertised.')
        profiles = tool('development.profiles')['profiles']
        need(len(profiles) == 1 and profiles[0]['name'] == 'native', 'Selected profile was not discovered.')
        need('environment' not in profiles[0], 'Profile discovery leaked environment values.')
        record['profile_fingerprint'] = profiles[0]['fingerprint']
        request = dict(request_id=uuid.uuid4().hex, profile='native', project='game/Game.csproj', output='native-game', type=GAME_TYPE)
        for method in ('development.compile', 'development.publish', 'development.export'):
            tool(method, request, error=-32602)  # edits alone do not grant build submission
        tool('development.publish', dict(request, environment=[]), error=-32602, build=True)
        tool('development.publish', dict(request, project='../outside.csproj'), error=-32602, build=True)
        tool('development.publish', dict(request, output='escape/unauthorized'), error=-32602, build=True)
        need(not list(outside.iterdir()), 'Linked output rejection wrote outside its root.')
        need(not tool('development.jobs')['jobs'], 'Rejected authority/path inputs created a job.')
        job = tool('development.publish', request, build=True)
        retry = tool('development.publish', request, build=True)
        need(retry['replayed'] and retry['job_id'] == job['job_id'], 'Queued retry duplicated publishing.')
        tool('development.export', {k: v for k, v in request.items() if k != 'type'}, error=-32009, build=True)
        published = wait(job)
        need(published['state'] == 'succeeded' and published['exit_code'] == 0, 'Native publication failed: '+str(published))
        result = published['result']
        descriptor = products/'native-game/native-gameplay.json'
        need(result['kind'] == 'native_gameplay' and result['descriptor_sha256'] == sha(descriptor), 'Successful receipt lacks verified artifact identity.')
        need(tool('development.publish', request, build=True)['replayed'], 'Completed publication could not be retried after output creation.')
        artifact = json.loads(descriptor.read_text())
        for member in artifact['files']:
            file = descriptor.parent/member['path']
            need(file.stat().st_size == member['size'] and sha(file) == member['sha256'], 'Artifact payload changed.')
        callbacks = cli('native-callbacks', [args.compatibility_test, '--native', descriptor])
        need('with compiled NativeAOT Tick/Control' in callbacks, 'No genuine compiled callback execution.')
        record['publication'] = dict(state=published['state'], elapsed_ms=published['elapsed_ms'], result=result)
        record['checks'] += ['separate_build_tool', 'explicit_profile', 'rejected_env_and_traversal', 'queued_and_completed_retry',
                             'cross_method_retry_rejected', 'native_artifact_inventory', 'genuine_native_callbacks']
        if not args.publish_only:
            game = source/'export-project'
            created = json.loads(cli('project-create', [args.binary, 'project', 'create', game, '--name', 'Native job check']))
            need(created['status'] == 'ok', 'Project creation failed.')
            shutil.copytree(descriptor.parent, game/'gameplay')
            manifest = game/'project.json'; spec = json.loads(manifest.read_text())
            spec['version'] = 2
            spec['gameplay'] = dict(descriptor='gameplay/native-gameplay.json', values={field['name']: '0' if field['kind'] in ('int64', 'uint64') else 0 for field in artifact['schema']['fields']})
            manifest.write_text(json.dumps(spec, indent=2)+'\n')
            export = dict(request_id=uuid.uuid4().hex, profile='native', project='export-project/project.json', output='bundle')
            exported = wait(tool('development.export', export, build=True))
            need(exported['state'] == 'succeeded' and exported['result']['kind'] == 'game_bundle', 'Native export failed: '+str(exported))
            bundle = products/'bundle/game.json'
            need(exported['result']['manifest_sha256'] == sha(bundle), 'Export receipt has the wrong manifest pin.')
            inspected = json.loads(cli('game-inspect', [args.binary, 'game', 'inspect', bundle]))
            need(inspected['status'] == 'ok' and inspected['result']['gameplay']['descriptor'] == 'gameplay/native-gameplay.json', 'Exported native gameplay closure missing.')
            need(tool('development.export', export, build=True)['replayed'], 'Completed export retry failed.')
        # Cancel an actual queued job behind a real failing compiler invocation.
        bad = source/'broken'; bad.mkdir()
        (bad/'Game.csproj').write_text(project.read_text())
        shutil.copyfile(fixture/'Poima.Gameplay.dll', bad/'Poima.Gameplay.dll')
        (bad/'Game.cs').write_text('public class Broken { invalid code !!! }')
        broken = dict(request, request_id=uuid.uuid4().hex, project='broken/Game.csproj', output='failed-native')
        failed_job = tool('development.publish', broken, build=True)
        queued = tool('development.publish', dict(request, request_id=uuid.uuid4().hex, output='cancelled-native'), build=True)
        observed = tool('development.inspect', dict(job_id=queued['job_id']))
        need(observed['state'] == 'queued', 'Cancellation fixture was not observed queued behind the actual failing build.')
        tool('development.cancel', dict(job_id=queued['job_id']))
        cancelled = wait(queued); failed = wait(failed_job)
        need(cancelled['state'] == 'cancelled' and not cancelled.get('result') and not (products/'cancelled-native').exists(), 'Queued cancellation published an artifact.')
        need(failed['state'] == 'failed' and failed['exit_code'] != 0 and not failed.get('result') and not (products/'failed-native').exists(), 'Compiler failure published successful artifact data.')
        if not args.publish_only:
            record['export'] = dict(state=exported['state'], elapsed_ms=exported['elapsed_ms'], result=exported['result'])
            record['checks'] += ['native_game_export_inventory']
        feedback = tool('development.diagnostics', dict(job_id=failed_job['job_id']))
        need(feedback['state'] == 'failed' and any(item['severity'] == 'error' for item in feedback['diagnostics']), 'Failed native publication lost structured compiler feedback.')
        record['failure'] = dict(state=failed['state'], exit_code=failed['exit_code'], diagnostics=feedback['diagnostics'])
        record['cancellation'] = dict(observed_before_cancel=observed['state'], state=cancelled['state'], result_absent=not cancelled.get('result'))
        record['checks'] += ['failed_compile_no_result', 'queued_cancellation_no_output', 'unchanged_world_revision']
        tool('session.close'); complete = True
    except BaseException as error:
        record['error'] = str(error); raise
    finally:
        try: process.stdin.close()
        except BaseException as error: record['cleanup_error'] = str(error)
        try: process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            row = {}; record['forced_cleanup'] = row
            try: runner.cleanup(process, row)
            except BaseException as error:
                record['cleanup_error'] = str(error)
                if process.poll() is None:
                    try: process.kill(); process.wait(timeout=20)
                    except BaseException as fallback: record['cleanup_fallback_error'] = str(fallback)
        stderr.close()
        record['host_exit_code'] = process.returncode
        record['calls'] = len(calls)
        (args.output/'rpc.json').write_text(json.dumps(calls, indent=2)+'\n')
        record['rpc_sha256'] = sha(args.output/'rpc.json')
        record['passed'] = complete and process.returncode == 0 and 'cleanup_error' not in record and 'forced_cleanup' not in record
        (args.output/'evidence.json').write_text(json.dumps(record, indent=2)+'\n')
    need(record['passed'], 'Host cleanup failed.')
    print(json.dumps(dict(passed=True, calls=record['calls'], checks=record['checks'])))


if __name__ == '__main__':
    main()
