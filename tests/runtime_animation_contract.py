#!/usr/bin/env python3
"""Editable authored rigs and transactional native fixed-tick animation."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import uuid
from animation_fixture import ribbon
from gltf_fixture import glb

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary',type=Path)
parser.add_argument('--runtime',type=int,choices=[0,1],default=1)
parser.add_argument('--windows-interop',action='store_true')
parser.add_argument('--evidence',type=Path)
args=parser.parse_args();BINARY=str(args.binary.resolve())
ROOT=Path(__file__).resolve().parents[1];SCRATCH=ROOT/'build/runtime-animation-contract'
SCRATCH.mkdir(parents=True,exist_ok=True);REPORTS=[]
def uid(n):return f'{n:032x}'
def rpc(method,**params):return {'jsonrpc':'2.0','method':method,'params':params}
def node(root,index):return hashlib.sha256(f'poima.instance.v1/{root}/node/{index}'.encode()).hexdigest()[:32]
def primitive(root,index,slot=0):return hashlib.sha256(f'poima.instance.v1/{root}/node/{index}/primitive/{slot}'.encode()).hexdigest()[:32]
def transform(position=(0,0,0),scale=(1,1,1)):return {'position':list(position),'rotation':[0,0,0,1],'scale':list(scale)}
def component(entity,kind,value):return {'op':'component.set','id':entity,'type':kind,'value':value}
def command(root,clip=0,time=0,speed=1,loop=True,playing=True):return dict(entity=root,clip=clip,time=time,speed=speed,loop=loop,playing=playing)
def step(tick,ticks,animations=(),session=900,request_id=None):
    return rpc('runtime.step',session_id=uid(session),request_id=request_id or uuid.uuid4().hex,expected_tick=tick,ticks=ticks,animations=list(animations))
def runtime_entity(entity,session=900):return rpc('runtime.entity',session_id=uid(session),id=entity)

class RuntimeAnimation(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=SCRATCH);self.root=Path(self.tmp.name)
        self.world=self.root/'animated.world.json';self.rig=uid(100)
    def tearDown(self):self.tmp.cleanup()
    def native(self,path):
        return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip() if args.windows_interop else str(path.resolve())
    def requests(self,*requests):
        p=subprocess.run([BINARY,'world',self.native(self.world)],input=''.join(json.dumps(dict(r,id=i))+'\n' for i,r in enumerate(requests)),
                         text=True,encoding='utf-8',capture_output=True,timeout=60)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        rows=[json.loads(line) for line in p.stdout.splitlines()];self.assertEqual(len(rows),len(requests),p.stdout+p.stderr);return rows
    def result(self,response):self.assertIn('result',response,response);return response['result']
    def error(self,response,code):self.assertEqual(response.get('error',{}).get('code'),code,response)
    def transact(self,ops,revision=1,preview=False):return rpc('world.transact',request_id=uuid.uuid4().hex,base_revision=revision,ops=ops,preview=preview)
    def prepare(self,*,doc=None,blob=None,two=False):
        if doc is None:doc,blob=ribbon()
        path=self.root/'source.glb';path.write_bytes(glb(doc,blob))
        self.asset=self.result(self.requests(rpc('asset.import',source=self.native(path)))[0])['asset'];path.unlink()
        ops=[{'op':'asset.instantiate','id':self.rig,'name':'First rig','asset':self.asset}]
        if two:ops += [{'op':'asset.instantiate','id':uid(200),'name':'Second rig','asset':self.asset},component(uid(200),'Transform',transform((10,0,0)))]
        self.result(self.requests(self.transact(ops,revision=0))[0]);return self.asset
    def start(self,session=900,revision=1):return rpc('runtime.start',session_id=uid(session),revision=revision)
    def test_schema_instantiation_source_independence_and_material(self):
        self.prepare();raw=self.world.read_bytes()
        rows=[self.result(r) for r in self.requests(rpc('world.describe'),rpc('entity.get',id=self.rig),
              rpc('entity.query',component='RigNode'),rpc('entity.query',component='SkinnedMesh'),
              rpc('entity.material',id=primitive(self.rig,0)))]
        schema,wrapper,bones,meshes,material=rows
        self.assertGreaterEqual(schema['schema_revision'],19)
        for kind in ['AnimationRig','RigNode','SkinnedMesh']:self.assertIn(kind,schema['components'])
        self.assertIn('animations',schema['methods']['runtime.step']['properties'])
        rig=wrapper['value']['components']['AnimationRig']
        self.assertEqual(rig,dict(asset=self.asset,clip=None,time=0,speed=1,loop=True,playing=False))
        bone_items=bones['entities'];mesh_items=meshes['entities']
        self.assertEqual({e['id'] for e in bone_items},{node(self.rig,i) for i in range(3)})
        self.assertEqual([e['id'] for e in mesh_items],[primitive(self.rig,0)])
        saved=json.loads(raw);entities=saved['entities']
        self.assertEqual(entities[node(self.rig,1)]['parent'],node(self.rig,2))
        self.assertEqual(entities[primitive(self.rig,0)]['components']['Transform'],transform())
        self.assertIn('PbrMaterial',entities[primitive(self.rig,0)]['components']);self.assertTrue(material)
        self.assertEqual(raw,self.world.read_bytes());REPORTS.append({'asset':self.asset,'editable_bones':3,'source_independent':True})
    def test_authored_bone_edits_and_atomic_rig_validation(self):
        self.prepare();tip=node(self.rig,1);skin=primitive(self.rig,0)
        self.result(self.requests(self.transact([component(tip,'Transform',transform((.25,3,0)))]))[0])
        before=self.world.read_bytes();doc=json.loads(before);rig=doc['entities'][self.rig]['components']['AnimationRig']
        skin_value=doc['entities'][skin]['components']['SkinnedMesh']
        invalid=[([{'op':'component.remove','id':tip,'type':'RigNode'}],-32602),
                 ([component(node(self.rig,0),'RigNode',{'rig':self.rig,'node':1})],-32602),
                 ([{'op':'entity.create','id':uid(999),'name':'Outside rig'},
                   {'op':'entity.reparent','id':tip,'parent':uid(999),'mode':'keep_local'}],-32602),
                 ([{'op':'component.remove','id':self.rig,'type':'AnimationRig'}],-32602),
                 ([component(skin,'SkinnedMesh',dict(skin_value,rig=uid(999)))],-32602),
                 ([component(skin,'SkinnedMesh',dict(skin_value,node=2))],-32602),
                 ([component(skin,'SkinnedMesh',dict(skin_value,primitive=9999))],-32050),
                 ([{'op':'component.remove','id':skin,'type':'SkinnedMesh'},
                   component(skin,'StaticMesh',{'asset':self.asset,'primitive':0,'visible':True})],-32050),
                 ([component(self.rig,'AnimationRig',dict(rig,clip=99))],-32602),
                 ([component(self.rig,'AnimationRig',dict(rig,speed=9))],-32602),
                 ([component(tip,'BoxCollider',{'half_extents':[.5,.5,.5],'motion':'static','mass':1,'friction':.5,'restitution':0})],-32602),
                 ([component(skin,'AcousticMaterial',{'absorption':[.1]*3,'transmission':[0]*3,'scattering':.5,'enabled':True})],-32602)]
        for ops,code in invalid:
            with self.subTest(ops=ops):self.error(self.requests(self.transact(ops,revision=2))[0],code);self.assertEqual(before,self.world.read_bytes())
        preview=self.result(self.requests(self.transact([component(tip,'Transform',transform((1,4,0)))],revision=2,preview=True))[0])
        self.assertFalse(preview['committed']);self.assertEqual(before,self.world.read_bytes())
    def test_all_model_nodes_and_rigid_animation(self):
        doc,blob=ribbon();doc.pop('skins');doc['nodes'][0].pop('skin')
        attrs=doc['meshes'][0]['primitives'][0]['attributes'];attrs.pop('JOINTS_0');attrs.pop('WEIGHTS_0')
        doc['nodes'].append({'name':'Nonselected animated node'});doc['animations'][0]['channels'][0]['target']['node']=3
        self.prepare(doc=doc,blob=blob)
        saved=json.loads(self.world.read_bytes())['entities'];self.assertIn(node(self.rig,3),saved)
        self.assertEqual(saved[node(self.rig,3)]['parent'],self.rig)
        self.assertIn('StaticMesh',saved[primitive(self.rig,0)]['components']);self.assertNotIn('SkinnedMesh',saved[primitive(self.rig,0)]['components'])
    def test_loading_broken_mapping_preserves_document(self):
        self.prepare();valid=self.world.read_bytes();document=json.loads(valid)
        document['entities'][node(self.rig,1)]['components'].pop('RigNode')
        raw=json.dumps(document).encode();self.world.write_bytes(raw)
        p=subprocess.run([BINARY,'world',self.native(self.world)],input=json.dumps(dict(rpc('world.inspect'),id=1))+'\n',
                         text=True,encoding='utf-8',capture_output=True,timeout=30)
        self.assertNotEqual(p.returncode,0,'A persisted incomplete rig opened successfully.')
        self.assertEqual(self.world.read_bytes(),raw)
        self.world.write_bytes(valid);self.result(self.requests(rpc('world.inspect'))[0])
    @unittest.skipUnless(args.runtime,'Native runtime not built in this configuration.')
    def test_authored_initial_playback_configuration(self):
        self.prepare();configuration=command(self.rig,time=.25,speed=2,loop=False);configuration.pop('entity');configuration['asset']=self.asset
        self.result(self.requests(self.transact([component(self.rig,'AnimationRig',configuration)]))[0]);before=self.world.read_bytes()
        rows=[self.result(r) for r in self.requests(self.start(revision=2),runtime_entity(node(self.rig,1)),runtime_entity(self.rig),
              step(0,15),runtime_entity(node(self.rig,1)),runtime_entity(self.rig))]
        self.assertAlmostEqual(rows[1]['local_transform']['position'][0],.25);self.assertAlmostEqual(rows[2]['animation']['time'],.25)
        self.assertAlmostEqual(rows[4]['local_transform']['position'][0],.75);self.assertAlmostEqual(rows[5]['animation']['time'],.75)
        self.assertEqual(before,self.world.read_bytes())
    @unittest.skipUnless(args.runtime,'Native runtime not built in this configuration.')
    def test_fixed_ticks_seek_pause_loop_clamp_and_rest(self):
        self.prepare();before=self.world.read_bytes();tip=node(self.rig,1)
        requests=[self.start(),step(0,30,[command(self.rig)]),runtime_entity(tip),runtime_entity(self.rig),
                  step(30,6,[command(self.rig,time=1.2,playing=False)]),runtime_entity(tip),runtime_entity(self.rig),
                  step(36,12,[command(self.rig,time=1.8,speed=2)]),runtime_entity(tip),runtime_entity(self.rig),
                  step(48,12,[command(self.rig,time=1.9,loop=False)]),runtime_entity(tip),runtime_entity(self.rig),
                  step(60,1,[command(self.rig,clip=None,time=99)]),runtime_entity(tip),runtime_entity(self.rig)]
        rows=[self.result(r) for r in self.requests(*requests)]
        for index,expected in [(2,.5),(5,1.2),(8,.2),(11,2),(14,0)]:self.assertAlmostEqual(rows[index]['local_transform']['position'][0],expected,places=6)
        self.assertTrue(rows[3]['animation']['playing']);self.assertFalse(rows[6]['animation']['playing'])
        self.assertFalse(rows[12]['animation']['playing']);self.assertEqual(rows[12]['animation']['time'],2)
        self.assertIsNone(rows[15]['animation']['clip']);self.assertFalse(rows[15]['animation']['playing']);self.assertEqual(rows[15]['animation']['time'],0)
        self.assertEqual(before,self.world.read_bytes())
    @unittest.skipUnless(args.runtime,'Native runtime not built in this configuration.')
    def test_authored_baseline_is_frozen_until_restart(self):
        self.prepare();tip=node(self.rig,1)
        self.result(self.requests(self.transact([component(tip,'Transform',transform((.25,3,0)))]))[0])
        rows=[self.result(r) for r in self.requests(self.start(revision=2),
              self.transact([component(tip,'Transform',transform((.75,5,0)))],revision=2),
              step(0,30,[command(self.rig)]),runtime_entity(tip),
              step(30,1,[command(self.rig,clip=None,playing=False)]),runtime_entity(tip),rpc('runtime.inspect',session_id=uid(900)),
              rpc('runtime.stop',session_id=uid(900)),self.start(session=901,revision=3),runtime_entity(tip,901))]
        self.assertAlmostEqual(rows[3]['local_transform']['position'][0],.5);self.assertEqual(rows[3]['local_transform']['position'][1:],[1,0])
        self.assertEqual(rows[5]['local_transform']['position'],[.25,3,0]);self.assertTrue(rows[6]['source_stale'])
        self.assertEqual(rows[9]['local_transform']['position'],[.75,5,0])
        saved=json.loads(self.world.read_bytes());self.assertEqual(saved['entities'][tip]['components']['Transform']['position'],[.75,5,0])
    @unittest.skipUnless(args.runtime,'Native runtime not built in this configuration.')
    def test_partition_equivalence_and_independent_instances(self):
        self.prepare(two=True);other=uid(200)
        commands=[command(self.rig),command(other,time=1,playing=False)]
        rows=[self.result(r) for r in self.requests(self.start(),step(0,90,commands),runtime_entity(node(self.rig,1)),runtime_entity(node(other,1)),runtime_entity(self.rig),
              rpc('runtime.stop',session_id=uid(900)),self.start(session=901),step(0,30,commands,session=901),step(30,60,session=901),
              runtime_entity(node(self.rig,1),901),runtime_entity(node(other,1),901),runtime_entity(self.rig,901))]
        for a,b in [(2,9),(3,10),(4,11)]:
            left=copy.deepcopy(rows[a]);right=copy.deepcopy(rows[b]);left.pop('session_id');right.pop('session_id');self.assertEqual(left,right)
        self.assertAlmostEqual(rows[2]['world_matrix'][12],1.5);self.assertAlmostEqual(rows[3]['world_matrix'][12],11)
    @unittest.skipUnless(args.runtime,'Native runtime not built in this configuration.')
    def test_receipts_invalid_commands_and_batch_atomicity(self):
        self.prepare();tip=node(self.rig,1);advance=step(0,30,[command(self.rig)],request_id=uid(400))
        changed=copy.deepcopy(advance);changed['params']['animations'][0]['speed']=2
        invalid=step(30,10,[command(self.rig,clip=99)],request_id=uid(401))
        corrected=step(30,10,[command(self.rig,time=.5)],request_id=uid(401))
        rows=self.requests(self.start(),advance,runtime_entity(tip),advance,runtime_entity(tip),changed,
                           step(0,1),invalid,runtime_entity(tip),rpc('runtime.inspect',session_id=uid(900)),corrected,runtime_entity(tip))
        first=self.result(rows[1]);self.assertEqual(self.result(rows[3]),dict(first,replayed=True))
        self.assertEqual(self.result(rows[2]),self.result(rows[4]));self.error(rows[5],-32010);self.error(rows[6],-32009)
        self.error(rows[7],-32040);self.assertEqual(self.result(rows[8]),self.result(rows[2]));self.assertEqual(self.result(rows[9])['tick'],30)
        self.assertEqual(self.result(rows[10])['tick'],40);self.assertAlmostEqual(self.result(rows[11])['local_transform']['position'][0],2/3)
    @unittest.skipUnless(args.runtime,'Native runtime not built in this configuration.')
    def test_invalid_animation_envelopes_do_not_advance(self):
        self.prepare();invalid=[]
        for field,value in [('time',-1),('speed',9),('loop',1),('playing','yes'),('clip',-1),('clip','0')]:
            value_command=command(self.rig);value_command[field]=value;invalid.append(step(0,1,[value_command]))
        duplicate=command(self.rig);invalid.append(step(0,1,[duplicate,duplicate]));invalid.append(step(0,1,[duplicate]*65))
        missing=command(self.rig);missing.pop('time');invalid.append(step(0,1,[missing]))
        rows=self.requests(self.start(),*invalid,rpc('runtime.inspect',session_id=uid(900)))
        for response in rows[1:-1]:self.error(response,-32602)
        self.assertEqual(self.result(rows[-1])['tick'],0)
    @unittest.skipUnless(args.runtime,'Native runtime not built in this configuration.')
    def test_cubic_scale_failure_rolls_back_all_ticks_and_pose(self):
        doc,blob=ribbon(cubic=True);doc['animations'][0]['channels'][0]['target']['path']='scale'
        sampler=doc['animations'][0]['samplers'][0];accessor=doc['accessors'][sampler['output']]
        view=doc['bufferViews'][accessor['bufferView']];data=bytearray(blob)
        # Positive endpoint keys, but the cubic x scale crosses zero inside.
        values=[(0,0,0),(1,1,1),(-4,0,0),(4,0,0),(1,1,1),(0,0,0)]
        struct.pack_into('<18f',data,view['byteOffset'],*[x for row in values for x in row])
        self.prepare(doc=doc,blob=bytes(data));before=self.world.read_bytes();tip=node(self.rig,1)
        rows=self.requests(self.start(),step(0,10,[command(self.rig)]),runtime_entity(tip),runtime_entity(self.rig),
                           step(10,50,request_id=uid(777)),runtime_entity(tip),runtime_entity(self.rig),rpc('runtime.inspect',session_id=uid(900)),
                           step(10,1,[command(self.rig,clip=None,playing=False)],request_id=uid(777)),runtime_entity(tip))
        self.result(rows[1]);self.error(rows[4],-32040)
        self.assertEqual(self.result(rows[2]),self.result(rows[5]));self.assertEqual(self.result(rows[3]),self.result(rows[6]))
        self.assertEqual(self.result(rows[7])['tick'],10);self.assertEqual(self.result(rows[8])['tick'],11)
        self.assertEqual(self.result(rows[9])['local_transform']['scale'],[1,1,1]);self.assertEqual(before,self.world.read_bytes())

if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(RuntimeAnimation))
    if args.evidence:args.evidence.write_text(json.dumps({'tests':result.testsRun,'skipped':len(result.skipped),
        'passed':result.wasSuccessful(),'runtime':bool(args.runtime),'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'reports':REPORTS},indent=2)+'\n')
    raise SystemExit(not result.wasSuccessful())
