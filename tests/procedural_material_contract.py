#!/usr/bin/env python3
"""Native material recipes, immutable PBR outputs and explicit authoring transactions."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
sys.path.insert(0,str(ROOT/'tools'))
from poima_client import WorldClient,RpcError
from benchmark_shared_session import OwnedHost
from gltf_fixture import glb
from texture_fixture import quad,png
ARGS=None
REPORTS=[]


def native(path):
    value=str(Path(path).resolve())
    return subprocess.check_output(['wslpath','-w',value],text=True,timeout=10).strip() if ARGS.windows_interop else value

def recipe(kind='brick',seed=1,size=64):
    return dict(format='poima.material.recipe.v1',kind=kind,seed=seed,width=size,height=size)

def uid(n):return '{:032x}'.format(n)

class Materials(unittest.TestCase):
    def setUp(self):
        scratch=ROOT/'build/procedural-material-contract';scratch.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=scratch);self.root=Path(self.temp.name);self.world=self.root/'world.json'
        self.clients=[];self.hosts=[];self.calls=[];self.client=self.open()
    def open(self):
        client=WorldClient.open(str(ARGS.binary.resolve()),native(self.world),close_timeout=5);self.clients.append(client);return client
    def tearDown(self):
        cleanup=[];errors=[]
        for client in reversed(self.clients):
            try:client.close()
            except BaseException as error:errors.append(str(error))
            cleanup.append(dict(exit_code=client.transport.returncode,closed=client.closed))
            if client.transport.returncode!=0 or not client.closed:errors.append('Owned client failed zero-exit cleanup')
        for host in reversed(self.hosts):
            try:
                closed=host.close(getattr(host,'material_shutdown',False),5);cleanup.append(closed)
                if closed['exit_code']!=0 or closed['forced'] or closed['diagnostic_error']:errors.append('Owned host failed graceful cleanup')
            except BaseException as error:errors.append(str(error))
        REPORTS.append(dict(test=self.id(),calls=self.calls,cleanup=cleanup,cleanup_errors=errors));self.temp.cleanup();self.assertEqual(errors,[])
    def call(self,method,params=None,code=None,client=None):
        row=dict(method=method,params={} if params is None else params);self.calls.append(row)
        try:result=(client or self.client).call(method,params,timeout=20)
        except RpcError as error:
            row['error']=dict(code=error.code,message=error.message);self.assertEqual(error.code,code,row);return row['error']
        row['result']=result;self.assertIsNone(code,row);return result
    def generate(self,source,client=None):
        submitted=self.call('asset.material.generate',dict(recipe=source),client=client)
        deadline=time.monotonic()+20
        while True:
            self.assertLess(time.monotonic(),deadline,'Bake deadline exceeded')
            job=self.call('asset.material.job',dict(id=submitted['id']),client=client)
            if job['state']!='baking':break
            time.sleep(.001)
        self.assertEqual(job['state'],'succeeded',job);return job['result']
    def tx(self,revision,ops,**extra):
        return self.call('world.transact',dict(base_revision=revision,request_id=uuid.uuid4().hex,ops=ops,**extra))
    def test_recipe_discovery_dedup_channels_and_reopen(self):
        full=self.call('world.describe');names=['generate','job','jobs','cancel','forget','inspect']
        for name in names:self.assertIn('asset.material.'+name,full['methods'])
        method=self.call('world.describe',dict(view='method',name='asset.material.generate'))
        self.assertEqual(set(method['methods']),{'asset.material.generate'})
        before=self.call('world.inspect');history=self.call('world.history')
        source=recipe('plaster');source.update(color='#204080',roughness=.5,roughness_variation=0,color_variation=0,grain_m=0)
        start=self.call('asset.material.generate',dict(recipe=source));self.assertEqual(start['state'],'baking')
        duplicate=self.call('asset.material.generate',dict(recipe=source));self.assertEqual(duplicate['id'],start['id'])
        result=self.generate(source);self.assertEqual(result['recipe_id'],start['id'])
        canonical=self.call('asset.material.generate',dict(recipe=result['recipe']));self.assertEqual(canonical['id'],start['id'])
        self.assertEqual(self.call('world.inspect'),before);self.assertEqual(self.call('world.history'),history)
        self.assertFalse(self.world.exists(),'Generation must not persist an authored-world edit')
        self.assertEqual(result['PbrMaterial']['roughness'],1);self.assertEqual(result['PbrMaterial']['metallic'],1)
        self.assertIsNone(result['PbrTextures']['emissive']);self.assertIsNone(result['PbrTextures']['occlusion'])
        store=Path(str(self.world)+'.assets')
        for slot,expected in [('base_color',[32,64,128,255]),('normal',[128,128,255,255]),('metallic_roughness',[0,128,0,255])]:
            fact=result['maps'][slot];data=(store/(fact['asset']+'.pimage')).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(),fact['asset']);self.assertEqual(data[:8],b'POIMAI01')
            header_length=int.from_bytes(data[8:12],'little');header=json.loads(data[16:16+header_length]);pixels=data[16+header_length:]
            self.assertEqual(header['srgb'],slot=='base_color');self.assertEqual(header['mips'][0]['width'],64)
            self.assertEqual(len(pixels),sum(m['bytes'] for m in header['mips']))
            self.assertEqual(pixels,bytes(expected)*(len(pixels)//4))
        descriptor=store/(result['recipe_id']+'.pmaterial');original=descriptor.read_bytes()
        self.client.close();self.client=self.open()
        self.assertEqual(self.call('asset.material.inspect',dict(recipe=result['recipe_id'])),result)
        again=self.generate(result['recipe']);self.assertEqual(again,result);self.assertEqual(descriptor.read_bytes(),original)
    def test_invalid_closed_inputs_and_owner_state_preserved(self):
        before=self.call('world.inspect');history=self.call('world.history')
        changes=[dict(seed=True),dict(seed=-1),dict(seed=2**32),dict(width=63),dict(width=513),dict(height=64.5),dict(rows=3),dict(columns=0),dict(grain_m=.02),dict(roughness=.01),dict(color='#ABCDEF'),dict(unknown=0),dict(joint_width_m=.2)]
        for change in changes:
            with self.subTest(change=change):self.call('asset.material.generate',dict(recipe=dict(recipe(),**change)),code=-32602)
        self.call('asset.material.generate',dict(recipe=dict(recipe('plaster'),rows=2)),code=-32602)
        self.call('asset.material.generate',dict(recipe=recipe(),request_id=uid(1)),code=-32602)
        for value in (None,True,uid(1),'A'*64):self.call('asset.material.job',dict(id=value),code=-32602)
        self.call('asset.material.job',dict(id='f'*64),code=-32004)
        self.assertEqual(self.call('asset.material.jobs'),{'jobs':[]});self.assertEqual(self.call('world.inspect'),before);self.assertEqual(self.call('world.history'),history)
        self.assertFalse(Path(str(self.world)+'.assets').exists())
    def test_cancel_retention_forget_and_no_lifetime_cap(self):
        submitted=self.call('asset.material.generate',dict(recipe=recipe(size=512)));identity=submitted['id']
        self.call('asset.material.forget',dict(id=identity),code=-32080)
        self.call('asset.material.cancel',dict(id=identity));deadline=time.monotonic()+20
        while True:
            status=self.call('asset.material.job',dict(id=identity))
            if status['state']!='baking':break
            self.assertLess(time.monotonic(),deadline);time.sleep(.001)
        self.assertEqual(status['state'],'cancelled');self.assertFalse((Path(str(self.world)+'.assets')/(identity+'.pmaterial')).exists())
        self.call('asset.material.forget',dict(id=identity));self.call('asset.material.job',dict(id=identity),code=-32004)
        for seed in range(8):self.generate(recipe('plaster',seed))
        self.assertEqual(len(self.call('asset.material.jobs')['jobs']),8)
        self.call('asset.material.generate',dict(recipe=recipe('plaster',100)),code=-32080)
        forgotten=self.call('asset.material.jobs')['jobs'][0]['id'];self.call('asset.material.forget',dict(id=forgotten))
        self.generate(recipe('plaster',100));self.assertEqual(len(self.call('asset.material.jobs')['jobs']),8)
    def test_guarded_material_apply_history_references_and_export_closure(self):
        material=self.generate(recipe());image=png(1,1,[255]*4);doc,blob=quad([image]);source=self.root/'quad.glb';source.write_bytes(glb(doc,blob))
        model=self.call('asset.import',dict(source=native(source)))['asset'];source.unlink()
        ops=[dict(op='entity.create',id=uid(1),name='Recipe surface',parent=None),dict(op='component.set',id=uid(1),type='StaticMesh',value=dict(asset=model,primitive=0,visible=True))]
        self.tx(0,ops)
        apply=[dict(op='component.set',id=uid(1),type=key,value=material[key]) for key in ('PbrMaterial','PbrTextures')]
        preview=self.tx(1,apply,preview=True);self.assertEqual(preview['revision'],2);self.assertEqual(self.call('world.inspect')['revision'],1)
        self.call('world.transact',dict(base_revision=0,request_id=uuid.uuid4().hex,ops=apply),code=-32009)
        request=dict(base_revision=1,request_id=uuid.uuid4().hex,ops=apply)
        committed=self.call('world.transact',request);replayed=self.call('world.transact',request)
        self.assertFalse(committed['replayed']);self.assertTrue(replayed['replayed'])
        self.assertEqual({k:v for k,v in committed.items() if k!='replayed'},{k:v for k,v in replayed.items() if k!='replayed'})
        effective=self.call('entity.material',dict(id=uid(1),revision=2))
        for slot in ('base_color','normal','metallic_roughness'):self.assertEqual(effective['textures'][slot]['asset'],material['maps'][slot]['asset'])
        edges=self.call('world.asset.references')['edges'];image_ids={row['asset'] for row in edges if row['kind']=='image'}
        self.assertEqual(image_ids,{m['asset'] for m in material['maps'].values()})
        closure=self.call('world.dependencies')['assets'];filenames={entry['filename'] for entry in closure}
        self.assertEqual(filenames,{model+'.pmodel'}|{asset+'.pimage' for asset in image_ids})
        self.assertNotIn(material['recipe_id']+'.pmaterial',filenames)
        self.call('world.undo',dict(base_revision=2,request_id=uuid.uuid4().hex));self.assertIsNone(self.call('entity.material',dict(id=uid(1)))['textures']['normal'])
        self.call('world.redo',dict(base_revision=3,request_id=uuid.uuid4().hex));self.assertEqual(self.call('entity.material',dict(id=uid(1)))['textures']['normal']['asset'],material['maps']['normal']['asset'])
        self.client.close();self.client=self.open();self.assertEqual(self.call('entity.material',dict(id=uid(1)))['textures']['normal']['asset'],material['maps']['normal']['asset'])
        package=Path(str(self.world)+'.assets')/(material['maps']['normal']['asset']+'.pimage');original=package.read_bytes();package.write_bytes(original+b'corrupt')
        self.call('asset.material.inspect',dict(recipe=material['recipe_id']),code=-32050);package.write_bytes(original)
    def test_stored_descriptor_corruption_and_profile_reuse_policy(self):
        source=recipe(seed=17);original=self.generate(source);other=self.generate(recipe(seed=18))
        descriptor=Path(str(self.world)+'.assets')/(original['recipe_id']+'.pmaterial');raw=descriptor.read_bytes();stored=json.loads(raw)
        for patch in ('recipe','facts','unknown'):
            altered=copy.deepcopy(stored)
            if patch=='recipe':altered['recipe']['color']='#INVALID'
            elif patch=='facts':altered['facts']['height']='unverified claim'
            else:altered['facts']['unexpected']='unverified claim'
            descriptor.write_text(json.dumps(altered),encoding='utf-8')
            self.call('asset.material.inspect',dict(recipe=original['recipe_id']),code=-32050)
            self.assertEqual(self.call('world.inspect')['revision'],0)
        descriptor.write_bytes(raw)
        swapped=copy.deepcopy(stored);swapped['maps']=other['maps'];swapped['PbrTextures']=other['PbrTextures'];descriptor.write_text(json.dumps(swapped),encoding='utf-8')
        inspected=self.call('asset.material.inspect',dict(recipe=original['recipe_id']))
        self.assertEqual(inspected['maps'],other['maps'],'Inspection verifies coherence, not recipe derivation authentication')
        self.call('asset.material.forget',dict(id=original['recipe_id']))
        submitted=self.call('asset.material.generate',dict(recipe=source));deadline=time.monotonic()+20
        while True:
            failed=self.call('asset.material.job',dict(id=submitted['id']))
            if failed['state']!='baking':break
            self.assertLess(time.monotonic(),deadline);time.sleep(.001)
        self.assertEqual(failed['state'],'failed');self.assertIsNone(failed['result'])
        self.assertIn('same-profile',failed['diagnostic']);self.assertLessEqual(len(failed['diagnostic'].encode('utf-8')),1024)
        self.assertEqual(json.loads(descriptor.read_bytes()),swapped,'Rejected regeneration must not replace the existing descriptor')
        self.call('asset.material.forget',dict(id=original['recipe_id']))
        foreign=copy.deepcopy(swapped);foreign['numerical_profile']['compiler_version']='unqualified-other-profile';descriptor.write_text(json.dumps(foreign),encoding='utf-8')
        reused=self.generate(source)
        self.assertEqual(reused['maps'],other['maps']);self.assertEqual(reused['numerical_profile'],foreign['numerical_profile'])
        self.assertEqual(json.loads(descriptor.read_bytes()),foreign,'Cross-profile reuse must retain the original descriptor and producer')
        descriptor.write_bytes(raw)
    def test_shared_clients_bake_and_explicit_owner_poll(self):
        self.tx(0,[dict(op='entity.create',id=uid(1),name='Shared owner',parent=None)]);self.client.close()
        endpoint='material_'+uuid.uuid4().hex;host=OwnedHost(ARGS.binary.resolve(),native(self.world),endpoint);self.hosts.append(host)
        first=WorldClient.connect(str(ARGS.binary.resolve()),endpoint,timeout_ms=12000,close_timeout=5);second=WorldClient.connect(str(ARGS.binary.resolve()),endpoint,timeout_ms=12000,close_timeout=5);self.clients.extend([first,second])
        source=recipe(size=512);submitted=self.call('asset.material.generate',dict(recipe=source),client=first)
        before=self.call('world.inspect',client=second);history=self.call('world.history',client=second)
        intervals=[]
        for unused in range(10):
            start=time.perf_counter();self.assertEqual(self.call('world.inspect',client=second),before);intervals.append((time.perf_counter()-start)*1000)
        self.assertEqual(self.call('asset.material.generate',dict(recipe=source),client=second)['id'],submitted['id'])
        result=self.generate(source,client=second);self.assertEqual(result['recipe_id'],submitted['id'])
        self.assertEqual(self.call('world.inspect',client=first),before);self.assertEqual(self.call('world.history',client=first),history)
        REPORTS.append(dict(test='shared_owner_inspect_observations',samples_ms=intervals,note='bounded authoring responsiveness fixture; not a gameplay performance claim'))
        self.assertTrue(self.call('host.shutdown',client=first)['closed']);host.material_shutdown=True


def main():
    global ARGS
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--output',type=Path)
    ARGS,remaining=parser.parse_known_args();suite=unittest.defaultTestLoader.loadTestsFromTestCase(Materials);result=unittest.TextTestRunner(verbosity=2).run(suite)
    if ARGS.output:ARGS.output.parent.mkdir(parents=True,exist_ok=True);ARGS.output.write_text(json.dumps(dict(passed=result.wasSuccessful(),tests_run=result.testsRun,failures=len(result.failures),errors=len(result.errors),reports=REPORTS),indent=2))
    return 0 if result.wasSuccessful() else 1
if __name__=='__main__':sys.exit(main())
