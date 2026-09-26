#!/usr/bin/env python3
"""Exercise native static glTF import, immutable packages and authored instances."""
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
from gltf_fixture import write_fixture,sphere,glb
from texture_fixture import quad,png
import base64

parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--evidence',type=Path)
args=parser.parse_args();BINARY=str(args.binary.resolve());ROOT=Path(__file__).resolve().parents[1]
SCRATCH=ROOT/'build/asset-contract';SCRATCH.mkdir(parents=True,exist_ok=True)
REPORTS=[]
def uid(n):return f'{n:032x}'
def rpc(method,params):return {'jsonrpc':'2.0','method':method,'params':params}
def txn(rev,ops,request=None,preview=False):return rpc('world.transact',{'request_id':request or uuid.uuid4().hex,'base_revision':rev,'ops':ops,'preview':preview})
def instance(asset,root=100):return {'op':'asset.instantiate','id':uid(root),'asset':asset,'name':'Imported sphere'}
def material(roughness=.4):return {'base_color':[.6,.2,.05],'emissive':[0,0,0],'metallic':.8,'roughness':roughness,'double_sided':False}
def derive(root,suffix):return hashlib.sha256(('poima.instance.v1/'+uid(root)+'/'+suffix).encode()).hexdigest()[:32]

class Assets(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory(dir=SCRATCH);self.root=Path(self.directory.name);self.world=self.root/'世界 world.json'
        self.glb,self.gltf=write_fixture(self.root/'source assets')
    def tearDown(self):self.directory.cleanup()
    def native(self,path):
        path=str(path.resolve());return subprocess.check_output(['wslpath','-w',path],text=True).strip() if args.windows_interop else path
    def run_requests(self,requests):
        requests=[{**r,'id':i+1} for i,r in enumerate(requests)]
        p=subprocess.run([BINARY,'world',self.native(self.world)],input=''.join(json.dumps(r)+'\n' for r in requests),text=True,encoding='utf-8',capture_output=True,timeout=30)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr);responses=[json.loads(s) for s in p.stdout.splitlines()];self.assertEqual(len(responses),len(requests));return responses
    def result(self,r):self.assertIn('result',r,r);return r['result']
    def error(self,r,code):self.assertEqual(r['error']['code'],code,r)
    def imported(self):return self.result(self.run_requests([rpc('asset.import',{'source':self.native(self.glb)})])[0])
    def cache(self,asset):return Path(str(self.world)+'.assets')/(asset+'.pmodel')
    def test_glb_external_data_uri_and_shipping_package(self):
        doc,blob=sphere();import base64
        doc['buffers'][0]['uri']='data:application/octet-stream;base64,'+base64.b64encode(blob).decode()
        embedded=self.root/'source assets/embedded.gltf';embedded.write_text(json.dumps(doc))
        responses=self.run_requests([rpc('asset.import',{'source':self.native(p)}) for p in [self.glb,self.gltf,embedded,self.glb]])
        results=[self.result(r) for r in responses];self.assertTrue(all(r==results[0] for r in results));imported=results[0]
        data=self.cache(imported['asset']).read_bytes();self.assertEqual(hashlib.sha256(data).hexdigest(),imported['asset'])
        self.assertEqual(data[:8],b'POIMAM02');meta_len,blob_len=struct.unpack_from('<II',data,8);self.assertEqual(len(data),16+meta_len+blob_len)
        metadata=json.loads(data[16:16+meta_len]);self.assertEqual(metadata['nodes'][1]['parent'],0)
        self.assertEqual(imported['vertices'],1225);self.assertEqual(imported['triangles'],2208)
        position_normal=struct.unpack_from('<6f',data,16+meta_len);self.assertEqual(position_normal,(0,1,0,0,1,0))
        self.assertFalse(self.world.exists(),'Import should not create a scene revision.')
        self.assertEqual(len(list(self.cache(imported['asset']).parent.glob('*.pmodel'))),1)
        REPORTS.append({'asset':imported['asset'],'bytes':len(data),'vertices':imported['vertices'],'triangles':imported['triangles'],'source_forms_identical':True})
    def test_instance_hierarchy_preview_retry_edit_and_source_independence(self):
        asset=self.imported()['asset'];spawn=txn(0,[instance(asset)],uid(500));preview=copy.deepcopy(spawn);preview['params']['preview']=True;preview['params']['request_id']=uid(501)
        node=derive(100,'node/1');mesh=derive(100,'node/1/primitive/0')
        r=self.run_requests([preview,rpc('world.inspect',{}),spawn,spawn,rpc('entity.get',{'id':node}),rpc('entity.get',{'id':mesh}),
                             rpc('asset.inspect',{'asset':asset,'section':'nodes','limit':1}),rpc('asset.inspect',{'asset':asset,'section':'primitives'})])
        self.assertFalse(self.result(r[0])['committed']);self.assertEqual(self.result(r[1])['revision'],0);self.assertEqual(len(self.result(r[2])['changed_ids']),4)
        self.assertTrue(self.result(r[3])['replayed']);self.assertEqual(self.result(r[4])['value']['parent'],derive(100,'node/0'))
        self.assertEqual(self.result(r[5])['value']['components']['StaticMesh']['asset'],asset)
        self.assertEqual(self.result(r[6])['next_offset'],1);self.assertAlmostEqual(self.result(r[7])['items'][0]['material']['metallic'],.7,places=6)
        self.glb.unlink();self.gltf.unlink();(self.gltf.parent/'sphere.bin').unlink()
        edit=txn(1,[{'op':'component.set','id':mesh,'type':'PbrMaterial','value':material()}])
        r=self.run_requests([edit,rpc('entity.get',{'id':mesh,'component':'PbrMaterial'}),txn(2,[instance(asset,101)])])
        self.assertEqual(self.result(r[1])['value'],material());self.assertEqual(self.result(r[2])['revision'],3)
        # Instance root IDs are caller-stable and cannot be recreated after deletion.
        r=self.run_requests([txn(3,[{'op':'entity.delete','id':uid(100),'recursive':True}]),txn(4,[instance(asset)])]);self.result(r[0]);self.error(r[1],-32602)
    def test_unsupported_or_invalid_source_leaves_scene_and_cache_unchanged(self):
        original=self.imported()['asset'];self.run_requests([txn(0,[instance(original)])]);before=self.world.read_bytes();original_files=sorted(p.name for p in self.cache(original).parent.iterdir())
        base,blob=sphere(4,8);variants=[]
        doc=copy.deepcopy(base);doc['extensionsRequired']=['KHR_draco_mesh_compression'];variants.append(doc)
        doc=copy.deepcopy(base);doc['nodes'][1]['scale']=[-1,1,1];variants.append(doc)
        doc=copy.deepcopy(base);doc['nodes'][1]['matrix']=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];variants.append(doc)
        doc=copy.deepcopy(base);doc['accessors'][0]['count']=999999999;variants.append(doc)
        doc=copy.deepcopy(base);doc['materials'][0]['alphaMode']='BLEND';variants.append(doc)
        doc=copy.deepcopy(base);doc['images']=[{'uri':'missing.png'}];doc['textures']=[{'source':0}];doc['materials'][0]['pbrMetallicRoughness']['baseColorTexture']={'index':0};variants.append(doc)
        doc=copy.deepcopy(base);doc['nodes'][1]['children']=[0];variants.append(doc)
        for i,doc in enumerate(variants):
            path=self.glb.parent/f'bad-{i}.glb';path.write_bytes(glb(doc,blob));r=self.run_requests([rpc('asset.import',{'source':self.native(path)})]);self.error(r[0],-32050)
        doc=copy.deepcopy(base);doc['buffers'][0]['uri']='../outside.bin';outside=self.root/'outside.bin';outside.write_bytes(blob)
        path=self.glb.parent/'outside.gltf';path.write_text(json.dumps(doc));self.error(self.run_requests([rpc('asset.import',{'source':self.native(path)})])[0],-32050)
        self.assertEqual(before,self.world.read_bytes());self.assertEqual(original_files,sorted(p.name for p in self.cache(original).parent.iterdir()))
    def test_corrupt_cooked_data_and_failed_instance_are_rejected(self):
        asset=self.imported()['asset'];data=self.cache(asset).read_bytes();broken=bytearray(data);broken[-1]^=1;self.cache(asset).write_bytes(broken)
        r=self.run_requests([rpc('asset.inspect',{'asset':asset}),txn(0,[instance(asset)]),rpc('world.inspect',{})]);self.error(r[0],-32050);self.error(r[1],-32050);self.assertEqual(self.result(r[2])['revision'],0)
        # A self-consistent filename does not bypass binary format validation.
        malformed=b'not a model';bad=hashlib.sha256(malformed).hexdigest();self.cache(bad).write_bytes(malformed)
        self.error(self.run_requests([rpc('asset.inspect',{'asset':bad})])[0],-32050)
        self.cache(asset).write_bytes(data)
        invalid=txn(0,[instance(asset),{'op':'component.set','id':derive(100,'node/1/primitive/0'),'type':'PbrMaterial','value':material(2)}])
        r=self.run_requests([invalid,rpc('world.inspect',{})]);self.error(r[0],-32602);self.assertEqual(self.result(r[1])['revision'],0)
    def test_textures_source_forms_channels_mips_and_inspection(self):
        image=png(3,1,[0,0,0,255,0,0,0,255,255,255,255,255])
        material={'pbrMetallicRoughness':{'baseColorTexture':{'index':0},'metallicRoughnessTexture':{'index':0}},'emissiveTexture':{'index':0},'occlusionTexture':{'index':0,'strength':.3}}
        doc,blob=quad([image],material);paths=[]
        path=self.glb.parent/'image.glb';path.write_bytes(glb(doc,blob));paths.append(path)
        for form in ['external','data']:
            copydoc=copy.deepcopy(doc);copydoc['images'][0]={'uri':'my%20image.png' if form=='external' else 'data:image/png;base64,'+base64.b64encode(image).decode()}
            path=self.glb.parent/(form+'.glb');path.write_bytes(glb(copydoc,blob));paths.append(path)
        (self.glb.parent/'my image.png').write_bytes(image)
        results=[self.result(r) for r in self.run_requests([rpc('asset.import',{'source':self.native(p)}) for p in paths])]
        self.assertTrue(all(r==results[0] for r in results));asset=results[0]['asset'];self.assertEqual(results[0]['images'],2)
        inspected=self.run_requests([rpc('asset.inspect',{'asset':asset,'section':'images','limit':1}),rpc('asset.inspect',{'asset':asset,'section':'primitives'})])
        self.assertEqual(self.result(inspected[0])['items'][0]['color_space'],'srgb');self.assertEqual(self.result(inspected[0])['next_offset'],1)
        primitive=self.result(inspected[1])['items'][0];self.assertEqual(primitive['textures']['base_color']['image'],primitive['textures']['emissive']['image'])
        self.assertNotEqual(primitive['textures']['base_color']['image'],primitive['textures']['metallic_roughness']['image'])
        data=self.cache(asset).read_bytes();size=struct.unpack_from('<I',data,8)[0];meta=json.loads(data[16:16+size]);offset=16+size+meta['geometry_bytes']
        self.assertEqual(list(data[offset+12:offset+16]),[156,156,156,255])
        self.assertEqual(list(data[offset+28:offset+32]),[85,85,85,255])
        for p in paths:p.unlink()
        (self.glb.parent/'my image.png').unlink()
        self.result(self.run_requests([txn(0,[instance(asset)])])[0])
        REPORTS.append({'texture_asset':asset,'source_forms_identical':True,'srgb_and_linear_mips_verified':True})
    def test_jpeg_and_invalid_texture_imports(self):
        image=(ROOT/'tests/fixtures/solid.jpg').read_bytes();doc,blob=quad([image]);self.glb.write_bytes(glb(doc,blob));asset=self.imported()['asset']
        result=self.result(self.run_requests([rpc('asset.inspect',{'asset':asset,'section':'images'})])[0]);self.assertEqual(result['items'][0]['mips'][0]['width'],3)
        source,raw=quad([png(1,1,[255,255,255,255])]);variants=[]
        doc=copy.deepcopy(source);del doc['meshes'][0]['primitives'][0]['attributes']['TEXCOORD_0'];variants.append((doc,raw))
        doc=copy.deepcopy(source);doc['materials'][0]['pbrMetallicRoughness']['baseColorTexture']['texCoord']=1;variants.append((doc,raw))
        doc=copy.deepcopy(source);doc['materials'][0]['normalTexture']={'index':0};variants.append((doc,raw))
        doc=copy.deepcopy(source);doc['samplers'][0]['wrapS']=1;variants.append((doc,raw))
        doc=copy.deepcopy(source);doc['images'][0]={'uri':'../escape.png'};variants.append((doc,raw));(self.root/'escape.png').write_bytes(png(1,1,[0,0,0,255]))
        doc=copy.deepcopy(source);doc['images'][0]={'uri':'data:image/png;base64,===='};variants.append((doc,raw))
        doc,bad=quad([b'broken PNG data']);variants.append((doc,bad))
        for doc,blob in variants:
            self.glb.write_bytes(glb(doc,blob));self.error(self.run_requests([rpc('asset.import',{'source':self.native(self.glb)})])[0],-32050)
        self.assertEqual(len(list(self.cache(asset).parent.glob('*.pmodel'))),1)
    def test_texture_package_validation_and_legacy_load(self):
        doc,blob=quad([png(1,1,[64,128,255,255])]);self.glb.write_bytes(glb(doc,blob));asset=self.imported()['asset'];data=self.cache(asset).read_bytes()
        size=struct.unpack_from('<I',data,8)[0];metadata=json.loads(data[16:16+size]);binary=data[16+size:]
        for kind in ['sampler','color','mip','length']:
            meta=copy.deepcopy(metadata)
            if kind=='sampler':meta['primitives'][0]['textures'][0]['wrap_s']=0
            if kind=='color':meta['images'][0]['srgb']=False
            if kind=='mip':meta['images'][0]['mips'].append(meta['images'][0]['mips'][0])
            if kind=='length':meta['images'][0]['mips'][0]['bytes']=2**32
            header=json.dumps(meta).encode();bad=b'POIMAM02'+struct.pack('<II',len(header),len(binary))+header+binary;name=hashlib.sha256(bad).hexdigest();self.cache(name).write_bytes(bad)
            self.error(self.run_requests([rpc('asset.inspect',{'asset':name})])[0],-32050)
        doc,blob=sphere(4,8);self.glb.write_bytes(glb(doc,blob));asset=self.imported()['asset'];data=self.cache(asset).read_bytes();size=struct.unpack_from('<I',data,8)[0];meta=json.loads(data[16:16+size]);meta['version']=1;meta.pop('images');meta.pop('geometry_bytes')
        for p in meta['primitives']:p.pop('textures');p.pop('occlusion_strength')
        header=json.dumps(meta).encode();binary=data[16+size:];legacy=b'POIMAM01'+struct.pack('<II',len(header),len(binary))+header+binary;name=hashlib.sha256(legacy).hexdigest();self.cache(name).write_bytes(legacy)
        result=self.result(self.run_requests([rpc('asset.inspect',{'asset':name}),txn(0,[instance(name)])])[0]);self.assertEqual(result['format'],'poima.static-model.v1')
    def test_missing_normals_are_generated_flat(self):
        doc,blob=sphere(4,8);del doc['meshes'][0]['primitives'][0]['attributes']['NORMAL'];self.glb.write_bytes(glb(doc,blob))
        imported=self.imported();self.assertTrue(imported['diagnostics']);self.assertEqual(imported['vertices'],imported['triangles']*3)

if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Assets))
    if args.evidence:
        args.evidence.parent.mkdir(parents=True,exist_ok=True);args.evidence.write_text(json.dumps({'binary':BINARY,'binary_sha256':hashlib.sha256(Path(BINARY).read_bytes()).hexdigest(),'passed':result.wasSuccessful(),'tests':result.testsRun,'reports':REPORTS},indent=2)+'\n')
    raise SystemExit(0 if result.wasSuccessful() else 1)
