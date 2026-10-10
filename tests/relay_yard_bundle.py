#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Export Relay Yard, restore its checkpoint in a fresh native game process.

Uses an existing genuine Windows NativeAOT artifact and installed runtime.
Builds/downloads nothing; drives the actual immutable game's native controller,
raycasts, compiled menus and navigation without teleports or gameplay patches.
The authored source copy is removed before either exported service starts.
"""
import argparse
import copy
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import uuid

from collection_gameplay_bundle import check, managed_pe, read_json, sha
from player_preferences_gameplay_bundle import clean_inventory, relative_path
import relay_yard_contract as contract

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient
from poima_client.errors import RpcError

REQUIRED = {'baseline_v7', 'component_collections_v1', 'animation_inertial_v1',
            'character_input_v1', 'navigation_query_v1', 'player_preferences_v1'}
WIDTH, HEIGHT = 1280, 720
MAX_EVIDENCE = 128*1024*1024


def fresh():
    return uuid.uuid4().hex


def launcher_module():
    path = ROOT/'examples/relay-yard/run.py'
    spec = importlib.util.spec_from_file_location('poima_relay_bundle_launcher', path)
    check(spec is not None and spec.loader is not None, 'Relay Yard launcher is missing')
    launcher = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(launcher)
    return launcher


def shipping_document(authored):
    result = copy.deepcopy(authored)
    for name in ('receipts', 'retired_ids', 'retired_component_schemas',
                 'retired_template_ids', 'retired_ui_ids'):
        if name in result:
            result[name] = []
    return result


def artifact_inputs(path, launcher):
    descriptor = read_json(path)
    check(descriptor['format'] == 'poima.native-gameplay' and descriptor['version'] == 2 and
          descriptor['type'] == launcher.TYPE and descriptor['identity'] == 'poima.examples.relay-yard' and
          descriptor['target_os'] == 'Windows' and descriptor['target_arch'] == 'x86_64',
          'Supply the genuine Windows Relay Yard artifact')
    check((descriptor['call_version'], descriptor['call_bytes'], descriptor['services_version'],
           descriptor['minimum_services_bytes']) == (1, 80, 7, 256) and
          set(descriptor['required_features']) == REQUIRED and
          len(descriptor['required_features']) == len(REQUIRED), 'Relay Yard ABI/features differ')
    names = set()
    rows = descriptor['files']
    check(1 <= len(rows) <= 256, 'Artifact file count exceeds its bound')
    for row in rows:
        relative = relative_path(row['path'])
        key = relative.as_posix().casefold()
        check(key not in names and key != 'native-gameplay.json', 'Duplicate/reserved artifact path')
        names.add(key)
        payload = path.parent/relative
        check(payload.resolve(strict=True).is_relative_to(path.parent) and payload.is_file() and
              payload.stat().st_size == row['size'] and sha(payload) == row['sha256'],
              'Actual artifact integrity differs')
    frozen = clean_inventory(path.parent)
    check(set(frozen) == {'native-gameplay.json', *(row['path'] for row in rows)},
          'Published artifact directory is not its exact closure')
    libraries = [row for row in rows if row['role'] == 'library']
    metadata = [row for row in rows if row['role'] == 'metadata']
    notices = [row for row in rows if row['role'] == 'notice']
    check(len(libraries) == len(metadata) == 1 and libraries[0]['path'] == descriptor['library'] and
          len(notices) >= 6 and not managed_pe(path.parent/descriptor['library']),
          'Actual native image/component manifest/notices are incomplete')
    notice_names = {Path(row['path']).name for row in notices}
    check({'POIMA-LICENSE.txt', 'POIMA-THIRD-PARTY-NOTICES.md'} <= notice_names,
          'Engine redistribution notices are missing')
    for prefix in ('DOTNET-RUNTIME-', 'DOTNET-NATIVEAOT-'):
        check(any(name.startswith(prefix) and name.endswith('-LICENSE.txt') for name in notice_names) and
              any(name.startswith(prefix) and name.endswith('-THIRD-PARTY-NOTICES.txt') for name in notice_names),
              'Published runtime/native-AOT notice pair is missing')
    manifest = read_json(path.parent/metadata[0]['path'])
    check(manifest['format'] == 'poima.components' and manifest['version'] == 1 and
          manifest['schemas'] == descriptor['schema']['components'], 'Component manifest/schema differ')
    schemas = {row['id']:row for row in manifest['schemas']}
    check(set(schemas) == {launcher.locomotion.CONFIG, launcher.locomotion.COMPONENT} and
          len(manifest['schemas']) == 2, 'Relay Yard native component identities differ')
    return descriptor, manifest, libraries[0], frozen


def image_oracle(path, modal, ui_scale, menu=False):
    """Bounded native pixels, opaque brand UI, and distinct visible scene colors.

    Character submission/animation are checked separately against native runtime
    observations. This is not OCR or an independent full-scene reference image.
    """
    data = path.read_bytes()
    check(54 <= len(data) <= 16*1024*1024 and data[:2] == b'BM', 'Invalid/bounded BMP readback')
    offset, dib = struct.unpack_from('<II', data, 10)
    width, height, planes, bits, compression = struct.unpack_from('<iiHHI', data, 18)
    check(dib >= 40 and width == WIDTH and abs(height) == HEIGHT and planes == 1 and
          bits in (24,32) and compression in (0,3), 'Native readback format/extent differs')
    if compression == 3:
        check(bits == 32 and dib >= 56 and
              struct.unpack_from('<IIII', data, 54) == (0xff0000,0xff00,0xff,0xff000000),
              'Native BMP bitfield masks differ')
    stride = ((width*bits+31)//32)*4
    check(offset >= 14+dib and offset+stride*abs(height) <= len(data), 'BMP pixels are truncated')
    def pixel(x,y):
        row = y if height < 0 else abs(height)-1-y
        point = offset+row*stride+x*(bits//8)
        return tuple(reversed(data[point:point+3]))
    def matches(value,color):
        return all(abs(a-b) <= 1 for a,b in zip(value,color))
    # The persistent opaque Menu button occupies the upper-right authored UI.
    # Its normal/hover/focus colors are authored, rather than calibrated from
    # this output. Slight raster conversion rounding is explicitly tolerated.
    brand = sum(any(matches(pixel(x,y), color) for color in ((114,47,55),(79,26,35)))
                for y in range(0,100,2) for x in range(WIDTH-360,WIDTH,2))
    check(brand >= 100, 'Readback is missing the visible branded native Menu control')
    dark = sum(matches(pixel(x,y),(48,37,42)) for y in range(80,HEIGHT-80,3)
               for x in range(20,600,3))
    if menu:
        check(dark >= 100, 'Visible compiled menu buttons are absent from native pixels')
    check(modal in (None,f'{910:032x}',f'{920:032x}') and
          math.isfinite(ui_scale) and 1 <= ui_scale <= 1.25,
          'Pixel oracle requires the observed authored modal and fixture UI scale')
    if menu:
        check(modal == f'{920:032x}', 'Menu pixel check requires the actual native checkpoint modal')
    # A modal covers the authored left-hand panel, so sample its separately
    # qualified right-hand scene region. Without a modal the whole scene is
    # visible; selecting only its right half would discard the actual courier
    # while admitting a tiny unrepresentative patch of floor/sky.
    scene_rect = [640 if modal is not None else 20,110,WIDTH-100,HEIGHT-140]
    # The title/Menu occupy the top band. The bottom HUD starts at
    # height - (16 + 88) * scale. Keep the scene rectangle above that authored
    # overlay; the centered text reticle is the only remaining root UI mask.
    scene_rect[3] = min(scene_rect[3],math.floor(HEIGHT-104*ui_scale)-2)
    reticle = [WIDTH//2-2,HEIGHT//2-2,
               math.ceil(WIDTH/2+16*ui_scale)+2,math.ceil(HEIGHT/2+20*ui_scale)+2]
    check(scene_rect[2]>scene_rect[0] and scene_rect[3]>scene_rect[1], 'Authored UI leaves no scene sampling area')
    def scene_sample(x,y):
        return not (reticle[0]<=x<reticle[2] and reticle[1]<=y<reticle[3])
    scene_colors = {pixel(x,y) for y in range(scene_rect[1],scene_rect[3],7)
                    for x in range(scene_rect[0],scene_rect[2],7) if scene_sample(x,y)}
    check(len(scene_colors) >= 8, 'Native scene region contains too few distinct colors')
    return dict(kind='native_renderer_readback', sha256=sha(path), bytes=len(data),
                width=WIDTH, height=HEIGHT, branded_control_samples=brand,
                menu_button_samples=dark, distinct_scene_sample_colors=len(scene_colors),
                scene_sampling_rect=scene_rect,scene_ui_masks=[reticle],
                observed_modal=modal,observed_ui_scale=ui_scale,
                scope='Authored opaque UI colors and visible scene variation; native rig submissions checked separately. No OCR or full-image reference claim.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary','runtime','artifact','source-directory','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--gpu',type=int,default=1)
    parser.add_argument('--timeout',type=float,default=1800)
    args = parser.parse_args()
    check(os.name == 'nt' and not sys.flags.optimize, 'Use native Windows Python without optimization')
    check(0 <= args.gpu <= 4095 and math.isfinite(args.timeout) and 180 <= args.timeout <= 3600,
          'GPU must be0..4095 and timeout finite180..3600seconds')
    binary, distribution, artifact, originals = (path.resolve(strict=True) for path in
        (args.binary,args.runtime,args.artifact,args.source_directory))
    check(binary.is_file() and distribution.is_dir() and artifact.is_file() and
          artifact.name == 'native-gameplay.json' and originals.is_dir(), 'Missing actual supplied inputs')
    output = args.output.resolve()
    check(not output.exists() and all(not output.is_relative_to(root) for root in
          (distribution,artifact.parent,originals)), 'Output must be new and outside supplied closures')
    output.mkdir(parents=True); logs = output/'logs'; logs.mkdir()
    started = time.monotonic(); deadline = started+args.timeout
    record = dict(passed=False,runner_sha256=sha(__file__),gpu=args.gpu,calls=[],owners=[],
        commands=[],transports=[],checks=[],cleanup_errors=[],pause_boundaries=[],captures={},
        limitations=['Windows development host; source-free relocation is not clean-machine deployment.',
            'Semantic guarded controller/UI requests and native Vulkan readbacks; not physical input or editor qualification.',
            'Two actual native host processes with one live player each; not multiplayer, representative performance or whole Alpha.',
            'Audio disabled; configured gain does not qualify sink behavior or audible output.',
            'Public player identities/configuration prove independent lifetimes; hidden preference epochs are not exposed or claimed.',
            'Original imported Run/Idle rig and native controller/navigation; no motion matching, IK, foot locking or general character creator.',
            'Owned source copies are deleted; caller originals, bundles, external saves and failed evidence are retained.'])
    author = None; hosted = []; protected = {}; bundle = frozen = None

    def remaining():
        duration = deadline-time.monotonic()
        check(duration > 0,'Relay Yard bundle qualification deadline exceeded')
        return duration

    def cleanup_process(child,row):
        if child.poll() is None:
            row['forced_cleanup'] = True
            try:
                result = subprocess.run([str(Path(os.environ['SYSTEMROOT'])/'System32/taskkill.exe'),
                    '/PID',str(child.pid),'/T','/F'],capture_output=True,timeout=15)
                row['tree_cleanup_exit_code'] = result.returncode
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=10)
            except BaseException:
                record['cleanup_errors'].append(traceback.format_exc())
        row['exit_code'] = child.poll()

    def begin(command,cwd,environment=None):
        remaining(); index=len(record['commands'])
        out,err=logs/f'command-{index}.stdout',logs/f'command-{index}.stderr'
        row=dict(command=list(map(str,command)),cwd=str(cwd),stdout=str(out),stderr=str(err),
                 exit_code=None,timed_out=False,forced_cleanup=False)
        record['commands'].append(row)
        streams=[out.open('wb'),err.open('wb')]
        try:
            child=subprocess.Popen(row['command'],cwd=cwd,env=environment,stdin=subprocess.DEVNULL,
                                   stdout=streams[0],stderr=streams[1])
            row['pid']=child.pid
            return child,row,streams
        except BaseException:
            for stream in streams:stream.close()
            raise

    def cli(executable,*arguments,cwd=output,environment=None):
        child,row,streams=begin([executable,*arguments],cwd,environment)
        try:
            try:child.wait(timeout=min(180,remaining()))
            except subprocess.TimeoutExpired:row['timed_out']=True;raise
        finally:
            cleanup_process(child,row)
            for stream in streams:stream.close()
            row.update(stdout_sha256=sha(Path(row['stdout'])),stderr_sha256=sha(Path(row['stderr'])))
        check(row['exit_code']==0 and not row['forced_cleanup'],'Native CLI failed; retained command logs identify it')
        response=read_json(row['stdout'])
        check(response.get('status')=='ok' and 'result' in response,'Native CLI returned an unsuccessful envelope')
        row['result']=response['result'];return response['result']

    def close_author():
        nonlocal author
        if author is None:return
        owned,author=author,None
        owned.close();transport=owned.client.transport
        record['transports'].append(dict(role='author',process_id=transport.process_id,
            exit_code=transport.returncode,stderr=transport.stderr_tail,stderr_truncated=transport.stderr_truncated))
        check(transport.returncode==0 and not transport.stderr_tail and not transport.stderr_truncated,
              'Owned authoring transport failed cleanup')

    try:
        launcher=launcher_module()
        descriptor,manifest,library,artifact_pin=artifact_inputs(artifact,launcher)
        runtime_spec=read_json(distribution/'runtime.json')
        check(runtime_spec['target_os']=='Windows' and runtime_spec['target_arch']=='x86_64' and
              (runtime_spec['gameplay_call_version'],runtime_spec['gameplay_call_bytes'],
               runtime_spec['gameplay_services_version'])==(1,80,7) and
              runtime_spec['gameplay_services_bytes']>=256 and REQUIRED<=set(runtime_spec['gameplay_features']) and
              all(runtime_spec['features'].get(name) is True for name in
                  ('simulation','renderer','native_gameplay','game_ui','navigation','asset_provenance')),
              'Installed native runtime lacks Relay Yard contracts')
        check(cli(binary,'version')['version']==runtime_spec['engine_version']==descriptor['engine_version'],
              'Exporter, installed runtime and supplied artifact must form one frozen cohort')
        originals_pin={name:sha(originals/name) for name in launcher.locomotion.SOURCE_SHA}
        check(originals_pin==launcher.locomotion.SOURCE_SHA,'Supply unchanged Kenney body,Run,Idle,CC0 license')
        protected=dict(runtime=(distribution,clean_inventory(distribution)),
                       artifact=(artifact.parent,artifact_pin))
        dependencies=[Path(__file__),Path(contract.__file__),ROOT/'examples/relay-yard/run.py',
            ROOT/'examples/relay-yard/RelayYardGame.cs',ROOT/'examples/relay-yard/Poima.RelayYardGame.csproj',
            ROOT/'examples/locomotion-yard/run.py',ROOT/'examples/locomotion-yard/LocomotionYardGame.cs',
            ROOT/'tests/collection_gameplay_bundle.py',ROOT/'tests/player_preferences_gameplay_bundle.py',
            ROOT/'tests/player_preferences_gameplay_contract.py',ROOT/'tests/player_service_contract.py',
            *sorted((ROOT/'tools/python/poima_client').glob('*.py'))]
        record['inputs']=dict(exporter_sha256=sha(binary),artifact_sha256=sha(artifact),
            native_image_sha256=library['sha256'],runtime_descriptor_sha256=sha(distribution/'runtime.json'),
            original_source_hashes=originals_pin,sources={p.relative_to(ROOT).as_posix():sha(p) for p in dependencies})
        record.update(artifact_descriptor=descriptor,runtime_descriptor=runtime_spec)
        project=output/'Owned Relay Yard source';project.mkdir()
        owned_sources=project/'Original sources';owned_sources.mkdir()
        for name in originals_pin:
            target=owned_sources/name;target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(originals/name,target)
            check(sha(target)==originals_pin[name],'Owned original-source copy differs')
        args.descriptor=artifact
        author=launcher.OwnedRelay(args,project/'world.json','author',record,deadline,
                                  lambda path:str(Path(path).resolve()),manifest)
        authored=author.author(owned_sources)
        authored_world=read_json(project/'world.json');close_author()
        local_artifact=project/'gameplay';local_artifact.mkdir()
        for name in artifact_pin:
            target=local_artifact/relative_path(name);target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(artifact.parent/name,target)
        project_file=project/'project.json'
        project_file.write_text(json.dumps(dict(format='poima.project',version=2,project_id=fresh(),
            name='Relay Yard',entry=dict(world='world.json',controller=launcher.uid(100),camera=launcher.uid(101)),
            audio=False,gameplay=dict(descriptor='gameplay/native-gameplay.json',values={})),indent=2)+'\n',encoding='utf-8')
        project_pin=clean_inventory(project)
        cli(binary,'project','inspect',project_file)
        exported=output/'Exported Relay Yard game'
        cli(binary,'project','build',project_file,'--runtime',distribution,'--output',exported)
        check(clean_inventory(project)==project_pin,'Inspect/export mutated the owned source')
        relocation=Path(tempfile.mkdtemp(prefix='Poima Relay Yard relocation ',dir=ROOT.parent)).resolve()
        check(not relocation.is_relative_to(ROOT),'Relocation must be outside the checkout')
        bundle=relocation/'Relocated Relay Yard game';shutil.move(str(exported),bundle)
        unrelated=relocation/'Unrelated working directory';unrelated.mkdir()
        game_file=bundle/'game.json';game_spec=read_json(game_file);frozen=clean_inventory(bundle)
        check(game_spec['version']==2 and game_spec['gameplay']['values']=={} and
              game_spec['entry']['controller']==launcher.uid(100) and game_spec['entry']['camera']==launcher.uid(101),
              'Native export changed declared values/entry')
        check(read_json(bundle/'content/world.json')==shipping_document(authored_world),
              'Native export changed authored content beyond shipping-history projection')
        check({n[8:]:h for n,h in frozen.items() if n.startswith('runtime/')}==protected['runtime'][1] and
              {n[9:]:h for n,h in frozen.items() if n.startswith('gameplay/')}==artifact_pin,
              'Exported runtime or genuine artifact closure differs')
        for asset,suffix in ((authored['asset'],'.pmodel'),(authored['navigation'],'.pnav')):
            check(frozen.get('content/world.json.assets/'+asset+suffix)==asset,'Model/navigation cooked closure differs')
        check('content/world.json.assets/'+authored['baseline']+'.pmodel' not in frozen,
              'Unreferenced original-source observation model leaked into export')
        roles={row['path']:row['role'] for row in game_spec['files']}
        check(any(role=='asset_provenance' for role in roles.values()) and
              roles.get('content/ASSET_CREDITS.txt')=='asset_credits','Exported licensed provenance/credits are missing')
        credits=(bundle/'content/ASSET_CREDITS.txt').read_text(encoding='utf-8')
        check('Kenney' in credits and 'CC0-1.0' in credits and launcher.locomotion.SOURCE_URL in credits,
              'Licensed original source attribution differs')
        check((bundle/'runtime/share/poima/licenses/RecastNavigation/License.txt').is_file(),
              'Runtime navigation license is missing')
        forbidden={'hostfxr.dll','libhostfxr.so','coreclr.dll','libcoreclr.so','poima.gameplay.dll',
                   'poima.managedbridge.dll','poima.relayyardgame.dll'}
        check(not any(Path(n).name.casefold() in forbidden or Path(n).suffix.casefold() in
              {'.fbx','.glb','.gltf','.cs','.csproj','.pdb','.sln'} or managed_pe(bundle/n) for n in frozen),
              'Source/debug/managed development content leaked into bundle')
        installed=(bundle/relative_path(game_spec['engine']['executable'])).resolve(strict=True)
        check(installed.is_relative_to(bundle),'Bundled executable escapes its immutable root')
        environment=dict(os.environ);system=Path(environment['SYSTEMROOT'])
        environment['PATH']=str(system/'System32')+';'+str(system)
        for key in list(environment):
            if key.upper().startswith(('DOTNET_','COREHOST_','POIMA_')):environment.pop(key)
        shutil.rmtree(project)
        check(not project.exists() and not exported.exists() and not owned_sources.exists(),
              'Owned source remains available before exported services')
        record.update(source_removed_before_service=True,relocation_root=str(relocation),
                      bundle_inventory=frozen,authored=authored)
        inspected=cli(installed,'game','inspect',game_file,cwd=unrelated,environment=environment)
        profile=None
        if inspected.get('input_profile'):
            profile=(bundle/relative_path(inspected['input_profile'])).resolve(strict=True)
            check(profile.is_relative_to(bundle) and profile.is_file(),'Inspected input profile escapes bundle')
        storage=output/'External Relay Yard saves';storage.mkdir()

        class HostedRelay(launcher.OwnedRelay):
            def __init__(self,label):
                self.args,self.world,self.label,self.record=args,bundle/'content/world.json',label,record
                self.deadline,self.native,self.manifest=deadline,lambda path:str(Path(path).resolve()),manifest
                self.session,self.tick,self.revision,self.closed,self.calls=None,0,0,False,0
                self.player_id=None;self.configuration=1;self.streams=[];self.client=None
                self.endpoint='relay_yard_'+fresh()
                self.child,self.command,self.streams=begin([installed,'game','serve',game_file,
                    '--endpoint',self.endpoint,'--save-root',storage],unrelated,environment)
                # Register ownership before connection/admission assertions.
                # A constructor failure must still retain and clean this host.
                hosted.append(self)
                inherited=dict(os.environ)
                try:
                    os.environ.clear();os.environ.update(environment)
                    self.client=WorldClient.connect(installed,self.endpoint,cwd=unrelated,close_timeout=10,
                        timeout_ms=min(30000,max(100,int(remaining()*1000))))
                finally:
                    os.environ.clear();os.environ.update(inherited)
                self.sync()
                check(self.tick==0,'Fresh exported host did not initialize at tick0')
                catalog=self.rpc('world.describe',dict(view='catalog'))
                check(catalog['read_only'] and catalog['mode']=='read_only_runtime',
                      'Exported host is not immutable runtime scope')
                absent=self.rpc('player.inspect')
                check(not absent['active'] and absent['generation']==0,'Service initialized an unrequested window')
                actual=self.module()['module']
                check(actual['backend']=='native_aot' and actual['type']==launcher.TYPE and
                      actual['schema']==descriptor['schema'] and actual['assembly_sha256']==library['sha256'] and
                      actual['native_diagnostics']==dict(dynamic_code_supported=False,dynamic_code_compiled=False),
                      'Fresh game service did not automatically load the actual NativeAOT')
            def sync(self):
                state=self.rpc('runtime.status')
                check(state['active'],'Exported native Runtime stopped unexpectedly')
                self.session,self.tick=state['session_id'],state['tick']
                return state
            def start_player(self):
                params=dict(session_id=self.session,request_id=fresh(),expected_tick=self.tick,
                    expected_generation=0,camera=launcher.uid(101),controller=launcher.uid(100),mode='interactive',
                    initially_paused=True,gamepad=dict(mode='disabled'),audio=False,width=WIDTH,height=HEIGHT,
                    gpu=args.gpu,samples=1,frames_in_flight=1,settings_overrides={
                        'camera.vertical_fov':60,'ui.scale':1,'audio.master_gain':1})
                if profile is not None:params['input_profile']=str(profile)
                result=self.rpc('player.start',params);self.player_id=result['player_id']
                self.presentation(0);return result
            def presentation(self,revision=None):
                observations=[];until=time.monotonic()+min(30,remaining())
                while time.monotonic()<until:
                    check(self.child.poll() is None,'Owned game host exited while observing its player')
                    value=self.rpc('player.inspect');observations.append(value)
                    application=(value.get('report') or {}).get('live_settings',{}).get('application',{})
                    if value['ready'] and value['paused'] and (revision is None or application.get('presented_revision')==revision):
                        check(value['player_id']==self.player_id and value['session_id']==self.session and
                              value['report']['hardware'] and value['report']['nvrhi_errors']==0,
                              'Live player did not present this actual runtime/owner')
                        return value
                    time.sleep(min(.01,remaining()))
                raise AssertionError('Live paused presentation deadline expired: '+repr(observations[-2:]))
            def freeze_after_control(self,action,receipt):
                # Begin/Close legitimately resume the native window. Pause by
                # its separate control guard, then adopt actual elapsed ticks;
                # never pretend the earlier UI acknowledgement froze time.
                before=self.rpc('player.inspect')
                if before['active'] and not before['paused']:
                    for attempt in range(3):
                        try:
                            self.rpc('player.control',dict(player_id=self.player_id,request_id=fresh(),
                                expected_control_revision=before['control_revision'],action='pause'))
                            break
                        except RpcError as error:
                            if error.code!=-32009 or attempt==2:raise
                            after=self.rpc('player.inspect')
                            check(after['control_revision']!=before['control_revision'],
                                  'Rejected pause without an observed concurrent control change')
                            before=after
                    self.presentation()
                self.sync()
                record['pause_boundaries'].append(dict(owner=self.label,action=action,
                    receipt_tick=receipt['current_tick'],observed_tick=self.tick,
                    naturally_elapsed_ticks=self.tick-receipt['current_tick']))
            def control(self,action):
                result=super().control(action)
                if self.player_id is not None:self.freeze_after_control(action,result)
                return result
            def preferences(self):
                return self.rpc('player.settings.inspect',dict(player_id=self.player_id))
            def capture(self,name,menu=False,rig=True):
                state=self.presentation()
                self.sync();state=self.rpc('player.inspect')
                observed_ui=self.rpc('runtime.ui.inspect',dict(session_id=self.session,tick=self.tick,limit=256))
                check(observed_ui['ui_revision']==state['ui_revision'],
                      'Native UI observation differs from the guarded capture state')
                path=output/(name+'.bmp')
                result=self.rpc('player.capture',dict(player_id=self.player_id,request_id=fresh(),
                    expected_control_revision=state['control_revision'],session_id=self.session,tick=self.tick,
                    expected_structure_revision=state['structure_revision'],expected_ui_revision=state['ui_revision'],path=str(path)))
                actual=result['capture'];draws=actual['render_diagnostics']['last_draws']
                check(actual['capture_written'] and actual['hardware'] and actual['samples']==1 and
                      actual['nvrhi_errors']==0 and result['tick']==self.tick and result['session_id']==self.session,
                      'Actual paused native Vulkan readback differs')
                check(draws['camera_draws']>0 and draws['camera_triangles']>0,'Native scene submitted no geometry')
                if rig:
                    check(draws['skinned_instances']==1 and draws['skinned_vertices']==1029,
                          'Native frame did not submit the original weighted courier')
                scale=state['report']['live_settings']['application']['effective_ui_scale']
                oracle=image_oracle(path,observed_ui['modal'],scale,menu)
                record['captures'][name]=dict(owner=self.label,result=result,oracle=oracle,
                    ui=observed_ui,animation=self.entity(400)['animation'],values=self.values())
                check(self.sync()['tick']==result['tick'],'Capturing advanced a paused simulation tick')
                return result
            def close(self):
                if self.closed:return
                state=self.rpc('player.inspect')
                if state['active']:
                    self.rpc('player.control',dict(player_id=state['player_id'],request_id=fresh(),
                        expected_control_revision=state['control_revision'],action='stop'))
                state=self.rpc('runtime.status')
                if state['active']:self.rpc('runtime.stop',dict(session_id=state['session_id']))
                self.rpc('host.shutdown');self.client.close()
                transport=self.client.transport
                record['transports'].append(dict(role=self.label,process_id=transport.process_id,
                    exit_code=transport.returncode,stderr=transport.stderr_tail,stderr_truncated=transport.stderr_truncated))
                check(transport.returncode==0 and not transport.stderr_tail and not transport.stderr_truncated,
                      'Exported client transport did not close cleanly')
                try:self.child.wait(timeout=min(30,remaining()))
                except subprocess.TimeoutExpired:self.command['timed_out']=True;raise
                cleanup_process(self.child,self.command)
                for stream in self.streams:stream.close()
                self.streams=[]
                self.command.update(stdout_sha256=sha(Path(self.command['stdout'])),
                                    stderr_sha256=sha(Path(self.command['stderr'])))
                check(self.command['exit_code']==0 and not self.command['forced_cleanup'] and
                      Path(self.command['stdout']).read_bytes()==b'' and
                      Path(self.command['stderr']).read_text(encoding='utf-8')=='Poima shared world ready: '+self.endpoint+'\n',
                      'Actual native host did not exit with its precise service diagnostics')
                self.closed=True

        first=HostedRelay('first-process')
        driver=contract.RelayDriver(first,record)
        first.start_player();driver.step(1)
        first.capture('first-welcome',rig=False)
        driver.control('begin')
        driver.drive_to(-6,7,'first-cell-approach')
        cell=first.values()['CellOne'];driver.use_target(cell)
        check(first.values()['Collected']==1 and first.values()['Won']==0,'First process did not collect exactly one cell')
        driver.aim_at(launcher.uid(300))
        driver.control('menu')
        driver.control('fov.plus');driver.control('ui.plus');driver.control('gain.minus');driver.control('refresh')
        first.presentation(3)
        first_prefs=first.preferences()
        check(first_prefs['settings']['revision']==3 and
              first_prefs['settings']['values']['camera.vertical_fov']==65 and
              first_prefs['settings']['values']['ui.scale']==1.25 and
              abs(first_prefs['settings']['values']['audio.master_gain']-.9)<1e-12,
              'Compiled native menu did not accept actual first-player preferences')
        first.capture('partial-menu',menu=True)
        save_reply=driver.control('save')
        check(save_reply['save_serviced'] and not save_reply['runtime_replaced'],'Compiled Save did not service the actual store')
        saved_snapshot=driver.snapshot()
        slot=storage/'slot-relay-yard';slot_manifest=read_json(slot/'current.json')
        selected=slot_manifest['payload']['current'];payload=slot/relative_path(selected['file'])
        check(len(relative_path(selected['file']).parts)==1 and selected['generation']==1 and
              payload.stat().st_size==selected['bytes'] and sha(payload)==selected['sha256'],
              'Actual compiled checkpoint integrity/generation differs')
        save_payload=read_json(payload)['snapshot']['payload']
        check(save_payload['tick']==first.tick and save_payload['gameplay']['values']['Collected']==1 and
              not any(k in save_payload for k in ('preferences','player_preferences','settings','live_settings')),
              'Saved partial progress/tick or independent preferences differ')
        save_pin=clean_inventory(storage)
        record['partial_checkpoint']=dict(snapshot=saved_snapshot,manifest_sha256=sha(slot/'current.json'),
            payload_sha256=sha(payload),saved_tick=first.tick,first_preferences=first_prefs,save_reply=save_reply)
        first.capture('saved-partial',menu=True)
        first.close()
        check(clean_inventory(bundle)==frozen and clean_inventory(storage)==save_pin,'First process changed bundle/save after shutdown')

        second=HostedRelay('fresh-process')
        check(second.child.pid!=first.child.pid,'Fresh continuation reused a native host process')
        resumed=contract.RelayDriver(second,record)
        second.start_player();resumed.step(1)
        second.capture('fresh-welcome',rig=False)
        fresh_prefs=second.preferences()
        check(fresh_prefs['player_id']!=first_prefs['player_id'] and fresh_prefs['settings']['revision']==0,
              'Fresh process inherited the previous public player/configuration lifetime')
        load_reply=resumed.control('welcome.load')
        check(load_reply['save_serviced'] and load_reply['runtime_replaced'],'Welcome Load did not replace actual fresh Runtime')
        restored_snapshot=resumed.snapshot()
        check(restored_snapshot==saved_snapshot,'Fresh actual checkpoint restoration differs before any new gameplay/control')
        after_load_prefs=second.preferences()
        check(after_load_prefs['player_id']==fresh_prefs['player_id'] and
              after_load_prefs['settings']['revision']==0 and
              after_load_prefs['settings']['values']==fresh_prefs['settings']['values'],
              'Runtime restoration overwrote independent current native preferences')
        for name,expected in {'camera.vertical_fov':60,'ui.scale':1,'audio.master_gain':1}.items():
            check(after_load_prefs['settings']['values'][name]==expected,'Saved preference intent leaked into fresh player: '+name)
        second.presentation(0)
        second.capture('restored-partial',menu=True)
        resumed.control('refresh')
        record['fresh_continuation']=dict(process_id=second.child.pid,first_process_id=first.child.pid,
            load_reply=load_reply,exact_snapshot_restored=True,current_preferences=after_load_prefs,
            public_player_identity_distinct=True,hidden_preference_epochs_observed=False)
        resumed.control('close')
        resumed.drive_to(-2,7.4,'second-cell-approach')
        resumed.use_target(second.values()['CellTwo'])
        resumed.drive_to(4.5,7,'third-cell-approach')
        resumed.use_target(second.values()['CellThree'])
        check(second.values()['Collected']==3 and second.values()['Won']==0,'Cells did not unlock actual courier navigation')
        for _ in range(60):
            resumed.step(1)
            if second.values()['AnimationMode']==1:break
        check(second.values()['AnimationMode']==1 and second.values()['AnimationClip']==authored['clips']['Run']['index'],
              'Actual courier did not switch to imported Run after native displacement')
        resumed.aim_at(launcher.uid(300))
        second.capture('courier-running')
        resumed.wait_for_delivery()
        check(second.values()['Phase']==2 and second.values()['Arrivals']==1 and second.values()['Plans']==1 and
              second.values()['LastPathStatus']==0 and second.values()['LastCornerCount']>=4,
              'Native courier did not complete its real route around cover')
        resumed.follow_waypoints(((5.2,5.2),(5.2,1.4)),'relay-terminal-approach')
        resumed.use_target(launcher.uid(600))
        check(second.values()['Won']==1,'Completion did not require actual in-range terminal Use after courier arrival')
        resumed.aim_at(launcher.uid(300));resumed.step(30)
        values=second.values()
        check(values['Moving']==0 and values['AnimationMode']==0 and
              values['AnimationClip']==authored['clips']['Idle']['index'] and
              values['AnimationRate']==1 and values['AnimationTransitions']>=3,
              'Delivered actual courier did not settle into the original Idle loop')
        second.capture('relay-complete')
        record['completed']=dict(values=values,snapshot=resumed.snapshot(),player=second.rpc('player.inspect'),
                                 preferences=second.preferences())
        check(record['completed']['player']['report']['runtime_replacements']==1,
              'Fresh player observed an unexpected number of actual replacements')
        second.close()
        check(clean_inventory(storage)==save_pin,'Fresh load/completion rewrote the durable partial checkpoint')
        check(clean_inventory(bundle)==frozen and all(clean_inventory(p)==pin for p,pin in protected.values()) and
              {name:sha(originals/name) for name in originals_pin}==originals_pin and
              sha(binary)==record['inputs']['exporter_sha256'] and
              all(sha(ROOT/name)==pin for name,pin in record['inputs']['sources'].items()),
              'Immutable bundle/supplied source/runtime/artifact/verifier inputs changed')
        record['checks']=[
            'Genuine Windows NativeAOT, imported body/Run/Idle, navigation, provenance and redistribution notices form an exact immutable export.',
            'Owned original sources removed before relocated game serves from unrelated cwd with system-only PATH and runtime variables removed.',
            'First native process drives its actual entry controller/ray to collect one cell, compiles no code, accepts native menu preferences and saves durable partial progress.',
            'A distinct fresh native process invokes Welcome Load and restores the exact partial snapshot before any subsequent control; its current public player/configuration remains independent.',
            'Remaining real controller pickups trigger native courier pathfinding and original Run/Idle playback; actual in-range terminal Use completes the objective.',
            'Seven paused native captures observe welcome, menu, partial save/fresh restore, running courier and final scene/UI; no physical input or image-reference claim.',
            'Both live players/runtime/hosts/transports exit cleanly; immutable bundle/supplied closures and durable checkpoint remain exact.']
        record['passed']=True
    except BaseException:
        record['error']=traceback.format_exc()
    finally:
        try:close_author()
        except BaseException:record['cleanup_errors'].append(traceback.format_exc())
        for owned in reversed(hosted):
            if not owned.closed:
                try:owned.close()
                except BaseException:record['cleanup_errors'].append(traceback.format_exc())
            if owned.child.poll() is None:cleanup_process(owned.child,owned.command)
            for stream in owned.streams:stream.close()
        if record['cleanup_errors'] or any(row['forced_cleanup'] or row['timed_out'] or row['exit_code']!=0
                                           for row in record['commands']):record['passed']=False
        record['elapsed_seconds']=time.monotonic()-started
        record['rpc_count']=len(record['calls'])
        encoded=(json.dumps(record,indent=2)+'\n').encode('utf-8')
        check(len(encoded)<=MAX_EVIDENCE,'Retained integration evidence exceeds128MiB')
        (output/'evidence.json').write_bytes(encoded)
        print(json.dumps(dict(passed=record['passed'],rpc_count=record['rpc_count'],evidence=str(output/'evidence.json'))))
    return 0 if record['passed'] else 1


if __name__=='__main__':
    raise SystemExit(main())
