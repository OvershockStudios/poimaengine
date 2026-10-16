#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Original-source topology pages and a topology-driven retarget consumer.

Uses original analytic glTF/GLB and ASCII/binary FBX fixtures. No compilation,
downloads, renderer or commercial character content. Native requests/replies
remain in per-owner raw wire files; --record optionally selects the summary.
Run Windows inside a caller-owned process-tree gate. The original sources are
only moved inside this test's own fresh output for the cooked reopen check.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import stat
import struct
import subprocess
import sys
import time
import unittest
import uuid

from rotation_retarget_fixture import expected_motion, write_fixtures
from fbx_fixture import binary_encode
from gltf_fixture import glb
from switchback_game_service import Budget, RetainedTransport

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError

ARGS = None
DEADLINE = 0
RECORD = dict(passed=False,calls=[],owners=[],checks=[],source_files={},source_replacements=[])
MAX_CALLS = 3000
IDENTITY = [1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0,0.0,0.0,0.0,0.0,1.0]


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def strict_equal(a,b):
    if type(a) is not type(b):return False
    if type(a) is dict:return a.keys() == b.keys() and all(strict_equal(a[key],b[key]) for key in a)
    if type(a) is list:return len(a) == len(b) and all(strict_equal(x,y) for x,y in zip(a,b))
    return a == b


def native(path):
    value = str(Path(path).resolve())
    if ARGS.windows_interop:
        return subprocess.check_output(['wslpath','-w',value],text=True,timeout=10).strip()
    return value


def translation_inverse(x=0.0,y=0.0,z=0.0):
    value = list(IDENTITY);value[12:15] = [-x,-y,-z]
    return value


def authored_gltf():
    """Independent input recipe/oracle, including two skins and two takes.

    Sixty-six joints/channels cross the actual64-row boundary. Unweighted
    helper ancestors and deliberately non-sorted palette/channel order are
    authored explicitly. Inverse binds are closed-form inverse translations.
    """
    blob = bytearray()
    document = dict(asset=dict(version='2.0',generator='Poima original topology fixture'),
        buffers=[],bufferViews=[],accessors=[],meshes=[],nodes=[],skins=[],animations=[],
        scenes=[dict(nodes=[0,1,69])],scene=0)
    def accessor(values,kind,components,fmt='f',component=5126):
        blob.extend(b'\0'*((-len(blob))%4))
        encoded = struct.pack('<'+fmt*len(values),*values)
        view = len(document['bufferViews'])
        document['bufferViews'].append(dict(buffer=0,byteOffset=len(blob),byteLength=len(encoded)))
        blob.extend(encoded)
        index = len(document['accessors'])
        document['accessors'].append(dict(bufferView=view,componentType=component,
            count=len(values)//components,type=kind))
        return index
    position = accessor([0,0,0,1,0,0,0,1,0],'VEC3',3)
    document['accessors'][position].update(min=[0,0,0],max=[1,1,0])
    normal = accessor([0,0,1]*3,'VEC3',3)
    joints = accessor([0,0,0,0]*3,'VEC4',4,'H',5123)
    weights = accessor([1,0,0,0]*3,'VEC4',4)
    indices = accessor([0,1,2],'SCALAR',1,'H',5123)
    document['meshes'] = [dict(primitives=[dict(attributes=dict(POSITION=position,NORMAL=normal,
        JOINTS_0=joints,WEIGHTS_0=weights),indices=indices)])]
    document['nodes'] = [dict(name='Mesh',mesh=0,skin=0),dict(name='Rig',children=[2]),
        dict(name='UnweightedHelper',children=list(range(3,69)))]
    for index in range(66):
        document['nodes'].append(dict(name=f'Joint{index:02d}',translation=[(index+1)/16,0,0]))
    document['nodes'].append(dict(name='SecondMesh',mesh=0,skin=1))
    palettes = [list(reversed(range(3,69))),[68,3,5]]
    expected_skins = []
    for skin,palette in enumerate(palettes):
        matrices = [translation_inverse((node-2)/16) for node in palette]
        ibm = accessor([number for matrix in matrices for number in matrix],'MAT4',16)
        document['skins'].append(dict(name=f'Palette{skin}',skeleton=2,joints=palette,inverseBindMatrices=ibm))
        expected_skins.append([dict(index=i,node=node,inverse_bind=matrix)
            for i,(node,matrix) in enumerate(zip(palette,matrices))])
    times = accessor([0,1],'SCALAR',1)
    samplers,channels,expected_channels = [],[],[]
    for index,node in enumerate(reversed(range(3,69))):
        mode = index%3
        if mode == 0:
            path,interpolation,kind,size = 'translation','LINEAR','VEC3',3
            x = (node-2)/16
            values = [x,0,0,x,.25,0]
        elif mode == 1:
            path,interpolation,kind,size = 'rotation','STEP','VEC4',4
            values = [0,0,0,1,0,0,1,0]
        else:
            path,interpolation,kind,size = 'scale','CUBICSPLINE','VEC3',3
            values = [0,0,0,1,1,1,0,0,0,0,0,0,1,1.5,1,0,0,0]
        output = accessor(values,kind,size)
        samplers.append(dict(input=times,output=output,interpolation=interpolation))
        channels.append(dict(sampler=index,target=dict(node=node,path=path)))
        expected_channels.append(dict(index=index,node=node,path=path,interpolation=interpolation,keys=2))
    document['animations'].append(dict(name='ManyChannels',samplers=samplers,channels=channels))
    hold_times = accessor([0,.5,1],'SCALAR',1)
    hold_values = accessor([0,0,0,0,.5,0,0,1,0],'VEC3',3)
    document['animations'].append(dict(name='HelperTake',samplers=[dict(input=hold_times,
        output=hold_values,interpolation='STEP')],channels=[dict(sampler=0,target=dict(node=2,path='translation'))]))
    document['buffers'] = [dict(byteLength=len(blob))]
    return document,bytes(blob),expected_skins,[expected_channels,
        [dict(index=0,node=2,path='translation',interpolation='STEP',keys=3)]]


class SourceTopology(unittest.TestCase):
    def setUp(self):
        ordinal = unittest.defaultTestLoader.getTestCaseNames(type(self)).index(self._testMethodName)
        self.directory = ARGS.output/f't{ordinal:02d}';self.directory.mkdir()
        self.sources = write_fixtures(self.directory/'sources')
        self.document,self.blob,self.skin_oracle,self.channel_oracle = authored_gltf()
        (self.sources/'topology.glb').write_bytes(glb(self.document,self.blob))
        external = copy.deepcopy(self.document);external['buffers'][0]['uri'] = 'topology.bin'
        (self.sources/'topology.gltf').write_text(json.dumps(external),encoding='utf-8')
        (self.sources/'topology.bin').write_bytes(self.blob)
        (self.sources/'animated-binary.fbx').write_bytes(binary_encode((self.sources/'animated.fbx').read_text(encoding='utf-8')))
        donor = (self.sources/'gauge_rest.fbx').read_text(encoding='utf-8')
        for old,new in [('Root','DonorRoot'),('Child','DonorChild')]:
            self.assertEqual(donor.count('"Model::'+old+'"'),1)
            donor = donor.replace('"Model::'+old+'"','"Model::'+new+'"')
        (self.sources/'renamed.fbx').write_text(donor,encoding='utf-8',newline='\n')
        self.input_files = {str(path.resolve()):sha(path) for path in sorted(self.sources.rglob('*')) if path.is_file()}
        RECORD['source_files'].update(self.input_files)
        self.world = self.directory/'world.json';self.client = self.transport = None
        self.addCleanup(self.close);self.open()
        for revision,identity in enumerate(('1'*32,'2'*32)):
            self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,
                ops=[dict(op='entity.create',id=identity,name='Retained topology authoring '+str(revision))]))

    def open(self):
        folder = self.directory/('owner-'+str(len([row for row in RECORD['owners'] if row['test'] == self.id()])))
        folder.mkdir()
        self.raw = []
        self.transport = RetainedTransport([str(ARGS.binary),'world',native(self.world)],
            folder,Budget(DEADLINE,[]),self.raw.append)
        self.client = WorldClient(self.transport)
        self.owner_record = dict(test=self.id(),directory=str(folder),process_id=self.transport.process.pid,
            raw_response_count=0,exit_code=None,cleanup_error=None)
        RECORD['owners'].append(self.owner_record)

    def close(self):
        client,self.client = self.client,None
        if client is None:return
        try:
            client.close()
            self.assertIsNone(client.cleanup_error)
            self.assertEqual(self.transport.returncode,0)
            self.assertEqual(self.transport.stderr_tail,'')
            self.assertFalse(self.transport.stderr_truncated)
        except BaseException as failure:
            self.owner_record['cleanup_error'] = str(failure);raise
        finally:
            self.owner_record.update(exit_code=self.transport.returncode,raw_response_count=len(self.raw),
                stderr=self.transport.stderr_tail,stderr_truncated=self.transport.stderr_truncated)

    def call(self,method,params=None,error=None):
        self.assertLess(len(RECORD['calls']),MAX_CALLS,'Native call budget exhausted')
        left = DEADLINE-time.monotonic();self.assertGreater(left,0,'Whole fixture deadline exhausted')
        row = dict(test=self.id(),method=method,params=copy.deepcopy(params or {}))
        RECORD['calls'].append(row)
        try:
            result = self.client.call(method,row['params'],timeout=min(60,left))
        except RpcError as failure:
            row['error'] = copy.deepcopy(self.raw[-1]['error'])
            self.assertIsNotNone(error,failure.message);self.assertEqual(failure.code,error)
            return row['error']
        row['result'] = copy.deepcopy(result);self.assertIsNone(error,'Expected native rejection')
        return result

    def path(self,name):return native(self.sources/name)

    def state(self):
        # Preserve every authoring sidecar except the specifically owned Windows
        # zero-byte exclusive writer lock; describe that exact shape separately.
        files = {}
        for path in sorted(self.directory.rglob('*')):
            if path.parent != self.directory and not path.is_relative_to(Path(str(self.world)+'.assets')):continue
            if not path.is_file():continue
            if path.name == 'world.json.lock':
                value = path.lstat()
                self.assertTrue(stat.S_ISREG(value.st_mode));self.assertEqual(value.st_size,0)
                self.assertEqual(value.st_nlink,1)
                files[path.name] = dict(bytes=0,ordinary_writer_lock=True)
            elif path.name.startswith('world.json') or path.is_relative_to(Path(str(self.world)+'.assets')):
                files[path.relative_to(self.directory).as_posix()] = dict(bytes=path.stat().st_size,sha256=sha(path))
        return dict(inspect=self.call('world.inspect'),history=self.call('world.history'),files=files)

    def same(self,a,b,message='Complete typed state differs'):
        self.assertTrue(strict_equal(a,b),message)

    def summary(self,name):
        result = self.call('asset.source.inspect',dict(source=self.path(name)))
        self.assertEqual(set(result),{'format','published','model_sha256','importer','retained_bytes',
            'nodes','primitives','skins','animations','images','vertices','triangles','roots',
            'diagnostics','diagnostic_count','diagnostics_truncated'})
        self.assertEqual(result['format'],'poima.source-inspection.v1');self.assertIs(result['published'],False)
        self.assertRegex(result['model_sha256'],r'^[0-9a-f]{64}$')
        self.assertNotIn('skin',result);self.assertNotIn('clip',result)
        return result

    def page(self,method,params,limit=64):
        rows,offset = [],0
        for _ in range(1024):
            response = self.call(method,dict(params,offset=offset,limit=limit))
            self.assertLessEqual(len(response['items']),limit)
            rows.extend(response['items'])
            following = response['next_offset']
            if following is None:return rows
            self.assertIs(type(following),int);self.assertEqual(following,offset+len(response['items']))
            self.assertGreater(following,offset);offset = following
        self.fail('Pagination did not terminate')

    def source_page(self,name,section,*,limit=64,**selectors):
        info = self.summary(name)
        return self.page('asset.source.inspect',dict(source=self.path(name),section=section,
            expected_model_sha256=info['model_sha256'],**selectors),limit)

    def test_analytic_gltf_and_glb_exact_topology_pages(self):
        before = self.state();fingerprints = []
        for name in ('topology.gltf','topology.glb'):
            info = self.summary(name);fingerprints.append(info['model_sha256'])
            self.assertEqual((info['nodes'],info['skins'],info['animations']),(70,2,2))
            for section,selector,oracles in [('skin_joints','skin',self.skin_oracle),
                                             ('animation_channels','clip',self.channel_oracle)]:
                for selected,wanted in enumerate(oracles):
                    params = dict(source=self.path(name),section=section,
                        expected_model_sha256=info['model_sha256'],**{selector:selected})
                    one = self.page('asset.source.inspect',params,1)
                    bulk = self.page('asset.source.inspect',params,64)
                    self.same(one,bulk)
                    self.same(one,wanted)
                    for offset in (0,1,63,64,65,len(wanted),len(wanted)+1,9007199254740991):
                        result = self.call('asset.source.inspect',dict(params,offset=offset,limit=64))
                        self.assertEqual(result[selector],selected)
                        self.assertNotIn('clip' if selector == 'skin' else 'skin',result)
                        self.assertEqual(result['total'],len(wanted))
                        self.same(result['items'],wanted[offset:offset+64])
                        self.assertEqual(result['next_offset'],offset+64 if offset+64<len(wanted) else None)
            for section in ('nodes','skins','animations'):
                result = self.call('asset.source.inspect',dict(source=self.path(name),section=section))
                self.assertNotIn('skin',result);self.assertNotIn('clip',result)
        self.assertEqual(fingerprints[0],fingerprints[1])
        self.same(self.state(),before,'Unpublished topology inspection mutated authoring/history/receipts/store')
        RECORD['checks'].append(dict(test=self.id(),joint_count=66,channel_count=66,
            authored_formats=['glTF external buffer','GLB'],skins=2,takes=2,interpolations=['LINEAR','STEP','CUBICSPLINE']))

    def test_selector_errors_fingerprint_and_same_request_repair_are_readonly(self):
        name = 'topology.glb';info = self.summary(name);before = self.state()
        missing = self.path('missing-original.glb')
        bad = []
        for section,selector in [('skin_joints','skin'),('animation_channels','clip')]:
            valid = dict(source=missing,section=section,**{selector:0})
            bad.append(dict(source=missing,section=section))
            for value in (None,True,False,-1,0.0,1.0,'0',[],{},9007199254740992):
                bad.append(dict(valid,**{selector:value}))
            bad += [dict(valid,pose=dict(kind='rest')),dict(valid,pose=dict(kind='sample',clip=0,time=0)),
                    dict(valid,**{'clip' if selector == 'skin' else 'skin':0}),dict(valid,extra=True)]
        for section in ('summary','nodes','skins','animations'):
            for selector in ('skin','clip'):
                bad.append(dict(source=missing,section=section,**{selector:0}))
        valid = dict(source=missing,section='skin_joints',skin=0)
        for field,values in [('offset',(None,True,-1,0.0,'0',9007199254740992)),
                             ('limit',(None,True,0,65,1.0,'1')),
                             ('expected_model_sha256',(None,True,'','A'*64,'f'*63))]:
            bad += [dict(valid,**{field:value}) for value in values]
        for value in bad:
            self.call('asset.source.inspect',value,error=-32602)
            self.same(self.state(),before,'Malformed source selector changed complete authoring state')
        for section,selector in [('skin_joints','skin'),('animation_channels','clip')]:
            for value in (2,255,9007199254740991):
                self.call('asset.source.inspect',dict(source=self.path(name),section=section,
                    expected_model_sha256=info['model_sha256'],**{selector:value}),error=-32050)
                self.same(self.state(),before)
        request = dict(source=self.path(name),section='skin_joints',skin=0,
                       expected_model_sha256=info['model_sha256'])
        original = (self.sources/name).read_bytes()
        changed = copy.deepcopy(self.document);changed['nodes'][1]['name'] = 'ChangedOriginalRig'
        changed_bytes = glb(changed,self.blob)
        (self.directory/'original-input.glb').write_bytes(original)
        (self.directory/'changed-input.glb').write_bytes(changed_bytes)
        try:
            (self.sources/name).write_bytes(changed_bytes)
            failure = self.call('asset.source.inspect',request,error=-32050)
            self.assertIn('fingerprint changed',failure['message'])
            precedence = self.call('asset.source.inspect',dict(request,skin=9007199254740991),error=-32050)
            self.assertIn('fingerprint changed',precedence['message'])
            self.same(self.state(),before)
        finally:(self.sources/name).write_bytes(original)
        repaired = self.call('asset.source.inspect',request)
        self.assertEqual(repaired['items'],self.skin_oracle[0][:64])
        self.same(self.state(),before,'Identical source inspection request repair consumed authoring authority')
        RECORD['source_replacements'].append(dict(path=str((self.sources/name).resolve()),
            original_sha256=hashlib.sha256(original).hexdigest(),changed_sha256=hashlib.sha256(changed_bytes).hexdigest(),
            restored_sha256=sha(self.sources/name),same_request=copy.deepcopy(request)))

    def test_original_ascii_binary_and_geometryless_fbx_topology(self):
        before = self.state();variants = []
        for name in ('animated.fbx','animated-binary.fbx','gauge_rest.fbx'):
            info = self.summary(name)
            nodes = self.source_page(name,'nodes');by_name = {row['name']:row for row in nodes}
            self.assertEqual(len(by_name),len(nodes));self.assertIn('FBX Scene',by_name)
            self.assertEqual(by_name['Child']['parent'],by_name['Root']['index'])
            skins = self.source_page(name,'skins');clips = self.source_page(name,'animations')
            self.assertEqual(len(clips),2)
            joint_rows = []
            if name == 'gauge_rest.fbx':
                self.assertEqual(info['primitives'],0);self.assertEqual(skins,[])
                self.call('asset.source.inspect',dict(source=self.path(name),section='skin_joints',skin=0),error=-32050)
            else:
                self.assertEqual(len(skins),1)
                joint_rows = self.source_page(name,'skin_joints',skin=0,limit=1)
                self.assertEqual(joint_rows,[dict(index=0,node=by_name['Root']['index'],inverse_bind=list(IDENTITY)),
                    dict(index=1,node=by_name['Child']['index'],inverse_bind=translation_inverse(y=1))])
            channels = []
            for clip in clips:
                rows = self.source_page(name,'animation_channels',clip=clip['index'],limit=1)
                self.assertEqual(len(rows),clip['channels']);self.assertEqual([row['index'] for row in rows],list(range(len(rows))))
                self.assertTrue(all(row['node'] in {n['index'] for n in nodes} and row['interpolation'] == 'LINEAR'
                    and type(row['keys']) is int and row['keys'] >= 2 for row in rows))
                named = {(nodes[row['node']]['name'],row['path']) for row in rows}
                required = {('Child','translation')} if clip['name'] == 'Move' else {('Root','rotation')}
                if name == 'gauge_rest.fbx':
                    # The original Reference take is a stationary authored
                    # hold; normalized baking may collapse constant tracks.
                    required = ({('Root','rotation'),('Child','rotation'),('Child','translation')}
                                if clip['name'] == 'Motion' else set())
                self.assertTrue(required <= named,'Original authored channel targets disappeared')
                channels.append(rows)
            if name != 'gauge_rest.fbx':variants.append(dict(nodes=nodes,skins=skins,joints=joint_rows,clips=clips,channels=channels))
        self.same(variants[0],variants[1],'Original ASCII/binary FBX normalize to different topology')
        self.call('asset.source.inspect',dict(source=self.path('skin.fbx'),section='animation_channels',clip=0),error=-32050)
        self.same(self.state(),before,'Original FBX inspection published a cooked package or mutated state')
        RECORD['checks'].append(dict(test=self.id(),oracle='Authored FBX joint names/inverse binds and transform-curve targets; additional normalized baked channels retained, not guessed'))

    def test_source_topology_drives_explicit_retarget_then_source_independent_cook(self):
        before = self.state()
        target_info,donor_info = self.summary('skin.fbx'),self.summary('renamed.fbx')
        target_nodes = self.source_page('skin.fbx','nodes');source_nodes = self.source_page('renamed.fbx','nodes')
        skins = self.source_page('skin.fbx','skins');clips = self.source_page('renamed.fbx','animations')
        motion = next(row for row in clips if row['name'] == 'Motion')
        joints = [joint for skin in skins for joint in self.source_page('skin.fbx','skin_joints',skin=skin['index'])]
        channels = self.source_page('renamed.fbx','animation_channels',clip=motion['index'])
        def closure(nodes,selected):
            indexed = {row['index']:row for row in nodes};result = set()
            for identity in selected:
                seen = set()
                while identity >= 0:
                    self.assertIn(identity,indexed);self.assertNotIn(identity,seen)
                    seen.add(identity);result.add(identity);identity = indexed[identity]['parent']
            return result
        target_required = closure(target_nodes,[row['node'] for row in joints])
        source_required = closure(source_nodes,[row['node'] for row in channels])
        aliases = {'DonorRoot':'Root','DonorChild':'Child'}
        source_by_semantic = {aliases.get(row['name'],row['name']):row for row in source_nodes}
        target_by_semantic = {row['name']:row for row in target_nodes}
        self.assertEqual(len(source_by_semantic),len(source_nodes));self.assertEqual(len(target_by_semantic),len(target_nodes))
        source_index = {row['index']:row for row in source_nodes};target_index = {row['index']:row for row in target_nodes}
        names = {target_index[index]['name'] for index in target_required}
        names |= {aliases.get(source_index[index]['name'],source_index[index]['name']) for index in source_required}
        pairs = [dict(source_node=source_by_semantic[name]['index'],target_node=target_by_semantic[name]['index']) for name in sorted(names)]
        mapping = {row['source_node']:row['target_node'] for row in pairs}
        self.assertTrue(source_required <= mapping.keys());self.assertTrue(target_required <= set(mapping.values()))
        for source,target in mapping.items():
            parent = source_index[source]['parent']
            self.assertEqual(target_index[target]['parent'],-1 if parent < 0 else mapping[parent])
        self.assertIn('FBX Scene',names)
        self.same(self.state(),before,'Map discovery must not cook or mutate authoring')
        policy = dict(policy='reference-rotation-v1',source_pose=dict(kind='rest'),target_pose=dict(kind='rest'),
            expected_source_model_sha256=donor_info['model_sha256'],expected_target_model_sha256=target_info['model_sha256'],
            positions=dict(kind='target_reference'),scales='target_reference',node_map=pairs)
        baseline = self.call('asset.import',dict(source=self.path('skin.fbx')))
        request = dict(source=self.path('skin.fbx'),animations=[dict(source=self.path('renamed.fbx'),
            clip=motion['index'],name='TopologyMotion',retarget=policy)])
        imported = self.call('asset.import',request)
        def package(asset):
            path = Path(str(self.world)+'.assets')/(asset+'.pmodel');data = path.read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(),asset);self.assertEqual(data[:8],b'POIMAM04')
            header_size,binary_size = struct.unpack_from('<II',data,8)
            self.assertEqual(len(data),16+header_size+binary_size)
            header = json.loads(data[16:16+header_size]);geometry = header['geometry_bytes']
            self.assertLessEqual(geometry,binary_size)
            return header,data[16+header_size:16+header_size+geometry]
        original_header,original_geometry = package(baseline['asset'])
        composed_header,composed_geometry = package(imported['asset'])
        self.assertEqual(len(original_geometry),252,'Original three-vertex skin geometry layout changed')
        self.assertEqual(original_geometry,composed_geometry,'Retarget changed any original vertex/normal/UV/tangent/weight/index byte')
        for key in ('nodes','roots','skins','primitives','geometry_bytes'):
            self.same(original_header[key],composed_header[key])
        def sample():
            nodes = self.page('asset.inspect',dict(asset=imported['asset'],section='nodes'))
            mesh = next(row for row in nodes if row['primitives'])
            by_name = {row['name']:row['index'] for row in nodes};result = []
            for time_value in (0,.25,.75,1):
                expected = expected_motion(time_value)
                pose = self.page('asset.animation.sample',dict(asset=imported['asset'],clip=0,time=time_value,loop=False,section='nodes'))
                indexed = {row['index']:row for row in pose}
                for name in ('Root','Child'):
                    actual_matrix,wanted_matrix = indexed[by_name[name]]['world'],expected[name.lower()]
                    self.assertEqual(len(actual_matrix),16);self.assertEqual(len(wanted_matrix),16)
                    self.assertLessEqual(max(abs(a-b) for a,b in zip(actual_matrix,wanted_matrix)),3e-5)
                vertices = self.page('asset.animation.sample',dict(asset=imported['asset'],clip=0,time=time_value,
                    loop=False,section='vertices',node=mesh['index'],primitive=mesh['primitives'][0]))
                self.assertEqual([row['index'] for row in vertices],[0,1,2])
                self.assertEqual(len(vertices),len(expected['vertices']))
                for row,wanted in zip(vertices,expected['vertices']):
                    self.assertEqual(len(row['world_position']),3);self.assertEqual(len(wanted),3)
                self.assertLessEqual(max(abs(a-b) for row,wanted in zip(vertices,expected['vertices'])
                    for a,b in zip(row['world_position'],wanted)),5e-5)
                result.append(dict(time=time_value,nodes=pose,vertices=vertices))
            return result
        observations = sample();committed = self.state()
        self.same(committed['inspect'],before['inspect']);self.same(committed['history'],before['history'])
        self.close()
        retained = self.directory/'retained-owned-originals'
        self.sources.rename(retained)
        try:
            self.assertFalse(self.sources.exists());self.open()
            self.same(sample(),observations,'Cooked reopened observations differ with source directory unavailable')
            reopened = self.state()
            self.same(reopened['inspect'],committed['inspect']);self.same(reopened['files'],committed['files'])
            self.assertTrue(reopened['history']['session_local']);self.assertEqual(reopened['history']['undo_count'],0)
            self.assertEqual(reopened['history']['redo_count'],0)
        finally:retained.rename(self.sources)
        RECORD['checks'].append(dict(test=self.id(),target_joint_ancestor_closure=sorted(target_required),
            selected_channel_ancestor_closure=sorted(source_required),semantic_aliases=aliases,node_map=pairs,
            cooked_asset=imported['asset'],geometry_bytes=len(original_geometry),
            oracle='Original analytic target FK/skin; exact original geometry prefix and target nodes/skins; complete same-lineage cooked observations after source absence'))


def main():
    global ARGS,DEADLINE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--output',type=Path)
    parser.add_argument('--record',type=Path);parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--timeout',type=float,default=600)
    ARGS = parser.parse_args()
    if not math.isfinite(ARGS.timeout) or not 60 <= ARGS.timeout <= 1800:parser.error('Timeout must be finite60..1800seconds')
    ARGS.binary = ARGS.binary.resolve(strict=True)
    ARGS.output = (ARGS.output or ROOT/'build/source-inspection-topology'/uuid.uuid4().hex).resolve()
    if not ARGS.binary.is_file() or ARGS.output.exists():parser.error('Use a native binary and fresh output')
    if ARGS.record is not None and ARGS.record.exists():parser.error('--record must name a fresh file')
    ARGS.output.mkdir(parents=True);DEADLINE = time.monotonic()+ARGS.timeout
    dependencies = [Path(__file__).with_name(name) for name in ('rotation_retarget_fixture.py',
        'frame_transfer_fixture.py','fbx_fixture.py','texture_fixture.py','gltf_fixture.py',
        'switchback_game_service.py','switchback_oracle.py')]
    dependencies += [path for path in sorted((ROOT/'tools/python/poima_client').rglob('*'))
        if path.is_file() and path.suffix in ('.py','.json')]
    RECORD.update(binary_sha256=sha(ARGS.binary),source_sha256=sha(Path(__file__)),
        dependencies_sha256={str(path.resolve()):sha(path) for path in dependencies},
        scope='Real native source topology and analytic topology-driven retarget/cooked continuation; no independent FBX parser, renderer, runtime gameplay, broad exporter or performance claim')
    try:
        result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(SourceTopology))
        RECORD.update(passed=result.wasSuccessful(),tests=result.testsRun,failures=len(result.failures),
            errors=len(result.errors),skipped=len(result.skipped),rpc_count=len(RECORD['calls']))
        RECORD['source_hashes_unchanged'] = all(Path(path).is_file() and sha(path) == identity
            for path,identity in RECORD['source_files'].items())
        RECORD['helpers_unchanged'] = sha(Path(__file__)) == RECORD['source_sha256'] and all(
            sha(Path(path)) == identity for path,identity in RECORD['dependencies_sha256'].items())
        RECORD['passed'] &= RECORD['source_hashes_unchanged'] and RECORD['helpers_unchanged']
    finally:
        encoded = (json.dumps(RECORD,indent=2,ensure_ascii=False,allow_nan=False)+'\n').encode('utf-8')
        if len(encoded)>32*1024**2:raise RuntimeError('Summary exceeds32MiB; raw owned wires remain retained')
        (ARGS.output/'evidence.json').write_bytes(encoded)
        if ARGS.record is not None and ARGS.record.resolve() != (ARGS.output/'evidence.json').resolve():
            ARGS.record.parent.mkdir(parents=True,exist_ok=True)
            with ARGS.record.open('xb') as stream:stream.write(encoded)
    return 0 if RECORD['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
