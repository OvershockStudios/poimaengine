#!/usr/bin/env python3
"""Versioned gamepad profiles and deterministic native input through JSON-RPC."""
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
parser.add_argument('--windows-interop',action='store_true')
parser.add_argument('--evidence',type=Path)
args=parser.parse_args();BINARY=str(args.binary.resolve())
ROOT=Path(__file__).resolve().parents[1];SCRATCH=ROOT/'build/gamepad-contract'
SCRATCH.mkdir(parents=True,exist_ok=True)
V1={'bindings':{'forward':['key.w'],'backward':['key.s'],'left':['key.a'],'right':['key.d'],'jump':['key.space'],'use':['key.e']},
    'sensitivity_x':.1,'sensitivity_y':.1,'invert_x':False,'invert_y':False}
V1_HASH='bdbb5f6d3b0ceb20692c2dedfa200633ab8c56e82ecd37ecbe11114c475b8690'
V2=copy.deepcopy(V1)
V2['bindings']['jump'].append('gamepad.south');V2['bindings']['use'].append('gamepad.west')
V2['gamepad']={'move':{'stick':'left','inner_deadzone':.15,'outer_deadzone':.95,'response':1,'invert_x':False,'invert_y':False},
    'look':{'stick':'right','inner_deadzone':.15,'outer_deadzone':.95,'response':1,'invert_x':False,'invert_y':False},
    'look_degrees_per_second':[180,120],'trigger_press':.55,'trigger_release':.45}
REPORTS=[]
def rpc(method,**params):return {'jsonrpc':'2.0','method':method,'params':params}
def button(name,down=True):return {'control':name,'down':down}
def axis(name,value):return {'gamepad_axis':{'axis':name,'value':value}}
def consume():return {'consume':True}
def connect(**values):return {'gamepad_connect':values}

class GamepadContract(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=SCRATCH);self.root=Path(self.tmp.name)
        self.world=self.root/'world.json';self.profile=self.root/'pad.poima-input.json'
        self.result(self.requests(rpc('world.transact',request_id=uuid.uuid4().hex,base_revision=0,
            ops=[{'op':'entity.create','id':'1'*32,'name':'Unchanged authored world'}]))[0])
        self.world_bytes=self.world.read_bytes()
    def tearDown(self):
        self.assertEqual(self.world.read_bytes(),self.world_bytes,'Gamepad operations changed the authored world.')
        self.tmp.cleanup()
    def native(self,path):
        return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip() if args.windows_interop else str(path.resolve())
    def requests(self,*requests):
        p=subprocess.run([BINARY,'world',self.native(self.world)],
            input=''.join(json.dumps(dict(r,id=i))+'\n' for i,r in enumerate(requests)),text=True,encoding='utf-8',capture_output=True,timeout=30)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        result=[json.loads(line) for line in p.stdout.splitlines()];self.assertEqual(len(result),len(requests));return result
    def result(self,response):self.assertIn('result',response,response);return response['result']
    def error(self,response,code=-32602):
        self.assertIn('error',response,response);self.assertEqual(response['error']['code'],code,response)
    def transaction(self,profile=V2,revision=0,path=None,**extra):
        return rpc('input.transact',path=self.native(path or self.profile),request_id=uuid.uuid4().hex,
            expected_revision=revision,profile=copy.deepcopy(profile),**extra)
    def evaluate(self,events,path=None,**extra):
        params={'events':events}
        if path is None:params['gamepad_defaults']=True
        else:params['path']=self.native(path)
        params.update(extra);return self.result(self.requests(rpc('input.evaluate',**params))[0])
    def test_discovery_and_device_query(self):
        schema,description,devices=[self.result(r) for r in self.requests(rpc('world.describe'),rpc('input.describe'),rpc('input.devices'))]
        self.assertGreaterEqual(schema['schema_revision'],18)
        self.assertIn('input.devices',schema['methods']);self.assertIn('gamepad',schema['methods']['runtime.play']['properties'])
        self.assertEqual(description['defaults'],V1);self.assertEqual(description['gamepad_defaults'],V2)
        self.assertEqual(description['supported_formats'],['poima.input.v1','poima.input.v2'])
        self.assertEqual(len(description['profile_schema']['anyOf']),2)
        controls={c['id']:c for c in description['controls']}
        for name in ['gamepad.south','gamepad.west','gamepad.left_trigger','gamepad.right_trigger','gamepad.dpad_up']:
            self.assertEqual(controls[name]['device'],'gamepad');self.assertFalse(controls[name]['reserved'])
        for name in ['gamepad.start','gamepad.guide']:self.assertIn(name,description['reserved_controls'])
        self.assertIsInstance(devices['available'],bool);self.assertIsInstance(devices['devices'],list)
        if not devices['available']:self.assertEqual(devices['devices'],[])
        REPORTS.append({'device_host_available':devices['available'],'reported_devices':len(devices['devices'])})
    def test_v1_hash_and_opt_in_remain_unchanged(self):
        result=self.result(self.requests(self.transaction(V1))[0]);self.assertEqual(result['content_hash'],V1_HASH)
        saved=self.profile.read_bytes();self.assertEqual(json.loads(saved)['format'],'poima.input.v1')
        default=self.result(self.requests(rpc('input.evaluate',events=[button('key.w'),consume()]))[0])
        self.assertEqual(default['frames'][0]['move'],[0,1])
        explicit_false=self.result(self.requests(rpc('input.evaluate',gamepad_defaults=False,events=[button('key.w'),consume()]))[0])
        self.assertEqual(explicit_false,default)
        for events in [[connect()],[button('gamepad.south')],[axis('left_x',0)],[{'gamepad_disconnect':True}]]:
            for params in [{'events':events},{'events':events,'path':self.native(self.profile)}]:
                self.error(self.requests(rpc('input.evaluate',**params))[0])
        self.assertEqual(self.profile.read_bytes(),saved)
        REPORTS.append({'v1_default_hash':V1_HASH,'v1_gamepad_opt_in_required':True})
    def test_v2_roundtrip_preview_replay_and_all_fields(self):
        profile=copy.deepcopy(V2);pad=profile['gamepad']
        pad['move'].update(stick='right',inner_deadzone=.2,outer_deadzone=.8,response=2,invert_x=True,invert_y=True)
        pad['look'].update(stick='left',inner_deadzone=.05,outer_deadzone=.9,response=.5,invert_x=True,invert_y=True)
        pad.update(look_degrees_per_second=[240,90],trigger_press=.7,trigger_release=.3)
        preview=self.result(self.requests(self.transaction(profile,preview=True))[0]);self.assertFalse(self.profile.exists())
        self.assertEqual(preview['profile'],profile)
        request=self.transaction(profile);first=self.result(self.requests(request)[0]);raw=self.profile.read_bytes()
        self.assertEqual(json.loads(raw)['format'],'poima.input.v2')
        inspect=self.result(self.requests(rpc('input.inspect',path=self.native(self.profile)))[0])
        self.assertEqual(inspect['profile'],profile);self.assertEqual(inspect['format'],'poima.input.v2')
        self.assertEqual(inspect['content_hash'],first['content_hash'])
        self.assertEqual(self.result(self.requests(request)[0]),dict(first,replayed=True));self.assertEqual(self.profile.read_bytes(),raw)
        no_op=self.result(self.requests(self.transaction(profile,revision=1))[0]);self.assertEqual(no_op['revision'],2)
        self.assertFalse(no_op['changed']);self.assertEqual(no_op['content_hash'],first['content_hash'])
        REPORTS.append({'v2_roundtrip_hash':first['content_hash'],'all_gamepad_parameters_roundtrip':True})
    def test_explicit_conversion_preserves_original_bytes_and_receipts(self):
        self.result(self.requests(self.transaction(V1))[0]);raw=self.profile.read_bytes()
        for preview in [False,True]:
            self.error(self.requests(self.transaction(V2,revision=1,preview=preview))[0]);self.assertEqual(self.profile.read_bytes(),raw)
        target=self.root/'converted.poima-input.json'
        converted=copy.deepcopy(V1);converted['gamepad']=copy.deepcopy(V2['gamepad'])
        self.result(self.requests(self.transaction(converted,path=target))[0]);self.assertEqual(self.profile.read_bytes(),raw)
        self.assertEqual(json.loads(target.read_bytes())['format'],'poima.input.v2')
        self.error(self.requests(self.transaction(V1,path=target,revision=1))[0])
    def test_mixed_format_receipts_and_envelopes_are_corrupt(self):
        old=self.root/'old.poima-input.json';self.result(self.requests(self.transaction(V1,path=old))[0])
        self.result(self.requests(self.transaction())[0]);valid=json.loads(self.profile.read_bytes())
        mixed=copy.deepcopy(valid);mixed['receipts'][0]=json.loads(old.read_bytes())['receipts'][0]
        wrong=copy.deepcopy(valid);wrong['format']='poima.input.v1'
        for document in [mixed,wrong]:
            raw=json.dumps(document).encode();self.profile.write_bytes(raw)
            for request in [rpc('input.inspect',path=self.native(self.profile)),self.transaction(revision=1),
                            rpc('input.evaluate',path=self.native(self.profile),events=[])]:
                self.error(self.requests(request)[0],-32070);self.assertEqual(self.profile.read_bytes(),raw)
    def test_analog_rates_repeat_for_sixty_ticks_and_combine_with_keys(self):
        result=self.evaluate([connect(),axis('right_x',32767),axis('left_y',-32768)]+[consume()]*60)
        self.assertEqual(len(result['frames']),60)
        for frame in result['frames']:self.assertEqual(frame['look'],[-3,0]);self.assertEqual(frame['move'],[0,1])
        self.assertEqual(sum(f['look'][0] for f in result['frames']),-180)
        combined=self.evaluate([connect(),axis('left_x',32767),button('key.a'),consume(),button('key.a',False),consume()])
        self.assertEqual(combined['frames'][0]['move'],[0,0]);self.assertEqual(combined['frames'][1]['move'],[1,0])
        self.assertTrue(result['gamepad']['connected']);self.assertTrue(result['gamepad']['armed'])
        REPORTS.append({'fixed_ticks':60,'full_right_stick_total_yaw':-180})
    def test_deadzone_response_swapped_sticks_and_disabled_sticks(self):
        p=copy.deepcopy(V2);p['gamepad']['move'].update(inner_deadzone=0,outer_deadzone=1,response=2)
        self.result(self.requests(self.transaction(p))[0])
        frames=self.evaluate([connect(),axis('left_x',16384),consume()],path=self.profile)['frames']
        self.assertAlmostEqual(frames[0]['move'][0],(16384/32767)**2,places=6)
        p['gamepad']['move'].update(stick='right',invert_x=True);p['gamepad']['look'].update(stick='left',invert_y=True)
        p['gamepad']['look_degrees_per_second']=[240,90]
        self.result(self.requests(self.transaction(p,revision=1))[0])
        frame=self.evaluate([connect(),axis('right_x',32767),axis('left_y',32767),consume()],path=self.profile)['frames'][0]
        self.assertEqual(frame['move'],[-1,0]);self.assertEqual(frame['look'],[0,1.5])
        p['gamepad']['move']['stick']='none';p['gamepad']['look']['stick']='none'
        self.result(self.requests(self.transaction(p,revision=2))[0])
        frame=self.evaluate([connect(),axis('right_x',32767),axis('left_y',32767),consume()],path=self.profile)['frames'][0]
        self.assertEqual(frame['move'],[0,0]);self.assertEqual(frame['look'],[0,0])
        frame=self.evaluate([connect(),axis('left_x',1000),axis('right_y',1000),consume()])['frames'][0]
        self.assertEqual(frame['move'],[0,0]);self.assertEqual(frame['look'],[0,0])
    def test_connection_neutral_gate_and_action_edges(self):
        blocked=self.evaluate([connect(axes=[32767,0,0,0,0,0],buttons=1),consume()])
        self.assertFalse(blocked['gamepad']['armed']);self.assertEqual(blocked['frames'][0]['move'],[0,0]);self.assertFalse(blocked['frames'][0]['jump'])
        result=self.evaluate([connect(axes=[32767,0,0,0,0,0],buttons=1),axis('left_x',0),button('gamepad.south',False),
            consume(),button('gamepad.south'),consume(),consume()])
        self.assertTrue(result['gamepad']['armed']);self.assertEqual([f['jump'] for f in result['frames']],[False,True,False])
    def test_raw_reserved_button_release_can_arm_without_binding(self):
        # Start and Guide are reserved authoring controls, but real releases
        # must still reach the native state machine after held-at-connect.
        for control,bit in [('gamepad.start',6),('gamepad.guide',5)]:
            with self.subTest(control=control):
                blocked=self.evaluate([connect(buttons=1<<bit),consume()])
                self.assertFalse(blocked['gamepad']['armed']);self.assertFalse(blocked['frames'][0]['jump'])
                result=self.evaluate([connect(buttons=1<<bit),
                    {'gamepad_button':{'control':control,'down':False}},consume(),
                    {'gamepad_button':{'control':'gamepad.south','down':True}},consume()])
                self.assertTrue(result['gamepad']['armed'])
                self.assertEqual([frame['jump'] for frame in result['frames']],[False,True])
                self.error(self.requests(rpc('input.evaluate',gamepad_defaults=True,events=[button(control,False)]))[0])
        for payload in [{'control':'key.w','down':True},{'control':'mouse.1','down':True},
                        {'control':'gamepad.left_trigger','down':True},{'control':'gamepad.right_trigger','down':False},
                        {'control':'gamepad.unknown','down':True},{'control':'gamepad.south','down':1},
                        {'control':'gamepad.south'},{'control':'gamepad.south','down':True,'extra':0}]:
            with self.subTest(payload=payload):
                self.error(self.requests(rpc('input.evaluate',gamepad_defaults=True,events=[{'gamepad_button':payload}]))[0])
        self.error(self.requests(rpc('input.evaluate',events=[{'gamepad_button':{'control':'gamepad.south','down':False}}]))[0])
    def test_disconnect_preserves_other_sources_and_clear_discards_pending(self):
        result=self.evaluate([connect(),button('gamepad.south'),button('key.e'),{'motion':[10,-20]},
            {'gamepad_disconnect':True},consume(),consume()])
        self.assertFalse(result['gamepad']['connected']);self.assertFalse(result['gamepad']['armed'])
        self.assertFalse(result['frames'][0]['jump']);self.assertTrue(result['frames'][0]['use'])
        self.assertEqual(result['frames'][0]['look'],[-1,2]);self.assertFalse(result['frames'][1]['use'])
        result=self.evaluate([connect(),axis('right_x',32767),button('key.space'),{'clear':True},consume(),
            axis('right_x',0),consume(),button('gamepad.south'),consume()])
        self.assertEqual(result['frames'][0]['look'],[0,0]);self.assertFalse(result['frames'][0]['jump'])
        self.assertFalse(result['frames'][1]['jump']);self.assertTrue(result['frames'][2]['jump'])
    def test_trigger_hysteresis_has_one_edge_until_release(self):
        p=copy.deepcopy(V2);p['bindings']['jump']=['key.space','gamepad.right_trigger']
        self.result(self.requests(self.transaction(p))[0])
        result=self.evaluate([connect(),axis('right_trigger',20000),consume(),axis('right_trigger',17000),consume(),
            axis('right_trigger',19000),consume(),axis('right_trigger',14000),axis('right_trigger',20000),consume()],path=self.profile)
        self.assertEqual([f['jump'] for f in result['frames']],[True,False,False,True])
    def test_invalid_gamepad_profiles_preserve_last_good(self):
        self.result(self.requests(self.transaction())[0]);raw=self.profile.read_bytes();variants=[]
        def altered(change):
            p=copy.deepcopy(V2);change(p);variants.append(p)
        for value in [None,False,[],{}]:altered(lambda p,v=value:p.update(gamepad=v))
        for key,value in [('stick','center'),('inner_deadzone',-.1),('outer_deadzone',1.1),('inner_deadzone',.95),
                          ('response',0),('response',9),('invert_x',1),('invert_y','true'),('unexpected',0)]:
            altered(lambda p,k=key,v=value:p['gamepad']['move'].update({k:v}))
        altered(lambda p:p['gamepad']['look'].update(stick='left'))
        for key,value in [('look_degrees_per_second',[1]),('look_degrees_per_second',[1,1081]),('look_degrees_per_second',[True,1]),
                          ('trigger_press',.45),('trigger_release',-.1),('trigger_press',1.1),('trigger_release','0.1'),('unknown',0)]:
            altered(lambda p,k=key,v=value:p['gamepad'].update({k:v}))
        for control in ['gamepad.start','gamepad.guide','gamepad.unknown']:
            altered(lambda p,c=control:p['bindings'].update(jump=[c]))
        for profile in variants:
            with self.subTest(profile=profile):
                self.error(self.requests(self.transaction(profile,revision=1))[0]);self.assertEqual(self.profile.read_bytes(),raw)
    def test_invalid_events_and_request_combinations_preserve_files(self):
        self.result(self.requests(self.transaction())[0]);raw=self.profile.read_bytes()
        bad_events=[connect(axes=[0]*5),connect(axes=[0,0,0,0,-1,0]),connect(axes=[True,0,0,0,0,0]),
            connect(buttons=-1),connect(buttons=1<<26),connect(buttons=True),connect(unexpected=0),
            axis('unknown',0),axis('left_x',32768),axis('left_x',-32769),axis('left_x',True),axis('left_x',.5),
            axis('right_trigger',-1),{'gamepad_disconnect':False},{'gamepad_disconnect':1},
            {'gamepad_axis':{'axis':'left_x','value':0,'extra':True}},{'gamepad_connect':{},'consume':True},
            button('gamepad.right_trigger'),button('gamepad.start'),button('gamepad.guide')]
        for event in bad_events:
            with self.subTest(event=event):
                self.error(self.requests(rpc('input.evaluate',gamepad_defaults=True,events=[event]))[0]);self.assertEqual(self.profile.read_bytes(),raw)
        for params in [{'path':self.native(self.profile),'gamepad_defaults':True,'events':[]},
                       {'path':self.native(self.profile),'gamepad_defaults':False,'events':[]},
                       {'gamepad_defaults':1,'events':[]},
                       {'gamepad_defaults':True,'events':[consume()]*257}]:
            self.error(self.requests(rpc('input.evaluate',**params))[0]);self.assertEqual(self.profile.read_bytes(),raw)

if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(GamepadContract))
    if args.evidence:args.evidence.write_text(json.dumps({'tests':result.testsRun,'passed':result.wasSuccessful(),
        'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'reports':REPORTS},indent=2)+'\n')
    raise SystemExit(not result.wasSuccessful())
