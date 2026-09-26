#!/usr/bin/env python3
"""glTF -> cooked package -> paginated agent inspection, independent of source."""
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
from animation_fixture import ribbon
from gltf_fixture import glb

parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--evidence',type=Path)
args=parser.parse_args();BINARY=str(args.binary.resolve());ROOT=Path(__file__).resolve().parents[1]
SCRATCH=ROOT/'build/animation-contract';SCRATCH.mkdir(parents=True,exist_ok=True)
REPORTS=[]
def rpc(method,**params):return {'jsonrpc':'2.0','method':method,'params':params}
class Animation(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=SCRATCH);self.root=Path(self.tmp.name);self.world=self.root/'world.json';self.source=self.root/'ribbon.glb'
    def tearDown(self):self.tmp.cleanup()
    def native(self,path):
        return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip() if args.windows_interop else str(path.resolve())
    def requests(self,*requests):
        process=subprocess.run([BINARY,'world',self.native(self.world)],input=''.join(json.dumps(dict(r,id=i))+'\n' for i,r in enumerate(requests)),text=True,capture_output=True,encoding='utf-8',timeout=30)
        self.assertEqual(process.returncode,0,process.stderr+process.stdout);out=[json.loads(line) for line in process.stdout.splitlines()];self.assertEqual(len(out),len(requests));return out
    def result(self,r):self.assertIn('result',r,r);return r['result']
    def imported(self,**options):
        doc,blob=ribbon(**options);self.source.write_bytes(glb(doc,blob));return self.result(self.requests(rpc('asset.import',source=self.native(self.source)))[0])
    def cache(self,asset):return Path(str(self.world)+'.assets')/(asset+'.pmodel')
    def test_package_inspection_and_source_independence(self):
        imported=self.imported();asset=imported['asset'];self.assertEqual(imported['format'],'poima.model.v4');self.assertEqual(imported['skins'],1);self.assertEqual(imported['animations'],1)
        package=self.cache(asset).read_bytes();self.assertEqual(package[:8],b'POIMAM04');self.assertEqual(hashlib.sha256(package).hexdigest(),asset)
        self.source.unlink()
        results=[self.result(r) for r in self.requests(rpc('asset.inspect',asset=asset,section='animations'),rpc('asset.inspect',asset=asset,section='skins'),rpc('asset.inspect',asset=asset,section='nodes',limit=1),rpc('asset.animation.skin',asset=asset,skin=0,limit=1),rpc('asset.animation.channel',asset=asset,clip=0,channel=0,offset=1,limit=1))]
        self.assertEqual(results[0]['items'],[{'index':0,'name':'Bend','duration':2.0,'channels':1}]);self.assertEqual(results[1]['items'][0]['joints'],2)
        self.assertEqual(results[2]['items'][0]['skin'],0);self.assertEqual(results[3]['items'][0]['node'],2);self.assertEqual(results[3]['next_offset'],1)
        self.assertEqual(results[4]['items'][0]['value'],[2,1,0,0]);self.assertIsNone(results[4]['next_offset']);self.assertFalse(self.world.exists())
        REPORTS.append({'asset':asset,'bytes':len(package),'source_independent':True,'format':imported['format']})
    def test_reference_vertices_loop_clamp_and_rest(self):
        asset=self.imported()['asset']
        requests=[rpc('asset.animation.sample',asset=asset,clip=0,time=t,loop=loop,section='vertices',node=0,primitive=0) for t,loop in [(0,False),(1,False),(3,False),(3,True)]]
        results=[self.result(r) for r in self.requests(*requests)]
        expected=[-.5,0,.5,0]
        for result,x in zip(results,expected):
            self.assertAlmostEqual(result['items'][2]['world_position'][0],x);self.assertEqual(result['items'][2]['world_position'][1],1)
            self.assertAlmostEqual(result['items'][2]['position'][0]+7,x);self.assertEqual(result['items'][2]['normal'],[0,0,1])
        result=self.result(self.requests(rpc('asset.animation.sample',asset=asset,time=99,offset=1,limit=1))[0]);self.assertEqual(result['items'][0]['position'],[0,1,0]);self.assertIsNone(result['clip']);self.assertEqual(result['next_offset'],2)
        result=self.result(self.requests(rpc('asset.animation.sample',asset=asset,clip=0,time=1,offset=100))[0]);self.assertEqual(result['items'],[]);self.assertIsNone(result['next_offset'])
    def test_cubic_tangents_and_quantized_weights(self):
        asset=self.imported(cubic=True,integer_weights=True)['asset']
        channel,pose=[self.result(r) for r in self.requests(rpc('asset.animation.channel',asset=asset,clip=0,channel=0),rpc('asset.animation.sample',asset=asset,clip=0,time=1,section='vertices',node=0,primitive=0))]
        self.assertEqual(channel['interpolation'],'CUBICSPLINE');self.assertEqual(channel['items'][0]['out_tangent'],[2,0,0,0])
        self.assertAlmostEqual(pose['items'][2]['world_position'][0],-.5+1.5*127/255,places=5)
    def test_flat_normal_expansion_keeps_influences(self):
        asset=self.imported(normals=False)['asset'];r=self.result(self.requests(rpc('asset.animation.sample',asset=asset,clip=0,time=1,section='vertices',node=0,primitive=0))[0])
        for item in r['items']:
            x,y,z=item['world_position'];expected_weight=y/2;self.assertAlmostEqual(item['weights'][1],expected_weight)
            self.assertTrue(abs(x-expected_weight-.5)<1e-5 or abs(x-expected_weight+.5)<1e-5)
    def test_invalid_requests_and_no_silent_static_instantiation(self):
        asset=self.imported()['asset'];base=dict(asset=asset,clip=0,time=1)
        requests=[rpc('asset.animation.sample',**dict(base,**v)) for v in [{'clip':999},{'time':-1},{'time':'1'},{'loop':1},{'section':'bad'},{'limit':65},{'node':0},{'section':'vertices'},{'section':'vertices','node':1,'primitive':0}]]
        requests+=[rpc('asset.animation.channel',asset=asset,clip=0,channel=99),rpc('asset.animation.skin',asset=asset,skin=1)]
        requests+=[rpc('world.transact',request_id='a'*32,base_revision=0,ops=[{'op':'asset.instantiate','id':'b'*32,'asset':asset,'name':'Rig'}])]
        for r in self.requests(*requests):self.assertEqual(r.get('error',{}).get('code'),-32602,r)
        self.assertFalse(self.world.exists())
    def test_invalid_imports_leave_packages_unchanged(self):
        asset=self.imported()['asset'];before=sorted(self.cache(asset).parent.iterdir());base,blob=ribbon();variants=[]
        for change in [lambda d:d['skins'][0].update(skeleton=0),lambda d:d['scenes'][0].update(nodes=[0]),lambda d:d['nodes'][0].pop('mesh'),lambda d:d['skins'][0].update(joints=[2,2]),lambda d:d['nodes'][2].pop('children'),lambda d:d['animations'][0]['channels'].append(copy.deepcopy(d['animations'][0]['channels'][0]))]:
            doc=copy.deepcopy(base);change(doc);variants.append(doc)
        for doc in variants:
            self.source.write_bytes(glb(doc,blob));r=self.requests(rpc('asset.import',source=self.native(self.source)))[0];self.assertEqual(r.get('error',{}).get('code'),-32050,r)
        self.assertEqual(sorted(self.cache(asset).parent.iterdir()),before);self.assertFalse(self.world.exists())
    def test_corrupt_package_checked_after_content_hash(self):
        asset=self.imported()['asset'];data=self.cache(asset).read_bytes();header_size,binary_size=struct.unpack_from('<II',data,8);meta=json.loads(data[16:16+header_size]);binary=data[16+header_size:]
        for change in [lambda d:d.update(animation_bytes=0),lambda d:d['animations'][0]['channels'][0].update(keys=1000001),lambda d:d['skins'][0].update(joints=[2,2]),lambda d:d['nodes'][0].update(skin=63)]:
            altered=copy.deepcopy(meta);change(altered);header=json.dumps(altered).encode();broken=data[:8]+struct.pack('<II',len(header),len(binary))+header+binary;bad=hashlib.sha256(broken).hexdigest();self.cache(bad).write_bytes(broken)
            r=self.requests(rpc('asset.inspect',asset=bad))[0];self.assertEqual(r.get('error',{}).get('code'),-32050,r)
    def test_rigid_animation_without_skin(self):
        doc,blob=ribbon();doc.pop('skins');doc['nodes'][0].pop('skin')
        attrs=doc['meshes'][0]['primitives'][0]['attributes'];attrs.pop('JOINTS_0');attrs.pop('WEIGHTS_0')
        doc['animations'][0]['channels'][0]['target']['node']=0
        self.source.write_bytes(glb(doc,blob));asset=self.result(self.requests(rpc('asset.import',source=self.native(self.source)))[0])['asset']
        result=self.result(self.requests(rpc('asset.animation.sample',asset=asset,clip=0,time=1,section='vertices',node=0,primitive=0))[0])
        self.assertEqual(result['items'][0]['world_position'],[.5,1,0]);self.assertNotIn('weights',result['items'][0])
    def test_missing_bind_matrices_skin_only_and_unused_weights(self):
        doc,blob=ribbon();doc.pop('animations');doc['skins'][0].pop('inverseBindMatrices')
        self.source.write_bytes(glb(doc,blob));asset=self.result(self.requests(rpc('asset.import',source=self.native(self.source)))[0])['asset']
        result=self.result(self.requests(rpc('asset.animation.sample',asset=asset,time=0,section='vertices',node=0,primitive=0))[0])
        self.assertEqual(result['items'][4]['world_position'],[-.5,3,0])
        doc.pop('skins');doc['nodes'][0].pop('skin');doc['nodes'][0].pop('mesh')
        self.source.write_bytes(glb(doc,blob));imported=self.result(self.requests(rpc('asset.import',source=self.native(self.source)))[0])
        self.assertEqual(imported['format'],'poima.model.v4');self.assertEqual(imported['skins'],0)
        primitive=self.result(self.requests(rpc('asset.inspect',asset=imported['asset'],section='primitives'))[0])['items'][0]
        self.assertTrue(primitive['skinned'])
    def test_capture_validation_does_not_create_output(self):
        asset=self.imported()['asset'];camera='1'*32
        ops=[{'op':'entity.create','id':camera,'name':'Camera'},{'op':'component.set','id':camera,'type':'Camera','value':{'vertical_fov':60,'near':.1,'far':100}}]
        self.result(self.requests(rpc('world.transact',request_id='a'*32,base_revision=0,ops=ops))[0]);before=self.world.read_bytes();output=self.root/'capture.bmp'
        base=dict(asset=asset,time=1,revision=1,camera=camera,path=self.native(output))
        for change,code in [({'asset':'0'*64},-32050),({'clip':99},-32602),({'time':-1},-32602),({'loop':1},-32602),({'revision':0},-32009)]:
            result=self.requests(rpc('asset.animation.capture',**dict(base,**change)))[0];self.assertEqual(result.get('error',{}).get('code'),code,result)
        self.assertFalse(output.exists());self.assertEqual(before,self.world.read_bytes())
    def test_schema_discovery(self):
        schema=self.result(self.requests(rpc('world.describe'))[0]);self.assertGreaterEqual(schema['schema_revision'],15)
        for method in ['asset.animation.sample','asset.animation.channel','asset.animation.skin','asset.animation.capture']:self.assertIn(method,schema['methods'])

if __name__=='__main__':
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(Animation);result=unittest.TextTestRunner(verbosity=2).run(suite)
    if args.evidence:args.evidence.write_text(json.dumps({'tests':result.testsRun,'passed':result.wasSuccessful(),'reports':REPORTS},indent=2)+'\n')
    raise SystemExit(not result.wasSuccessful())
