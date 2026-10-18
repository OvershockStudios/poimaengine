# SPDX-License-Identifier: Apache-2.0
"""Pinned KayKit Knight source profile for Locomotion Yard.

No downloads, source rewriting, clip removal, retargeting or runtime work on
import. The existing Kenney authoring path remains a separate unchanged default.
Select this profile explicitly and supply the original source and license.
"""
from pathlib import Path
import hashlib,math,stat,uuid

SOURCE_SHA='60428e3abc09ba83e595d256e3af8c5c976b46cdae599f0802fc82b4a3445168'
LICENSE_SHA='ae322141814056dda0deea7540d74c41d87aee1da319977cd1bd84ee5a923629'
SOURCE_URL='https://github.com/KayKit-Game-Assets/KayKit-Character-Pack-Adventures-1.0/tree/672074b73ba276876a19e8816ecdc5241817ab47'
# These values are source/profile declarations, not inferred foot-contact or
# stride calibration. Actual poses/motion remain separate native observations.
REST_MIN_Y=-2.6835075073047815e-05
REST_HEIGHT=2.4665790070154516
CAPSULE_RADIUS=.42
IDLE_NAME,RUN_NAME='Idle','Running_A'
IDLE_INDEX,RUN_INDEX=36,48
ATTACHMENT={'position':[0,-REST_MIN_Y,0],'rotation':[0,1,0,0],'scale':[1,1,1]}


def need(value,message):
    if not value:raise AssertionError(message)


def exact(a,b):
    if type(a)is not type(b):return False
    if type(a)is dict:return a.keys()==b.keys()and all(exact(a[k],b[k])for k in a)
    if type(a)is list:return len(a)==len(b)and all(exact(x,y)for x,y in zip(a,b))
    return a==b


def original(path,size,sha):
    s=path.lstat();need(stat.S_ISREG(s.st_mode)and s.st_nlink==1 and not getattr(s,'st_file_attributes',0)&0x400 and s.st_size==size,'Ordinary exact original source input')
    data=path.read_bytes();need(len(data)==size and hashlib.sha256(data).hexdigest()==sha,'Original source/license identity differs')
    return {'bytes':size,'sha256':sha}


def author_knight(game,source_directory,launcher):
    """Compose the existing compiled game with one intact original library.

    `game` is the admitted existing OwnedGame/retained-wrapper interface:
    rpc(method,params), page(method,params), native(path), manifest. `launcher`
    provides the existing fixture(), uid(), node_id(), CONFIG/COMPONENT/RIG.
    This function issues no speculative retry or runtime/compile/render request.
    """
    source_directory=Path(source_directory).resolve(strict=True)
    source=source_directory/'Knight.glb';notice=source_directory/'LICENSE.txt'
    pins={'Knight.glb':original(source,3659532,SOURCE_SHA),'LICENSE.txt':original(notice,891,LICENSE_SHA)}
    # Same schema request and authored revisions as the retained Kenney route;
    # native canonicalization stays authoritative, with the original manifest
    # retained separately by the consumer rather than Python number conversion.
    schema=game.rpc('component.schema.import',dict(request_id=uuid.uuid4().hex,base_revision=0,manifest=game.manifest))
    info=game.rpc('asset.source.inspect',dict(source=game.native(source)))
    fingerprint=info['model_sha256']
    guard=dict(source=game.native(source),expected_model_sha256=fingerprint)
    nodes=game.page('asset.source.inspect',dict(guard,section='nodes'))
    takes=game.page('asset.source.inspect',dict(guard,section='animations'))
    need(len(nodes)==57 and len(takes)==76,'Entire original node/take population required')
    need(sum(c['channels']for c in takes)==8712,'Entire original aggregate channel population required')
    for name,index,count in((IDLE_NAME,IDLE_INDEX,123),(RUN_NAME,RUN_INDEX,122)):
        matches=[c for c in takes if c['name']==name]
        need(len(matches)==1 and matches[0]['index']==index and matches[0]['channels']==count,'Inspected original locomotion take differs')
    import_params=dict(source=game.native(source))
    if getattr(getattr(game,'args',None),'guard_sources',False):
        import_params['expected_model_sha256']=fingerprint
    imported=game.rpc('asset.import',import_params)
    asset=imported['asset'];need(asset==fingerprint,'Imported full-library fingerprint differs from inspected source')
    cooked=game.page('asset.inspect',dict(asset=asset,section='animations'))
    need(exact(cooked,takes) and len(cooked)==76,'Every original take must survive cooking unchanged')
    native_bounds=None
    if getattr(getattr(game,'args',None),'native_bounds',False):
        native_bounds=game.rpc('asset.animation.bounds',dict(asset=asset,time=0))
        need(not native_bounds['empty'] and native_bounds['bounds'] is not None,'Original library has no selected geometry')
        measured=native_bounds['bounds']
        need(abs(measured['min'][1]-REST_MIN_Y)<=2e-5 and
             abs((measured['max'][1]-measured['min'][1])-REST_HEIGHT)<=2e-5,
             'Measured whole-library standing bounds differ from the declared source profile')
    # SOURCE raw FK/skin arithmetic fixes these declarations before execution.
    # The independent consumer also checks native original standing bounds;
    # don't silently recalculate policy from an unexpected imported result.
    capsule={'radius':CAPSULE_RADIUS,'height':REST_HEIGHT}
    need(2*CAPSULE_RADIUS<REST_HEIGHT<=4 and math.isfinite(REST_HEIGHT),'Declared capsule profile')
    ops=launcher.fixture(asset,IDLE_INDEX,RUN_INDEX,capsule,ATTACHMENT)
    visual=[o for o in ops if o.get('op')=='asset.instantiate'and o.get('id')==launcher.uid(launcher.RIG)]
    need(len(visual)==1,'Exactly one existing visual attachment')
    visual[0]['name']='KayKit Knight original library'
    authored_receipt=game.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=1,ops=ops))
    navigation=game.rpc('world.navigation.bake',dict(revision=2,profile=dict(**capsule,climb=0)))['asset']
    provenance=game.rpc('asset.provenance.create',dict(record=dict(format='poima.asset-provenance',version=1,asset=asset,kind='model',title='KayKit Adventurers1.0 Knight — original76-take library',creator='Kay Lousberg',source=SOURCE_URL,license=dict(identifier='CC0-1.0',notice='Kay Lousberg, KayKit Adventurers1.0. Original CC0-1.0 license retained with the supplied source. All76 original takes, geometry, bindings and embedded atlas retained. Idle and Running_A are gameplay selections; no source conversion, retargeting or stride/contact calibration.'))))
    final_receipt=game.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=2,ops=[dict(op='navigation.set',asset=navigation),dict(op='asset.provenance.set',asset=asset,records=[provenance['record']])]))
    # Store explicit profile facts without renaming actual cooked clip records.
    # 'Run' is only the existing sample's configuration dictionary key; its
    # whole original record remains name='Running_A', index48.
    game.authored=dict(revision=3,asset=asset,baseline=asset,rig=launcher.uid(launcher.RIG),nodes=[launcher.node_id(launcher.uid(launcher.RIG),n['index'])for n in nodes],imported=imported,navigation=navigation,source_profiles={'kind':'kaykit-knight-original-glb-v1','summary':info,'all_takes':takes},base_nodes=nodes,base_info=info,base_bounds={'min_y':REST_MIN_Y,'standing_height':REST_HEIGHT,'scope':'Original full-mesh rest bounds; includes helmet, weapons and cape.'},attachment=ATTACHMENT,capsule=capsule,clips={'Idle':takes[IDLE_INDEX],'Run':takes[RUN_INDEX]},source_files={name:v['sha256']for name,v in pins.items()},profile={'name':'kaykit-knight-original-glb-v1','source_inputs':pins,'selected_original_names':[IDLE_NAME,RUN_NAME],'retained_takes':76,'retained_channels':8712,'root_motion':'disabled; capsule owns travel','nominal_run_speed_mps':launcher.NOMINAL_RUN_SPEED,'calibrated_stride':False},receipts={'component_schema':schema,'authored':authored_receipt,'provenance':provenance,'final':final_receipt})
    if native_bounds is not None:game.authored['native_bounds']=native_bounds
    # Fence caller originals again after all source-read operations; a failure
    # remains the actual outcome, never an automatic import/transaction retry.
    need(original(source,3659532,SOURCE_SHA)==pins['Knight.glb']and original(notice,891,LICENSE_SHA)==pins['LICENSE.txt'],'Caller originals changed')
    return game.authored
