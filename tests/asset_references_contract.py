#!/usr/bin/env python3
"""Real authored asset provenance, bounded pages and separate package resolution."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
import wave

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
sys.path.insert(0,str(ROOT/'tools'))
from poima_client import WorldClient, RpcError
from benchmark_shared_session import OwnedHost
from gltf_fixture import glb
from texture_fixture import png,quad
from animation_fixture import ribbon
ARGS=None
REPORTS=[]
TRANSFORM={'position':[0,0,0],'rotation':[0,0,0,1],'scale':[1,1,1]}
METHOD='world.asset.references'


def uid(n):return '{:032x}'.format(n)
def native(path):
    resolved=str(Path(path).resolve())
    return subprocess.check_output(['wslpath','-w',resolved],text=True,timeout=10).strip() if ARGS.windows_interop else resolved
def edge(kind,n,component,path,asset,package,subresource=None):
    return dict(owner=dict(kind=kind,id=uid(n) if isinstance(n,int) else n),component=component,path=path,
                asset=asset,kind=package,subresource=subresource)
def cursor(row):return {key:copy.deepcopy(row[key]) for key in ('owner','component','path')}
def ordering(row):return (row['owner']['kind'],row['owner']['id'],row['component'],row['path'])
def create(n):return dict(op='entity.create',id=uid(n),name='Owner '+str(n))
def component(n,kind,value):return dict(op='component.set',id=uid(n),type=kind,value=value)
def texture(asset,image=None):return {'asset':asset} if image is None else {'asset':asset,'image':image}


class References(unittest.TestCase):
    def setUp(self):
        scratch=ROOT/'build/asset-references-contract';scratch.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=scratch);self.root=Path(self.temp.name);self.world=self.root/'world.json'
        self.clients=[];self.calls=[];self.hosts=[]
        self.client=self.open(self.world)
    def open(self,path):
        client=WorldClient.open(str(ARGS.binary.resolve()),native(path),close_timeout=5)
        self.clients.append(client);return client
    def tearDown(self):
        errors=[];processes=[]
        for client in reversed(self.clients):
            try:client.close()
            except BaseException as failure:errors.append(str(failure))
            processes.append(dict(exit_code=client.transport.returncode,closed=client.closed))
            if client.transport.returncode!=0 or not client.closed:errors.append('Owned client exit differs from zero')
        for host in reversed(self.hosts):
            try:
                cleanup=host.close(getattr(host,'references_shutdown',False),5);processes.append(cleanup)
                if cleanup['exit_code']!=0 or cleanup['forced'] or cleanup['diagnostic_error']:errors.append('Owned host did not shut down cleanly')
            except BaseException as failure:errors.append(str(failure))
        REPORTS.append(dict(test=self.id(),calls=self.calls,processes=processes,cleanup_errors=errors))
        self.temp.cleanup();self.assertEqual(errors,[])
    def call(self,method,params=None,code=None,client=None):
        row=dict(method=method,params={} if params is None else params);self.calls.append(row)
        try:value=(client or self.client).call(method,params,timeout=20)
        except RpcError as failure:
            row['error']=dict(code=failure.code,message=failure.message)
            self.assertEqual(failure.code,code,row);return row['error']
        row['result']=value;self.assertIsNone(code,row);return value
    def tx(self,revision,ops,client=None):
        return self.call('world.transact',dict(base_revision=revision,request_id=uuid.uuid4().hex,ops=ops),client=client)
    def preserved(self):
        files={}
        for path in self.root.rglob('*'):
            # The native owner holds this coordination lock exclusively on
            # Windows. Authored content, history and every other file remain
            # checked; unrelated permission or I/O errors still fail the test.
            if path==Path(str(self.world)+'.lock'):
                continue
            if path.is_file():
                files[str(path.relative_to(self.root))]=hashlib.sha256(path.read_bytes()).hexdigest()
        return self.call('world.inspect'),self.call('world.history'),files
    def assert_preserved(self,before):self.assertEqual(self.preserved(),before)
    def authored_fixture(self):
        # Normal authoring resolves rig/skin and template bindings. Synthetic
        # package identities and maximal indices belong to the pure collector.
        return self.real_fixture()[0]
    def test_empty_discovery_and_legacy_no_upgrade(self):
        self.tx(0,[create(1)])
        self.assertEqual(json.loads(self.world.read_text())['version'],1)
        before=self.preserved();full=self.call('world.describe')
        self.assertEqual(full['protocol_version'],1);self.assertEqual(full['schema_revision'],55)
        self.assertEqual(full['authoring_contract']['version'],1)
        focused=self.call('world.describe',dict(view='method',name=METHOD))
        self.assertEqual(focused['methods'],{METHOD:full['methods'][METHOD]})
        result=self.call(METHOD)
        self.assertEqual(result,dict(revision=1,scope='authored',selection={},edges=[],next_after=None))
        self.assertEqual(self.call(METHOD,dict(owner=dict(kind='entity',id=uid(1))))['edges'],[])
        self.assert_preserved(before)
    def test_complete_typed_edges_namespaces_and_no_package_io(self):
        expected=self.authored_fixture()
        audio=next(row['asset'] for row in expected if row['component']=='AudioEmitter')
        (Path(str(self.world)+'.assets')/(audio+'.paudio')).unlink()
        before=self.preserved()
        self.assertEqual(self.call(METHOD)['edges'],expected)
        for kind in ('entity','template'):
            result=self.call(METHOD,dict(owner=dict(kind=kind,id=uid(1))))
            self.assertEqual(result['selection'],dict(owner=dict(kind=kind,id=uid(1))))
            self.assertEqual(result['edges'],[row for row in expected if row['owner']==dict(kind=kind,id=uid(1))])
        unused=self.call(METHOD,dict(asset='f'*64))
        self.assertEqual(unused['edges'],[]);self.assertIsNone(unused['next_after'])
        # The existing standalone image/audio storage boundary reports -32000;
        # model resolution has a different wrapped -32050 error convention.
        self.call('world.dependencies',code=-32000)
        self.assert_preserved(before)
    def test_pages_exact_final_and_filtered_lookahead(self):
        expected=self.authored_fixture();before=self.preserved()
        animated=next(row['asset'] for row in expected if row['component']=='AnimationRig')
        for selection in ({},{'asset':animated},{'owner':dict(kind='template',id=uid(1))}):
            wanted=[row for row in expected if not selection or
                ('asset' in selection and row['asset']==selection['asset']) or
                ('owner' in selection and row['owner']==selection['owner'])]
            for limit in (1,2,256):
                params=dict(selection,limit=limit);collected=[]
                for page_number in range(20):
                    result=self.call(METHOD,params);self.assertEqual(result['selection'],selection)
                    self.assertEqual(result['revision'],1);self.assertLessEqual(len(result['edges']),limit)
                    collected.extend(result['edges'])
                    remaining=wanted[len(collected):]
                    self.assertEqual(result['next_after'],cursor(result['edges'][-1]) if remaining else None)
                    if result['next_after'] is None:break
                    params=dict(selection,revision=1,after=result['next_after'],limit=limit)
                else:self.fail('Pagination failed to advance')
                self.assertEqual(collected,wanted)
        gap=dict(owner=dict(kind='entity',id=uid(1)),component='PbrTextures',path='/occlusion/asset')
        self.assertEqual(self.call(METHOD,dict(revision=1,after=gap))['edges'],[row for row in expected if ordering(row)>ordering(gap)])
        self.assertEqual(self.call(METHOD,dict(revision=1,after=dict(owner=dict(kind='template',id=uid(999)),component='StaticMesh',path='/asset')))['edges'],[])
        self.assert_preserved(before)
    def test_strict_nested_guards_errors_and_unchanged_history(self):
        expected=self.authored_fixture();before=self.preserved();after=cursor(expected[0])
        invalid=[{'unknown':True},{'asset':None},{'asset':'A'*64},{'asset':'0'*63},
            {'owner':None},{'owner':{'kind':'entity'}},{'owner':dict(kind='asset',id=uid(1))},
            {'owner':dict(kind='entity',id=uid(1),extra=0)}, {'owner':dict(kind='entity',id=True)},
            {'asset':'a'*64,'owner':dict(kind='entity',id=uid(1))},{'after':after},
            {'revision':1,'after':None},{'revision':1,'after':dict(after,extra=0)},
            {'revision':1,'after':dict(after,path='/base_color')},
            {'revision':1,'after':dict(after,component='MeshRenderer')},
            {'revision':1,'after':dict(owner=dict(kind='template',id=uid(1)),component='AudioEmitter',path='/asset')},
            {'revision':1,'after':dict(after,owner=dict(kind='entity',id=uid(1),extra=0))}]
        invalid += [{'revision':value} for value in (None,True,-1,1.0,.5,9007199254740992)]
        invalid += [{'limit':value} for value in (None,True,0,257,1.0,.5,9007199254740992)]
        for params in invalid:
            with self.subTest(params=params):self.call(METHOD,params,code=-32602)
        for revision in (0,2,9007199254740991):self.call(METHOD,dict(revision=revision),code=-32009)
        self.call(METHOD,dict(owner=dict(kind='entity',id=uid(999))),code=-32004)
        self.call(METHOD,dict(owner=dict(kind='template',id=uid(2))),code=-32004)
        self.assert_preserved(before)
    def test_deleted_reference_history_and_stale_continuation(self):
        expected=self.authored_fixture();first=self.call(METHOD,dict(limit=1))
        self.tx(1,[dict(op='entity.delete',id=uid(3),recursive=False)])
        self.call(METHOD,dict(revision=1,after=first['next_after']),code=-32009)
        remaining=[row for row in expected if row['owner']!=dict(kind='entity',id=uid(3))]
        self.assertEqual(self.call(METHOD)['edges'],remaining)
        self.call(METHOD,dict(owner=dict(kind='entity',id=uid(3))),code=-32004)
        self.call('world.undo',dict(base_revision=2,request_id=uuid.uuid4().hex))
        self.assertEqual(self.call(METHOD)['edges'],expected)
        self.call('world.redo',dict(base_revision=3,request_id=uuid.uuid4().hex))
        self.assertEqual(self.call(METHOD)['edges'],remaining)
    def real_fixture(self):
        image=png(1,1,[100,140,220,255]);model_doc,blob=quad([image]);source=self.root/'model.glb';source.write_bytes(glb(model_doc,blob))
        model=self.call('asset.import',dict(source=native(source)))['asset']
        other_doc,other_blob=quad([png(1,1,[80,20,10,255])]);source.write_bytes(glb(other_doc,other_blob))
        other=self.call('asset.import',dict(source=native(source)))['asset']
        rdoc,rblob=ribbon();source.write_bytes(glb(rdoc,rblob));animated=self.call('asset.import',dict(source=native(source)))['asset'];source.unlink()
        image_path=self.root/'image.png';image_path.write_bytes(image)
        srgb=self.call('asset.image.import',dict(source=native(image_path),color_space='srgb'))['asset']
        linear=self.call('asset.image.import',dict(source=native(image_path),color_space='linear'))['asset'];image_path.unlink()
        wav=self.root/'clip.wav'
        with wave.open(str(wav),'wb') as stream:
            stream.setparams((1,2,48000,8,'NONE','not compressed'));stream.writeframes(struct.pack('<8h',0,200,-200,0,100,-100,0,0))
        audio=self.call('asset.audio.import',dict(source=native(wav)))['asset'];wav.unlink()
        maps=dict(base_color=texture(srgb),emissive=texture(other,0),metallic_roughness=texture(linear),normal=None,occlusion=None)
        self.tx(0,[create(1),component(1,'StaticMesh',dict(asset=model,primitive=0,visible=False)),component(1,'PbrTextures',maps),
            create(2),component(2,'MeshCollider',dict(asset=model,primitive=0,friction=.5,restitution=0)),
            create(3),component(3,'AudioEmitter',dict(asset=audio,gain=1,loop=False,enabled=False)),
            dict(op='asset.instantiate',id=uid(300),name='Compiled skin instance',asset=animated),
            dict(op='template.set',id=uid(1),name='Unused recipe',components={'Transform':TRANSFORM,
                'StaticMesh':dict(asset=model,primitive=0,visible=False),'PbrTextures':maps}),
            dict(op='entity.create',id=uid(6),name=model),
            dict(op='component.schema.set',schema=dict(id=uid(900),name=model,version=1,
                fields=[dict(id=uid(901),name=srgb,kind='int32',default=0)])),
            component(6,'game:'+uid(900),{uid(901):42})])
        skin=hashlib.sha256(('poima.instance.v1/'+uid(300)+'/node/0/primitive/0').encode()).hexdigest()[:32]
        expected=[]
        for kind in ('entity','template'):
            expected.extend([edge(kind,1,'PbrTextures','/base_color/asset',srgb,'image'),
                edge(kind,1,'PbrTextures','/emissive/asset',other,'model',dict(kind='image',index=0)),
                edge(kind,1,'PbrTextures','/metallic_roughness/asset',linear,'image'),
                edge(kind,1,'StaticMesh','/asset',model,'model',dict(kind='primitive',index=0))])
        expected.extend([edge('entity',2,'MeshCollider','/asset',model,'model',dict(kind='primitive',index=0)),
            edge('entity',3,'AudioEmitter','/asset',audio,'audio'),edge('entity',300,'AnimationRig','/asset',animated,'model'),
            edge('entity',skin,'SkinnedMesh','/asset',animated,'model',dict(kind='primitive',index=0))])
        return sorted(expected,key=ordering),srgb
    def test_real_import_edges_equal_external_package_closure(self):
        expected,_=self.real_fixture();before=self.preserved()
        self.assertEqual(self.call(METHOD)['edges'],expected)
        dependencies=self.call('world.dependencies')
        extensions={'model':'.pmodel','image':'.pimage','audio':'.paudio'}
        targets={(row['asset'],row['asset']+extensions[row['kind']]) for row in expected}
        self.assertEqual(targets,{(row['sha256'],row['filename']) for row in dependencies['assets']})
        self.assertTrue(dependencies['needs_audio']);self.assert_preserved(before)
        if ARGS.runtime:
            session=uuid.uuid4().hex;self.call('runtime.start',dict(session_id=session,revision=1))
            pins=dict(session_id=session,tick=0,ids=[uid(1)],entity_fields=[])
            live=self.call('runtime.observe',pins);state=self.call('runtime.inspect',dict(session_id=session))
            for selection in ({},{'asset':expected[0]['asset']},{'owner':dict(kind='template',id=uid(1))}):self.call(METHOD,selection)
            self.assertEqual(self.call('runtime.observe',pins),live);self.assertEqual(self.call('runtime.inspect',dict(session_id=session)),state)
            self.assert_preserved(before)
            # A running session keeps its frozen content; provenance still reads
            # the authoritative authored document after an interleaved edit.
            self.tx(1,[dict(op='component.remove',id=uid(3),type='AudioEmitter')])
            before=self.preserved();current=self.call(METHOD)
            self.assertEqual(current['revision'],2)
            self.assertEqual(current['edges'],[row for row in expected if row['component']!='AudioEmitter'])
            after_live=self.call('runtime.observe',pins)
            after_state=self.call('runtime.inspect',dict(session_id=session))
            for actual,original in ((after_live,live),(after_state,state)):
                self.assertEqual(actual['current_authored_revision'],2);self.assertTrue(actual['source_stale'])
                self.assertEqual({key:value for key,value in actual.items() if key not in ('current_authored_revision','source_stale')},
                    {key:value for key,value in original.items() if key not in ('current_authored_revision','source_stale')})
            self.assert_preserved(before)
    def test_missing_and_corrupt_package_provenance_still_available(self):
        expected,srgb=self.real_fixture()
        package=Path(str(self.world)+'.assets')/(srgb+'.pimage');raw=package.read_bytes()
        try:
            package.unlink()
            before=self.preserved();self.assertEqual(self.call(METHOD)['edges'],expected)
            self.call('world.dependencies',code=-32000);self.assert_preserved(before)
            damaged=bytearray(raw);damaged[0]^=0xff;package.write_bytes(damaged)
            before=self.preserved();self.assertEqual(self.call(METHOD)['edges'],expected)
            self.call('world.dependencies',code=-32000);self.assert_preserved(before)
        finally:
            package.write_bytes(raw)
        # No invalid bindings are authored: existing authoring validation remains
        # responsible for checking template/rig material and subresource content.
        self.assertEqual(self.call(METHOD)['edges'],expected)
        self.assertTrue(self.call('world.dependencies')['assets'])
    def test_shared_owner_guarded_mutation_rejects_mixed_page(self):
        expected=self.authored_fixture()
        audio=next(row['asset'] for row in expected if row['component']=='AudioEmitter')
        (Path(str(self.world)+'.assets')/(audio+'.paudio')).unlink()
        self.client.close();self.assertEqual(self.client.transport.returncode,0)
        endpoint='references-'+uuid.uuid4().hex;host=OwnedHost(ARGS.binary.resolve(),native(self.world),endpoint);self.hosts.append(host)
        readers=[]
        try:
            for _ in range(2):
                readers.append(WorldClient.connect(str(ARGS.binary.resolve()),endpoint,timeout_ms=10000,close_timeout=5));self.clients.append(readers[-1])
            self.assertEqual(self.call(METHOD,client=readers[0])['edges'],expected)
            first=self.call(METHOD,dict(limit=1),client=readers[0]);self.assertEqual(first['revision'],1)
            self.tx(1,[dict(op='component.remove',id=uid(3),type='AudioEmitter')],client=readers[1])
            self.call(METHOD,dict(revision=1,after=first['next_after']),code=-32009,client=readers[0])
            current=self.call(METHOD,dict(revision=2),client=readers[0])
            self.assertEqual(current['edges'],[row for row in expected if row['component']!='AudioEmitter'])
            self.assertEqual(self.call('world.describe',dict(view='method',name=METHOD),client=readers[0])['session_scope'],'shared_headless')
        finally:
            if readers:
                self.call('host.shutdown',client=readers[0]);host.references_shutdown=True


def main():
    global ARGS
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path)
    parser.add_argument('--runtime',type=int,choices=(0,1),default=1);parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--evidence',type=Path);ARGS=parser.parse_args()
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(References)
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    if ARGS.evidence:
        ARGS.evidence.parent.mkdir(parents=True,exist_ok=True)
        ARGS.evidence.write_text(json.dumps(dict(passed=result.wasSuccessful(),tests=result.testsRun,records=REPORTS),indent=2)+'\n')
    return 0 if result.wasSuccessful() else 1


if __name__=='__main__':raise SystemExit(main())
