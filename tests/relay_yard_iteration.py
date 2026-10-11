#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual Relay Yard CoreCLR build/reload and frozen-scene iteration.

Run natively on Windows beneath the caller's process-tree supervisor. Compiler
processes are launched only by the engine's development worker. No downloads,
teleports, gameplay patches, save modification or implicit scene hot apply.
"""
import argparse
import copy
import hashlib
import json
import math
import os
import shutil
from pathlib import Path
import struct
import sys
import time
import traceback
import uuid
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
import player_service_contract as shared
import relay_yard_contract as contract
from scene_capture import pixels

need, uid, sha = contract.need, contract.uid, contract.sha
fresh = lambda: uuid.uuid4().hex
MARKER = 1500
WIDTH, HEIGHT = 960, 540


def inventory(root):
    need(root.is_dir() and not root.is_symlink(), 'Missing regular input closure')
    result, seen, total = {}, set(), 0
    for path in sorted(root.rglob('*')):
        need(not path.is_symlink(), 'Linked input closure')
        if path.is_dir():
            continue
        need(path.is_file(), 'Special input closure member')
        name = path.relative_to(root).as_posix()
        need(name.casefold() not in seen and len(result) < 50000, 'Closure collision or file bound')
        seen.add(name.casefold()); total += path.stat().st_size
        need(total <= 512*1024*1024, 'Input closure byte bound')
        result[name] = dict(bytes=path.stat().st_size, sha256=sha(path))
    return result


def stage_projects(output, sdk, generator):
    """Copy only reviewed sample sources; reference pinned existing tooling."""
    inputs = [ROOT/'examples/relay-yard/RelayYardGame.cs',
              ROOT/'examples/relay-yard/RelayAudio.cs',
              ROOT/'examples/locomotion-yard/LocomotionYardGame.cs']
    source = inputs[0].read_text(encoding='utf-8')
    old = 'const double RateEpsilon=.01, UseDistance=3;'
    need(source.count(old) == 1 and source.count('public int LockedUses;') == 1,
         'Reviewed source variant anchors changed; review the draft before running')
    variants = {'a': source, 'b': source.replace(old, old.replace('=3;', '=4;')),
                'bad': source.replace('public int LockedUses;', 'public long LockedUses;')}
    variants['warm'] = variants['b']
    projects = {}
    for name, text in variants.items():
        folder = output/'source'/name
        folder.mkdir(parents=True)
        (folder/'RelayYardGame.cs').write_text(text, encoding='utf-8')
        for original in inputs[1:]:
            (folder/original.name).write_bytes(original.read_bytes())
        project = folder/'Iteration.csproj'
        project.write_text('''<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework><LangVersion>14.0</LangVersion>
    <Nullable>enable</Nullable><ImplicitUsings>enable</ImplicitUsings>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks><TreatWarningsAsErrors>true</TreatWarningsAsErrors>
    <Deterministic>true</Deterministic><AssemblyName>Poima.RelayYardGame</AssemblyName>
    <BuildInParallel>false</BuildInParallel><UseSharedCompilation>false</UseSharedCompilation>
    <ConcurrentBuild>false</ConcurrentBuild>
  </PropertyGroup>
  <ItemGroup>
    <Reference Include="Poima.Gameplay"><HintPath>'''+escape(str(sdk))+'''</HintPath><Private>true</Private></Reference>
    <Analyzer Include="'''+escape(str(generator), {'"': '&quot;'})+'''" />
  </ItemGroup>
</Project>
''', encoding='utf-8')
        projects[name] = project
    return projects, {p.relative_to(ROOT).as_posix(): sha(p) for p in inputs}


def stored_payload(root, slot):
    directory = root/('slot-'+slot)
    manifest = json.loads((directory/'current.json').read_text(encoding='utf-8'))
    row = manifest['payload']['current']
    name = row['file']
    need(isinstance(name, str) and len(name) == 38 and name.startswith('p-') and
         name.endswith('.bin') and all(c in '0123456789abcdef' for c in name[2:-4]),
         'Invalid native store payload name')
    path = directory/name
    need(path.stat().st_size == row['bytes'] and sha(path) == row['sha256'],
         'Durable native payload differs from its manifest')
    return json.loads(path.read_bytes())['snapshot']['payload']


def unchanged_payload(before, after, expected_revision):
    """Every native payload member and all global values, not selected actors."""
    left, right = copy.deepcopy(before), copy.deepcopy(after)
    need(right['gameplay_revision'] == expected_revision and
         expected_revision == left['gameplay_revision']+1, 'Reload revision did not advance once')
    left.pop('gameplay_revision'); right.pop('gameplay_revision')
    old_image = left['gameplay'].pop('assembly_sha256')
    new_image = right['gameplay'].pop('assembly_sha256')
    need(old_image != new_image and left == right,
         'Reload changed native checkpoint payload, schema, or retained gameplay values')


class BoundedClient(shared.Client):
    def send(self, method, params=None):
        need(time.monotonic() < DEADLINE, 'Overall iteration deadline expired')
        return super().send(method, params)

    def receive(self):
        remaining = DEADLINE-time.monotonic()
        need(remaining > 0, 'Overall iteration deadline expired')
        previous = shared.ARGS.timeout
        shared.ARGS.timeout = min(previous, remaining)
        try:
            return super().receive()
        finally:
            shared.ARGS.timeout = previous


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'dotnet', 'hostfxr', 'bridge', 'sdk', 'component-generator',
                 'manifest', 'source-directory', 'audio-directory', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    parser.add_argument('--overall-timeout', type=float, default=1800)
    parser.add_argument('--warm-samples', type=int, default=20)
    args = parser.parse_args()
    need(os.name == 'nt' and not sys.flags.optimize, 'Use native Windows Python without optimization')
    need(180 <= args.overall_timeout <= 3600 and math.isfinite(args.overall_timeout) and
         0 <= args.gpu <= 4095 and args.warm_samples == 20,
         'Timeout/GPU outside bounded admission; this reviewed cohort uses20 warm edits')
    for name in ('binary', 'dotnet', 'hostfxr', 'bridge', 'sdk', 'component_generator', 'manifest'):
        path = getattr(args, name).resolve(strict=True)
        need(path.is_file() and not path.is_symlink(), 'Supply regular compiled input: '+name)
        setattr(args, name, path)
    args.source_directory = args.source_directory.resolve(strict=True)
    args.audio_directory = args.audio_directory.resolve(strict=True)
    need(args.source_directory.is_dir() and args.audio_directory.is_dir(), 'Missing original licensed inputs')
    args.output = args.output.resolve()
    need(not args.output.exists() and all(not args.output.is_relative_to(p) for p in
         (args.source_directory, args.audio_directory, args.bridge.parent, args.sdk.parent,
          args.component_generator.parent, args.binary.parent, args.dotnet.parent,
          args.hostfxr.parent, args.manifest.parent)), 'Use a new output outside immutable input closures')
    args.output.mkdir(parents=True)
    args.windows_interop, args.timeout, args.descriptor = False, 30, None
    global DEADLINE
    DEADLINE = time.monotonic()+args.overall_timeout
    started = time.monotonic()
    record = dict(passed=False, format='poima.relay-yard-iteration.v1', calls=[], owners=[],
        checks=[], captures={}, jobs={}, snapshots={}, cleanup_errors=[],
        limitations=['CoreCLR development and guarded semantic play; not Native AOT hot reload or physical input.',
            'Compilation runs while this actual game is paused; observer timings do not prove unpaused-play throughput.',
            'Setup builds use fresh projects; machine/.NET/package caches are not cleared.',
            'Warm edit-to-first-verified-tick includes IPC, exact preservation reads and durable checkpoint verification.',
            'Authored geometry changes require explicit stop/start; no general live scene apply.',
            'Ordinary saves select their exact original image, not a rebuilt compatible image.',
            'Candidate constructors/Initialize run during staging before retained fields are copied.',
            'Bounded iteration fixture, not a 30-minute soak, whole Alpha or external-agent autonomy.'])
    shared.ARGS, shared.RECORD = args, record
    launcher = contract.load_launcher()
    manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
    dependencies = [Path(__file__), Path(contract.__file__), Path(shared.__file__),
        ROOT/'tests/scene_capture.py', ROOT/'examples/relay-yard/run.py',
        ROOT/'examples/locomotion-yard/run.py',
        *sorted((ROOT/'tools/python/poima_client').glob('*.py'))]
    immutable_files = [args.binary, args.dotnet, args.hostfxr, args.bridge, args.sdk,
                       args.component_generator, args.manifest]
    # Pin supplied SDK/bridge/generator dependency metadata and managed images.
    # The existing machine .NET installation is not a copied hermetic SDK.
    for directory in {args.bridge.parent, args.component_generator.parent, args.sdk.parent}:
        immutable_files += [p for p in directory.iterdir() if p.is_file() and
                            (p.suffix == '.dll' or p.name.endswith(('.deps.json', '.runtimeconfig.json')))]
    if (args.binary.parent/'phonon.dll').is_file():
        immutable_files.append(args.binary.parent/'phonon.dll')
    immutable = {str(p): sha(p) for p in set(immutable_files+dependencies)}
    originals = {str(args.source_directory/p): sha(args.source_directory/p)
                 for p in launcher.locomotion.SOURCE_SHA}
    need({p: sha(args.source_directory/p) for p in launcher.locomotion.SOURCE_SHA} ==
         launcher.locomotion.SOURCE_SHA, 'Original licensed character sources differ')
    audio_pin = inventory(args.audio_directory)
    host = client = observer = game = None
    jobs = []
    complete = False

    def persist():
        record['elapsed_seconds'] = time.monotonic()-started
        text = json.dumps(record, indent=2, allow_nan=False)+'\n'
        need(len(text.encode('utf-8')) <= 256*1024*1024, 'Retained iteration evidence exceeds256MiB')
        (args.output/'evidence.json').write_text(text, encoding='utf-8')

    try:
        projects, source_pin = stage_projects(args.output, args.sdk, args.component_generator)
        immutable.update({str(ROOT/p): value for p, value in source_pin.items()})
        record['input_pins'] = immutable
        record['original_pins'] = originals
        record['audio_inventory'] = audio_pin
        staged_pin = inventory(args.output/'source')
        record['staged_sources'] = staged_pin
        endpoint = 'relay-iteration-'+fresh()
        world = args.output/'world.json'
        host = shared.ProcessOwner([str(args.binary), 'serve', str(world), '--endpoint', endpoint],
                                   'iteration-host', endpoint, 'serve')
        client = BoundedClient(endpoint, 'iteration-driver')
        observer = BoundedClient(endpoint, 'iteration-observer')
        discovery = observer.call('world.describe', {'view':'catalog'})
        need(not discovery['read_only'] and discovery['session_scope'] == 'shared_headless',
             'Development requires the genuine shared authoring owner')
        record['build'] = discovery.get('build')

        class HostedRelay(launcher.OwnedRelay):
            def __init__(self):
                self.args, self.world, self.label, self.record = args, world, 'iteration', record
                self.deadline, self.native, self.manifest = DEADLINE, lambda p:str(Path(p).resolve()), manifest
                self.session, self.tick, self.revision, self.closed, self.calls = None, 0, 0, False, 0
                self.player_id = None
                self.player_generation = None
                self.player_audio_enabled = None
                self.assembly = None

            def rpc(self, method, params=None, error=None, timeout=30):
                need(time.monotonic() < DEADLINE and host.process.poll() is None,
                     'Owned authoring host ended or deadline expired')
                need(self.calls < 20000 and len(record['calls']) < 50000, 'Iteration request budget exceeded')
                row = dict(owner=self.label, method=method, params=copy.deepcopy(params or {}))
                record['calls'].append(row); self.calls += 1
                reply = client.reply(method, params or {})
                row['reply'] = reply
                if error is not None:
                    need(reply.get('error', {}).get('code') == error, reply)
                    row['error'] = reply['error']; return reply['error']
                need('result' in reply, reply)
                row['result'] = reply['result']; return reply['result']

            def config(self):
                return dict(hostfxr=str(args.hostfxr), bridge=str(args.bridge),
                            assembly=str(self.assembly), type=launcher.TYPE)

            def load(self, error=None):
                params = self.load_parameters()
                result = self.rpc('runtime.gameplay.load', params, error)
                if error is None: self.revision = result['revision']
                return result

            def load_parameters(self):
                state, module = self.inspect(), self.module()
                return dict(session_id=self.session, request_id=fresh(), expected_tick=self.tick,
                    expected_revision=module['revision'], expected_structure_revision=state['structure_revision'],
                    **self.config())

            def sync(self):
                state = self.rpc('runtime.status')
                need(state['active'], 'Expected active real runtime')
                self.session, self.tick = state['session_id'], state['tick']
                return state

            def paused(self):
                until = min(DEADLINE, time.monotonic()+30)
                while time.monotonic() < until:
                    state = self.rpc('player.inspect')
                    need(state['active'], 'Player ended while awaiting presentation')
                    need(state['player_id'] == self.player_id and
                         state['generation'] == self.player_generation,
                         'Paused boundary belongs to a replaced presentation owner/generation')
                    if state['ready'] and state['paused']:
                        need(state['report']['hardware'] and state['report']['nvrhi_errors'] == 0,
                             'Native player failed presentation')
                        need(state['report']['audio']['enabled'] == self.player_audio_enabled,
                             'Actual presentation audio differs from its accepted launch')
                        self.sync()
                        need(state['session_id'] == self.session and state['tick'] == self.tick,
                             'Presentation and native runtime disagree')
                        record.setdefault('paused_boundaries', []).append(state)
                        return state
                    time.sleep(.01)
                raise AssertionError('Actual paused player readiness deadline expired')

            def control(self, action):
                result = super().control(action)
                if self.player_id is not None:
                    state = self.rpc('player.inspect')
                    if not state['paused']:
                        self.rpc('player.control', shared.control_parameters(state, 'pause'))
                    self.paused()
                    record.setdefault('control_boundaries', []).append(dict(action=action,
                        receipt_tick=result['current_tick'], actual_tick=self.tick,
                        naturally_elapsed_ticks=self.tick-result['current_tick']))
                return result

            def start_player(self):
                absent = self.rpc('player.inspect')
                need(not absent['active'], 'Cannot overwrite an existing player')
                state = self.inspect()
                params = shared.start_parameters(self.session, absent['generation'], expected_tick=self.tick,
                    expected_structure_revision=state['structure_revision'], width=WIDTH, height=HEIGHT,
                    gpu=args.gpu, audio=True, settings_overrides={'camera.vertical_fov':60,'ui.scale':1})
                result = self.rpc('player.start', params)
                self.player_id = result['player_id']
                self.player_generation = result['generation']
                self.player_audio_enabled = params['audio']
                need(self.player_generation == absent['generation']+1,
                     'Accepted player launch did not advance the observed generation once')
                record.setdefault('player_starts', []).append(dict(request=params, receipt=result))
                self.paused()
                return result

            def stop_player(self):
                state = self.rpc('player.inspect')
                if self.player_id is None:
                    need(not state['active'], 'An unowned presentation must not be stopped')
                    return None
                need(state['player_id'] == self.player_id and state['generation'] == self.player_generation,
                     'Stop boundary belongs to an unexpected presentation owner/generation')
                if state['active']:
                    self.rpc('player.control', shared.control_parameters(state, 'stop'))
                    state = shared.await_state(observer, lambda v:not v['active'], 'stop-'+fresh())
                record.setdefault('player_terminals', []).append(state)
                need(state['player_id'] == self.player_id and state['generation'] == self.player_generation and
                     state['state'] == 'finished' and state['report']['success'] and
                     state['report']['nvrhi_errors'] == 0 and
                     state['report']['audio']['enabled'] == self.player_audio_enabled and
                     (not self.player_audio_enabled or state['report']['audio']['stream_drained']),
                     'Owned player ended without successful rendering and drained requested audio')
                self.player_id = None
                self.player_generation = None
                self.player_audio_enabled = None
                return state

        game = HostedRelay()

        def compile_variant(name, project_key=None, expected_success=True, warm=False):
            project = projects[project_key or name]
            out = args.output/'compiled'/('warm' if warm else name)
            before = observer.call('world.inspect')['revision']
            began = time.monotonic()
            request = dict(request_id=fresh(), executable=str(args.dotnet), project=str(project),
                           output=str(out), configuration='Release',
                           timeout_ms=min(600000, max(100, int((DEADLINE-time.monotonic())*1000))))
            job = game.rpc('development.compile', request); jobs.append(job['job_id'])
            record['jobs'][name] = dict(request=request, accepted=job)
            source_copy = args.output/'retained-sources'/name
            source_copy.parent.mkdir(exist_ok=True)
            source_inventory = inventory(project.parent)
            shutil.copytree(project.parent, source_copy)
            need(inventory(source_copy) == source_inventory, 'At-run compiler source closure copy differs')
            record['jobs'][name].update(source_inventory=source_inventory,
                retained_source_directory=source_copy.relative_to(args.output).as_posix())
            record['jobs'][name]['classification'] = ('warm_incremental_edit' if name.startswith('warm-') else
                'syntax_error_setup' if name == 'syntax-error' else
                'repaired_warm_project_setup' if name == 'syntax-repair' else 'new_project_setup')
            need(game.rpc('development.compile', request) == {**job, 'replayed':True},
                 'Compile retry did not retain the original job')
            observations, observer_samples, state = [], [], None
            while time.monotonic() < DEADLINE:
                state = game.rpc('development.inspect', {'job_id':job['job_id']})
                observations.append(state)
                if state['state'] not in ('queued', 'running'): break
                need(len(observations) <= 6000, 'Compile poll bound exceeded')
                for method in ('world.inspect', 'player.inspect') if game.player_id is not None else ('world.inspect',):
                    sent = time.monotonic(); result = observer.call(method)
                    elapsed = time.monotonic()-sent
                    observer_samples.append(dict(method=method, seconds=elapsed,
                        job_state=state['state'], active_player=game.player_id is not None))
                    if method == 'world.inspect':
                        need(result['revision'] == before, 'Background compiler changed authored revision')
                    else:
                        need(result['active'] and result['paused'] and result['player_id'] == game.player_id and
                             result['generation'] == game.player_generation and result['tick'] == game.tick,
                             'Compiler observation lost the paused real-game owner/state')
                time.sleep(.02)
            need(state is not None and state['state'] not in ('queued', 'running'),
                 'Actual compile did not reach a terminal state within the overall deadline')
            need(inventory(project.parent) == source_inventory,
                 'Background compiler or another writer changed the at-run project sources')
            need(state['stdout_truncated'] is False and state['stderr_truncated'] is False,
                 'Actual compiler output was truncated; no complete-diagnostics claim is possible')
            record['jobs'][name].update(observations=observations, seconds=time.monotonic()-began)
            record['jobs'][name]['observer_response_samples'] = observer_samples
            record['jobs'][name]['diagnostics'] = game.rpc('development.diagnostics', {'job_id':job['job_id'], 'limit':128})
            need(record['jobs'][name]['diagnostics']['more'] is False and
                 record['jobs'][name]['diagnostics']['incomplete'] is False,
                 'Structured compiler diagnostics are incomplete or exceed the retained bound')
            if not expected_success:
                need(state['state'] == 'failed' and state['exit_code'] is not None and state['exit_code'] != 0,
                     'Intentional C# syntax error did not fail the actual compiler')
                need(any(item['severity'] == 'error' and item['code'].startswith('CS')
                         for item in record['jobs'][name]['diagnostics']['diagnostics']),
                     'Intentional syntax failure supplied no structured C# error')
                need(observer.call('world.inspect')['revision'] == before, 'Failed compilation changed authored world')
                persist(); return None
            need(state['state'] == 'succeeded' and state['exit_code'] == 0,
                 'Actual engine-owned compile failed; retained diagnostics identify it')
            need(observer.call('world.inspect')['revision'] == before, 'Compilation mutated authored world')
            assembly = out/'managed/Poima.RelayYardGame.dll'
            need(assembly.is_file() and (assembly.parent/'Poima.Gameplay.dll').is_file(),
                 'Compiler did not emit the real game/SDK closure')
            record['jobs'][name]['managed_inventory'] = inventory(out/'managed')
            retained = args.output/'retained-images'/name
            retained.parent.mkdir(exist_ok=True)
            shutil.copytree(out/'managed', retained)
            need(inventory(retained) == record['jobs'][name]['managed_inventory'],
                 'At-run managed closure copy differs from compiled image')
            record['jobs'][name]['retained_image_directory'] = retained.relative_to(args.output).as_posix()
            persist(); return assembly

        def preferences():
            selected = observer.call('player.settings.inspect')
            return dict(player_id=selected['player_id'], values=selected['settings']['values'],
                        revision=selected['settings']['revision'])

        def snapshot():
            value = driver.snapshot()
            value['voices'] = game.rpc('runtime.audio.voices', dict(session_id=game.session, tick=game.tick, limit=256))
            need(not value['voices']['has_more'], 'Voice oracle truncated')
            value['cadence'] = {uid(n):game.rpc('runtime.component.get', dict(session_id=game.session,
                tick=game.tick, id=uid(n), type=launcher.AUDIO_CADENCE)) for n in (100,300)}
            value['preferences'] = preferences()
            value['marker'] = game.entity(MARKER)
            return contract.normalized(value)

        def capture(label):
            state = game.paused()
            path = args.output/(label+'.bmp')
            result = game.rpc('player.capture', dict(player_id=state['player_id'], request_id=fresh(),
                expected_control_revision=state['control_revision'], session_id=game.session, tick=game.tick,
                expected_structure_revision=state['structure_revision'], expected_ui_revision=state['ui_revision'],
                path=str(path)))
            raw = path.read_bytes(); actual = result['capture']
            width, height = struct.unpack_from('<ii', raw, 18)
            need(raw[:2] == b'BM' and (width, abs(height)) == (WIDTH, HEIGHT) and
                 actual['capture_written'] and actual['hardware'] and actual['nvrhi_errors'] == 0,
                 'Missing actual native Vulkan readback')
            draws = actual['render_diagnostics']['last_draws']
            need(draws['skinned_instances'] == 1 and draws['skinned_vertices'] == 1029,
                 'Original imported courier was not actually submitted')
            colors = {pixel for row in pixels(path)[100:HEIGHT-120:7] for pixel in row[20:WIDTH-40:7]}
            need(len(colors) >= 8, 'Native scene readback contains no useful variation')
            need(game.sync()['tick'] == state['tick'], 'Paused capture advanced simulation')
            value = dict(sha256=sha(path), bytes=len(raw), result=result, distinct_colors=len(colors))
            record['captures'][label] = value; persist(); return value

        def save(slot):
            receipt = game.write(slot)
            payload = stored_payload(args.output/'saves', slot)
            record.setdefault('save_payloads', {})[slot] = payload
            return payload, receipt

        game.assembly = compile_variant('a')
        game.author(args.source_directory)
        marker_transform = dict(position=[-4,1,3.5], rotation=[0,0,0,1], scale=[.8,.8,.8])
        authored_revision = game.rpc('world.inspect')['revision']
        game.rpc('world.transact', dict(request_id=fresh(), base_revision=authored_revision, ops=[
            dict(op='entity.create', id=uid(MARKER), name='Iteration visual marker', parent=None),
            dict(op='component.set', id=uid(MARKER), type='Transform', value=marker_transform),
            dict(op='component.set', id=uid(MARKER), type='MeshRenderer',
                 value=dict(primitive='box', visible=True, albedo=[.8,.04,.04]))]))
        game.start(); loaded = game.load(); game.saves(args.output/'saves')
        need(loaded['module']['backend'] == 'coreclr' and
             loaded['module']['schema']['components'] == manifest['schemas'] and
             len(loaded['module']['values']) == 50, 'Compiled A differs from the selected current sample schema')
        game.start_player()
        driver = contract.RelayDriver(game, record)
        driver.step(); driver.control('begin'); driver.step(16)
        driver.drive_to(-6,7,'first-cell'); driver.use_target(driver.values()['CellOne'])
        need(driver.values()['Collected'] == 1 and driver.values()['CellOne'] == uid(0),
             'Actual first collectible did not despawn')
        driver.drive_to(-2,9,'range-proof')
        target = driver.values()['CellTwo']
        ray_a = driver.aim_at(target)
        camera = game.entity(101); matrix = camera['world_matrix']
        ray_four = game.rpc('runtime.raycast', dict(session_id=game.session, tick=game.tick,
            origin=contract.position(camera), direction=[-matrix[8],-matrix[9],-matrix[10]],
            distance=4, ignore=[uid(100)]))
        need(ray_a['hit'] is None and ray_four['hit'] is not None and
             ray_four['hit']['entity'] == target and 3 < ray_four['hit']['distance'] < 4,
             'Independent native ray did not place the pickup strictly between A/B ranges')
        driver.step(use=True); driver.step()
        need(driver.values()['Collected'] == 1 and driver.values()['CellTwo'] == target,
             'Original 3m code unexpectedly collected a beyond-3m cell')
        record['range_oracle'] = dict(target=target, ray_three=ray_a, ray_four=ray_four)
        before = snapshot(); before_image = capture('before-code-reload')
        payload_a, _ = save('iteration-a')
        compiled_b = compile_variant('b')
        need(snapshot() == before, 'Background compilation changed paused native game state')
        game.assembly = compiled_b
        request = game.load_parameters(); began = time.monotonic()
        receipt = game.rpc('runtime.gameplay.load', request)
        after = snapshot()
        allowed = copy.deepcopy(before); allowed['gameplay_revision'] = receipt['revision']
        need(receipt['revision'] == before['gameplay_revision']+1 and after == allowed,
             'Compatible code reload changed observed simulation/UI/audio/preferences')
        migration = receipt['module']['migration']
        need(migration['added'] == migration['removed'] == [] and
             migration['preserved'] == sorted(before['values']), 'Not every original field was retained')
        need(receipt['module']['assembly_sha256'] == sha(compiled_b), 'Reload did not select actual B image')
        record['reload'] = dict(request=request, receipt=receipt, seconds=time.monotonic()-began)
        need(game.rpc('runtime.gameplay.load', request) == {**receipt, 'replayed':True} and snapshot() == after,
             'Exact reload retry reapplied the module or changed native state')
        game.rpc('runtime.gameplay.load', {**request, 'request_id':fresh()}, error=-32009)
        need(snapshot() == after, 'Stale reload mutated state')
        payload_b, _ = save('iteration-b')
        unchanged_payload(payload_a, payload_b, receipt['revision'])
        after_image = capture('after-code-reload')
        need(before_image['sha256'] == after_image['sha256'], 'State-preserving reload changed same-context pixels')
        record['snapshots']['before_reload'], record['snapshots']['after_reload'] = before, after
        game.assembly = compile_variant('bad')
        stable = snapshot(); bad = game.load_parameters()
        game.rpc('runtime.gameplay.load', bad, error=-32060)
        need(snapshot() == stable, 'Rejected retained-field type change mutated native/managed state')
        game.assembly = compiled_b
        collection = game.rpc('runtime.gameplay.collect', dict(session_id=game.session))
        need(collection['active_modules'] == 1 and collection['retired_alive'] == 0,
             'Compatible/rejected candidates left retired collectible modules alive')
        record['managed_collection'] = collection

        # A true compiler failure is different from the successfully compiled
        # incompatible state type above. It must never replace the live image.
        warm_source = projects['warm'].parent/'RelayYardGame.cs'
        a_source = (projects['a'].parent/'RelayYardGame.cs').read_text(encoding='utf-8')
        b_source = (projects['b'].parent/'RelayYardGame.cs').read_text(encoding='utf-8')
        need(b_source.count('UseDistance=4;') == 1, 'Reviewed warm-edit anchor differs')
        failed_before = snapshot()
        failed_payload, _ = save('compiler-failure-before')
        warm_source.write_text(b_source.replace('UseDistance=4;', 'UseDistance=;'), encoding='utf-8')
        compile_variant('syntax-error', project_key='warm', expected_success=False, warm=True)
        need(snapshot() == failed_before, 'Actual failed compiler changed the running game')
        rejected_payload, _ = save('compiler-failure-after')
        need(rejected_payload == failed_payload, 'Failed compiler changed complete durable native state')
        warm_source.write_text(b_source, encoding='utf-8')
        compile_variant('syntax-repair', project_key='warm', warm=True)
        need(snapshot() == failed_before, 'Repaired background build changed the original paused game')
        repaired_payload, _ = save('compiler-repair-after')
        need(repaired_payload == failed_payload, 'Compiler repair changed complete original native state')
        record['checks'].append('real_syntax_failure_and_repair_preserve_complete_paused_game')

        # Repeated range-sensitive interaction uses a still-locked terminal:
        # unlike a pickup it stays in the world. No object/state reset is used.
        # Both routes skirt the existing cover using real controller inputs.
        driver.follow_waypoints([(6,7),(6,2.7)], 'warm-terminal-approach')
        short_ray = driver.aim_at(600)
        terminal_camera = game.entity(101); terminal_matrix = terminal_camera['world_matrix']
        long_ray = game.rpc('runtime.raycast', dict(session_id=game.session, tick=game.tick,
            origin=contract.position(terminal_camera),
            direction=[-terminal_matrix[8],-terminal_matrix[9],-terminal_matrix[10]],
            distance=4, ignore=[uid(100)]))
        need(short_ray['hit'] is None and long_ray['hit'] is not None and
             long_ray['hit']['entity'] == uid(600) and 3 < long_ray['hit']['distance'] < 4,
             'Independent locked-terminal ray is not strictly between3m and4m')
        record['warm_range_oracle'] = dict(ray_three=short_ray, ray_four=long_ray)
        record['warm_iterations'] = []
        for index in range(args.warm_samples):
            variant = 'a' if index%2 == 0 else 'b'
            before_reload = snapshot()
            native_before, _ = save(f'warm-{index+1:02d}-before')
            source_text = a_source if variant == 'a' else b_source
            edited_at = time.monotonic()
            warm_source.write_text(source_text, encoding='utf-8')
            source_digest = sha(warm_source)
            job_name = f'warm-{index+1:02d}-{variant}'
            built = compile_variant(job_name, project_key='warm', warm=True)
            need(sha(warm_source) == source_digest and snapshot() == before_reload,
                 'Warm compile altered source or paused native state')
            game.assembly = built
            reload_request = game.load_parameters()
            reload_receipt = game.rpc('runtime.gameplay.load', reload_request)
            game.revision = reload_receipt['revision']
            expected = copy.deepcopy(before_reload)
            expected['gameplay_revision'] = reload_receipt['revision']
            need(snapshot() == expected and reload_receipt['revision'] == before_reload['gameplay_revision']+1 and
                 reload_receipt['module']['assembly_sha256'] == sha(built) and
                 reload_receipt['module']['migration'] == dict(added=[], removed=[], preserved=sorted(before_reload['values'])),
                 'Warm compatible reload did not preserve every inspected original value/schema')
            native_after, _ = save(f'warm-{index+1:02d}-after')
            unchanged_payload(native_before, native_after, reload_receipt['revision'])
            prior_values = driver.values(); prior_tick = game.tick
            driver.step(use=True)
            updated = driver.values()
            need(game.tick == prior_tick+1 and updated['LockedUses'] == prior_values['LockedUses']+(variant == 'b') and
                 updated['Collected'] == prior_values['Collected'] == 1 and updated['Won'] == 0 and
                 all(updated[field] == prior_values[field] for field in contract.CELL_FIELDS),
                 'First updated real Tick did not prove the changed3m/4m terminal behavior')
            elapsed = time.monotonic()-edited_at
            record['warm_iterations'].append(dict(sample=index+1, variant=variant,
                source_sha256=source_digest, assembly_sha256=sha(built),
                seconds=elapsed, tick_before=prior_tick, first_verified_tick=game.tick,
                locked_uses_before=prior_values['LockedUses'], locked_uses_after=updated['LockedUses'],
                reload_receipt=reload_receipt,
                complete_payload_before_sha256=hashlib.sha256(json.dumps(native_before,sort_keys=True,
                    separators=(',',':'),allow_nan=False).encode()).hexdigest(),
                complete_payload_after_sha256=hashlib.sha256(json.dumps(native_after,sort_keys=True,
                    separators=(',',':'),allow_nan=False).encode()).hexdigest(),
                exact_preservation_verified=True, player_paused=True))
            driver.step()  # Explicit neutral release, outside the timed first-update boundary.
            persist()
        timings = sorted(row['seconds'] for row in record['warm_iterations'])
        p95 = timings[math.ceil(.95*len(timings))-1]
        record['edit_to_first_verified_tick'] = dict(samples=len(timings), unit='seconds',
            min=timings[0], max=timings[-1], mean=sum(timings)/len(timings),
            p50=timings[math.ceil(.50*len(timings))-1], p95=p95,
            p99=timings[math.ceil(.99*len(timings))-1], p95_budget_seconds=3,
            p95_budget_passed=p95<=3, functional_result_independent_of_budget=True,
            includes='Source write, compile admission/poll, compatible reload, full preservation/checkpoint reads and first verified Tick',
            excludes='Pre-edit checkpoint, neutral release, explicit collection and route repositioning',
            cache_scope='Same warm project/output/object paths; existing SDK/package caches',
            alpha_gate_closed=False)
        after_warm_collection = game.rpc('runtime.gameplay.collect', dict(session_id=game.session))
        need(after_warm_collection['active_modules'] == 1 and after_warm_collection['retired_alive'] == 0,
             'Repeated warm replacements stranded collectible modules')
        record['warm_managed_collection'] = after_warm_collection
        record['checks'].append('twenty_real_warm_edits_exact_preservation_and_first_verified_behavior')
        driver.follow_waypoints([(6,7),(-2,9)], 'return-to-original-cell-range')
        final_short = driver.aim_at(target)
        final_camera = game.entity(101); final_matrix = final_camera['world_matrix']
        final_long = game.rpc('runtime.raycast', dict(session_id=game.session, tick=game.tick,
            origin=contract.position(final_camera), direction=[-final_matrix[8],-final_matrix[9],-final_matrix[10]],
            distance=4, ignore=[uid(100)]))
        need(final_short['hit'] is None and final_long['hit'] is not None and
             final_long['hit']['entity'] == target and 3 < final_long['hit']['distance'] < 4,
             'Warm iteration did not return through real inputs to the original cell range')
        driver.step(use=True)
        need(driver.values()['Collected'] == 2 and driver.values()['CellTwo'] == uid(0),
             'New actual 4m compiled Use behavior did not collect the independently measured target')
        record['checks'] += ['real_engine_owned_A_B_builds', 'all_native_payload_and50_fields_preserved',
            'reload_retry_stale_and_incompatible_atomic', 'same_context_exact_reload_pixels',
            'independent_3_to4_meter_changed_behavior']

        # Authored visual edits stay outside this frozen game. No collider or
        # baked navigation geometry is moved; no live-update API is invented.
        old_authored = world.read_bytes(); old_runtime = snapshot()
        old_marker = game.entity(MARKER)
        old_image = capture('before-authored-edit')
        edit_transform = copy.deepcopy(marker_transform); edit_transform['position'][0] -= .25
        revision = game.rpc('world.inspect')['revision']
        edit = dict(request_id=fresh(), base_revision=revision, ops=[
            dict(op='component.set', id=uid(MARKER), type='Transform', value=edit_transform),
            dict(op='component.set', id=uid(MARKER), type='MeshRenderer',
                 value=dict(primitive='box', visible=True, albedo=[.03,.7,.05]))])
        committed = game.rpc('world.transact', edit)
        need(world.read_bytes() != old_authored and snapshot() == old_runtime,
             'Authored scene edit unexpectedly changed frozen simulation')
        edited = game.rpc('entity.get', dict(id=uid(MARKER), component='Transform'))
        need(edited['value'] == edit_transform and game.inspect()['source_stale'],
             'Authored edit was not visible separately as stale runtime content')
        stale_image = capture('after-authored-edit-frozen')
        need(stale_image['sha256'] == old_image['sha256'], 'Authored edit silently changed running pixels')
        need(game.rpc('world.transact', edit) == {**committed, 'replayed':True}, 'Authored retry changed result')
        record['authored_edit'] = dict(request=edit, receipt=committed, new_transform=edited,
                                      old_runtime_marker=old_marker)
        edited_bytes, assets = world.read_bytes(), inventory(Path(str(world)+'.assets'))
        record['checks'].append('authored_edit_is_stale_and_exact_running_pixels_unchanged')

        driver.drive_to(4.5,7,'third-cell'); driver.use_target(driver.values()['CellThree'])
        driver.wait_for_delivery()
        driver.follow_waypoints([(6,6),(6,1.5)],'terminal-approach')
        driver.use_target(600)
        need(driver.values()['Won'] == 1 and driver.values()['Arrivals'] == 1,
             'Real continued game did not complete after code and authored edits')
        record['snapshots']['completed'] = snapshot()
        record['checks'].append('real_reloaded_game_continues_pickups_navigation_animation_and_terminal_win')

        # Explicit stop/start adopts the authored edit, with fresh game state.
        old_session = game.session
        game.stop_player(); game.start(); game.load()
        need(game.session != old_session and not game.inspect()['source_stale'] and
             driver.values()['Collected'] == 0 and driver.values()['Won'] == 0 and
             driver.values()['Started'] == 0 and driver.values()['Initialized'] == 0,
             'Fresh authored start falsely retained old match state or old content')
        need(all(driver.values()[name] == uid(0) for name in contract.CELL_FIELDS),
             'Fresh Initialize unexpectedly retained the previous match handles')
        driver.step()
        fresh_marker = game.entity(MARKER)
        need(fresh_marker['local_transform'] == edit_transform,
             'Fresh runtime failed to adopt authored marker transform')
        game.start_player(); capture('fresh-authored-start')
        record['snapshots']['fresh_start'] = snapshot()
        record['checks'].append('explicit_stop_start_adopts_scene_and_resets_match')

        # Exact old A save is deliberately restored with original A, not B.
        # The current authored scene remains edited and therefore stale.
        game.assembly = args.output/'compiled/a/managed/Poima.RelayYardGame.dll'
        restored = game.restore('iteration-a'); game.paused()
        need(game.inspect()['source_stale'] and world.read_bytes() == edited_bytes and
             driver.values() == payload_a['gameplay']['values'] and
             contract.normalized(game.entity(MARKER)) == before['marker'],
             'Exact original-image restore rewrote authoring or lost frozen original content/state')
        again, _ = save('iteration-a-restored')
        need(again == payload_a, 'Exact old-image restored native payload is not identical')
        record['exact_original_save_restore'] = restored
        record['checks'].append('original_image_save_restores_original_frozen_scene_without_implicit_conversion')
        need(world.read_bytes() == edited_bytes and inventory(Path(str(world)+'.assets')) == assets,
             'Iteration changed cooked assets or rewrote the final authored edit')
        record['final_world_sha256'] = sha(world)
        need(inventory(args.output/'source') == staged_pin, 'Compiler or verifier mutated staged game source')
        need(all(inventory(args.output/item['retained_image_directory']) == item['managed_inventory']
                 for item in record['jobs'].values() if 'retained_image_directory' in item),
             'A retained original compiled image/closure was modified')
        need(all(inventory(args.output/item['retained_source_directory']) == item['source_inventory']
                 for item in record['jobs'].values()), 'An at-run compiler source closure was modified')
        complete = True
    except BaseException:
        record['error'] = traceback.format_exc()
        raise
    finally:
        # Cancel accepted nonterminal jobs before asking the owner to close.
        # Root's exact Windows Job supervisor remains the emergency tree owner.
        # Cleanup receives a separate bounded allowance after workload expiry;
        # it must still attempt normal shutdown rather than failing every send.
        DEADLINE = max(DEADLINE, time.monotonic()+45)
        if client is not None and host is not None and host.process.poll() is None:
            for job in jobs:
                try:
                    result = client.call('development.inspect', dict(job_id=job))
                    if result['state'] in ('queued','running'):
                        client.call('development.cancel', dict(job_id=job))
                except BaseException:
                    record['cleanup_errors'].append(traceback.format_exc())
            try:
                if game is not None:
                    game.stop_player()
                else:
                    state = client.call('player.inspect')
                    need(not state['active'], 'No registered owner exists for an active player')
                state = client.call('runtime.status')
                if state['active']: client.call('runtime.stop', dict(session_id=state['session_id']))
                client.call('host.shutdown')
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
        for owned in (observer, client, host):
            if owned is not None:
                try:
                    owned.close(expected=True)
                    need(all(not thread.is_alive() for thread in owned.threads),
                         'Owned transport log readers did not drain within their bound')
                    expected_stderr = 'Poima shared world ready: '+owned.marker+'\n' if owned.role == 'serve' else ''
                    need(owned.record['stderr'] == expected_stderr and not owned.record['stderr_truncated'],
                         'Owned transport stderr is unexpected or incomplete')
                except BaseException: record['cleanup_errors'].append(traceback.format_exc())
        try:
            record['inputs_unchanged'] = all(Path(p).is_file() and sha(p) == value
                                           for p,value in {**immutable, **originals}.items())
            record['audio_unchanged'] = inventory(args.audio_directory) == audio_pin
        except BaseException:
            record['input_check_error'] = traceback.format_exc()
            record['inputs_unchanged'] = False
        record['passed'] = bool(complete and not record['cleanup_errors'] and
            record['inputs_unchanged'] and record.get('audio_unchanged') and
            all(row.get('exit_code') == 0 and row.get('stderr_truncated') is False
                for row in record['owners']))
        persist()
    need(record['passed'], 'Iteration failed or owned cleanup/input preservation failed; evidence retained')
    print(json.dumps(dict(passed=True, checks=record['checks'], elapsed_seconds=record['elapsed_seconds'])))


if __name__ == '__main__':
    main()
