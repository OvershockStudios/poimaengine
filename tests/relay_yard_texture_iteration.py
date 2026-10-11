#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise guarded texture adoption while the retained CoreCLR Relay game lives.

Caller gates imports in a Windows kill-on-close Job and admits the current
native producer. This uses the exact retained .98 partial checkpoint and A DLL,
plus .88 CoreCLR bridge/SDK. No compile, reload, renderer, source save rewrite or
teleport occurs. Settled semantic rows are losslessly retained, not wire logs.
"""
import argparse,copy,hashlib,json,os,shutil,struct,sys,time,traceback
from pathlib import Path

ROOT=next(p for p in Path(__file__).resolve().parents if (p/'tests/relay_yard_component_addition.py').is_file())
sys.path.insert(0,str(ROOT/'tests'))
import relay_yard_component_addition as retained

need,uid,sha,read,exact,inventory,stored=retained.need,retained.uid,retained.sha,retained.read,retained.exact,retained.inventory,retained.stored
fresh,contract=retained.fresh,retained.contract
MATERIAL='4b1bbb13482f06b36cef8605756bd5c6'
SOURCE_SLOT=retained.SOURCE_SLOT
TARGET=dict(base_color=[.25,.5,.75],emissive=[.125,.25,0.],metallic=.25,roughness=.5,double_sided=False)


def native_material(value):
    # PbrMaterial stores float32; the retained authored JSON's decimal .64
    # factors are rounded exactly as native parsing does, without a tolerance.
    f32=lambda x:struct.unpack('<f',struct.pack('<f',x))[0]
    return dict(base_color=[f32(x) for x in value['base_color']],emissive=[f32(x) for x in value['emissive']],
        metallic=f32(value['metallic']),roughness=f32(value['roughness']),double_sided=value['double_sided'])


class Owner(retained.Owner):
    def guards(self):
        result=super().guards()
        result['expected_material_revision']=self.inspect()['material_revision'] if self.session else None
        return result

    def materials(self):
        state=self.inspect()
        return self.rpc('runtime.texture.inspect',dict(session_id=self.session,tick=self.tick,
            material_revision=state['material_revision'],ids=[MATERIAL]))

    def apply_params(self):
        state=self.inspect()
        return dict(request_id=fresh(),session_id=self.session,expected_tick=self.tick,
            expected_structure_revision=state['structure_revision'],expected_material_revision=state['material_revision'],
            revision=self.rpc('world.inspect')['revision'],expected_authored_revision=state['authored_revision'],
            expected_content_sha256=state['content_sha256'],ids=[MATERIAL])


def domain(snapshot):
    result=copy.deepcopy(snapshot['payload'])
    for name in ('authored_revision','content_sha256'):result.pop(name)
    return result


def qualify(args,record,owners,deadline):
    run=args.baseline/'run';historical=read(args.baseline/'owner.json')
    need(historical['passed'] is True and historical['child_passed'] is True and historical['forced_cleanup'] is False
        and type(historical['exit_code']) is int and historical['exit_code']==0
        and historical['job_accounting_at_child_exit']['active_processes']==historical['job_accounting_at_child_exit']['terminated_processes']==0
        and historical['job_assigned'] is True and historical['cleanup_errors']==[]
        and historical['changed_sources']==[] and historical['changed_native_inputs']==[],
        'Retained source cohort is not clean and qualified')
    original,original_path,entry=stored(run/'saves',SOURCE_SLOT);old=original['snapshot']['payload']
    source_image=run/'compiled/a/managed/Poima.RelayYardGame.dll'
    need(entry['generation']==1 and original_path.stat().st_size==120965 and sha(original_path)==retained.SOURCE_PAYLOAD_SHA
        and sha(source_image)==retained.SOURCE_IMAGE_SHA and old['gameplay']['assembly_sha256']==retained.SOURCE_IMAGE_SHA,
        'Exact retained checkpoint/A image differs')
    need(original['snapshot']['version']==5 and old['tick']==140 and len(old['entities'])==83 and len(old['gameplay']['values'])==50
        and len(old['components']['types'])==3 and old['gameplay']['backend']=='coreclr'
        and old['gameplay']['values']['Collected']==1 and old['gameplay']['values']['Started']==1
        and old['gameplay']['values']['Won']==0 and old['gameplay']['values']['CellOne']==uid(0),
        'Not the qualified partial Relay Yard checkpoint')
    bag=original['document']['entities'][MATERIAL]['components']
    need('PbrTextures' not in bag and 'PbrMaterial' in bag and 'SkinnedMesh' in bag and bag['SkinnedMesh']['primitive']==0
        and bag['SkinnedMesh']['visible'] is True and bag['PbrMaterial']['double_sided'] is False
        and set(bag['PbrMaterial'])==set(TARGET),'Retained explicit skinned PBR target differs')
    source_assets=inventory(run/'world.json.assets');source_managed=inventory(source_image.parent)
    source_slot=inventory(run/'saves'/('slot-'+SOURCE_SLOT));storage=args.output/'saves'
    shutil.copytree(run/'saves'/('slot-'+SOURCE_SLOT),storage/('slot-'+SOURCE_SLOT))
    copied_slot=inventory(storage/('slot-'+SOURCE_SLOT));need(copied_slot==source_slot,'Checkpoint copy differs')
    record.update(retained_owner_sha256=sha(args.baseline/'owner.json'),retained_payload_sha256=retained.SOURCE_PAYLOAD_SHA,
        retained_source_image_sha256=retained.SOURCE_IMAGE_SHA,retained_source_assets=source_assets,retained_source_managed=source_managed,
        retained_source_slot=source_slot,original_snapshot=original['snapshot'],selected_id=MATERIAL,
        original_material=bag['PbrMaterial'])

    def owner(label,document):
        path=args.output/(label+'.json');retained.write_json(path,document)
        closure=run/'world.json.assets' if label=='live-original-game' else args.output/'live-original-game.json.assets'
        shutil.copytree(closure,Path(str(path)+'.assets'))
        need(inventory(Path(str(path)+'.assets'))==inventory(closure),'Cooked closure copy differs')
        game=Owner(args,source_image,path,storage,label,record,deadline);owners.append(game);return game

    game=owner('live-original-game',original['document'])
    description=game.rpc('world.describe',dict(view='method',name='runtime.texture.apply'))
    need(description['schema_revision']==83 and 'runtime.texture.apply' in description['methods'],'Current material discovery unavailable')
    record['discovery']=description
    restored=game.restore_request(game.load_parameters())
    before=game.snapshot('source-exact')
    need(exact(before,original['snapshot']) and game.tick==140 and game.inspect()['material_revision']==0,
        'Current engine did not restore the complete original game/baseline exactly')
    baseline_material=game.materials()
    need(baseline_material['textures'][0]['id']==MATERIAL and baseline_material['textures'][0]['authored'] is None,
        'Runtime original float32 material differs')
    from texture_fixture import png
    image_path=args.output/'new-live-image.png'
    # Reviewable deterministic checker recipe; create/import AFTER exact restore.
    rgba=[]
    for y in range(8):
        for x in range(8):rgba.extend(([32,128,224] if (x+y)%2 else [16,64,112])+[255])
    image_path.write_bytes(png(8,8,rgba))
    image=game.rpc('asset.image.import',dict(source=str(image_path),color_space='srgb'))
    texture_target=dict(base_color=dict(asset=image['asset'],wrap_s=10497,wrap_t=10497,min_filter=9987,mag_filter=9729))
    record['target_texture']=texture_target
    image_path.unlink() # persistence must depend on cooked bytes, not PNG source
    before_fingerprint=game.fingerprint();world_revision=game.rpc('world.inspect')['revision']
    author=game.rpc('world.transact',dict(request_id=fresh(),base_revision=world_revision,
        ops=[dict(op='component.set',id=MATERIAL,type='PbrTextures',value=texture_target)]))
    need(game.inspect()['source_stale'] is True and exact(game.materials()['textures'],baseline_material['textures']),
        'Ordinary authored edit changed runtime before explicit adoption')
    parameters=game.apply_params();accepted=game.rpc('runtime.texture.apply',parameters)
    after=game.snapshot('immediate-material-adoption');state=game.inspect();adopted=game.materials()
    need(accepted['replayed'] is False and accepted['material_revision']==state['material_revision']==1
        and accepted['session_id']==game.session and accepted['tick']==game.tick==140
        and accepted['authored_revision']==state['authored_revision']==author['revision']
        and accepted['content_sha256']==state['content_sha256']==after['payload']['content_sha256']
        and accepted['ids']==[MATERIAL] and adopted['textures'][0]['authored']==texture_target
        and adopted['source_stale'] is False,'Accepted material/baseline receipt differs')
    need(exact(domain(before),domain(after)) and after['payload']['content_sha256']!=before['payload']['content_sha256'],
        'Material adoption changed full physics/animation/UI/sound/components/gameplay state')
    # No schema/module/image exclusions: the original compiled game and all its
    # persistent globals remain byte-equivalent in the complete payload above.
    after_fingerprint=game.fingerprint()
    for key in ('module','route','ui','saves'):
        need(exact(before_fingerprint[key],after_fingerprint[key]),'Material adoption changed '+key)
    saved,_,saved_receipt=game.write('texture-adopted')
    need(exact(saved['snapshot'],after) and exact(saved['document']['entities'][MATERIAL]['components']['PbrTextures'],texture_target),
        'Durable checkpoint did not retain the adopted baseline')
    prior=game.fingerprint();prior_snapshot=game.snapshot('before-replays')
    replay=game.rpc('runtime.texture.apply',parameters)
    need(exact(replay,dict(accepted,replayed=True)) and exact(game.fingerprint(),prior)
        and exact(game.snapshot('exact-replay'),prior_snapshot),'Exact material retry reapplied or changed state')
    game.rpc('runtime.texture.apply',dict(parameters,ids=[uid(100)]),-32010)
    stale=game.apply_params();stale['expected_material_revision']=0;game.rpc('runtime.texture.apply',stale,-32009)
    need(exact(game.fingerprint(),prior) and exact(game.snapshot('rejected-replays'),prior_snapshot),
        'Conflicting or stale material apply changed complete state')
    record['material_adoption']=dict(author_receipt=author,apply_parameters=parameters,receipt=accepted,
        replay=replay,source_restore=restored,before_snapshot=before,after_snapshot=after,saved_receipt=saved_receipt,
        full_payload_except_identity_equal=True,unchanged_domains=['physics','entities','animation','sound','ui','components','gameplay'],
        same_runtime_session=True,same_gameplay_image=True,no_tick_during_apply=True)

    driver=contract.RelayDriver(game,record);before_values=game.values();driver.step();values=game.values()
    need(values['Ticks']==before_values['Ticks']+1 and values['Collected']==1 and values['Won']==0
        and game.inspect()['material_revision']==1 and game.module()['module']['assembly_sha256']==retained.SOURCE_IMAGE_SHA,
        'Original compiled callback did not continue after texture adoption')
    record['first_callback']=dict(tick=game.tick,before_ticks=before_values['Ticks'],after_ticks=values['Ticks'],values=values)
    # An exact receipt remains inert even after ordinary native ticks advance.
    advanced=game.snapshot('before-late-replay');fingerprint=game.fingerprint()
    need(exact(game.rpc('runtime.texture.apply',parameters),dict(accepted,replayed=True))
        and exact(game.snapshot('late-replay'),advanced) and exact(game.fingerprint(),fingerprint),
        'Late exact material replay required current clocks or reran adoption')
    for index,field in enumerate(('CellTwo','CellThree'),1):
        driver.drive_to(*contract.CELL_APPROACHES[index],'remaining-'+str(index))
        values=driver.use_target(old['gameplay']['values'][field]);need(values['Collected']==index+1,'Real remaining pickup did not collect')
    driver.wait_for_delivery();driver.follow_waypoints(((5.2,5.2),(5.2,1.4)),'terminal');final=driver.use_target(contract.TERMINAL)
    need(final['Won']==1 and final['Arrivals']==1 and final['Collected']==3
        and game.inspect()['material_revision']==1 and exact(game.materials()['textures'][0]['authored'],texture_target),
        'Original semantic game did not finish with adopted material intact')
    completed,_,receipt=game.write('texture-complete');record['completed']=dict(snapshot=completed['snapshot'],receipt=receipt,values=final)
    game.close()
    reopened=owner('fresh-native-process',completed['document'])
    reopened.restore_request(reopened.load_parameters('texture-complete'))
    need(exact(reopened.snapshot('fresh-exact'),completed['snapshot']) and reopened.inspect()['material_revision']==0
        and exact(reopened.materials()['textures'][0]['authored'],texture_target),
        'Fresh native owner did not exactly restore complete material/game checkpoint')
    follow=contract.RelayDriver(reopened,record);fresh_values=reopened.values();follow.step(5);continued=reopened.values()
    need(continued['Ticks']==fresh_values['Ticks']+5 and continued['Won']==1 and continued['Collected']==3
        and continued['Arrivals']==1 and reopened.tick==completed['snapshot']['payload']['tick']+5
        and reopened.module()['module']['assembly_sha256']==retained.SOURCE_IMAGE_SHA,
        'Five ordinary fresh callbacks did not continue the original completed game')
    record['fresh_continuation']=dict(snapshot=reopened.snapshot('after-five-callbacks'),values=continued,
        callbacks=5,material_revision=reopened.inspect()['material_revision']);reopened.close()
    need(inventory(storage/('slot-'+SOURCE_SLOT))==copied_slot and inventory(run/'saves'/('slot-'+SOURCE_SLOT))==source_slot
        and inventory(run/'world.json.assets')==source_assets and inventory(source_image.parent)==source_managed,
        'Original checkpoint/cooked/source image closure changed')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','baseline','hostfxr','bridge','output'):parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--timeout',type=int,default=300)
    args=parser.parse_args();need(os.name=='nt' and not sys.flags.optimize,'Use native Windows Python inside caller gated Job')
    need(1<=args.timeout<=300,'Whole fixture deadline must be 1..300 seconds')
    for name in ('binary','baseline','hostfxr','bridge'):setattr(args,name,getattr(args,name).resolve(strict=True))
    args.output=args.output.resolve()
    need(not args.output.exists() and not args.output.is_relative_to(args.baseline)
        and not args.output.is_relative_to(args.binary.parent) and not args.output.is_relative_to(args.bridge.parent),
        'Fresh output outside immutable native/tool/source roots required')
    started=time.monotonic();deadline=started+args.timeout;args.output.mkdir(parents=True);owners=[]
    inputs=[Path(__file__),Path(retained.__file__),Path(contract.__file__),Path(retained.raw_json.__file__),
        ROOT/'examples/relay-yard/run.py',ROOT/'examples/locomotion-yard/run.py',ROOT/'tests/texture_fixture.py',ROOT/'tests/gltf_fixture.py',args.binary,args.hostfxr,args.bridge,
        args.baseline/'owner.json',args.baseline/'run/saves/slot-iteration-a/current.json',
        args.baseline/'run/saves/slot-iteration-a/p-ed3a592935868be5108a38e327f32276.bin']
    inputs.extend(p for p in (ROOT/'tools/python/poima_client').iterdir() if p.is_file() and p.suffix in ('.py','.json'))
    inputs.extend(p for p in (args.baseline/'run/source/a').iterdir() if p.is_file())
    for directory in {args.bridge.parent,args.baseline/'run/compiled/a/managed',args.binary.parent}:
        inputs.extend(p for p in directory.iterdir() if p.is_file() and (p.suffix=='.dll' or p.name.endswith(('.deps.json','.runtimeconfig.json'))))
    pins={str(p.resolve()):sha(p) for p in inputs}
    record=dict(passed=False,calls=[],owners=[],source_pins=pins,cleanup_errors=[],
        scope='Current native texture adoption; retained .98 CoreCLR Relay Yard A checkpoint and .88 bridge/SDK; no compile/reload/GPU/latency qualification')
    args.ledger=retained.SettledCalls(args.output/'calls.jsonl')
    try:qualify(args,record,owners,deadline);record['passed']=True
    except BaseException as error:
        record['error']=traceback.format_exc();retained.failure_receipt(record,args.output,'qualification',error)
    finally:
        for owner in reversed(owners):
            try:owner.close()
            except BaseException as error:
                record['cleanup_errors'].append(traceback.format_exc());retained.failure_receipt(record,args.output,'native_owner_cleanup',error)
        try:
            args.ledger.close();record['rpc_ledger']=args.ledger.verify(record['calls'])
            record['changed_inputs']=[p for p,pin in pins.items() if not Path(p).is_file() or sha(p)!=pin]
            record['seconds']=time.monotonic()-started
            record['retained_bytes_before_evidence']=sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())
            need(not record['changed_inputs'] and not record['cleanup_errors'] and all(o['exit_code']==0 for o in record['owners'])
                and record['seconds']<=args.timeout and record['retained_bytes_before_evidence']<=128*1048576-16*1048576,
                'Final source/owner/time/output closure failed')
            encoded=(json.dumps(record,separators=(',',':'),allow_nan=False)+'\n').encode('utf-8')
            need(len(encoded)<=16*1048576,'Summary evidence cap exceeded')
            (args.output/'evidence.json').write_bytes(encoded)
            need(time.monotonic()-started<=args.timeout,'Whole deadline expired during evidence publication')
        except BaseException as error:
            retained.failure_receipt(record,args.output,'final_retention',error)
            summary=dict(passed=False,first_failure=record['first_failure'],secondary_failures=record.get('secondary_failures',[])[:8],
                owners=record['owners'],cleanup_errors=record['cleanup_errors'][:8],seconds=time.monotonic()-started,
                rpc_partial=dict(rows=args.ledger.count,bytes=args.ledger.bytes,sha256=args.ledger.hash.hexdigest(),
                    failed=args.ledger.failed,closed=args.ledger.closed))
            encoded=(json.dumps(summary,separators=(',',':'),allow_nan=False)+'\n').encode('utf-8')
            need(len(encoded)<=256*1024,'Small failed evidence cap exceeded');(args.output/'evidence.json').write_bytes(encoded)
    print(json.dumps(dict(passed=record['passed'],error=record.get('error'))));return 0 if record['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
