#!/usr/bin/env python3
"""Core world undo/redo, retirement and cross-method retry behavior."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary',type=Path)
parser.add_argument('--windows-interop',action='store_true')
parser.add_argument('--evidence',type=Path)
args=parser.parse_args();BINARY=str(args.binary.resolve())
ROOT=Path(__file__).resolve().parents[1];SCRATCH=ROOT/'build/world-history-contract';SCRATCH.mkdir(parents=True,exist_ok=True)
A='a'*32;B='b'*32

class WorldHistory(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=SCRATCH);self.root=Path(self.tmp.name);self.world=self.root/'world.json';self.process=None;self.open()
    def tearDown(self):self.close();self.tmp.cleanup()
    def native(self,path):
        return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip() if args.windows_interop else str(path.resolve())
    def open(self):self.process=subprocess.Popen([BINARY,'world',self.native(self.world)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
    def close(self):
        if self.process:
            self.process.stdin.close();self.process.wait(timeout=15)
            self.assertEqual(self.process.returncode,0,self.process.stderr.read());self.process.stdout.close();self.process.stderr.close();self.process=None
    def request(self,method,**params):
        self.process.stdin.write(json.dumps({'jsonrpc':'2.0','id':1,'method':method,'params':params})+'\n');self.process.stdin.flush()
        line=self.process.stdout.readline();self.assertTrue(line,'World service ended unexpectedly.');return json.loads(line)
    def ok(self,method,**params):
        response=self.request(method,**params);self.assertIn('result',response,response);return response['result']
    def error(self,method,code,**params):
        response=self.request(method,**params);self.assertEqual(response.get('error',{}).get('code'),code,response)
    def transact(self,revision,ops,**extra):return self.ok('world.transact',request_id=extra.pop('request_id',uuid.uuid4().hex),base_revision=revision,ops=ops,**extra)
    def restore(self,method,revision,**extra):return self.ok('world.'+method,request_id=extra.pop('request_id',uuid.uuid4().hex),base_revision=revision,**extra)
    def create(self,revision=0):return self.transact(revision,[{'op':'entity.create','id':A,'name':'Original'}])
    def test_schema_and_initial_history(self):
        schema=self.ok('world.describe');self.assertGreaterEqual(schema['schema_revision'],20)
        for method in ['world.history','world.undo','world.redo']:self.assertIn(method,schema['methods'])
        h=self.ok('world.history');self.assertEqual((h['undo_count'],h['redo_count'],h['bytes']),(0,0,0));self.assertTrue(h['session_local'])
        self.error('world.undo',-32004,request_id=uuid.uuid4().hex,base_revision=0);self.assertFalse(self.world.exists())
    def test_known_history_restores_ids_without_public_reuse(self):
        self.create();self.restore('undo',1);before=self.world.read_bytes()
        self.error('entity.get',-32004,id=A)
        self.error('world.transact',-32602,request_id=uuid.uuid4().hex,base_revision=2,ops=[{'op':'entity.create','id':A,'name':'Illegal reuse'}])
        self.assertEqual(before,self.world.read_bytes());self.assertEqual(self.ok('world.history')['redo_count'],1)
        self.restore('redo',2);self.assertEqual(self.ok('entity.get',id=A)['value']['name'],'Original')
        self.transact(3,[{'op':'entity.delete','id':A,'recursive':False}]);self.restore('undo',4)
        self.assertEqual(self.ok('entity.get',id=A)['value']['name'],'Original')
    def test_recursive_delete_restores_hierarchy_and_components(self):
        self.create();self.transact(1,[{'op':'entity.create','id':B,'name':'Child','parent':A},
            {'op':'component.set','id':B,'type':'MeshRenderer','value':{'primitive':'box','albedo':[.2,.4,.6],'visible':True}}])
        original=self.ok('entity.get',id=B)['value'];self.transact(2,[{'op':'entity.delete','id':A,'recursive':True}])
        self.assertEqual(self.ok('world.inspect')['entity_count'],0);self.restore('undo',3)
        self.assertEqual(self.ok('entity.get',id=B)['value'],original);self.assertEqual(self.ok('world.inspect')['entity_count'],2)
        self.restore('redo',4);self.assertEqual(self.ok('world.inspect')['entity_count'],0)
    def test_new_commit_invalidates_redo_and_retains_retired_ids(self):
        self.create();self.transact(1,[{'op':'entity.rename','id':A,'name':'Changed'}]);self.restore('undo',2)
        self.transact(3,[{'op':'entity.rename','id':A,'name':'Branch'}]);self.assertEqual(self.ok('world.history')['redo_count'],0)
        self.error('world.redo',-32004,request_id=uuid.uuid4().hex,base_revision=4)
        self.restore('undo',4);self.assertEqual(self.ok('entity.get',id=A)['value']['name'],'Original')
    def test_preview_and_validation_failures_do_not_consume_history(self):
        self.create();history=self.ok('world.history');before=self.world.read_bytes()
        self.transact(1,[{'op':'entity.rename','id':A,'name':'Preview'}],preview=True)
        self.error('world.transact',-32602,request_id=uuid.uuid4().hex,base_revision=1,ops=[{'op':'entity.reparent','id':A,'parent':A,'mode':'keep_local'}])
        self.error('world.undo',-32009,request_id=uuid.uuid4().hex,base_revision=0)
        self.assertEqual(self.ok('world.history'),history);self.assertEqual(self.world.read_bytes(),before)
        self.restore('undo',1)
    def test_retry_receipts_are_cross_method_safe_and_persist(self):
        create_id=uuid.uuid4().hex;self.transact(0,[{'op':'entity.create','id':A,'name':'Original'}],request_id=create_id)
        undo_id=uuid.uuid4().hex;first=self.restore('undo',1,request_id=undo_id);raw=self.world.read_bytes()
        self.assertEqual(self.restore('undo',1,request_id=undo_id),dict(first,replayed=True));self.assertEqual(self.world.read_bytes(),raw)
        self.error('world.redo',-32010,request_id=undo_id,base_revision=1)
        self.error('world.undo',-32010,request_id=create_id,base_revision=0)
        self.error('world.transact',-32010,request_id=undo_id,base_revision=2,ops=[{'op':'entity.create','id':B,'name':'Conflict'}])
        self.close();self.open();self.assertEqual(self.ok('world.history')['undo_count'],0);self.assertEqual(self.ok('world.history')['redo_count'],0)
        self.assertEqual(self.restore('undo',1,request_id=undo_id),dict(first,replayed=True));self.assertEqual(self.world.read_bytes(),raw)
        self.error('world.redo',-32004,request_id=uuid.uuid4().hex,base_revision=2)
    def test_history_entry_bound_and_monotonic_revisions(self):
        self.create()
        for index in range(1,36):self.transact(index,[{'op':'entity.rename','id':A,'name':'Name '+str(index)}])
        h=self.ok('world.history');self.assertEqual(h['undo_count'],32);self.assertLessEqual(h['bytes'],h['max_bytes'])
        for revision in range(36,68):self.assertEqual(self.restore('undo',revision)['revision'],revision+1)
        self.assertEqual(self.ok('entity.get',id=A)['value']['name'],'Name 3')
        self.error('world.undo',-32004,request_id=uuid.uuid4().hex,base_revision=68)
        self.assertEqual(self.ok('world.history')['redo_count'],32)
    def test_external_write_does_not_consume_history(self):
        self.create();before=self.world.read_bytes();history=self.ok('world.history');self.world.write_bytes(before+b' ')
        self.error('world.undo',-32009,request_id=uuid.uuid4().hex,base_revision=1)
        self.assertEqual(self.ok('world.history'),history);self.assertEqual(self.world.read_bytes(),before+b' ')
        self.world.write_bytes(before);self.restore('undo',1)
    def test_history_byte_budget_evicts_before_entry_limit(self):
        ids=[f'{index+100:032x}' for index in range(1024)];revision=0
        for offset in range(0,len(ids),256):
            self.transact(revision,[{'op':'entity.create','id':entity,'name':'Long authored name '+('x'*220)} for entity in ids[offset:offset+256]])
            revision+=1
        for index in range(40):
            self.transact(revision,[{'op':'entity.rename','id':ids[0],'name':'Budget edit '+str(index)}]);revision+=1
        h=self.ok('world.history');self.assertGreater(h['undo_count'],0);self.assertLess(h['undo_count'],32)
        self.assertLessEqual(h['bytes'],16*1024*1024);self.assertEqual(h['redo_count'],0)
        previous_depth=h['undo_count'];self.restore('undo',revision)
        h=self.ok('world.history');self.assertEqual(h['undo_count'],previous_depth-1);self.assertEqual(h['redo_count'],1)
        self.assertLessEqual(h['bytes'],16*1024*1024)
    def test_rig_restoration_revalidates_whole_binding_graph(self):
        asset=self.ok('asset.import',source=self.native(ROOT/'examples/assets/animated-ribbon.glb'))['asset']
        self.transact(0,[{'op':'asset.instantiate','id':A,'asset':asset,'name':'Rig'}]);count=self.ok('world.inspect')['entity_count']
        self.assertGreater(count,2);self.transact(1,[{'op':'entity.delete','id':A,'recursive':True}]);self.restore('undo',2)
        self.assertEqual(self.ok('world.inspect')['entity_count'],count)
        self.assertEqual(self.ok('entity.get',id=A)['value']['components']['AnimationRig']['asset'],asset)
        self.restore('redo',3);self.assertEqual(self.ok('world.inspect')['entity_count'],0)

if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(WorldHistory))
    if args.evidence:args.evidence.write_text(json.dumps({'tests':result.testsRun,'passed':result.wasSuccessful(),
        'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()},indent=2)+'\n')
    raise SystemExit(not result.wasSuccessful())
