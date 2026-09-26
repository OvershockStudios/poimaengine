#!/usr/bin/env python3
"""Drive real native runtime commands; no Python participates in simulation."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

parser=argparse.ArgumentParser()
parser.add_argument('binary',type=Path)
parser.add_argument('--windows-interop',action='store_true')
parser.add_argument('--evidence',type=Path)
args=parser.parse_args()
BINARY=str(args.binary.resolve())
ROOT=Path(__file__).resolve().parents[1]
SCRATCH=ROOT/'build/runtime-contract';SCRATCH.mkdir(parents=True,exist_ok=True)
FIXTURE=json.loads((ROOT/'examples/physics-room.jsonl').read_text())
REPLAY_TRACE=[]


def uid(n):return f'{n:032x}'
def rpc(method,params):return {'jsonrpc':'2.0','method':method,'params':params}
def start(session=900,revision=1):return rpc('runtime.start',{'session_id':uid(session),'revision':revision})
def step(tick,count,inputs=(),session=900,request_id=None):
    return rpc('runtime.step',{'session_id':uid(session),'request_id':request_id or uuid.uuid4().hex,'expected_tick':tick,'ticks':count,'inputs':list(inputs)})
def inspect_entity(n=100,session=900,tick=None):
    p={'session_id':uid(session),'id':uid(n)}
    if tick is not None:p['tick']=tick
    return rpc('runtime.entity',p)
def input_value(move=(0,0),look=(0,0),jump=False,n=100):return {'entity':uid(n),'move':list(move),'look':list(look),'jump':jump}
def transform(n,position):return {'op':'component.set','id':uid(n),'type':'Transform','value':{'position':position,'rotation':[0,0,0,1],'scale':[1,1,1]}}
def txn(revision,ops):return rpc('world.transact',{'base_revision':revision,'request_id':uuid.uuid4().hex,'ops':ops})


class RuntimeContract(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory(dir=SCRATCH)
        self.path=Path(self.directory.name)/'physics world.json'
    def tearDown(self):self.directory.cleanup()
    def native(self,path):
        path=str(path.resolve())
        return subprocess.check_output(['wslpath','-w',path],text=True).strip() if args.windows_interop else path
    def run_requests(self,requests):
        requests=[{**request,'id':n+1} for n,request in enumerate(requests)]
        p=subprocess.run([BINARY,'world',self.native(self.path)],input=''.join(json.dumps(r)+'\n' for r in requests),
                         capture_output=True,text=True,encoding='utf-8',timeout=60)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        responses=[json.loads(line) for line in p.stdout.splitlines()]
        self.assertEqual(len(responses),len(requests),p.stdout+p.stderr)
        for n,response in enumerate(responses):self.assertEqual(response['id'],n+1)
        return responses
    def result(self,response):
        self.assertIn('result',response,response);return response['result']
    def error(self,response,code):self.assertEqual(response['error']['code'],code,response)

    def test_collision_jump_retries_and_live_authoring_separation(self):
        forward=step(120,180,[input_value(move=(0,1))],request_id=uid(10000))
        requests=[FIXTURE,start(),step(0,120),inspect_entity(),inspect_entity(3),forward,forward,inspect_entity(tick=300),
            step(120,1),{**forward,'params':{**forward['params'],'ticks':179}},
            step(300,1,[input_value(n=99)]),inspect_entity(tick=300),
            step(300,10,[input_value(jump=True)]),inspect_entity(),step(310,120),inspect_entity(),
            txn(1,[transform(2,[50,1.5,-3])]),rpc('runtime.inspect',{'session_id':uid(900)}),inspect_entity(2),
            rpc('entity.get',{'id':uid(2),'component':'Transform'}),start(),
            rpc('runtime.stop',{'session_id':uid(901)}),rpc('runtime.stop',{'session_id':uid(900)}),
            rpc('runtime.stop',{'session_id':uid(900)}),start(),start(901,2),step(0,120,session=901),
            step(120,120,[input_value(move=(0,1))],session=901),inspect_entity(session=901),inspect_entity()]
        r=self.run_requests(requests)
        landed=self.result(r[3]);self.assertEqual(landed['ground'],'on_ground');self.assertLess(abs(landed['world_matrix'][13]),.05)
        self.assertAlmostEqual(self.result(r[4])['world_matrix'][13],.38,delta=.03)
        self.assertTrue(self.result(r[6])['replayed'])
        wall=self.result(r[7]);self.assertGreater(wall['world_matrix'][14],-2.5);self.assertLess(wall['world_matrix'][14],-2.3)
        self.error(r[8],-32009);self.error(r[9],-32010);self.error(r[10],-32040)
        self.assertEqual(wall,self.result(r[11]))
        self.assertGreater(self.result(r[13])['world_matrix'][13],.5)
        self.assertEqual(self.result(r[15])['ground'],'on_ground')
        summary=self.result(r[17]);self.assertTrue(summary['source_stale']);self.assertEqual(summary['authored_revision'],1);self.assertEqual(summary['current_authored_revision'],2)
        self.assertEqual(self.result(r[18])['world_matrix'][12],0)
        self.assertEqual(self.result(r[19])['value']['position'][0],50)
        self.assertTrue(self.result(r[20])['replayed'])
        self.error(r[21],-32030);self.assertTrue(self.result(r[23])['replayed']);self.error(r[24],-32010)
        self.assertLess(self.result(r[28])['world_matrix'][14],-5)
        self.error(r[29],-32030)
        saved=json.loads(self.path.read_text())
        self.assertEqual(saved['revision'],2)
        self.assertEqual(saved['entities'][uid(100)]['components']['Transform']['position'],[0,1,2])
        self.assertEqual(saved['entities'][uid(3)]['components']['Transform']['position'],[2,3,0])

    def test_repeatable_replay_and_chunk_equivalence(self):
        def sequence(split):
            requests=[FIXTURE,start()]
            requests+=([step(0,60),step(60,60)] if split else [step(0,120)])
            requests += [inspect_entity(),inspect_entity(3)]
            requests += ([step(120,90,[input_value(move=(0,1))]),step(210,90,[input_value(move=(0,1))])] if split else [step(120,180,[input_value(move=(0,1))])])
            requests += [inspect_entity(),step(300,10,[input_value(jump=True)]),inspect_entity(),
                step(310,120),step(430,1,[input_value(look=(45,20))]),inspect_entity(),inspect_entity(101),
                step(431,60,[input_value(move=(.4,-.7))]),inspect_entity(),inspect_entity(101)]
            return requests
        first=self.run_requests(sequence(False));before=self.path.read_bytes()
        second=self.run_requests(sequence(False));split=self.run_requests(sequence(True))
        states=lambda responses:[self.result(r) for r in responses if 'result' in r and 'world_matrix' in r['result']]
        self.assertEqual(states(first),states(second));self.assertEqual(states(first),states(split))
        REPLAY_TRACE.extend(states(first))
        self.assertEqual(self.path.read_bytes(),before)

    def test_validation_failed_start_recovery_and_stale_observation(self):
        bad_controller={'op':'component.set','id':uid(100),'type':'CharacterController','value':{'radius':.3,'height':1.8,'speed':4,'jump_speed':5,'camera':uid(999)}}
        good_controller={**bad_controller,'value':{**bad_controller['value'],'camera':uid(101)}}
        capture={'session_id':uid(900),'tick':1,'camera':uid(101),'path':self.native(Path(self.directory.name)/'never.bmp')}
        requests=[FIXTURE,txn(1,[bad_controller]),start(revision=2),txn(2,[good_controller]),start(revision=3),
            step(0,0),step(0,601),step(0,1,[input_value(move=(2,0))]),step(0,1,[input_value(),input_value()]),
            inspect_entity(tick=1),rpc('runtime.capture',capture),rpc('runtime.inspect',{'session_id':uid(900)}),
            step(0,1),start(901,3),rpc('runtime.capture',{**capture,'tick':1,'camera':uid(999)})]
        r=self.run_requests(requests)
        self.error(r[2],-32602);self.result(r[4])
        for n in [5,6,7]:self.error(r[n],-32602)
        self.error(r[8],-32040)
        for n in [9,10]:self.error(r[n],-32009)
        self.assertEqual(self.result(r[11])['tick'],0);self.assertEqual(self.result(r[12])['tick'],1)
        self.error(r[13],-32031)
        self.error(r[14],-32004)
        self.assertFalse((Path(self.directory.name)/'never.bmp').exists())

    def test_receipt_window_and_repeated_lifetimes(self):
        original=step(0,1,request_id=uid(10000))
        requests=[FIXTURE,start(),original]
        requests += [step(tick,1) for tick in range(1,34)]
        requests += [original,rpc('runtime.stop',{'session_id':uid(900)})]
        for session in range(901,931):requests += [start(session),step(0,1,session=session),rpc('runtime.stop',{'session_id':uid(session)})]
        r=self.run_requests(requests)
        self.error(r[36],-32009)
        for n,response in enumerate(r):
            if n!=36:self.result(response)


if __name__=='__main__':
    import hashlib
    import sys
    result=unittest.main(argv=['runtime_contract'],verbosity=2,exit=False).result
    if args.evidence:
        args.evidence.parent.mkdir(parents=True,exist_ok=True)
        args.evidence.write_text(json.dumps({'binary':BINARY,'binary_sha256':hashlib.sha256(Path(BINARY).read_bytes()).hexdigest(),
            'tests_run':result.testsRun,'passed':result.wasSuccessful(),'skips':result.skipped,'replay_trace':REPLAY_TRACE},indent=2)+'\n')
    sys.exit(0 if result.wasSuccessful() else 1)
