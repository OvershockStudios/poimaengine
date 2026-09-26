#!/usr/bin/env python3
"""Exercise authored/resolved lighting and frozen simulation through the native CLI."""
# SPDX-License-Identifier: Apache-2.0
import json
import math
import subprocess
import tempfile
import unittest
import uuid
from pathlib import Path
from world_contract import Client, BINARY, SCRATCH, create, uid


def component(n,kind,value):return {'op':'component.set','id':uid(n),'type':kind,'value':value}
def remove(n,kind):return {'op':'component.remove','id':uid(n),'type':kind}
def transform(n,p=(0,0,0),q=(0,0,0,1),s=(1,1,1)):
    return component(n,'Transform',{'position':list(p),'rotation':list(q),'scale':list(s)})
def light(n,kind='point',**kw):return component(n,'Light',{'kind':kind,'color':[1,1,1],'intensity':1,'enabled':True,**kw})
def environment(n,ambient=(0,0,0),exposure=1):return component(n,'LightingEnvironment',{'ambient':list(ambient),'exposure':exposure})


class LightingContract(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory(prefix='lights-',dir=SCRATCH)
        self.path=Path(self.directory.name)/'lights.json';self.clients=[]
    def tearDown(self):
        for c in self.clients:
            try:c.close()
            finally:
                if c.process.poll() is None:c.process.kill();c.process.wait(timeout=10)
        self.directory.cleanup()
    def open(self):
        c=Client(self.path);self.clients.append(c);return c
    def test_discovery_preview_receipts_persistence_and_fallback(self):
        c=self.open();d=c.rpc('world.describe')
        self.assertGreaterEqual(d['schema_revision'],8)
        for name in ('world.lighting','runtime.lighting'):self.assertIn(name,d['methods'])
        self.assertEqual(len(d['components']['Light']['oneOf']),3)
        self.assertEqual(d['limits']['enabled_lights'],64)
        original=c.rpc('world.lighting');self.assertTrue(original['preview_fallback']);self.assertFalse(self.path.exists())
        request={'base_revision':0,'request_id':uuid.uuid4().hex,'ops':[create(1),light(1,enabled=False)]}
        c.rpc('world.transact',{**request,'preview':True});self.assertEqual(c.rpc('world.lighting'),original)
        c.rpc('world.transact',request);self.assertTrue(c.rpc('world.transact',request)['replayed'])
        disabled=c.rpc('world.lighting',{'revision':1});self.assertFalse(disabled['preview_fallback']);self.assertEqual(disabled['lights'],[])
        self.assertEqual(len(c.rpc('entity.query',{'component':'Light'})['entities']),1)
        c.rpc('world.lighting',{'revision':0},error=-32009)
        before=self.path.read_bytes();c.close();c=self.open();self.assertEqual(c.rpc('world.lighting'),disabled)
        c.rpc('world.transact',request);self.assertEqual(self.path.read_bytes(),before)
        c.txn(1,[remove(1,'Light'),environment(1,ambient=(.1,.2,.3),exposure=2)])
        env=c.rpc('world.lighting');self.assertFalse(env['preview_fallback']);self.assertEqual(env['lights'],[]);self.assertEqual(env['exposure'],2)
        c.txn(2,[remove(1,'LightingEnvironment')]);self.assertTrue(c.rpc('world.lighting')['preview_fallback'])
    def test_transformed_pose_and_units(self):
        c=self.open();half=math.sqrt(.5)
        c.txn(0,[create(1),transform(1,(10,20,30),(0,half,0,half),(2,3,4)),create(2,uid(1)),transform(2,(1,2,3),s=(5,6,7)),light(2,'spot',range=8,inner_angle=15,outer_angle=30,intensity=40),create(3),light(3,'directional')])
        lights={x['id']:x for x in c.rpc('world.lighting')['lights']};spot=lights[uid(2)]
        for a,b in zip(spot['position'],[22,26,28]):self.assertAlmostEqual(a,b)
        for a,b in zip(spot['direction'],[-1,0,0]):self.assertAlmostEqual(a,b)
        self.assertEqual((spot['intensity'],spot['range'],spot['inner_angle'],spot['outer_angle']),(40,8,15,30))
        self.assertEqual(spot['intensity_unit'],'candela');self.assertEqual(lights[uid(3)]['intensity_unit'],'lux')
    def test_invalid_values_and_global_limits_are_atomic(self):
        c=self.open();c.txn(0,[create(1),light(1)])
        invalid=[light(1,kind='unknown'),light(1,kind='directional',range=0),light(1,inner_angle=0),light(1,enabled=1),light(1,intensity=-1),light(1,intensity=1e9+.1),light(1,range=1e9+.1),light(1,color=[1,2,3]),light(1,intensity='1'),light(1,kind='spot',outer_angle=90.000001),light(1,kind='spot',inner_angle=30,outer_angle=30),light(1,kind='spot',inner_angle=-.1),environment(1,exposure=-1),environment(1,ambient=(0,0,1e6+1))]
        before=self.path.read_bytes()
        for op in invalid:
            c.txn(1,[transform(1,(99,0,0)),op],error=-32602);self.assertEqual(self.path.read_bytes(),before)
        c.txn(1,[op for n in range(2,65) for op in (create(n),light(n))]);self.assertEqual(len(c.rpc('world.lighting')['lights']),64)
        before=self.path.read_bytes();c.txn(2,[create(65),light(65)],error=-32602);self.assertEqual(self.path.read_bytes(),before)
        c.txn(2,[create(65),light(65,enabled=False),environment(1)])
        c.txn(3,[environment(2)],error=-32602);c.txn(3,[light(65)],error=-32602)
        c.txn(3,[light(1,enabled=False),light(65)]);self.assertEqual(len(c.rpc('world.lighting')['lights']),64)
    def test_runtime_freezes_settings_and_tracks_moving_parent(self):
        # Capability is queried from the built binary, so this test also runs in headless-only builds.
        cap=json.loads(subprocess.check_output([BINARY,'capabilities'],text=True))
        if not cap['result']['features']['simulation']:self.skipTest('Simulation disabled in this build')
        c=self.open();session=uid(900)
        c.txn(0,[create(1),transform(1,(0,3,0)),component(1,'BoxCollider',{'half_extents':[.25,.25,.25],'motion':'dynamic','mass':1,'friction':.5,'restitution':0}),create(2,uid(1)),transform(2,(1,2,3)),light(2,'spot',intensity=20),environment(1,exposure=2)])
        c.rpc('runtime.start',{'session_id':session,'revision':1});initial=c.rpc('runtime.lighting',{'session_id':session,'tick':0})
        self.assertEqual(initial['lights'][0]['position'],[1,5,3])
        c.txn(1,[light(2,'spot',intensity=99),environment(1,exposure=4)])
        self.assertEqual(c.rpc('runtime.lighting',{'session_id':session}),initial)
        c.rpc('runtime.lighting',{'session_id':session,'tick':1},error=-32009)
        c.rpc('runtime.lighting',{'session_id':uid(901)},error=-32030)
        c.rpc('runtime.step',{'session_id':session,'request_id':uid(901),'expected_tick':0,'ticks':30})
        current=c.rpc('runtime.lighting',{'session_id':session,'tick':30});parent=c.rpc('runtime.entity',{'session_id':session,'id':uid(1),'tick':30})
        self.assertLess(current['lights'][0]['position'][1],initial['lights'][0]['position'][1]-.5)
        self.assertAlmostEqual(current['lights'][0]['position'][1],parent['world_matrix'][13]+2)
        self.assertEqual(current['lights'][0]['intensity'],20);self.assertEqual(current['exposure'],2)
        self.assertEqual(c.rpc('world.lighting')['lights'][0]['intensity'],99)
        c.rpc('runtime.stop',{'session_id':session})
        c.rpc('runtime.start',{'session_id':uid(902),'revision':2});self.assertEqual(c.rpc('runtime.lighting',{'session_id':uid(902)})['exposure'],4)


if __name__=='__main__':unittest.main(argv=['lighting_contract'],verbosity=2)
