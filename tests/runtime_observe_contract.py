#!/usr/bin/env python3
"""Real native compact runtime-observation parity, bounds and revision guards."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError

ARGS = None
CALLS = []
PROCESSES = []


def uid(n):
    return '{:032x}'.format(n)


A, B, C = uid(10), uid(20), uid(30)
TYPE, OTHER = uid(100), uid(200)
FIELDS = [uid(1000+i) for i in range(4)]
OTHER_FIELD = uid(2000)
TEMPLATE, LABEL = uid(3000), uid(4000)
SCHEMA = dict(id=TYPE, name='Observed inventory', version=2, fields=[
    dict(id=FIELDS[0], name='Count', kind='int32', default=0),
    dict(id=FIELDS[1], name='Exact score', kind='int64', default='0'),
    dict(id=FIELDS[2], name='Target', kind='entity', default=uid(0)),
    dict(id=FIELDS[3], name='Ordered parts', kind='array', element_kind='int32', capacity=3, default=[])])
OTHER_SCHEMA = dict(id=OTHER, name='Observed distance', version=1, fields=[
    dict(id=OTHER_FIELD, name='Distance', kind='float64', default=0)])
DEFAULT_FIELDS = {'world_matrix', 'layout', 'velocity', 'has_body', 'is_character', 'ground', 'yaw', 'pitch'}
ALL_FIELDS = DEFAULT_FIELDS | {'local_transform', 'animation', 'motion', 'kinematic_target', 'motion_remaining_ticks'}
HEADER_FIELDS = ('structure_revision', 'component_revision', 'gameplay_revision', 'ui_revision', 'control_sequence')


def transform(position=(0,0,0), scale=(1,1,1)):
    return dict(position=list(position), rotation=[0,0,0,1], scale=list(scale))


def values(count=7, parts=(9,2,9), target=B):
    return dict(zip(FIELDS, [count, '9223372036854775807', target, list(parts)]))


def create(identity):
    return dict(op='entity.create', id=identity, name='Observed '+identity[-4:])


def set_component(identity, type, value):
    return dict(op='component.set', id=identity, type=type, value=value)


class Contract(unittest.TestCase):
    def setUp(self):
        scratch = ROOT/'build/runtime-observe-contract'
        scratch.mkdir(parents=True,exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.world = self.root/'world.json'
        self.client = None
        self.session = None
        self.client = WorldClient.open(str(ARGS.binary.resolve()), self.native(self.world), close_timeout=10)
        self.addCleanup(self.close_client)
        self.tx(0, [dict(op='component.schema.set',schema=SCHEMA),
            dict(op='component.schema.set',schema=OTHER_SCHEMA), create(A),create(B),create(C),
            set_component(A,'Transform',transform((2,3,4),(2,2,2))),
            set_component(B,'Transform',transform((-1,.5,2))),
            dict(op='entity.reparent',id=B,parent=A,mode='keep_local'),
            set_component(C,'Transform',transform((9,0,-1))),
            set_component(A,'game:'+TYPE,values()),
            set_component(C,'game:'+TYPE,values(-2,(-3,8,-3),A)),
            set_component(B,'game:'+OTHER,{OTHER_FIELD:12.5}),
            dict(op='template.set',id=TEMPLATE,name='Native observed spawn',components={
                'Transform':transform(),'game:'+TYPE:values(3,(2,1),A)}),
            dict(op='ui.element.set',id=LABEL,element=dict(name='Observed feedback',kind='label',parent=None,
                text='Initial',visible=True,enabled=True,action=None))])

    def native(self, path):
        if ARGS.windows_interop:
            return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True,timeout=5).strip()
        return str(path.resolve())

    def close_client(self):
        if self.client:
            transport = self.client.transport
            self.client.close()
            PROCESSES.append(dict(closed=transport.closed,returncode=transport.returncode))
            self.assertTrue(transport.closed)
            self.assertEqual(transport.returncode,0,transport.stderr_tail)

    def call(self, method, **params):
        record = dict(method=method)
        CALLS.append(record)
        try:
            result = self.client.call(method,params,timeout=20)
            record['ok'] = True
            return result
        except RpcError as error:
            record.update(ok=False,code=error.code)
            raise

    def reject(self, code=-32602, **params):
        with self.assertRaises(RpcError) as caught:
            self.call('runtime.observe',**params)
        self.assertEqual(caught.exception.code,code,str(caught.exception))

    def tx(self, revision, ops):
        return self.call('world.transact',request_id=uuid.uuid4().hex,base_revision=revision,ops=ops)

    def start(self, revision=1):
        if not ARGS.runtime:
            self.skipTest('Simulation disabled in this build')
        self.session = uuid.uuid4().hex
        self.call('runtime.start',session_id=self.session,revision=revision)
        return dict(session_id=self.session,tick=0)

    def observe(self, **params):
        return self.call('runtime.observe',session_id=self.session,tick=params.pop('tick',0),**params)

    def raw_component(self, identity, type=TYPE, tick=0, structure=0):
        return self.call('runtime.component.get',session_id=self.session,tick=tick,
            structure_revision=structure,id=identity,type=type)

    def stable_state(self):
        return dict(world=self.world.read_bytes(),history=self.call('world.history'),
            runtime=self.call('runtime.inspect',session_id=self.session),
            components=self.raw_component(A),
            ui=self.call('runtime.ui.inspect',session_id=self.session,tick=0),
            gameplay=self.call('runtime.gameplay.inspect',session_id=self.session,tick=0),
            saves=self.call('save.status'))

    def test_discovery_and_absent_runtime(self):
        if not ARGS.runtime:
            self.assertNotIn('runtime.observe',self.call('world.describe')['methods'])
            with self.assertRaises(RpcError) as unavailable:
                self.call('world.describe',view='method',name='runtime.observe')
            self.assertEqual(unavailable.exception.code,-32602)
            self.reject(-32003,session_id=uuid.uuid4().hex,tick=0,ids=[A])
            self.assertFalse(self.call('runtime.status')['active'])
            return
        description = self.call('world.describe',view='method',name='runtime.observe')
        self.assertEqual(set(description['methods']),{'runtime.observe'})
        schema = description['methods']['runtime.observe']
        self.assertEqual(set(schema['required']),{'session_id','tick'})
        self.assertEqual(schema['properties']['ids']['maxItems'],32)
        self.assertEqual(schema['properties']['query']['properties']['limit']['maximum'],64)
        self.assertEqual(schema['properties']['components']['maxItems'],4)
        self.assertEqual(schema['properties']['entity_fields']['maxItems'],13)
        self.assertFalse(schema['properties']['include_schemas']['default'])
        self.reject(-32030 if ARGS.runtime else -32003,session_id=uuid.uuid4().hex,tick=0,ids=[A])
        # A reader must not start a runtime or alter authored receipts.
        self.assertFalse(self.call('runtime.status')['active'])
        self.assertEqual(self.call('world.inspect')['revision'],1)

    def test_entity_and_typed_component_parity(self):
        self.start()
        result = self.observe(ids=[C,B,A],entity_fields=sorted(ALL_FIELDS),
            components=[dict(type=OTHER),dict(type=TYPE)],include_schemas=True)
        self.assertEqual([r['id'] for r in result['entities']],[A,B,C])
        self.assertIsNone(result['query'])
        self.assertEqual(result['authored_revision'],1)
        self.assertEqual(result['current_authored_revision'],1)
        self.assertFalse(result['source_stale'])
        self.assertEqual([r['type'] for r in result['component_types']],[TYPE,OTHER])
        metadata = {r['type']:r for r in result['component_types']}
        for row in result['entities']:
            entity = self.call('runtime.entity',session_id=self.session,tick=0,structure_revision=0,id=row['id'])
            self.assertEqual(row['state'],{key:entity[key] for key in ALL_FIELDS})
            self.assertEqual(set(row['components']),{TYPE,OTHER})
            for type in (TYPE,OTHER):
                component = self.raw_component(row['id'],type)
                self.assertEqual(row['components'][type],component['values'])
                self.assertEqual(metadata[type]['schema'],component['schema'])
                self.assertEqual(metadata[type]['fingerprint'],component['schema']['fingerprint'])
        first = result['entities'][0]['components'][TYPE]
        self.assertEqual(first[FIELDS[3]],[9,2,9], 'Ordered duplicates must survive observation')
        self.assertEqual(first[FIELDS[1]],'9223372036854775807')
        self.assertEqual(first[FIELDS[2]],B)
        self.assertIsNone(result['entities'][1]['components'][TYPE])
        self.assertEqual(result['entities'][1]['state']['world_matrix'][12:15],[0,4,8])
        for name in HEADER_FIELDS:
            self.assertEqual(result[name],0)

    def test_projection_membership_and_no_schema_duplication(self):
        self.start()
        result = self.observe(ids=[B,A],components=[dict(type=TYPE,fields=[FIELDS[3],FIELDS[1]])])
        self.assertEqual(result['component_types'][0]['type'],TYPE)
        self.assertNotIn('schema',result['component_types'][0])
        self.assertEqual(result['entities'][0]['components'][TYPE],{FIELDS[1]:'9223372036854775807',FIELDS[3]:[9,2,9]})
        self.assertIsNone(result['entities'][1]['components'][TYPE])
        self.assertEqual(set(result['entities'][0]['state']),DEFAULT_FIELDS)
        membership = self.observe(ids=[A,B],entity_fields=[],components=[dict(type=TYPE,fields=[])])
        self.assertEqual([r['state'] for r in membership['entities']],[{},{}])
        self.assertEqual([r['components'][TYPE] for r in membership['entities']],[{},None])
        pose_only = self.observe(ids=[A],components=[])
        self.assertEqual(pose_only['component_types'],[])
        self.assertEqual(pose_only['entities'][0]['components'],{})

    def test_query_union_dedup_and_pinned_pagination(self):
        self.start()
        first = self.observe(ids=[B,A],query=dict(type=TYPE,limit=1),components=[dict(type=TYPE)])
        self.assertEqual([r['id'] for r in first['entities']],[A,B])
        self.assertEqual(first['query'],dict(type=TYPE,entities=[A],next_after=A))
        native = self.call('runtime.component.query',session_id=self.session,tick=0,structure_revision=0,type=TYPE,limit=1)
        self.assertEqual(first['query']['entities'],native['entities'])
        self.assertEqual(first['query']['next_after'],native['next_after'])
        for pins in ({},{'structure_revision':0},{'component_revision':0}):
            self.reject(session_id=self.session,tick=0,query=dict(type=TYPE,after=A,limit=1),**pins)
        second = self.observe(query=dict(type=TYPE,after=A,limit=1),structure_revision=0,component_revision=0)
        self.assertEqual(second['query'],dict(type=TYPE,entities=[C],next_after=None))
        self.assertEqual([r['id'] for r in second['entities']],[C])
        self.assertEqual(second['component_types'],[], 'Query type must not implicitly request component payloads')
        self.assertEqual(second['entities'][0]['components'],{})

    def test_same_tick_component_structure_and_ui_guards(self):
        self.start()
        initial = self.observe(ids=[A],query=dict(type=TYPE,limit=1),components=[dict(type=TYPE)])
        edited = self.call('runtime.component.edit',session_id=self.session,request_id=uuid.uuid4().hex,
            expected_tick=0,expected_revision=0,id=A,type=TYPE,values=values(91,(3,9),C))
        self.assertEqual(edited['tick'],0)
        self.reject(-32009,session_id=self.session,tick=0,ids=[A],component_revision=initial['component_revision'])
        self.reject(-32009,session_id=self.session,tick=0,query=dict(type=TYPE,after=A),structure_revision=0,component_revision=0)
        current = self.observe(ids=[A],component_revision=1,components=[dict(type=TYPE)])
        self.assertEqual(current['entities'][0]['components'][TYPE],values(91,(3,9),C))
        spawned = self.call('runtime.structure.transact',session_id=self.session,request_id=uuid.uuid4().hex,
            expected_tick=0,expected_structure_revision=0,spawns=[dict(template_id=TEMPLATE)],despawns=[])
        born = spawned['spawned'][0]
        self.assertEqual(spawned['tick'],0)
        self.reject(-32009,session_id=self.session,tick=0,ids=[A],structure_revision=0)
        self.reject(-32009,session_id=self.session,tick=0,query=dict(type=TYPE,after=A),structure_revision=0,component_revision=1)
        live = self.observe(ids=[born],query=dict(type=TYPE),structure_revision=1,
            component_revision=spawned['component_revision'],components=[dict(type=TYPE)])
        self.assertIn(born,live['query']['entities'])
        row = next(r for r in live['entities'] if r['id']==born)
        self.assertEqual(row['components'][TYPE],values(3,(2,1),A))
        self.call('runtime.ui.edit',session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=0,
            expected_ui_revision=0,edits=[dict(id=LABEL,text='Same-tick UI')])
        self.reject(-32009,session_id=self.session,tick=0,ids=[A],ui_revision=0)
        observed = self.observe(ids=[A],ui_revision=1,control_sequence=0,gameplay_revision=0)
        self.assertEqual(observed['ui_revision'],1)
        self.assertEqual(observed['tick'],0)

    def test_frozen_authoring_and_exact_tick(self):
        self.start()
        before = self.observe(ids=[A,B],entity_fields=sorted(ALL_FIELDS),components=[dict(type=TYPE)])
        self.tx(1,[set_component(A,'Transform',transform((30,0,0))),set_component(A,'game:'+TYPE,values(1,(1,),C))])
        after = self.observe(ids=[A,B],entity_fields=sorted(ALL_FIELDS),components=[dict(type=TYPE)])
        self.assertEqual(after['entities'],before['entities'])
        self.assertEqual(after['authored_revision'],1)
        self.assertEqual(after['current_authored_revision'],2)
        self.assertTrue(after['source_stale'])
        self.call('runtime.step',session_id=self.session,request_id=uuid.uuid4().hex,expected_tick=0,ticks=1)
        self.reject(-32009,session_id=self.session,tick=0,ids=[A])
        self.assertEqual(self.observe(ids=[A],tick=1)['tick'],1)

    def test_invalid_requests_never_partially_publish_or_mutate(self):
        self.start()
        original = self.stable_state()
        invalid = [dict(),dict(ids=[]),dict(ids=[A,A]),dict(ids='not-an-array'),dict(ids=[A],extra=True),
            dict(ids=[uid(10000+i) for i in range(33)]),dict(ids=['X'*32]),
            dict(query={}),dict(query=dict(type='game:'+TYPE)),dict(query=dict(type=TYPE,limit=0)),
            dict(query=dict(type=TYPE,limit=65)),dict(query=dict(type=TYPE,limit=True)),dict(query=dict(type=TYPE,extra=1)),
            dict(ids=[A],entity_fields=['world_matrix','world_matrix']),dict(ids=[A],entity_fields=['unknown']),
            dict(ids=[A],entity_fields=['id']),dict(ids=[A],entity_fields='world_matrix'),
            dict(ids=[A],include_schemas=1),dict(ids=[A],components={}),
            dict(ids=[A],components=[dict(type=TYPE),dict(type=TYPE)]),
            dict(ids=[A],components=[dict(type=TYPE,extra=1)]),dict(ids=[A],components=[dict(type='game:'+TYPE)]),
            dict(ids=[A],components=[dict(type=TYPE,fields=[FIELDS[0],FIELDS[0]])]),
            dict(ids=[A],components=[dict(type=TYPE,fields=[uid(999)])]),
            dict(ids=[A],components=[dict(type=TYPE,fields='all')]),
            dict(ids=[A],components=[dict(type=uid(999))])]
        for params in invalid:
            with self.subTest(params=params):
                self.reject(session_id=self.session,tick=0,**params)
        self.reject(-32004,session_id=self.session,tick=0,ids=[A,uid(999)])
        self.reject(-32030,session_id=uuid.uuid4().hex,tick=0,ids=[A])
        with self.assertRaises(RpcError) as missing:
            self.call('runtime.observe',session_id=self.session,ids=[A])
        self.assertEqual(missing.exception.code,-32602)
        self.assertEqual(self.stable_state(),original)

    def test_invalid_revision_numbers_and_stale_pins(self):
        self.start()
        before = self.stable_state()
        for name in HEADER_FIELDS:
            self.reject(-32009,session_id=self.session,tick=0,ids=[A],**{name:1})
            for value in (True,-1,1.5,'0',None,9007199254740992):
                with self.subTest(guard=name,value=value):
                    self.reject(session_id=self.session,tick=0,ids=[A],**{name:value})
        for value in (True,-1,1.5,'0',None,9007199254740992):
            self.reject(session_id=self.session,tick=value,ids=[A])
        result = self.observe(ids=[A],**dict.fromkeys(HEADER_FIELDS,0))
        self.assertTrue(all(result[name]==0 for name in HEADER_FIELDS))
        self.assertEqual(self.stable_state(),before)

    def test_full_bounded_union_is_complete_and_success_reads_are_inert(self):
        if not ARGS.runtime:
            self.skipTest('Simulation disabled in this build')
        explicit = [uid(10000+i) for i in range(32)]
        queried = [uid(500+i) for i in range(64)]
        extra_types = [uid(300),uid(400)]
        ops = []
        for type in extra_types:
            ops.append(dict(op='component.schema.set',schema=dict(id=type,name='Type '+type,version=1,
                fields=[dict(id=uid(9000),name='Unused',kind='int32',default=0)])))
        for identity in explicit+queried:
            ops.append(create(identity))
        for identity in queried:
            ops.append(set_component(identity,'game:'+OTHER,{OTHER_FIELD:float(int(identity,16))}))
        self.tx(1,ops)
        self.start(2)
        before = self.stable_state()
        selected = [TYPE,OTHER,*extra_types]
        observed = self.observe(ids=explicit,query=dict(type=OTHER,limit=64),entity_fields=sorted(ALL_FIELDS),
            components=[dict(type=type) for type in selected],include_schemas=True)
        expected_page = [B,*queried[:-1]]
        self.assertEqual(observed['query'],dict(type=OTHER,entities=expected_page,next_after=queried[-2]))
        self.assertEqual([r['id'] for r in observed['entities']],sorted(explicit+expected_page))
        self.assertEqual(len(observed['entities']),96)
        self.assertEqual([r['type'] for r in observed['component_types']],selected)
        for row in observed['entities']:
            self.assertEqual(set(row['state']),ALL_FIELDS)
            self.assertEqual(set(row['components']),set(selected))
            for type in extra_types:
                self.assertIsNone(row['components'][type])
        self.assertLessEqual(len(json.dumps(observed,ensure_ascii=False,separators=(',',':')).encode('utf-8')),1024*1024)
        last = self.observe(query=dict(type=OTHER,after=queried[-2],limit=64),structure_revision=0,component_revision=0)
        self.assertEqual(last['query'],dict(type=OTHER,entities=[queried[-1]],next_after=None))
        self.assertEqual(self.stable_state(),before)


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--runtime',type=int,choices=(0,1),default=1)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--evidence',type=Path)
    ARGS = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Contract))
    if ARGS.evidence:
        ARGS.evidence.parent.mkdir(parents=True,exist_ok=True)
        ARGS.evidence.write_text(json.dumps(dict(tests=result.testsRun,skipped=len(result.skipped),
            failures=len(result.failures),errors=len(result.errors),passed=result.wasSuccessful(),
            runtime=bool(ARGS.runtime),windows_interop=ARGS.windows_interop,rpc_count=len(CALLS),calls=CALLS,
            processes=PROCESSES,binary_sha256=hashlib.sha256(ARGS.binary.read_bytes()).hexdigest(),
            test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()),indent=2)+'\n',encoding='utf-8')
    return 0 if result.wasSuccessful() else 1


if __name__=='__main__':
    raise SystemExit(main())
