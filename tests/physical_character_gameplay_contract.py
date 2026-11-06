#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Play an independently prepared original-character sample through native inputs.

Consumes a world, original licensed source and precompiled CoreCLR gameplay.
Requires an external process-tree supervisor; builds nothing and opens no GPU,
window, audio device or provider. Full requests, responses, snapshots and input
identities remain in fresh output, including failures. Native logical input is
not physical-device qualification. Export and live capture are separate checks.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import time
import traceback
import uuid

sys.dont_write_bytecode = True
import world_physical_contract as f
from managed_physical_gameplay_contract import strict_json
from character_library_intake import LibraryOracle, decode_cooked, SOURCE_SHA, LICENSE_SHA

ROOT = Path(__file__).resolve().parents[1]
SELF = Path(__file__).resolve()
GAME = 'Poima.Examples.PhysicalCharacterGame'
IDENTITY = 'poima.example.physical-character'
TARGET, PLAYER, CAMERA, TARGET_CAMERA, RIG, FLOOR = (f'{n:032x}' for n in (1, 2, 3, 4, 100, 10))
ACTIONS = dict(begin=503, menu=504, save=505, load=506, resume=507, retry=508, **{'target-motion':510})
MIB = 1024**2
SECONDS, CALLS, MAX_OUTPUT = 420, 1800, 192*MIB
need, exact = f.need, f.exact
FIELD_GROUPS = (
    ('int32','Active TargetMoving TargetMode PhysicalMode PatrolDirection'),
    ('int32','IdleClip RunClip BlendTicks AnimationClip AnimationChanges'),
    ('int32','GroundingEnabled GroundingChanges HasPreviousTarget'),
    ('float64','PreviousTargetX PreviousTargetZ HorizontalSpeed AnimationRate'),
    ('int64','PreviousTargetTick TickCount'),
    ('float64','Health Damage ShotImpulse Range PatrolExtent RunInput'),
    ('int32','Shots Hits Misses Takeovers Cooldown FireQueued RefusedShots'),
    ('int32','LastShotKind LastHitPart LastPhysicalCommand'),
    ('entity','LastHitEntity LastHitRegion'),
    ('int64','LastShotTick'),
    ('float64','LastOriginX LastOriginY LastOriginZ'),
    ('float64','LastAimX LastAimY LastAimZ LastHitX LastHitY LastHitZ'),
    ('float64','LastImpulseX LastImpulseY LastImpulseZ'),
    ('int32','StartReady SavePending SavePurpose SaveNotice LastSaveState LastSaveError'),
    ('int64','SaveEpochHigh SaveEpochLow SaveSequence LastSaveGeneration LastRestoreTick'),
)
FIELDS = {name:kind for kind,names in FIELD_GROUPS for name in names.split()}


def typed(values, schema):
    need(type(values) is dict and set(values) == set(FIELDS) and len(schema['fields']) == len(FIELDS) == 61 and
         {row['name']:row['kind'] for row in schema['fields']} == FIELDS, 'Complete independent 61-field contract')
    offset, layout = 0, {}
    for name,kind in FIELDS.items():
        size = 16 if kind == 'entity' else 4 if kind == 'int32' else 8
        alignment = min(size,8)
        offset = (offset+alignment-1)//alignment*alignment
        layout[name] = dict(name=name,kind=kind,offset=offset,bytes=size)
        offset += size
        value = values[name]
        valid = (type(value) is str and re.fullmatch('[0-9a-f]{32}',value) is not None) if kind == 'entity' else (
            type(value) is int and -(2**31) <= value < 2**31) if kind == 'int32' else (
            type(value) is str and re.fullmatch(r'(?:0|-[1-9][0-9]*|[1-9][0-9]*)',value) is not None and
            -(2**63) <= int(value) < 2**63) if kind == 'int64' else type(value) is float and math.isfinite(value)
        need(valid, 'Exact scalar type/range: '+name)
    need(type(schema['bytes']) is int and schema['bytes'] == (offset+7)//8*8 and
         all(exact(row,layout[row['name']]) for row in schema['fields']), 'Independent full native gameplay layout')
    return values


def uid():
    return uuid.uuid4().hex


def identity(path, limit=64*MIB):
    path = Path(path)
    before = path.lstat()
    need(stat.S_ISREG(before.st_mode) and not path.is_symlink() and
         not getattr(before, 'st_file_attributes', 0)&0x400 and before.st_size <= limit,
         'Bounded ordinary file required: '+str(path))
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    after = path.lstat()
    need((before.st_size,before.st_mtime_ns,before.st_ino) ==
         (after.st_size,after.st_mtime_ns,after.st_ino), 'Input changed while hashing')
    return dict(bytes=after.st_size, sha256=digest)


def read(path, limit=32*MIB):
    identity(path, limit)
    return strict_json(Path(path).read_bytes())


def vector(value, count):
    need(type(value) is list and len(value) == count and
         all(type(x) is float and math.isfinite(x) for x in value), 'Finite typed native vector required')
    return value


def node_id(index):
    return hashlib.sha256(f'poima.instance.v1/{RIG}/node/{index}'.encode()).hexdigest()[:32]


def source_contract(args, report):
    """Independent original accessor and profile checks; never import the sample launcher."""
    need(identity(args.source)['sha256'] == SOURCE_SHA and identity(args.license)['sha256'] == LICENSE_SHA,
         'Exact unchanged licensed Knight inputs required')
    oracle = LibraryOracle(args.source.read_bytes())
    document = read(args.world)
    need(document['format'] == 'poima.authored-world' and type(document['revision']) is int,
         'Actual prepared world required')
    entities = document['entities']
    profile = entities[RIG]['components']['PhysicalRig']
    baseline = read(ROOT/'examples/physical-character-yard/profiles/knight.json')
    need(exact(profile['parts'],baseline['parts']) and exact(profile['joints'],baseline['joints']) and
         profile['root'] == baseline['root'] and profile['initial_mode'] == 'animated' and
         profile['pose_frame'] == 'skeletal_bounded_scale' and
         profile['controller_handoff'] == dict(actor=TARGET,follow='translate_keep_heading') and
         profile['procedural_pose'] == 'grounded_animated_only', 'Original physical profile and explicit gameplay variant')
    rig = entities[RIG]['components']['AnimationRig']
    need(oracle.clips[36]['name'] == 'Idle' and oracle.clips[48]['name'] == 'Running_A' and
         len(oracle.nodes) == 57 and len(oracle.clips) == 76 and rig['clip'] == 36,
         'Complete source library and true selected clip identities')
    mapping = {e['components']['RigNode']['node']:key for key,e in entities.items()
               if e['components'].get('RigNode',{}).get('rig') == RIG}
    need(mapping == {i:node_id(i) for i in range(57)}, 'All original source-node identities')
    for index, key in mapping.items():
        parent = node_id(oracle.parents[index]) if index in oracle.parents else RIG
        need(entities[key]['parent'] == parent, 'Original source hierarchy changed')
    regions = {}
    for index, part in enumerate(profile['parts']):
        key = f'{2000+index:032x}'
        entity = entities[key]
        region = entity['components']['HitRegion']
        need(entity['parent'] == mapping[part['node']] and region['owner'] == TARGET and
             region['name'] == part['name'], 'Source bone to named physical-part region binding')
        regions[part['name']] = dict(index=index,entity=key,node=part['node'],node_entity=mapping[part['node']])
    need(len(regions) == 15 and len(profile['joints']) == 14 and
         {key for key,e in entities.items() if e['components'].get('HitRegion',{}).get('owner') == TARGET} ==
         {r['entity'] for r in regions.values()}, 'Complete unique physical-part hit-region population')
    need(entities[PLAYER]['components']['CharacterController']['camera'] == CAMERA and
         entities[TARGET]['components']['CharacterController']['camera'] == TARGET_CAMERA and
         entities[CAMERA]['parent'] == PLAYER and entities[RIG]['parent'] == TARGET,
         'Independent player eye and target ownership')
    asset = rig['asset']
    need(type(asset) is str and re.fullmatch('[0-9a-f]{64}',asset), 'Content-addressed original model')
    package = Path(str(args.world)+'.assets')/(asset+'.pmodel')
    need(identity(package)['sha256'] == asset, 'Complete cooked content identity')
    decode_cooked(package.read_bytes(),oracle)
    report['source_contract'] = dict(world=identity(args.world),source=identity(args.source),license=identity(args.license),
        package=dict(path=str(package),identity=identity(package)),revision=document['revision'],
        original_nodes=57,original_clips=76,profile=profile,regions=regions)
    return document, profile, regions


def copied_inputs(args, report):
    originals = [(args.world,args.output/'world.json')]
    asset_root = Path(str(args.world)+'.assets')
    need(asset_root.is_dir() and not asset_root.is_symlink(), 'Ordinary prepared asset directory')
    assets = list(asset_root.iterdir())
    need(0 < len(assets) <= 32, 'Bounded complete asset directory')
    originals += [(p,Path(str(args.output/'world.json')+'.assets')/p.name) for p in assets]
    need(sum(identity(p)['bytes'] for p,_ in originals) <= 32*MIB, 'Bounded whole prepared project copy')
    for source,target in originals:
        pin = identity(source)
        target.parent.mkdir(parents=True,exist_ok=True)
        with source.open('rb') as left,target.open('xb') as right:
            shutil.copyfileobj(left,right)
        need(identity(target) == pin, 'Original complete copied input bytes')
        report['inputs'][str(source)] = pin
        report['copies'][str(target)] = pin


def census(root):
    total = count = 0
    for base, dirs, files in os.walk(root,followlinks=False):
        for name in dirs+files:
            path = Path(base)/name
            info = path.lstat()
            need(not stat.S_ISLNK(info.st_mode) and not getattr(info,'st_file_attributes',0)&0x400,
                 'Ordinary owned output membership')
            if name in files:
                need(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, 'Independent owned output file')
                total += info.st_size
                count += 1
    need(total <= MAX_OUTPUT and count <= 2048, 'Bounded complete output')
    return dict(bytes=total,files=count)


def whole(report, label, left, right):
    a,b = Path(left['path']),Path(right['path'])
    need(a.read_bytes() == b.read_bytes() and exact(read(a),read(b)), 'Complete byte/typed archive equality: '+label)
    report['whole_pairs'].append(dict(label=label,left=str(a),right=str(b),identity=identity(a),excluded_fields=[]))


class Driver:
    def __init__(self, io, report, config, revision, saves, profile, regions):
        self.io,self.report,self.config,self.revision,self.saves = io,report,config,revision,saves
        self.profile,self.regions = profile,regions
        self.session,self.tick = uid(),0

    def state(self):
        value = self.io.rpc('runtime.inspect',dict(session_id=self.session))
        need(value['session_id'] == self.session and type(value['tick']) is int and value['tick'] == self.tick,
             'Actual native session and clock')
        return value

    def game(self):
        value = self.io.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True))
        module = value['module']
        need(module['backend'] == 'coreclr' and module['assembly_sha256'] == self.report['assembly']['sha256'] and
             module['schema']['identity'] == IDENTITY, 'Actual supplied compiled gameplay and schema identity')
        typed(module['values'],module['schema'])
        return value

    def values(self):
        return self.game()['module']['values']

    def query(self, method, **fields):
        value = self.io.rpc(method,dict(session_id=self.session,tick=self.tick,**fields))
        need(set(value) == {'session_id','tick','source','state'} and value['session_id'] == self.session and
             type(value['tick']) is int and value['tick'] == self.tick and value['source'] == 'runtime',
             'Complete committed observation envelope')
        return value['state']

    def entity(self, key):
        value = self.io.rpc('runtime.entity',dict(session_id=self.session,tick=self.tick,id=key))
        need(value['session_id'] == self.session and value['tick'] == self.tick and value['id'] == key,
             'Actual source entity observation')
        vector(value['world_matrix'],16)
        return value

    def physical(self):
        state = self.query('runtime.physical_rig',entity=RIG,include_parts=True,include_joints=True)
        need(state['entity'] == RIG and len(state['parts']) == state['body_count'] == 15 and
             len(state['joints']) == state['joint_count'] == 14 and
             [x['name'] for x in state['parts']] == [x['name'] for x in self.profile['parts']] and
             [x['name'] for x in state['joints']] == [x['name'] for x in self.profile['joints']],
             'Complete native source physical graph')
        need(len({p['body_id'] for p in state['parts']}) == 15, 'Distinct native part identities')
        for observed, source in zip(state['parts'],self.profile['parts']):
            need(set(observed) == f.PART_FIELDS and observed['node'] == source['node'] and
                 observed['node_entity'] == node_id(source['node']) and type(observed['body_id']) is int,
                 'Native body and original source-node join')
        return state

    def start(self):
        value = self.io.rpc('runtime.start',dict(session_id=self.session,revision=self.revision))
        need(value['session_id'] == self.session and value['tick'] == 0 and value['started'] is True,
             'Actual fresh runtime start')
        self.io.rpc('runtime.gameplay.load',dict(session_id=self.session,request_id=uid(),expected_tick=0,
                    expected_revision=0,**self.config))
        return self.game()

    def step(self, ticks=1, move=(0,0), look=(0,0), fire=False, error=None, **fields):
        need(type(ticks) is int and 1 <= ticks <= 60 and not (fire and look != (0,0)),
             'Bounded input batch and separate aim/fire boundaries')
        record = dict(entity=PLAYER,move=list(move),look=list(look),jump=False,
            actions=[dict(name='fire',held=fire,pressed=fire,released=False)])
        request = dict(session_id=self.session,request_id=uid(),expected_tick=self.tick,ticks=ticks,
                       inputs=[record],**fields)
        result = self.io.rpc('runtime.step',request,error)
        if error is None:
            need(result['replayed'] is False and type(result['current_tick']) is int,
                 'Actual new gameplay step receipt')
            if not result['runtime_replaced']:
                need(result['current_tick'] == self.tick+ticks and result['current_session_id'] == self.session,
                     'Native step advances the requested bounded ticks')
            self.session,self.tick = result['current_session_id'],result['current_tick']
        self.report['steps'].append(dict(request=request,result=result,expected_error=error))
        return request,result

    def control(self, action):
        state,game = self.state(),self.game()
        ui = self.io.rpc('runtime.ui.inspect',dict(session_id=self.session,tick=self.tick,limit=256))
        key = f'{ACTIONS[action]:032x}'
        selected = [r for r in ui['elements'] if r['id'] == key]
        need(len(selected) == 1 and selected[0]['action'] == action and selected[0]['eligible'] is True,
             'Actual visible/eligible authored gameplay action: '+action)
        request = dict(session_id=self.session,request_id=uid(),expected_tick=self.tick,
            expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=game['revision'],expected_structure_revision=state['structure_revision'],id=key)
        result = self.io.rpc('runtime.ui.activate',request)
        need(result['replayed'] is False, 'New logical control receipt')
        self.session,self.tick = result['current_session_id'],result['current_tick']
        self.report['controls'].append(dict(action=action,request=request,result=result))
        return request,result

    def snapshot(self, label):
        state,game = self.state(),self.game()
        slot = 'verify-'+str(len(self.report['archives'])+1)
        receipt = self.io.rpc('save.write',dict(request_id=uid(),configuration_generation=1,slot=slot,
            expected_generation=0,session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=game['revision'],
            expected_structure_revision=state['structure_revision'],expected_material_revision=state['material_revision'],
            expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence']))
        return self.archive(slot,label,receipt,game)

    def archive(self, slot, label, receipt=None, current_game=None):
        manifest_path = self.saves/('slot-'+slot)/'current.json'
        manifest = read(manifest_path,MIB)
        member = manifest['payload']['current']
        need(type(member['file']) is str and re.fullmatch('p-[0-9a-f]{32}[.]bin',member['file']), 'Safe original save member')
        path = manifest_path.parent/member['file']
        pin = identity(path,16*MIB)
        need(pin == {k:member[k] for k in ('bytes','sha256')}, 'Complete manifest/payload identity')
        if receipt is not None:
            need(pin == {k:receipt[k] for k in ('bytes','sha256')}, 'Complete write receipt/payload identity')
        doc = read(path,16*MIB)
        payload = doc['snapshot']['payload']
        need(doc['format'] == 'poima.world-save' and doc['version'] == 1 and
             payload['gameplay']['backend'] == 'coreclr' and payload['gameplay']['type'] == GAME and
             payload['gameplay']['assembly_sha256'] == self.report['assembly']['sha256'], 'Whole typed supplied gameplay save')
        if current_game is not None:
            need(payload['tick'] == self.tick and exact(payload['gameplay']['values'],current_game['module']['values']),
                 'Saved complete gameplay values equal current typed values')
        row = dict(label=label,slot=slot,generation=member['generation'],tick=payload['tick'],path=str(path),
            identity=pin,manifest_path=str(manifest_path),manifest_identity=identity(manifest_path),receipt=receipt)
        self.report['archives'].append(row)
        census(self.report['output'])
        return row

    def restore(self, row, cold=False):
        state,game = (None,None) if cold else (self.state(),self.game())
        new = uid()
        result = self.io.rpc('save.load',dict(request_id=uid(),configuration_generation=1,slot=row['slot'],
            expected_generation=row['generation'],revision=self.revision,new_session_id=new,gameplay=self.config,
            expected_session_id=None if cold else self.session,expected_tick=None if cold else self.tick,
            expected_gameplay_revision=None if cold else game['revision'],
            expected_structure_revision=None if cold else state['structure_revision'],
            expected_material_revision=None if cold else state['material_revision'],
            expected_ui_revision=None if cold else state['ui_revision'],
            expected_control_sequence=None if cold else state['control_sequence']))
        need(result['tick'] == row['tick'] and result.get('session_id',result.get('new_session_id')) == new,
             'Actual original checkpoint restored')
        self.session,self.tick = new,row['tick']
        need(exact(self.values(),read(row['path'])['snapshot']['payload']['gameplay']['values']),
             'Complete typed gameplay state survives restore')

    def observe(self, label, all_nodes=False):
        row = dict(label=label,tick=self.tick,game=self.game(),player=self.entity(PLAYER),camera=self.entity(CAMERA),
            target=self.entity(TARGET),rig=self.entity(RIG),physical=self.physical(),
            handoff=self.query('runtime.controller_handoff',entity=RIG),
            procedural=self.query('runtime.procedural_pose',entity=RIG),
            feet=[self.query('runtime.ik',entity=RIG,slot=i,include_grounding=True,include_planting=True,
                            include_end_orientation=True,include_metric=True) for i in (1,2)])
        if all_nodes:
            row['all_nodes'] = [self.entity(node_id(i)) for i in range(57)]
        self.report['observations'].append(row)
        return row

    def aim(self, part):
        """A verifier supplies look input; the game still performs its actual camera query."""
        region = self.entity(self.regions[part]['entity'])
        camera,player = self.entity(CAMERA),self.entity(PLAYER)
        origin = camera['world_matrix'][12:15]
        delta = [region['world_matrix'][12+i]-origin[i] for i in range(3)]
        distance = math.sqrt(sum(x*x for x in delta))
        need(distance > .1, 'Target region is ahead of a distinct real player camera')
        yaw = math.degrees(math.atan2(-delta[0],-delta[2]))
        pitch = math.degrees(math.asin(delta[1]/distance))
        need(-85 <= pitch <= 85, 'Target is within ordinary player camera pitch')
        self.step(look=((yaw-player['yaw']+180)%360-180,pitch-player['pitch']))
        return self.camera_hit()

    def camera_hit(self):
        camera = self.entity(CAMERA)
        matrix = camera['world_matrix']
        direction = [-matrix[i] for i in (8,9,10)]
        length = math.sqrt(sum(x*x for x in direction))
        need(length > .99, 'Actual ordinary camera forward')
        direction = [x/length for x in direction]
        origin = matrix[12:15]
        result = self.io.rpc('runtime.hit_test',dict(session_id=self.session,tick=self.tick,origin=origin,
            direction=direction,distance=self.values()['Range'],ignore=[PLAYER]))
        need(set(result) == {'session_id','tick','hit'} and result['session_id'] == self.session and
             type(result['tick']) is int and result['tick'] == self.tick, 'Actual camera hit envelope')
        hit = result['hit']
        need(hit is not None and hit['entity'] == TARGET and hit['region'] in self.regions and
             hit['region_entity'] == self.regions[hit['region']]['entity'], 'Real camera ray hits an original target part')
        row = dict(tick=self.tick,origin=origin,direction=direction,result=result,
                   expected_part=self.regions[hit['region']])
        self.report['camera_hits'].append(row)
        return row


def grounded(observation):
    need(observation['physical']['mode'] == 'animated' and observation['handoff']['phase'] == 'locomotion' and
         observation['handoff']['capsule_enrolled'] is True and observation['target']['ground'] == 'on_ground' and
         observation['rig']['animation']['clip'] == 36 and observation['rig']['animation']['transition'] is None,
         'Original grounded Idle and real locomotion ownership')
    for foot in observation['feet']:
        contact = foot['grounding']
        need(foot['enabled'] is True and contact['cast'] is True and contact['status'] == 'contact' and
             contact['hit']['entity'] == FLOOR and contact['weight'] > 0 and contact['position_error'] < .01 and
             contact['decision_tick'] == observation['tick'], 'Both real native static-floor grounding decisions')


def settled(driver, require_start=True):
    for _ in range(8):
        values = driver.values()
        if values['SavePending'] == 0 and (not require_start or values['StartReady'] == 1):
            need(all(values[n] == '0' for n in ('SaveEpochHigh','SaveEpochLow','SaveSequence')),
                 'Settled durable state does not retain opaque live tickets')
            return values
        driver.step()
    raise AssertionError('Gameplay save ticket did not settle within eight ordinary ticks')


def fired(driver, before, ray, impulse, label):
    old = before['game']['module']['values']
    request, result = driver.step(fire=True)
    current = driver.observe(label,all_nodes=True)
    values,hit = current['game']['module']['values'],ray['result']['hit']
    expected = ray['expected_part']
    need(values['Shots'] == old['Shots']+1 and values['Hits'] == old['Hits']+1 and
         values['Misses'] == old['Misses'] and values['Takeovers'] == old['Takeovers']+1 and
         values['LastShotKind'] == 3 and values['LastPhysicalCommand'] == (1 if impulse == 0 else 3) and
         values['LastHitPart'] == expected['index'] and values['LastHitRegion'] == expected['entity'] and
         values['LastHitEntity'] == TARGET and values['LastShotTick'] == str(before['tick']) and
         values['Health'] == max(0.,old['Health']-old['Damage']), 'Actual compiled shot/hit/part/damage/takeover state')
    for prefix,expected_vector in (('LastOrigin',ray['origin']),('LastAim',ray['direction']),
                                   ('LastHit',hit['position']),('LastImpulse',[x*impulse for x in ray['direction']])):
        actual = [values[prefix+axis] for axis in 'XYZ']
        need(all(type(x) is float and abs(x-y) < 1e-8 for x,y in zip(actual,expected_vector)),
             'Compiled actual camera/native hit/localized impulse vector: '+prefix)
    need(current['physical']['mode'] == 'passive' and current['handoff']['phase'] == 'physical' and
         current['handoff']['capsule_enrolled'] is False and
         current['handoff']['capsule_body_id'] == before['handoff']['capsule_body_id'] and
         current['handoff']['takeover_tick'] == before['tick'] and
         all(p['enrolled'] is True for p in current['physical']['parts']) and
         current['procedural']['phase'] == 'physical' and current['procedural']['sampled_tick'] == before['tick'],
         'Actual single-owner physical takeover consumes the committed source pose')
    saved = driver.snapshot(label)
    rigs = read(saved['path'])['snapshot']['payload']['physical']['rigs']
    need(len(rigs) == 1 and rigs[0]['entity'] == RIG, 'One original physical owner saved')
    terminal = rigs[0]['procedural_terminal']
    expected_poses = [dict(entity=row['id'],**row['local_transform']) for row in before['all_nodes']]
    need(terminal['captured_tick'] == before['tick'] and exact(terminal['poses'],expected_poses),
         'Every original source local survives the genuine grounded-to-physical boundary')
    driver.report['shots'].append(dict(label=label,request=request,result=result,ray=ray,
        original_pose_tick=before['tick'],archive=saved,impulse=impulse))
    return current,saved


def closed_records(report):
    report['recordings'] = []
    for owner in report['owners']:
        need(owner['natural_exit'] is True and owner['forced_cleanup'] is False and owner['exit_code'] == 0 and
             owner['reader_failure'] is None, 'Every native owner closed naturally')
        calls = [c for c in report['calls'] if c['owner'] == owner['directory']]
        for name,key in (('requests','request'),('responses','response')):
            path = Path(owner['directory'])/(name+'.jsonl')
            rows = [strict_json(line) for line in path.read_bytes().splitlines() if line.strip()]
            need(exact(rows,[c[key] for c in calls]), 'Complete literal native stream joins')
            report['recordings'].append(dict(path=str(path),identity=identity(path),rows=len(rows)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary','world','source','license','hostfxr','bridge','assembly','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    args = parser.parse_args()
    need(not sys.flags.optimize, 'Nonoptimized Python required')
    for name in ('binary','world','source','license','hostfxr','bridge','assembly','output'):
        setattr(args,name,getattr(args,name).resolve())
    need(not args.output.exists(), 'Fresh output required; never overwrite a prior attempt')
    args.output.mkdir(parents=True)
    args.windows_interop = False
    f.ARGS,f.DEADLINE,f.MAX_CALLS,f.MAX_RECORDED = args,time.monotonic()+SECONDS,CALLS,48*MIB
    report = dict(format='poima.physical-character-gameplay.v1',passed=False,errors=[],calls=[],owners=[],
        archives=[],whole_pairs=[],observations=[],controls=[],steps=[],shots=[],camera_hits=[],inputs={},copies={},
        checks=[],output=str(args.output),assembly=identity(args.assembly),
        limits=dict(seconds=SECONDS,rpc=CALLS,output_bytes=MAX_OUTPUT,owners=3),
        scope='Prepared original licensed character and actual compiled CoreCLR sample through native logical input. Whole same-host saves, source-pose takeover and zero-versus-positive impulse response. No physical-device, renderer, export, AOT, gait-quality, performance or force-accuracy qualification.')
    f.REPORT = report
    io = None
    try:
        document,profile,regions = source_contract(args,report)
        files = {SELF,args.binary,args.source,args.license,args.hostfxr,args.bridge,args.assembly,
            ROOT/'examples/physical-character-yard/PhysicalCharacterGame.cs',
            ROOT/'examples/physical-character-yard/Poima.PhysicalCharacterGame.csproj',
            ROOT/'examples/physical-character-yard/source-manifest.json',
            ROOT/'examples/physical-character-yard/profiles/knight.json',ROOT/'managed/Directory.Build.props'}
        for module in tuple(sys.modules.values()):
            name = getattr(module,'__file__',None)
            if name and Path(name).suffix == '.py' and Path(name).resolve().is_relative_to(ROOT):
                files.add(Path(name).resolve())
        for image in (args.bridge,args.assembly):
            files.add(image.parent/'Poima.Gameplay.dll')
            for suffix in ('.deps.json','.runtimeconfig.json'):
                companion = image.with_suffix(suffix)
                if companion.is_file():
                    files.add(companion)
        for path in (ROOT/'managed/Poima.Gameplay').rglob('*.cs'):
            if not {'obj','bin'} & set(path.relative_to(ROOT).parts):
                files.add(path)
        need(identity(args.bridge.parent/'Poima.Gameplay.dll') == identity(args.assembly.parent/'Poima.Gameplay.dll'),
             'Actual bridge and sample use identical compiled SDK bytes')
        report['inputs'].update({str(p):identity(p) for p in sorted(files)})
        copied_inputs(args,report)
        saves = args.output/'saves'
        saves.mkdir()
        config = dict(hostfxr=str(args.hostfxr),bridge=str(args.bridge),assembly=str(args.assembly),type=GAME)

        def open_owner(name):
            nonlocal io
            need(io is None and len(report['owners']) < 3, 'One live native owner and bounded population')
            io = f.Session(args.output/name,args.output/'world.json')
            configured = io.rpc('save.configure',dict(request_id=uid(),expected_generation=0,root=str(saves)))
            need(configured['generation'] == 1, 'Actual fresh save routing')
            return Driver(io,report,config,document['revision'],saves,profile,regions)

        def close_owner():
            nonlocal io
            current = io
            need(current is not None, 'Owned CLI required')
            current.close()
            need(current.process.poll() is not None, 'No producer remains before a new owner')
            io = None

        d = open_owner('primary')
        d.start()
        need(d.values()['Active'] == 0, 'Ordinary initial welcome state')
        d.control('begin')
        settled(d)
        d.step(12)
        initial = d.observe('begun-grounded-idle',all_nodes=True)
        grounded(initial)
        need(initial['game']['module']['values']['Active'] == 1 and initial['game']['module']['values']['Shots'] == 0,
             'Begin runs actual game without a manufactured shot')
        start_slot = d.archive('physical-character-start','game-created-start')
        request,result = d.control('menu')
        menu = d.snapshot('menu-before-exact-retry')
        need(d.values()['Active'] == 2 and exact(d.io.rpc('runtime.ui.activate',request),dict(result,replayed=True)),
             'Original ordinary control receipt replays without execution')
        whole(report,'menu retry purity',menu,d.snapshot('menu-after-exact-retry'))
        d.control('resume')

        d.control('target-motion')
        before_conflict = d.snapshot('motion-conflict-before')
        d.step(error=-32040,physical_rigs=[dict(entity=RIG,activate=True)])
        whole(report,'actual target motion/activation conflict rollback',before_conflict,d.snapshot('motion-conflict-after'))
        before_run = d.entity(TARGET)['world_matrix'][12:15]
        d.step(40)
        running = d.observe('native-running',all_nodes=True)
        need(running['rig']['animation']['clip'] == 48 and running['rig']['animation']['playing'] is True and
             running['rig']['animation']['transition'] is None and running['physical']['mode'] == 'animated' and
             math.dist(before_run,running['target']['world_matrix'][12:15]) > .2 and
             running['game']['module']['values']['GroundingEnabled'] == 0 and
             not exact([r['local_transform'] for r in initial['all_nodes']],
                       [r['local_transform'] for r in running['all_nodes']]), 'Real target movement and original Running_A pose')
        d.control('target-motion')
        d.step(20)
        stopped = d.observe('stopped-grounded',all_nodes=True)
        grounded(stopped)
        player_start = d.entity(PLAYER)['world_matrix'][12:15]
        d.step(8,move=(1,0))
        player_moved = d.entity(PLAYER)['world_matrix'][12:15]
        need(math.dist(player_start,player_moved) > .1, 'Ordinary native player movement changes position')
        d.step(8,move=(-1,0))
        report['checks'].append('Begin/menu/resume, retry receipts, real player/target movement, original Idle/Running_A and live foot grounding')

        d.aim('chest')
        aimed = d.observe('aimed-animated',all_nodes=True)
        animated = d.snapshot('animated-aimed-checkpoint')
        baseline_values = d.values()
        positive_impulse = baseline_values['ShotImpulse']
        need(positive_impulse > 0 and baseline_values['Health'] <= baseline_values['Damage'],
             'Ordinary authored one-hit and positive impulse setup')
        game = d.game()
        tuning = dict(session_id=d.session,request_id=uid(),expected_tick=d.tick,
                      expected_revision=game['revision'],values=dict(ShotImpulse=0.))
        d.io.rpc('runtime.gameplay.edit',tuning)
        need(exact(d.values(),dict(baseline_values,ShotImpulse=0.)), 'Only the supported typed impulse tuning changes gameplay')
        zero_before = d.observe('zero-impulse-authored-counterfactual',all_nodes=True)
        zero_input = d.snapshot('zero-impulse-input')
        original_doc,zero_doc = read(animated['path']),read(zero_input['path'])
        expected_doc = copy.deepcopy(original_doc)
        expected_doc['snapshot']['payload']['gameplay']['values']['ShotImpulse'] = 0.
        expected_doc['snapshot']['payload']['gameplay_revision'] += 1
        # This is an explicit authored counterfactual, not a whole-equality pair.
        # Its payload checksum must change with the typed value and revision;
        # both original complete byte identities remain recorded independently.
        need(re.fullmatch('[0-9a-f]{64}',zero_doc['snapshot']['sha256']) is not None and
             zero_doc['snapshot']['sha256'] != original_doc['snapshot']['sha256'], 'Tuning changes the native payload checksum')
        expected_doc['snapshot']['sha256'] = zero_doc['snapshot']['sha256']
        need(exact(zero_doc,expected_doc), 'Whole counterfactual differs only in ShotImpulse, its gameplay revision and checksum')
        need(exact(aimed['physical'],zero_before['physical']) and exact(aimed['all_nodes'],zero_before['all_nodes']),
             'Tuning does not change native physical/source-pose input')
        ray = d.camera_hit()
        zero,zero_saved = fired(d,zero_before,ray,0.,'zero-impulse-takeover')
        d.restore(animated)
        whole(report,'positive replay restores whole original animated input',animated,d.snapshot('positive-input-restored'))
        before_shot = d.snapshot('shot-conflict-before')
        d.step(fire=True,error=-32040,physical_rigs=[dict(entity=RIG,activate=True)])
        whole(report,'actual compiled shot/caller physical conflict rollback',before_shot,d.snapshot('shot-conflict-after'))
        positive_before = d.observe('positive-impulse-original-input',all_nodes=True)
        positive_ray = d.camera_hit()
        need(positive_ray['result']['session_id'] == d.session and
             exact(positive_ray,dict(ray,result=dict(ray['result'],session_id=d.session))),
             'Counterfactual camera/hit values are exact; only the explicitly replaced session identity differs')
        positive,passive = fired(d,positive_before,positive_ray,positive_impulse,'positive-impulse-takeover')
        deltas = []
        for a,b in zip(zero['physical']['parts'],positive['physical']['parts']):
            need(a['name'] == b['name'] and a['body_id'] == b['body_id'], 'Same native part lineage across counterfactual')
            deltas.append(dict(part=a['name'],linear=math.dist(a['linear_velocity'],b['linear_velocity']),
                               angular=math.dist(a['angular_velocity'],b['angular_velocity'])))
        need(max(max(r['linear'],r['angular']) for r in deltas) > 1e-5, 'Positive authored impulse changes actual native response')
        report['impulse_counterfactual'] = dict(original=animated,zero_input=zero_input,tuning=tuning,
            zero=zero_saved,positive=passive,native_velocity_differences=deltas,
            changed_fields=['/snapshot/payload/gameplay/values/ShotImpulse',
                            '/snapshot/payload/gameplay_revision','/snapshot/sha256'],
            scope='Same complete original input plus the explicit ShotImpulse tuning and its gameplay revision. Solver response differs; contacts, gravity and constraints prevent interpreting this as an exact force/momentum oracle.')
        report['checks'].append('Real camera HitTest maps source region to physical part, full source pose takeover, zero/positive actual solver response and complete failed-shot rollback')

        d.restore(animated)
        whole(report,'warm animated whole restore',animated,d.snapshot('warm-animated'))
        d.step(3)
        animated_future = d.snapshot('warm-animated-future')
        d.restore(passive)
        whole(report,'warm passive whole restore',passive,d.snapshot('warm-passive'))
        d.step(3)
        passive_future = d.snapshot('warm-passive-future')
        close_owner()

        d = open_owner('cold-animated')
        d.restore(animated,cold=True)
        whole(report,'cold animated whole restore',animated,d.snapshot('cold-animated'))
        d.step(3)
        whole(report,'whole warm/cold animated continuation',animated_future,d.snapshot('cold-animated-future'))
        close_owner()

        d = open_owner('cold-passive')
        d.restore(passive,cold=True)
        whole(report,'cold passive whole restore',passive,d.snapshot('cold-passive'))
        d.step(3)
        whole(report,'whole warm/cold passive continuation',passive_future,d.snapshot('cold-passive-future'))
        d.control('save')
        user_save = d.archive('physical-character','ordinary-game-save')
        settled(d)
        d.step(2)
        old_session = d.session
        load_request,loaded = d.control('load')
        need(loaded['runtime_replaced'] is True and d.session != old_session and d.tick == user_save['tick'],
             'Ordinary game Load actually replaces the native runtime')
        whole(report,'ordinary gameplay Load restores complete saved bytes',user_save,d.snapshot('ordinary-load-immediate'))
        before_retry = d.snapshot('load-receipt-before-retry')
        need(exact(d.io.rpc('runtime.ui.activate',load_request),dict(loaded,replayed=True)),
             'Original gameplay Load receipt survives replacement')
        whole(report,'load retry does not replace or mutate again',before_retry,d.snapshot('load-receipt-after-retry'))
        settled(d)
        need(d.values()['LastSaveState'] == 3 and d.values()['TargetMode'] == 2, 'Restored pending gameplay ticket settles honestly')
        old_session = d.session
        _,retried = d.control('retry')
        need(retried['runtime_replaced'] is True and d.session != old_session and d.tick == start_slot['tick'],
             'Ordinary Retry loads the real game-created starting checkpoint')
        whole(report,'Retry restores complete original start checkpoint',start_slot,d.snapshot('ordinary-retry-immediate'))
        settled(d)
        d.step(3)
        reset = d.observe('ordinary-retry-running')
        need(reset['physical']['mode'] == 'animated' and reset['handoff']['phase'] == 'locomotion' and
             reset['game']['module']['values']['Shots'] == 0 and reset['game']['module']['values']['Health'] == 100.,
             'Retry restores actual gameplay, source pose and native ownership')
        close_owner()
        report['checks'].append('Complete warm/cold animated and passive save continuation, ordinary game Save/Load/Retry and idempotent Load receipt')
        closed_records(report)
        need(len(report['owners']) == 3 and all(identity(Path(p)) == pin for p,pin in report['inputs'].items()) and
             all(identity(Path(p)) == pin for p,pin in report['copies'].items()), 'Every original/copied input remained unchanged')
        need(time.monotonic() < f.DEADLINE, 'Finite whole workflow deadline')
        report['passed'] = True
    except BaseException:
        report['errors'].append(traceback.format_exc())
    finally:
        if io is not None:
            try:
                io.close()
            except BaseException:
                report['errors'].append(traceback.format_exc())
        report['passed'] = report['passed'] and not report['errors']
        report['rpc_count'] = len(report['calls'])
        report['natural_cli_owners'] = sum(o.get('natural_exit') is True and o.get('exit_code') == 0 and
                                         o.get('forced_cleanup') is False for o in report['owners'])
        try:
            report['output_census_before_report'] = census(args.output)
            raw = (json.dumps(report,indent=2,allow_nan=False)+'\n').encode()
            need(len(raw) <= 32*MIB and report['output_census_before_report']['bytes']+len(raw) <= MAX_OUTPUT,
                 'Bounded complete failure/success report')
            with (args.output/'evidence.json').open('xb') as stream:
                stream.write(raw)
        finally:
            print(args.output/'evidence.json',flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
