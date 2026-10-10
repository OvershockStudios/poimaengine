#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Qualify a source-free compiled game through its shared native service.

Requires an existing Windows renderer runtime and genuine settings NativeAOT
fixture. Builds nothing and sends no OS keyboard/mouse messages. The exported
game owns one read-only WorldSession, native Runtime and player preference owner;
two thin WorldClient connections exercise the same public player/UI operations.

Example (native Windows Python):
  python tests/game_service_player.py --binary PATH/poima.exe \
    --runtime PATH/runtime --artifact PATH/native-gameplay.json \
    --output NEW/qualification --gpu 1

Adding this verifier is not an execution or qualification claim.
"""
import argparse
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
import uuid

import player_preferences_gameplay_bundle as bundle_fixture
import player_preferences_gameplay_contract as fixture
from collection_gameplay_bundle import check, managed_pe, read_json, sha

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient
from poima_client.errors import RpcError

FEATURES = bundle_fixture.FEATURES
WIDTH, HEIGHT = bundle_fixture.WIDTH, bundle_fixture.HEIGHT
inventory = bundle_fixture.clean_inventory
relative = bundle_fixture.relative_path
numeric = bundle_fixture.numeric_values
near = bundle_fixture.near


def fresh():
    return uuid.uuid4().hex


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'runtime', 'artifact', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--gpu', type=int, default=1)
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    check(os.name == 'nt' and not sys.flags.optimize,
          'Use native Windows Python without optimization')
    check(0 <= args.gpu <= 4095 and math.isfinite(args.timeout) and 30 <= args.timeout <= 1800,
          'GPU must be 0..4095 and timeout 30..1800 seconds')
    binary, distribution, artifact = (path.resolve(strict=True) for path in
                                     (args.binary, args.runtime, args.artifact))
    check(binary.is_file() and distribution.is_dir() and artifact.is_file() and
          artifact.name == 'native-gameplay.json', 'Supply exporter, installed runtime and NativeAOT descriptor')
    output = args.output.resolve()
    check(not output.exists() and not output.is_relative_to(distribution) and
          not output.is_relative_to(artifact.parent), 'Output must be new and outside supplied closures')
    output.mkdir(parents=True)
    logs = output/'logs'; logs.mkdir()
    started = time.monotonic(); deadline = started+args.timeout
    record = dict(passed=False, runner_sha256=sha(__file__), binary_sha256=sha(binary),
        artifact_sha256=sha(artifact), gpu=args.gpu, commands=[], calls=[], transports=[], checks=[],
        cleanup_errors=[], input_source='Guarded native runtime.ui.activate requests through shared endpoint',
        physical_input_qualified=False, limitations=[
            'One retained genuine NativeAOT settings fixture and selected GPU; no broad Alpha or performance claim.',
            'Guarded semantic UI activation, not physical pointer hit testing or OS screenshot qualification.',
            'Audio disabled: master gain is configured but audible output and actual sink gain are not qualified.',
            'A source-free relocation and sanitized environment are not a clean-machine deployment test.',
            'One same-process save/load replacement, not fresh-process save continuation or preference persistence.',
            'Only owned source is deleted; relocated bundle, saves and diagnostic evidence are retained.'])
    host = host_entry = author = client = observer = None
    host_streams = []
    installed = unrelated = environment = None
    runtime_pin = artifact_pin = None
    current_session = player_id = None

    def remaining():
        duration = deadline-time.monotonic()
        check(duration > 0, 'Overall exported service qualification deadline exceeded')
        return duration

    def begin(command, cwd, env=None):
        remaining()
        index = len(record['commands'])
        out, err = logs/f'command-{index}.stdout', logs/f'command-{index}.stderr'
        row = dict(command=list(map(str, command)), cwd=str(cwd), stdout=str(out), stderr=str(err),
                   exit_code=None, timed_out=False, forced_cleanup=False)
        record['commands'].append(row)
        streams = [out.open('wb'), err.open('wb')]
        try:
            child = subprocess.Popen(row['command'], cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                     stdout=streams[0], stderr=streams[1])
            row['pid'] = child.pid
            return child, row, streams
        except BaseException:
            for stream in streams:
                stream.close()
            raise

    def cleanup_owned(child, row):
        if child.poll() is None:
            row['forced_cleanup'] = True
            try:
                killer = Path(os.environ['SYSTEMROOT'])/'System32/taskkill.exe'
                killed = subprocess.run([str(killer), '/PID', str(child.pid), '/T', '/F'],
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
                row['tree_cleanup_exit_code'] = killed.returncode
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=10)
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
        row['exit_code'] = child.poll()

    def cli(executable, *arguments, cwd=None, env=None):
        child, row, streams = begin([executable, *arguments], cwd or output, env)
        try:
            try:
                child.wait(timeout=min(120, remaining()))
            except subprocess.TimeoutExpired:
                row['timed_out'] = True
                raise
        finally:
            cleanup_owned(child, row)
            for stream in streams:
                stream.close()
        check(row['exit_code'] == 0 and not row['forced_cleanup'], 'Native CLI failed; inspect command logs')
        result = read_json(row['stdout'])
        check(result.get('status') == 'ok' and 'result' in result, 'Unsuccessful native CLI envelope')
        row['result'] = result['result']
        return row['result']

    def call(connection, method, params=None, role='driver'):
        value = connection.call(method, params, timeout=min(30, remaining()))
        record['calls'].append(dict(role=role, method=method, params=params, result=value))
        return value

    def reject(connection, method, params, code):
        try:
            call(connection, method, params)
        except RpcError as error:
            record['calls'].append(dict(role='driver', method=method, params=params,
                                        error=dict(code=error.code, message=error.message)))
            check(error.code == code, 'Unexpected rejection code for '+method+': '+str(error.code))
            return
        raise AssertionError('Expected native rejection for '+method)

    def close_connection(connection, label):
        connection.close()
        transport = connection.transport
        value = dict(role=label, process_id=transport.process_id, exit_code=transport.returncode,
                     stderr=transport.stderr_tail, stderr_truncated=transport.stderr_truncated)
        record['transports'].append(value)
        check(value['exit_code'] == 0 and not value['stderr'] and not value['stderr_truncated'],
              'Owned '+label+' transport did not exit cleanly')

    def connect(label):
        # WorldClient deliberately has no implicit environment mutation/retries.
        # Scope the inherited environment to this explicit child launch only;
        # transport reader/writer threads never launch other processes.
        inherited = dict(os.environ)
        try:
            os.environ.clear(); os.environ.update(environment)
            attached = WorldClient.connect(installed, endpoint, cwd=unrelated, close_timeout=10,
                timeout_ms=min(30000, max(100, int(remaining()*1000))))
        finally:
            os.environ.clear(); os.environ.update(inherited)
        record.setdefault('connections', []).append(dict(role=label,
            process_id=attached.transport.process_id, endpoint=endpoint,
            inherited_environment='System-only PATH; DOTNET_, COREHOST_ and POIMA_ removed'))
        return attached

    def runtime():
        return call(client, 'runtime.inspect', dict(session_id=current_session))

    def gameplay():
        return call(client, 'runtime.gameplay.inspect', dict(session_id=current_session, include_schema=True))

    def preferences(connection=None):
        return call(connection or client, 'player.settings.inspect', dict(player_id=player_id))

    def stable_state():
        # Application observations can advance asynchronously as the renderer
        # catches up. Compare the actual owner revision/value authority instead.
        prefs = preferences()
        return dict(runtime=runtime(), gameplay=gameplay(), player_id=prefs['player_id'],
                    settings_revision=prefs['settings']['revision'], settings_values=prefs['settings']['values'])

    def presented(revision, label):
        observations = []
        until = time.monotonic()+min(30, remaining())
        while time.monotonic() < until:
            check(host.poll() is None, 'Owned game service exited while observing '+label)
            state = call(observer, 'player.inspect', role='observer')
            observations.append(state)
            application = (state.get('report') or {}).get('live_settings', {}).get('application', {})
            if state['ready'] and application.get('presented_revision') == revision:
                prefs = preferences(observer)
                check(prefs['player_id'] == player_id and state['player_id'] == player_id and
                      prefs['session_id'] == state['session_id'] == current_session and
                      state['paused'] and state['tick'] == 0 and state['report']['hardware'] and
                      state['report']['nvrhi_errors'] == 0, 'Presentation owner/runtime mismatch at '+label)
                check(all(prefs['settings']['application'][name] == revision for name in
                    ('observed_revision', 'applied_revision', 'presented_revision')),
                    'Incomplete actual application observation at '+label)
                record.setdefault('presentations', {})[label] = dict(player=state, preferences=prefs,
                                                                    observations=len(observations))
                return state
            time.sleep(min(.01, remaining()))
        raise AssertionError('Native presentation did not reach '+label+': '+repr(observations[-2:]))

    def activate(action):
        nonlocal current_session
        state, game = runtime(), gameplay()
        identity = fixture.MENU_IDS.get(action, fixture.IDS.get(action))
        check(identity is not None, 'Unknown fixture action '+action)
        params = dict(session_id=current_session, request_id=fresh(), expected_tick=state['tick'],
            expected_ui_revision=state['ui_revision'], expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=game['revision'], expected_structure_revision=state['structure_revision'],
            id=identity)
        result = call(client, 'runtime.ui.activate', params)
        current_session = result['current_session_id']
        record.setdefault('menu_actions', []).append(dict(action=action, params=params, result=result))
        return params, result

    def capture(name, targets):
        state = call(observer, 'player.inspect', role='observer')
        path = output/(name+'.bmp')
        request = dict(player_id=player_id, request_id=fresh(), expected_control_revision=state['control_revision'],
            session_id=current_session, tick=state['tick'], expected_structure_revision=state['structure_revision'],
            expected_ui_revision=state['ui_revision'], path=str(path))
        result = call(client, 'player.capture', request)
        report = result['capture']
        check(report['capture_written'] and report['hardware'] and report['samples'] == 1 and
              report['nvrhi_errors'] == 0 and result['session_id'] == current_session and
              result['tick'] == state['tick'] == 0, 'Actual same-runtime Vulkan capture failed at '+name)
        oracle = bundle_fixture.capture_oracle(path, targets)
        check(runtime()['tick'] == 0, 'Native capture advanced the paused runtime')
        record.setdefault('captures', {})[name] = dict(result=result, oracle=oracle)
        return request, result

    try:
        descriptor = read_json(artifact)
        check(descriptor['format'] == 'poima.native-gameplay' and descriptor['version'] == 2 and
              descriptor['engine_version'] == '0.0.81' and descriptor['type'] == fixture.GAME and
              descriptor['identity'] == fixture.IDENTITY and descriptor['target_os'] == 'Windows' and
              descriptor['target_arch'] == 'x86_64', 'Supply the retained genuine .81 Windows settings fixture')
        check((descriptor['call_version'], descriptor['call_bytes'], descriptor['services_version'],
               descriptor['minimum_services_bytes']) == (1, 80, 7, 256) and
              set(descriptor['required_features']) == FEATURES and
              len(descriptor['required_features']) == len(FEATURES) and
              not descriptor['schema'].get('components'), 'Retained fixture ABI/features/schema differ')
        rows = descriptor['files']; seen = set()
        for row in rows:
            path = relative(row['path']); key = path.as_posix().casefold()
            check(key not in seen and key != 'native-gameplay.json', 'Duplicate/reserved artifact path')
            seen.add(key)
            payload = artifact.parent/path
            check(payload.resolve(strict=True).is_relative_to(artifact.parent) and payload.is_file() and
                  payload.stat().st_size == row['size'] and sha(payload) == row['sha256'],
                  'Supplied NativeAOT artifact integrity differs')
        libraries = [row for row in rows if row['role'] == 'library']
        check(len(libraries) == 1 and libraries[0]['path'] == descriptor['library'] and
              sum(row['role'] == 'notice' for row in rows) >= 6, 'Retained native image/notice closure differs')
        artifact_pin = inventory(artifact.parent)
        check(set(artifact_pin) == {artifact.name, *(row['path'] for row in rows)},
              'Published artifact directory must be the exact source-free closure')
        installed_runtime = read_json(distribution/'runtime.json')
        check(installed_runtime['target_os'] == 'Windows' and installed_runtime['target_arch'] == 'x86_64' and
              installed_runtime['gameplay_call_version'] == 1 and installed_runtime['gameplay_call_bytes'] == 80 and
              installed_runtime['gameplay_services_version'] == 7 and
              installed_runtime['gameplay_services_bytes'] >= 256 and
              FEATURES <= set(installed_runtime['gameplay_features']) and
              all(installed_runtime['features'].get(name) is True for name in
                  ('simulation', 'renderer', 'native_gameplay', 'game_ui')),
              'Installed runtime lacks the actual compiled player contract')
        check(cli(binary, 'version')['version'] == installed_runtime['engine_version'],
              'Exporter and installed runtime versions differ')
        runtime_pin = inventory(distribution)
        source_paths = [Path(__file__), Path(bundle_fixture.__file__), Path(fixture.__file__),
            ROOT/'tests/player_live_settings_contract.py', ROOT/'tests/player_service_contract.py',
            ROOT/'tests/collection_gameplay_bundle.py',
            ROOT/'tests/managed_player_preferences_gameplay/PlayerPreferencesMenuGame.cs',
            ROOT/'tests/managed_player_preferences_gameplay/Poima.PlayerPreferencesMenuGame.csproj',
            *sorted((ROOT/'tools/python/poima_client').glob('*.py'))]
        record['fixture_sources'] = {str(path.relative_to(ROOT)):sha(path) for path in source_paths}
        record['runtime_descriptor'] = installed_runtime
        record['artifact_descriptor'] = descriptor
        record['artifact_lineage'] = dict(engine_version=descriptor['engine_version'], type=descriptor['type'],
            image_sha256=libraries[0]['sha256'], descriptor_sha256=sha(artifact),
            running_engine_version=installed_runtime['engine_version'])
        project = output/'Owned compiled service source'
        cli(binary, 'project', 'create', project, '--name', 'Source-free compiled game service acceptance')
        project_file = project/'project.json'; spec = read_json(project_file)
        world_file = project/'compiled-service-world.json'
        author = WorldClient.open(binary, world_file, cwd=project, close_timeout=10)
        class Author:
            def call(self, method, params=None):
                return call(author, method, params, role='author')
        targets = bundle_fixture.author(Author())
        call(author, 'session.close', role='author')
        close_connection(author, 'author'); author = None
        authored_world = read_json(world_file)
        artifact_dir = project/'gameplay'; artifact_dir.mkdir()
        for name in artifact_pin:
            destination = artifact_dir/relative(name)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(artifact.parent/name, destination)
        spec.update(version=2, audio=False, entry=dict(world=world_file.relative_to(project).as_posix(),
                    controller=fixture.uid(100), camera=fixture.uid(101)),
                    gameplay=dict(descriptor='gameplay/native-gameplay.json', values=dict(Mode=0)))
        project_file.write_text(json.dumps(spec, indent=2)+'\n', encoding='utf-8')
        source_pin = inventory(project)
        cli(binary, 'project', 'inspect', project_file)
        exported = output/'Exported compiled service game'
        cli(binary, 'project', 'build', project_file, '--runtime', distribution, '--output', exported)
        check(inventory(project) == source_pin and inventory(distribution) == runtime_pin,
              'Native inspection/export mutated owned source or selected runtime')
        relocation = Path(tempfile.mkdtemp(prefix='Poima game service relocation ', dir=ROOT.parent)).resolve()
        check(not relocation.is_relative_to(ROOT), 'Relocation must be outside the checkout')
        record['relocation_root'] = str(relocation)
        bundle = relocation/'Relocated compiled service game'; shutil.move(str(exported), bundle)
        shutil.rmtree(project)
        check(not project.exists() and not exported.exists(), 'Owned source/export locations remain available')
        record['source_removed_before_service'] = True
        game_file = bundle/'game.json'; game = read_json(game_file)
        check(game['version'] == 2 and game['gameplay']['values'] == dict(Mode=0), 'Exported initial values differ')
        check(read_json(bundle/'content/world.json') == bundle_fixture.shipping_document(authored_world),
              'Exported read-only world differs from the native projection')
        frozen = inventory(bundle)
        check({name[8:]:digest for name,digest in frozen.items() if name.startswith('runtime/')} == runtime_pin and
              {name[9:]:digest for name,digest in frozen.items() if name.startswith('gameplay/')} == artifact_pin,
              'Relocated runtime or retained NativeAOT closure differs')
        forbidden = {'hostfxr.dll', 'coreclr.dll', 'poima.gameplay.dll', 'poima.managedbridge.dll',
                     'poima.playerpreferencesmenugame.dll', 'libhostfxr.so', 'libcoreclr.so'}
        check(not any(Path(name).name.casefold() in forbidden or managed_pe(bundle/name) or
                      Path(name).suffix.casefold() in {'.cs', '.csproj', '.pdb', '.sln'} for name in frozen),
              'Development source/debug/CoreCLR/managed PE shipped')
        installed = (bundle/relative(game['engine']['executable'])).resolve(strict=True)
        check(installed.is_relative_to(bundle), 'Bundled executable escapes immutable root')
        unrelated = relocation/'Unrelated working directory'; unrelated.mkdir()
        environment = dict(os.environ); system = Path(environment['SYSTEMROOT'])
        environment['PATH'] = str(system/'System32')+';'+str(system)
        for key in list(environment):
            if key.upper().startswith(('DOTNET_', 'COREHOST_', 'POIMA_')):
                environment.pop(key)
        inspected = cli(installed, 'game', 'inspect', game_file, cwd=unrelated, env=environment)
        check(inspected.get('input_profile') == game.get('input_profile') and game.get('input_profile'),
              'game inspect did not expose the validated relative input profile')
        profile = (bundle/relative(inspected['input_profile'])).resolve(strict=True)
        check(profile.is_relative_to(bundle) and profile.is_file(), 'Inspected relative input profile escapes bundle')
        storage = output/'External compiled service saves'; storage.mkdir()
        endpoint = 'poima_settings_'+fresh()
        host, host_entry, host_streams = begin([installed, 'game', 'serve', game_file,
            '--endpoint', endpoint, '--save-root', storage], unrelated, environment)
        client = connect('driver'); observer = connect('observer')
        discovery = call(client, 'world.describe', dict(view='catalog'))
        check(discovery['read_only'] and discovery['mode'] == 'read_only_runtime',
              'Exported game service is not authoritative read-only runtime scope')
        reject(client, 'world.transact', dict(base_revision=authored_world['revision'], request_id=fresh(), ops=[]), -32081)
        state = call(client, 'runtime.status')
        check(state['active'] and state['tick'] == 0, 'Game service did not start declared runtime at tick zero')
        current_session = initial_session = state['session_id']
        absent = call(client, 'player.inspect')
        check(not absent['active'] and not absent['ready'] and absent['player_id'] is None and
              absent['state'] == 'absent' and absent['generation'] == 0,
              'Serving game initialized a window before explicit player.start')
        module = gameplay()['module']
        check(module['backend'] == 'native_aot' and module['type'] == fixture.GAME and
              module['schema'] == descriptor['schema'] and module['assembly_sha256'] == libraries[0]['sha256'] and
              module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False) and
              numeric(module['values'], descriptor['schema'])['Mode'] == 0,
              'Service did not automatically load the supplied genuine NativeAOT artifact/values')
        parameters = dict(session_id=current_session, request_id=fresh(), expected_tick=0,
            expected_generation=0, camera=game['entry']['camera'], controller=game['entry']['controller'],
            mode='interactive', initially_paused=True, gamepad=dict(mode='disabled'),
            audio=False, width=WIDTH, height=HEIGHT, gpu=args.gpu, samples=1, frames_in_flight=1,
            input_profile=str(profile), settings_overrides={'ui.scale':1, 'camera.vertical_fov':60, 'audio.master_gain':1})
        acknowledgment = call(client, 'player.start', parameters)
        player_id = acknowledgment['player_id']
        check(acknowledgment['active'] and acknowledgment['tick'] == 0, 'Native player did not attach to exported runtime')
        presented(0, 'initial')
        original_preferences = preferences()
        activate('pref.open')
        retry_params, retry_receipt = activate('pref.ui.plus')
        presented(1, 'ui-scale')
        before = stable_state()
        check(call(observer, 'runtime.ui.activate', retry_params, role='observer') == {**retry_receipt, 'replayed':True},
              'Exact compiled Control retry did not recover its original receipt')
        check(stable_state() == before, 'Exact compiled retry ran gameplay or republished preferences')
        reject(client, 'player.settings.transact', dict(player_id=player_id, request_id=fresh(),
            expected_control_revision=original_preferences['control_revision'],
            expected_settings_revision=original_preferences['settings']['revision'], set={'ui.scale':2}), -32009)
        check(stable_state() == before, 'Stale settings edit changed the authoritative compiled game/owner')
        activate('pref.fov.plus'); activate('pref.gain.minus'); activate('pref.refresh')
        presented(3, 'before-save')
        capture('before-save', targets)
        activate('pref.stage.save')
        presented(4, 'saved')
        manifest_path = storage/('slot-'+fixture.SLOT)/'current.json'
        check(manifest_path.is_file(), 'Actual compiled Save callback produced no durable witness')
        manifest = read_json(manifest_path); selected = manifest['payload']['current']
        check(manifest['format'] == 'poima.save-slot' and manifest['version'] == 1 and
              manifest['payload']['generation'] == selected['generation'] == 1,
              'Compiled Save did not publish exactly generation one')
        payload_name = relative(selected['file'])
        check(len(payload_name.parts) == 1, 'Save witness payload must be a basename')
        payload_file = manifest_path.parent/payload_name
        check(payload_file.stat().st_size == selected['bytes'] and sha(payload_file) == selected['sha256'],
              'Durable compiled save integrity differs')
        saved = read_json(payload_file)['snapshot']['payload']
        saved_values = numeric(saved['gameplay']['values'], descriptor['schema'])
        for name, expected in dict(Mode=0, Available=1, Replay=0, Controls=6, Revision=3,
            TicketSequence=4, StageRejection=0, ResultState=1, ResultRejection=0, AcceptedRevision=-1,
            ReadAfterRevision=3, ReadUnchanged=1, FovPresent=1, UiPresent=1).items():
            check(saved_values[name] == expected, 'Saved actual compiled Control witness differs: '+name)
        for name, expected in dict(Fov=65, UiScale=1.25, Gain=.9).items():
            near(saved_values[name], expected, 'Saved '+name)
        owner = (saved_values['OwnerHigh'], saved_values['OwnerLow'])
        check(owner != (0, 0) and (saved_values['TicketHigh'], saved_values['TicketLow']) == owner and
              saved_values['SaveSequence'] == 1 and (saved_values['SaveHigh'], saved_values['SaveLow']) != (0,0) and
              saved_values['Ticks'] == saved['tick'] == 0, 'Durable owner/ticket or paused tick differs')
        check(not any(key in saved for key in ('preferences','player_preferences','live_settings','settings')),
              'Runtime save imported native preference authority')
        save_pin = inventory(storage)
        record['saved_witness'] = dict(manifest_sha256=sha(manifest_path), payload_sha256=sha(payload_file),
                                      tick=saved['tick'], values=saved_values)
        before = stable_state()
        close_connection(client, 'driver-detach'); client = None
        client = connect('driver-reconnect')
        check(stable_state() == before, 'Detaching/reconnecting replaced the authoritative native game or owner')
        activate('pref.fov.plus'); activate('pref.refresh')
        presented(5, 'before-load')
        capture('before-load', targets)
        _, replacement = activate('pref.stage.load')
        check(replacement['runtime_replaced'] and replacement['save_serviced'] and current_session != initial_session,
              'Compiled Load did not replace the actual exported service Runtime')
        presented(6, 'restored')
        activate('pref.refresh')
        presented(6, 'restored-refresh')
        capture('after-load', targets)
        capture_hashes = [record['captures'][name]['oracle']['sha256']
                          for name in ('before-save', 'before-load', 'after-load')]
        check(len(set(capture_hashes)) == 3,
              'Compiled menu status/settings/restore did not change the actual rendered observations')
        module = gameplay()['module']; values = numeric(module['values'], descriptor['schema'])
        check(module['backend'] == 'native_aot' and module['type'] == fixture.GAME and
              module['schema'] == descriptor['schema'] and module['assembly_sha256'] == libraries[0]['sha256'] and
              module['native_diagnostics'] == dict(dynamic_code_supported=False, dynamic_code_compiled=False),
              'Restored gameplay is not the original genuine NativeAOT fixture')
        for name, expected in dict(Mode=0, Available=1, Replay=0, Controls=7, Revision=6,
            TicketSequence=4, StageRejection=0, ResultState=2, ResultRejection=0, AcceptedRevision=4,
            ReadAfterRevision=3, ReadUnchanged=1, FovPresent=1, UiPresent=1,
            EffectiveFovPresent=1, EffectiveUiPresent=1, ObservedRevision=6,
            AppliedRevision=6, PresentedRevision=6).items():
            check(values[name] == expected, 'Actual restored/queried compiled state differs: '+name)
        check((values['OwnerHigh'], values['OwnerLow']) == owner and
              (values['TicketHigh'], values['TicketLow']) == owner, 'Runtime load changed native owner or retained ticket')
        for name, expected in dict(Fov=80, UiScale=1.75, Gain=.7, EffectiveFov=80, EffectiveUi=1.75).items():
            near(values[name], expected, 'Compiled final '+name)
        prefs = preferences(); settings = prefs['settings']; application = settings['application']
        check(settings['revision'] == 6 and prefs['player_id'] == player_id and
              prefs['session_id'] == current_session, 'Final current native owner revision/identity differs')
        for name, expected in {'camera.vertical_fov':80, 'ui.scale':1.75, 'audio.master_gain':.7}.items():
            near(settings['values'][name], expected, 'Native configured '+name)
        for name, expected in dict(effective_vertical_fov=80, effective_ui_scale=1.75, requested_master_gain=.7).items():
            near(application[name], expected, 'Native actual '+name)
        check(application['audio_outcome'] == 'disabled' and application['sink_gain'] is None,
              'Audio-disabled export invented a verified sink gain')
        restore_status = call(client, 'runtime.save.status', dict(session_id=current_session))
        check(restore_status['last_restore']['restored_tick'] == restore_status['last_restore']['source_tick'] == 0 and
              restore_status['last_restore']['generation'] == 1 and not restore_status['last_restore']['recovered'],
              'Actual native restore ledger differs')
        check(inventory(storage) == save_pin, 'Loading/current settings changed the durable save witness')
        check(inventory(bundle) == frozen and inventory(distribution) == runtime_pin and
              inventory(artifact.parent) == artifact_pin, 'Service/player mutated immutable supplied closures')
        check(all(sha(ROOT/name) == digest for name,digest in record['fixture_sources'].items()),
              'Verifier fixture sources changed during qualification')
        record['bundle_inventory'] = frozen
        final_player = call(observer, 'player.inspect', role='observer')
        check(final_player['report']['runtime_replacements'] == 1 and
              final_player['report']['initial_session_id'] == initial_session and
              final_player['report']['current_session_id'] == current_session,
              'Actual player lifetime did not observe exactly one exported Runtime replacement')
        record['final'] = dict(runtime=runtime(), gameplay=gameplay(), preferences=prefs,
                              save=restore_status, player=final_player)
        state = call(client, 'player.inspect')
        stopped = call(client, 'player.control', dict(player_id=player_id, request_id=fresh(),
                        expected_control_revision=state['control_revision'], action='stop'))
        check(not stopped['active'], 'Player stop did not release owned native window')
        call(client, 'runtime.stop', dict(session_id=current_session))
        check(not call(observer, 'runtime.status', role='observer')['active'], 'Runtime stop did not release exported simulation')
        call(client, 'host.shutdown')
        close_connection(client, 'driver-final'); client = None
        close_connection(observer, 'observer-final'); observer = None
        try:
            host.wait(timeout=min(30, remaining()))
        except subprocess.TimeoutExpired:
            host_entry['timed_out'] = True
            raise
        cleanup_owned(host, host_entry)
        for stream in host_streams:
            stream.close()
        host_streams = []
        check(host_entry['exit_code'] == 0 and not host_entry['forced_cleanup'], 'Game service did not exit cleanly')
        diagnostic = Path(host_entry['stderr']).read_text(encoding='utf-8')
        check(diagnostic == 'Poima shared world ready: '+endpoint+'\n',
              'Native game service stderr differs from its exact endpoint readiness line')
        check(inventory(bundle) == frozen and inventory(distribution) == runtime_pin and
              inventory(artifact.parent) == artifact_pin and inventory(storage) == save_pin and
              sha(binary) == record['binary_sha256'], 'Shutdown mutated immutable inputs or durable save')
        record['checks'] = [
            'Source-free immutable relocation serves one read-only world and automatically loaded genuine NativeAOT Runtime at tick0 without opening a window.',
            'Explicit existing player.start presents the exported Runtime through the same native authority; no OS input emulation or second preference store.',
            'Guarded compiled Control retry and stale settings rejection preserve exact authoritative state; explicit client reconnect retains the game/owner.',
            'Compiled Save stages ticket4 at observed revision3; later Load commits current settings revision6 before replacing Runtime and preserves same owner/ticket4 accepted revision4.',
            'Three actual native Vulkan captures contain the visible authored settings menu; final compiled/configured/applied/presented observations agree at FOV80/UI1.75/gain0.7.',
            'Paused tick0, immutable bundle/supplied closures/durable save and explicit player/runtime/host/transport shutdown verified.']
        record['passed'] = True
    except BaseException:
        record['error'] = traceback.format_exc()
    finally:
        # Graceful shutdown is attempted only through the already-owned endpoint.
        # A failed normal path never turns green because cleanup succeeded.
        if host is not None and host.poll() is None and client is not None and not client.closed:
            try:
                state = client.call('player.inspect', timeout=5)
                if state['active']:
                    client.call('player.control', dict(player_id=state['player_id'], request_id=fresh(),
                        expected_control_revision=state['control_revision'], action='stop'), timeout=5)
                state = client.call('runtime.status', timeout=5)
                if state['active']:
                    client.call('runtime.stop', dict(session_id=state['session_id']), timeout=5)
                client.call('host.shutdown', timeout=5)
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
        for connection, label in ((author,'author-cleanup'), (client,'driver-cleanup'), (observer,'observer-cleanup')):
            if connection is not None:
                try:
                    close_connection(connection, label)
                except BaseException:
                    record['cleanup_errors'].append(traceback.format_exc())
        if host is not None and host_entry is not None:
            if host.poll() is None:
                try:
                    host.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
            cleanup_owned(host, host_entry)
        for stream in host_streams:
            stream.close()
        if record['cleanup_errors'] or any(row['forced_cleanup'] or row['exit_code'] != 0 for row in record['commands']):
            record['passed'] = False
        record['elapsed_seconds'] = time.monotonic()-started
        (output/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(dict(passed=record['passed'], checks=len(record['checks']), evidence=str(output/'evidence.json'))))
    if not record['passed']:
        raise SystemExit('Compiled game service qualification failed; see evidence.json')


if __name__ == '__main__':
    main()
