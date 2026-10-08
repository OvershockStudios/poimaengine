#!/usr/bin/env python3
"""Authored native UI definitions: versioning, transactions, history and freezing."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary',type=Path)
parser.add_argument('--runtime',type=int,choices=(0,1),default=1)
parser.add_argument('--windows-interop',action='store_true')
parser.add_argument('--evidence',type=Path)
args=parser.parse_args()
BINARY=str(args.binary.resolve());CALLS=[]
SCRATCH=Path(__file__).resolve().parents[1]/'build/world-ui-contract';SCRATCH.mkdir(parents=True,exist_ok=True)
def uid(n):return f'{n:032x}'
A,B,C=uid(1),uid(2),uid(3)
def element(kind='panel',parent=None,text='',action=None,name='Element'):
    return dict(parent=parent,name=name,kind=kind,text=text,action=action,visible=True,enabled=True)
def put(identity=A,**values):return dict(op='ui.element.set',id=identity,element=element(**values))
def remove(identity):return dict(op='ui.element.remove',id=identity)

class Contract(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(dir=SCRATCH);self.world=Path(self.temp.name)/'world.json';self.process=None;self.open()
    def open(self):
        path=str(self.world.resolve())
        if args.windows_interop:path=subprocess.check_output(['wslpath','-w',path],text=True).strip()
        self.process=subprocess.Popen([BINARY,'world',path],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
    def close(self):
        if self.process:
            self.process.stdin.close();self.process.wait(timeout=20)
            self.assertEqual(self.process.returncode,0,self.process.stderr.read())
            self.process.stdout.close();self.process.stderr.close();self.process=None
    def tearDown(self):self.close();self.temp.cleanup()
    def request(self,method,**params):
        self.process.stdin.write(json.dumps(dict(jsonrpc='2.0',id=len(CALLS)+1,method=method,params=params))+'\n');self.process.stdin.flush()
        line=self.process.stdout.readline();self.assertTrue(line,'World service exited')
        reply=json.loads(line);CALLS.append(dict(method=method,ok='result' in reply));return reply
    def ok(self,method,**params):
        result=self.request(method,**params);self.assertIn('result',result,result);return result['result']
    def error(self,method,code=-32602,**params):
        result=self.request(method,**params);self.assertEqual(result.get('error',{}).get('code'),code,result)
    def tx(self,revision,ops,**extra):
        return self.ok('world.transact',request_id=extra.pop('request_id',uuid.uuid4().hex),base_revision=revision,ops=ops,**extra)
    def history(self,method,revision):return self.ok(method,request_id=uuid.uuid4().hex,base_revision=revision)

    def test_upgrade_preview_discovery_and_persistence(self):
        discovery=self.ok('world.describe');self.assertEqual(discovery['schema_revision'],46)
        self.assertIn('world.ui.list',discovery['methods']);self.assertIn('world.ui.get',discovery['methods'])
        self.assertEqual(self.ok('world.ui.list')['elements'],[])
        self.tx(0,[dict(op='entity.create',id=C,name='Legacy')]);self.assertEqual(json.loads(self.world.read_text())['version'],1)
        before=self.world.read_bytes();preview=self.tx(1,[put()],preview=True)
        self.assertFalse(preview['committed']);self.assertEqual(preview['changed_ui_ids'],[A]);self.assertEqual(self.world.read_bytes(),before)
        self.assertEqual(self.ok('world.ui.list')['elements'],[])
        self.tx(1,[put()]);doc=json.loads(self.world.read_text());self.assertEqual(doc['version'],4)
        for field in ('component_schemas','retired_component_schemas','templates','retired_template_ids','ui','retired_ui_ids'):self.assertIn(field,doc)
        self.assertEqual(doc['ui'][A],element());self.assertEqual(self.ok('world.inspect')['entity_count'],1)
        self.close();self.open();self.assertEqual(self.ok('world.ui.get',id=A)['element'],element())
        self.assertEqual(self.ok('world.dependencies')['assets'],[])

    def test_final_tree_pagination_and_retries(self):
        ops=[put(C,kind='button',parent=A,text='Start',action='game.start'),put(B,kind='label',parent=A,text='Ready'),put()]
        request=uuid.uuid4().hex;committed=self.tx(0,ops,request_id=request)
        self.assertEqual(committed['changed_ui_ids'],[A,B,C]);self.assertEqual(self.tx(0,ops,request_id=request),dict(committed,replayed=True))
        changed=copy.deepcopy(ops);changed[0]['element']['enabled']=1
        self.error('world.transact',-32010,request_id=request,base_revision=0,ops=changed)
        page=self.ok('world.ui.list',limit=2);self.assertEqual([e['id'] for e in page['elements']],[A,B]);self.assertEqual(page['next_after'],B)
        tail=self.ok('world.ui.list',revision=1,after=B);self.assertEqual([e['id'] for e in tail['elements']],[C]);self.assertIsNone(tail['next_after'])
        self.error('world.ui.list',after=B);self.error('world.ui.list',-32009,revision=0,after=B)
        for limit in (0,257,1.0,True):self.error('world.ui.list',limit=limit)
        self.tx(1,[remove(A),remove(B),remove(C)])
        self.assertEqual(self.ok('world.ui.list')['elements'],[])

    def test_kind_specific_discovery_matches_authoring(self):
        declared=self.ok('world.describe',view='section',name='ui')['sections']['ui']['element']
        operations=self.ok('world.describe',view='method',name='world.transact')['methods']['world.transact']['properties']['ops']['items']['oneOf']
        mutation=next(item for item in operations if item['properties']['op'].get('const')=='ui.element.set')
        self.assertEqual(mutation['properties']['element'],declared)
        variants={item['properties']['kind']['const']:item for item in declared['oneOf']}
        self.assertEqual(set(variants),{'panel','label','button'})
        self.assertEqual(len(declared['oneOf']),3)
        for kind,variant in variants.items():
            with self.subTest(kind=kind):
                self.assertEqual(variant['type'],'object')
                self.assertFalse(variant['additionalProperties'])
                self.assertEqual(set(variant['required']),set(element(kind=kind)))
        self.assertEqual(variants['panel']['properties']['text'],dict(type='string',const=''))
        for kind in ('panel','label'):
            self.assertEqual(variants[kind]['properties']['action'],dict(type='null'))
        self.assertEqual(variants['button']['properties']['action'],dict(type='string',pattern='^[A-Za-z0-9_.-]{1,128}$'))
        for kind in ('label','button'):
            self.assertEqual(variants[kind]['properties']['text'],dict(type='string',maxLength=16384))
        self.tx(0,[put(),put(B,kind='label',text='Status'),put(C,kind='button',text='Start',action='game.start')])
        before=self.world.read_bytes()
        forbidden=[put(text='Panel title'),put(action='game.start'),put(B,kind='label',action='game.start')]
        forbidden += [put(C,kind='button',action=action) for action in (None,'','bad token','x'*129,True)]
        for op in forbidden:
            with self.subTest(element=op['element']):
                self.error('world.transact',request_id=uuid.uuid4().hex,base_revision=1,ops=[op])
                self.assertEqual(self.world.read_bytes(),before)

    def test_invalid_definitions_are_atomic(self):
        self.tx(0,[put(),put(B,kind='label',parent=A,text='Kept')]);before=self.world.read_bytes()
        invalid=[put(C,kind='button',action=None),put(C,action='forbidden'),put(C,text='panel text'),put(C,parent=uid(999)),
                 put(C,parent=B),put(A,parent=A),put(C,kind='toggle'),put(uid(0)),put(C,kind='button',action='bad token'),
                 put(C,name='x'*129),put(C,kind='label',text='x'*16385),remove(A)]
        malformed=put(C);malformed['element']['visible']=1;invalid.append(malformed)
        malformed=put(C);malformed['element']['extra']=True;invalid.append(malformed)
        for op in invalid:
            with self.subTest(op=str(op)[:90]):
                self.error('world.transact',request_id=uuid.uuid4().hex,base_revision=1,ops=[put(B,kind='label',parent=A,text='Changed'),op])
                self.assertEqual(self.world.read_bytes(),before)
        cycle=[put(A,parent=C),put(C,parent=A)]
        self.error('world.transact',request_id=uuid.uuid4().hex,base_revision=1,ops=cycle);self.assertEqual(self.world.read_bytes(),before)

    def test_history_retirement_and_v3_templates(self):
        transform=dict(position=[0,0,0],rotation=[0,0,0,1],scale=[1,1,1])
        recipe=dict(op='template.set',id=C,name='Crate',components=dict(Transform=transform))
        self.tx(0,[recipe]);self.tx(1,[put(),put(B,kind='label',parent=A,text='One')])
        self.history('world.undo',2);self.assertEqual(self.ok('world.ui.list')['elements'],[])
        self.assertEqual(self.ok('template.get',id=C)['template']['name'],'Crate')
        self.history('world.redo',3);self.assertEqual(self.ok('world.ui.get',id=B)['element']['text'],'One')
        self.tx(4,[remove(B)]);self.error('world.transact',request_id=uuid.uuid4().hex,base_revision=5,ops=[put(B)])
        self.history('world.undo',5);self.assertEqual(self.ok('world.ui.get',id=B)['element']['text'],'One')
        self.history('world.redo',6);self.error('world.ui.get',-32004,id=B)
        self.tx(7,[dict(op='template.remove',id=C)]);self.history('world.undo',8)
        self.assertEqual(self.ok('template.get',id=C)['template']['name'],'Crate')
        self.assertEqual(self.ok('world.ui.list')['elements'],[dict(id=A,element=element())])

    def test_hierarchy_and_membership_bounds(self):
        chain=[put(uid(i),parent=uid(i-1) if i>1 else None) for i in range(1,33)]
        self.tx(0,chain);before=self.world.read_bytes()
        self.error('world.transact',request_id=uuid.uuid4().hex,base_revision=1,ops=[put(uid(33),parent=uid(32))]);self.assertEqual(self.world.read_bytes(),before)
        self.tx(1,[put(uid(i)) for i in range(33,257)]);before=self.world.read_bytes()
        self.error('world.transact',request_id=uuid.uuid4().hex,base_revision=2,ops=[put(uid(257))]);self.assertEqual(self.world.read_bytes(),before)

    @unittest.skipUnless(args.runtime,'Runtime unavailable')
    def test_frozen_source_reports_authored_ui_edits(self):
        self.tx(0,[put(),put(B,kind='label',parent=A,text='Old')]);sid=uuid.uuid4().hex
        self.ok('runtime.start',session_id=sid,revision=1)
        self.assertFalse(self.ok('runtime.inspect',session_id=sid)['source_stale'])
        self.tx(1,[put(B,kind='label',parent=A,text='New')])
        observed=self.ok('runtime.inspect',session_id=sid);self.assertTrue(observed['source_stale']);self.assertEqual(observed['authored_revision'],1)
        self.assertEqual(self.ok('world.ui.get',id=B)['element']['text'],'New')

result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Contract))
if args.evidence:
    args.evidence.parent.mkdir(parents=True,exist_ok=True)
    args.evidence.write_text(json.dumps(dict(passed=result.wasSuccessful(),tests=result.testsRun,skipped=len(result.skipped),rpc_count=len(CALLS),
        binary_sha256=hashlib.sha256(Path(BINARY).read_bytes()).hexdigest(),calls=CALLS),indent=2)+'\n')
raise SystemExit(0 if result.wasSuccessful() else 1)
