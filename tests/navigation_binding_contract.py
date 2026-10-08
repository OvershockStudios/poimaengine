#!/usr/bin/env python3
"""Durable navigation binding, fresh content closure and saved-source restore."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient,RpcError
ARGS=None
CALLS=[]
OWNERS=[]

def uid(value):return f'{value:032x}'
def native(path):
    value=str(Path(path).resolve())
    return subprocess.check_output(['wslpath','-w',value],text=True,timeout=10).strip() if ARGS.windows_interop else value

def pose(x=0):return dict(position=[x,-.5,0],rotation=[0,0,0,1],scale=[1,1,1])
def fixture():return [dict(op='entity.create',id=uid(1),name='Static floor',parent=None),
    dict(op='component.set',id=uid(1),type='Transform',value=pose()),
    dict(op='component.set',id=uid(1),type='BoxCollider',value=dict(half_extents=[5,.5,5],motion='static',mass=1,friction=.5,restitution=0))]
def binding(asset):return dict(op='navigation.set',asset=asset)
def normalize(value):
    if isinstance(value,dict):return {key:normalize(item) for key,item in value.items() if key!='session_id'}
    if isinstance(value,list):return [normalize(item) for item in value]
    return value

def tree(root):return {str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(root.rglob('*')) if p.is_file()}

class NavigationBinding(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='binding-',dir=ARGS.output)
        self.directory=Path(self.temp.name);self.world=self.directory/'bound world.json'
        self.owners=[];self.client=self.open();self.revision=0;self.session=None;self.tick=0
        self.edit(fixture())
    def tearDown(self):
        errors=[]
        for owner in reversed(self.owners):
            try:owner.close()
            except BaseException as error:errors.append(str(error))
            code=owner.transport.returncode;OWNERS.append(dict(test=self.id(),exit_code=code))
            if code!=0:errors.append('Native owner exit was not zero')
        self.temp.cleanup();self.assertEqual(errors,[])
    def open(self):
        owner=WorldClient.open(str(ARGS.binary.resolve()),native(self.world));self.owners.append(owner);return owner
    def call(self,method,params=None,error=None):
        params=params or {};record=dict(test=self.id(),method=method,params=params);CALLS.append(record)
        try:result=self.client.call(method,params,timeout=45)
        except RpcError as failure:
            record['error']=dict(code=failure.code,message=str(failure));self.assertIsNotNone(error);self.assertEqual(failure.code,error);return failure
        record['result']=result;self.assertIsNone(error,'Expected a rejection');return result
    def edit(self,ops,**extra):
        result=self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=self.revision,ops=ops,**extra))
        if not extra.get('preview',False):self.revision=result['revision']
        return result
    def view(self):return dict(inspect=self.call('world.inspect'),history=self.call('world.history'),bytes=self.world.read_bytes())
    def bake(self):return self.call('world.navigation.bake',dict(revision=self.revision))['asset']
    def enabled(self):
        if not ARGS.navigation:self.skipTest('Navigation backend disabled')
    def state(self):return self.call('runtime.entity',dict(session_id=self.session,tick=self.tick,id=uid(1)))
    def step(self,ticks=1):
        result=self.call('runtime.step',dict(session_id=self.session,expected_tick=self.tick,request_id=uuid.uuid4().hex,ticks=ticks));self.tick=result['tick']
    def configure(self):
        self.saves=self.directory/'saves';self.saves.mkdir(exist_ok=True)
        self.generation=self.call('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(self.saves)))['generation']
    def load(self,error=None):
        params=dict(request_id=uuid.uuid4().hex,configuration_generation=self.generation,slot='bound',expected_generation=1,
            revision=self.revision,expected_session_id=self.session,expected_tick=self.tick if self.session else None,
            expected_gameplay_revision=0 if self.session else None,new_session_id=uuid.uuid4().hex)
        result=self.call('save.load',params,error=error)
        if error is None:self.session=params['new_session_id'];self.tick=result['tick']
        return result
    def test_no_binding_and_backend_admission(self):
        inspect=self.call('world.inspect');self.assertNotIn('navigation',inspect)
        self.assertNotIn('needs_navigation',self.call('world.dependencies'))
        self.assertEqual(self.call('world.asset.references',dict(revision=self.revision,owner=dict(kind='world',id=inspect['world_id'])))['edges'],[])
        before=self.view()
        for op in (dict(op='navigation.set'),dict(op='navigation.set',asset=True),dict(op='navigation.set',asset='A'*64),dict(op='navigation.set',asset=None,id=uid(1))):
            self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=1,ops=[op]),error=-32602)
        self.assertEqual(self.view(),before)
        if not ARGS.navigation:
            self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=1,ops=[binding('a'*64)]),error=-32003)
            self.assertEqual(self.view(),before)
        cleared=self.edit([binding(None)]);self.assertNotIn('changed_world_fields',cleared)
    def test_final_candidate_history_retry_and_fresh_package(self):
        self.enabled();asset=self.bake();before=self.view()
        preview=self.edit([binding(asset)],preview=True);self.assertEqual(preview['changed_world_fields'],['navigation']);self.assertEqual(self.view(),before)
        params=dict(request_id=uuid.uuid4().hex,base_revision=1,ops=[binding(asset)])
        result=self.call('world.transact',params);self.revision=2;self.assertEqual(result['changed_world_fields'],['navigation'])
        before=self.view();self.assertEqual(self.call('world.transact',params),dict(result,replayed=True));self.assertEqual(self.view(),before)
        package=Path(str(self.world)+'.assets')/(asset+'.pnav');raw=package.read_bytes()
        dependencies=self.call('world.dependencies');self.assertTrue(dependencies['needs_navigation'])
        self.assertEqual(dependencies['assets'],[dict(filename=asset+'.pnav',sha256=asset,bytes=len(raw))])
        undone=self.call('world.undo',dict(request_id=uuid.uuid4().hex,base_revision=2));self.revision=3
        self.assertEqual(undone['changed_world_fields'],['navigation']);self.assertNotIn('navigation',self.call('world.inspect'))
        redone=self.call('world.redo',dict(request_id=uuid.uuid4().hex,base_revision=3));self.revision=4
        self.assertEqual(redone['changed_world_fields'],['navigation'])
        self.edit([dict(op='component.set',id=uid(1),type='Transform',value=pose(1))]);before=self.view()
        self.call('world.dependencies',error=-32009);self.assertEqual(self.view(),before)
        final=self.edit([binding(asset),dict(op='component.set',id=uid(1),type='Transform',value=pose())]);self.assertNotIn('changed_world_fields',final)
        self.assertTrue(self.call('world.dependencies')['needs_navigation'])
        self.call('world.navigation.inspect',dict(revision=self.revision,asset=asset))
        before=self.view();package.write_bytes(b'corrupt')
        self.call('world.dependencies',error=-32050)
        self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=self.revision,ops=[binding(asset)]),error=-32050)
        self.assertEqual(self.view(),before);package.write_bytes(raw)
        self.client.close();self.client=self.open();self.assertEqual(self.call('world.inspect')['navigation'],dict(asset=asset))
        self.assertTrue(self.call('world.dependencies')['needs_navigation'])
    def test_saved_source_binding_fresh_validation_and_restore(self):
        self.enabled()
        if not ARGS.runtime:self.skipTest('Simulation disabled')
        asset=self.bake();self.edit([binding(asset)]);self.configure();self.session=uuid.uuid4().hex
        self.call('runtime.start',dict(revision=self.revision,session_id=self.session));self.step(5)
        saved=self.state();written=self.call('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=self.generation,
            slot='bound',expected_generation=0,session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=0))
        self.assertEqual(written['generation'],1)
        # Today's authoring has no binding and different topology. Restoration
        # must validate the saved frozen source and preserve current authoring.
        self.edit([binding(None),dict(op='component.set',id=uid(1),type='Transform',value=pose(2))])
        authored=self.view();files=tree(self.saves);package=Path(str(self.world)+'.assets')/(asset+'.pnav');raw=package.read_bytes()
        for missing in (False,True):
            if missing:package.unlink()
            else:package.write_bytes(b'corrupt')
            before=self.state();self.load(error=-32070);self.assertEqual(self.state(),before);self.assertEqual(self.view(),authored);self.assertEqual(tree(self.saves),files)
            self.step();package.write_bytes(raw)
        restored=self.load();self.assertTrue(restored['source_stale']);self.assertEqual(restored['authored_revision'],2)
        self.assertEqual(normalize(self.state()),normalize(saved));self.assertEqual(self.view(),authored);self.assertEqual(tree(self.saves),files)
        self.step(3);continued=self.state()
        self.client.close();self.client=self.open();self.session=None;self.tick=0;self.configure()
        fresh=self.load();self.assertTrue(fresh['source_stale']);self.assertEqual(normalize(self.state()),normalize(saved))
        self.step(3);self.assertEqual(normalize(self.state()),normalize(continued));self.assertEqual(self.world.read_bytes(),authored['bytes'])
        self.assertEqual(tree(self.saves),files)


def main():
    global ARGS
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path)
    parser.add_argument('--navigation',type=int,choices=(0,1),required=True);parser.add_argument('--runtime',type=int,choices=(0,1),required=True)
    parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--output',type=Path)
    ARGS=parser.parse_args()
    if ARGS.output is None:ARGS.output=ROOT/'build/navigation-binding-contract'/uuid.uuid4().hex
    if sys.flags.optimize:parser.error('Assertions must be enabled')
    if ARGS.output.exists():parser.error('Output must be new')
    ARGS.output.mkdir(parents=True)
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(NavigationBinding))
    evidence=dict(passed=result.wasSuccessful(),tests=result.testsRun,skipped=len(result.skipped),rpc_calls=len(CALLS),
        methods=dict(sorted(Counter(call['method'] for call in CALLS).items())),owners=OWNERS,calls=CALLS)
    (ARGS.output/'local-evidence.json').write_text(json.dumps(evidence,ensure_ascii=False,indent=2)+'\n')
    return 0 if result.wasSuccessful() else 1
if __name__=='__main__':raise SystemExit(main())
