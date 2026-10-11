#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Real compiled ADS in one interactive window; external Windows Job required.

Consumes a qualified Native AOT artifact. No compilation, physical input,
between-Tick interpolation, reconstruction or latency claim.
"""
import argparse,copy,json,math,os,sys,time,traceback
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'));sys.path.insert(0,str(ROOT/'tools/python'))
from live_material_player import Owner,need,uid,encoded,same,sha,compare,WIDTH,HEIGHT
from shadow_controls_capture import component,transform
from scene_capture import pixels
from poima_client import RpcError
CAM,RED,GREEN,ARM,ENV=(f'{n:032x}' for n in (101,201,202,203,204))
GAME='Poima.Tests.ManagedCameraZoomGame'

def effective(fov):
    return 90.0 if fov==70 else math.degrees(2*math.atan(math.tan(math.pi/4)*math.tan(math.radians(fov)/2)/math.tan(math.radians(70)/2)))
def evaluate(start,target,elapsed,duration):
    if duration==0 or elapsed>=duration:return target
    if elapsed==0 or start==target:return start
    t=elapsed/duration;s=t*t*(3-2*t)
    # Independent fractional-power expression, rather than the SDK's log/exp.
    return math.degrees(2*math.atan(math.tan(math.radians(start)/2)**(1-s)*math.tan(math.radians(target)/2)**s))
def recipe(a):
    return dict(camera=CAM,width=WIDTH,height=HEIGHT,gpu=a.gpu,samples=1,frames_in_flight=2,
        capture_frames=2,lighting_path=a.lighting_path,ambient_occlusion=dict(mode='none'),reconstruction='none')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','output','native-descriptor'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--gpu',type=int,required=True);p.add_argument('--lighting-path',choices=('forward','deferred'),required=True)
    a=p.parse_args();a.binary=a.binary.resolve();a.output=a.output.resolve();a.native_descriptor=a.native_descriptor.resolve()
    need(os.name=='nt' and not sys.flags.optimize and not a.output.exists(),'Fresh root-owned Windows fixture required')
    a.output.mkdir(parents=True);started=time.monotonic();deadline=started+300
    descriptor=json.loads(a.native_descriptor.read_bytes())
    need(descriptor['type']==GAME and descriptor['identity']=='poima.test.managed-camera-zoom' and descriptor['minimum_services_bytes']==320,'Wrong native zoom consumer')
    sources=[Path(__file__),a.binary,a.binary.parent/'phonon.dll',a.native_descriptor]
    sources += [ROOT/'tests'/n for n in ('live_material_player.py','scene_capture.py','shadow_controls_capture.py','texture_fixture.py','gltf_fixture.py')]
    for row in descriptor['files']:
        rel=Path(row['path']);target=(a.native_descriptor.parent/rel).resolve(strict=True)
        need(not rel.is_absolute() and '..' not in rel.parts and target.is_relative_to(a.native_descriptor.parent) and target.stat().st_size==row['size'] and sha(target)==row['sha256'],'Artifact closure differs')
        sources.append(target)
    sources += [x for x in (ROOT/'tools/python/poima_client').rglob('*') if x.is_file() and x.suffix in ('.py','.json')]
    pins={str(x):sha(x) for x in sources}
    r=dict(passed=False,calls=[],transport=[],captures=[],checkpoints=[],checks=[],comparisons=[],running_reads=[],references=[],source_pins=pins,
        scope='One real interactive native-AOT player, committed fixed-Tick ADS, running query/mutation guards, paused exact captures and independently authored lens references. No sub-Tick interpolation, FSR, physical input or performance claim.')
    owner=None
    def game():return owner.rpc('runtime.gameplay.inspect',dict(session_id=owner.session))
    def state():return game()['module']['values']
    def native_state():return owner.rpc('runtime.inspect',dict(session_id=owner.session))
    def lens():
        tick=owner.inspect()['tick']
        return owner.rpc('runtime.camera_lens',dict(session_id=owner.session,tick=tick,entity=CAM))
    def edit(**values):
        info=native_state();g=game()
        return owner.rpc('runtime.gameplay.edit',dict(request_id=uid(),session_id=owner.session,expected_tick=info['tick'],
            expected_revision=g['revision'],expected_structure_revision=info['structure_revision'],values=values))
    def step():
        info=native_state()
        return owner.rpc('runtime.step',dict(request_id=uid(),session_id=owner.session,expected_tick=info['tick'],ticks=1))
    def ready():
        until=min(deadline,time.monotonic()+30)
        while time.monotonic()<until:
            v=owner.inspect()
            if v['ready'] and v['report']['materials']['presented']:return v
            time.sleep(.01)
        raise AssertionError('No successful native window presentation')
    def capture(label):
        info=ready();need(info['paused'],'Capture phase must be paused');before=state();current=lens()['state']
        need(before['WorldCurrent']==current['world']['vertical_fov'] and before['ViewmodelCurrent']==current['viewmodel']['vertical_fov'],'Compiled game phase differs from committed native lens')
        expected_world=effective(current['world']['vertical_fov'])
        path=a.output/(label+'.bmp');params=dict(player_id=info['player_id'],request_id=uid(),expected_control_revision=info['control_revision'],
            session_id=owner.session,tick=info['tick'],expected_material_revision=0,path=str(path))
        result=owner.rpc('player.capture',params);c=result['capture'];d=c['render_diagnostics'];scene=d['scene_receipts']['presented']
        need(d['lighting_path']==a.lighting_path and c['build_version']=='0.0.123' and c['hardware'] and c['capture_written'] and c['nvrhi_errors']==0 and c['width']==WIDTH and c['height']==HEIGHT,'Actual native capture failed')
        need(same(scene,d['scene_receipts']['submitted']) and scene['camera_id']==CAM and scene['camera_lens']==c['lens'] and abs(scene['camera_lens']['vertical_fov']-expected_world)<1e-12 and
            scene['camera_draw']['viewmodel_lens']==current['viewmodel'] and scene['material_revision']==0 and scene['frame_sequence']>0 and scene['submission_id']>0,'Exact submitted/presented projection differs')
        need(same(before,state()) and owner.inspect()['tick']==info['tick'],'Capture advanced gameplay phase')
        replay=owner.rpc('player.capture',params);need(same(replay,{**result,'replayed':True}),'Capture retry changed its original receipt')
        entry=dict(label=label,path=path.name,bytes=path.stat().st_size,sha256=sha(path),result=c,lens=current,game=before,request=params)
        r['captures'].append(entry);owner.bound();return entry
    def running_query():
        until=min(deadline,time.monotonic()+15)
        for attempt in range(32):
            need(time.monotonic()<until,'Running lens read local15s bound expired')
            info=owner.inspect();need(not info['paused'],'Running read was tested while paused')
            try:value=owner.rpc('runtime.camera_lens',dict(session_id=owner.session,tick=info['tick'],entity=CAM))
            except RpcError as error:
                need(error.code==-32009,'Unpaused pure read rejected for another reason');continue
            need(value['tick']==info['tick'] and value['state']['entity']==CAM,'Running read identity differs')
            presented=info['report']['materials']['presented']
            need(presented is not None and presented['session_id']==owner.session and presented['player_generation']==owner.player['generation'] and
                presented['tick']<=info['tick'] and presented['scene']['frame_sequence']>0 and presented['scene']['submission_id']>0 and
                info['report']['hardware'] and info['report']['nvrhi_errors']==0,'Running presentation ownership differs')
            r['running_reads'].append(dict(state=value,player=info));return value
        raise AssertionError('No successful guarded running read within32attempts')
    def transition(mode,label):
        edit(Mode=mode,DurationTicks=120);start=owner.inspect()['tick'];owner.control('resume')
        until=min(deadline,time.monotonic()+20)
        while owner.inspect()['tick']<start+30:
            need(time.monotonic()<until,'Interactive owner did not advance compiled Ticks');time.sleep(.01)
        running_query()
        info=owner.inspect()
        try:owner.rpc('runtime.step',dict(session_id=owner.session,request_id=uid(),expected_tick=info['tick'],ticks=1))
        except RpcError as error:need(error.code==-32080,'Running runtime mutation rejected for another reason')
        else:raise AssertionError('Running runtime mutation unexpectedly accepted')
        owner.control('pause');v=state();need(0<v['ElapsedTicks']<120,'No intermediate transition sampled')
        need(abs(v['WorldCurrent']-evaluate(v['WorldStart'],v['WorldTarget'],v['ElapsedTicks'],120))<1e-12 and
            abs(v['ViewmodelCurrent']-evaluate(v['ViewmodelStart'],v['ViewmodelTarget'],v['ElapsedTicks'],120))<1e-12,'Actual compiled curve differs from independent oracle')
        return capture(label)
    try:
        owner=Owner(a,r,deadline);discovery=owner.rpc('world.describe');need(discovery['schema_revision']==91,'Schema changed unexpectedly')
        ops=[]
        for entity in (CAM,RED,GREEN,ARM,ENV):ops.append(dict(op='entity.create',id=entity,name='ADS projection witness',parent=CAM if entity==ARM else None))
        ops += [transform(CAM,(0,0,3)),component(CAM,'Camera',dict(vertical_fov=70,near=.1,far=100,viewmodel=dict(vertical_fov=75,near=.01,far=10))),
            transform(ENV),component(ENV,'LightingEnvironment',dict(ambient=[0,0,0],exposure=1))]
        for entity,position,scale,color in [(RED,(.5,0,0),(.3,.8,.3),[.5,0,0]),(GREEN,(-.4,.2,-.3),(.4,.3,.3),[0,.5,0]),(ARM,(.2,-.15,-.5),(.15,.08,.1),[0,0,.5])]:
            ops += [transform(entity,position,scale),component(entity,'MeshRenderer',dict(primitive='box',visible=True,albedo=[0,0,0])),
                component(entity,'PbrMaterial',dict(base_color=[0,0,0],emissive=color,metallic=0,roughness=1,double_sided=True))]
        ops.append(component(ARM,'CameraDraw',dict(camera=CAM,mode='viewmodel')))
        owner.revision=owner.rpc('world.transact',dict(base_revision=0,request_id=uid(),ops=ops))['revision'];original=sha(a.output/'world.json')
        owner.session=uid();owner.rpc('runtime.start',dict(session_id=owner.session,revision=owner.revision))
        owner.rpc('runtime.gameplay.load_native',dict(session_id=owner.session,request_id=uid(),expected_tick=0,expected_revision=0,
            expected_structure_revision=0,descriptor=str(a.native_descriptor),expected_descriptor_sha256=sha(a.native_descriptor)))
        launch=recipe(a);launch.pop('capture_frames')
        owner.player=owner.rpc('player.start',dict(**launch,session_id=owner.session,expected_tick=0,expected_generation=0,request_id=uid(),
            initially_paused=True,mode='interactive',audio=False,gamepad=dict(mode='disabled'),settings_overrides={'camera.vertical_fov':90}))
        first=capture('neutral');r['checks'].append('Paused baseline uses player neutral90, authored70, independent VM75')
        aimed=transition(1,'aim-intermediate');released=transition(2,'release-intermediate')
        need(released['game']['WorldStart']==aimed['game']['WorldCurrent'],'Retarget did not start at committed intermediate FOV')
        edit(Mode=6);step();capture('snap-aim')
        need(sha(a.output/'world.json')==original,'Runtime easing rewrote authored scene')
        r['checks'].append('Running compiled aim/release, independent fractional-power curve, C0 reversal, pure reads and mutation guards')
        # Free the actual graphics owner before independent authored captures.
        owner.control('stop');owner.rpc('runtime.stop',dict(session_id=owner.session));owner.session=None
        for entry in r['captures']:
            c=entry['result'];camera=dict(c['lens'],viewmodel=entry['lens']['viewmodel'])
            owner.revision=owner.rpc('world.transact',dict(base_revision=owner.revision,request_id=uid(),ops=[component(CAM,'Camera',camera)]))['revision']
            path=a.output/('reference-'+entry['label']+'.bmp')
            result=owner.rpc('world.capture',dict(**recipe(a),revision=owner.revision,path=str(path)))
            need(result['hardware'] and result['capture_written'] and result['nvrhi_errors']==0,'Authored reference missing')
            comparison=compare(pixels(a.output/entry['path']),pixels(path));need(comparison['max_rgb_delta']<=1,'Intermediate player differs from independent authored projection')
            r['references'].append(dict(label=entry['label'],path=path.name,bytes=path.stat().st_size,sha256=sha(path),result=result))
            r['comparisons'].append(dict(label=entry['label'],**comparison));owner.bound()
        visible=compare(pixels(a.output/first['path']),pixels(a.output/aimed['path']))
        need(visible['strong_changed_pixels']>=50,'Intermediate ADS has no visible projection effect')
        r['visible_effect']=visible;r['checks'].append('Four exact retryable captures leave phase unchanged; independent authored references show visible ADS')
        r['passed']=True
    except BaseException:r['error']=traceback.format_exc()
    finally:
        if owner is not None:
            try:owner.close()
            except BaseException:r['cleanup_error']=traceback.format_exc();r['passed']=False
        r['seconds']=time.monotonic()-started;r['changed_sources']=[name for name,h in pins.items() if not Path(name).is_file() or sha(name)!=h]
        r['passed']=bool(r['passed'] and not r['changed_sources'] and r['seconds']<=300)
        try:
            data=encoded(r)+b'\n';need(sum(x.stat().st_size for x in a.output.rglob('*') if x.is_file())+len(data)<=64*1048576,'Whole output cap exceeded')
            output=a.output/'evidence.json';output.write_bytes(data)
            if time.monotonic()>deadline:
                r['passed']=False;r['publication_error']='Whole300s bound expired during publication';output.rename(a.output/'late-evidence.json');output.write_bytes(encoded(r)+b'\n')
            need(sum(x.stat().st_size for x in a.output.rglob('*') if x.is_file())<=64*1048576,'Published output cap exceeded')
        except BaseException as error:
            r['passed']=False;r['publication_error']=repr(error)
            output=a.output/'evidence.json'
            if output.exists():output.rename(a.output/'unqualified-evidence.json')
            summary={k:r.get(k) for k in ('passed','error','cleanup_error','publication_error','seconds')}
            (a.output/'failure.json').write_bytes(encoded(summary)+b'\n')
    print(json.dumps(dict(passed=r['passed'],captures=len(r['captures']),error=r.get('error'))));return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
