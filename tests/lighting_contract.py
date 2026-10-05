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
def sky(**changes):
    return {'enabled':True,'zenith':[.06,.22,.55],'horizon':[.55,.70,.85],'ground':[.12,.10,.08],
            'horizon_falloff':.35,'sun':None,'sun_size_degrees':.53,'sun_intensity':20,**changes}
def sky_environment(n,value):return component(n,'LightingEnvironment',{'ambient':[0,0,0],'exposure':1,'sky':value})


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
    def test_shadow_defaults_validation_and_resource_limits(self):
        c=self.open();d=c.rpc('world.describe');self.assertGreaterEqual(d['schema_revision'],9)
        self.assertEqual(d['limits']['shadow_views'],16);self.assertEqual(d['limits']['shadow_bytes'],128*1024*1024)
        c.txn(0,[create(1),light(1,'directional',shadow={'enabled':True}),create(2),light(2,'point',shadow={'enabled':True}),create(3),light(3,'spot',shadow={'enabled':True}),environment(1)])
        state=c.rpc('world.lighting');self.assertEqual(state['shadow_views'],11);self.assertEqual(state['shadow_bytes'],11*1024*1024*4)
        self.assertTrue(state['lights'][0]['shadow']['enabled'])
        invalid=[light(1,'directional',shadow={'enabled':1}),light(1,'directional',shadow={'enabled':True,'near':0}),light(1,shadow={'enabled':True,'near':2,'distance':1}),light(1,shadow={'enabled':True,'bias':-.1}),light(1,shadow={'enabled':True,'normal_bias':2}),light(1,shadow={'enabled':True,'unknown':1}),light(1,'spot',outer_angle=90,shadow={'enabled':True}),light(1,range=.01,shadow={'enabled':True}),component(1,'LightingEnvironment',{'ambient':[0,0,0],'exposure':1,'shadow_resolution':1000})]
        before=self.path.read_bytes()
        for op in invalid:c.txn(1,[op],error=-32602);self.assertEqual(self.path.read_bytes(),before)
        # Resolution alone would grow the current eleven depth layers beyond 128 MiB.
        c.txn(1,[component(1,'LightingEnvironment',{'ambient':[0,0,0],'exposure':1,'shadow_resolution':2048})],error=-32602)
        c.txn(1,[create(4),light(4,shadow={'enabled':True})],error=-32602)
        c.txn(1,[light(2,enabled=False,shadow={'enabled':True}),component(1,'LightingEnvironment',{'ambient':[0,0,0],'exposure':1,'shadow_resolution':2048})])
        state=c.rpc('world.lighting');self.assertEqual(state['shadow_views'],5);self.assertEqual(state['shadow_resolution'],2048)
        c.close();c=self.open();self.assertEqual(c.rpc('world.lighting'),state)
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
    def test_sky_discovery_defaults_strict_validation_and_persistence(self):
        c=self.open();d=c.rpc('world.describe');self.assertGreaterEqual(d['schema_revision'],23)
        schema=d['components']['LightingEnvironment']['properties']['sky']
        self.assertEqual(set(schema['required']),set(sky()))
        self.assertEqual(set(schema['default']),set(sky()))
        self.assertFalse(schema['default']['enabled']);self.assertIsNone(schema['default']['sun'])
        for key,value in schema['default'].items():self.assertEqual(schema['properties'][key]['default'],value)
        original=c.rpc('world.lighting');self.assertFalse(original['sky']['enabled']);self.assertIsNone(original['sky_sun'])
        c.txn(0,[create(1),environment(1)]);self.assertFalse(c.rpc('world.lighting')['sky']['enabled'])
        invalid=[None,[],{},sky(enabled=1),sky(extra=True),sky(sun='sun'),sky(sun=3),sky(sun='A'*32),
                 sky(zenith=[1,0]),sky(horizon=[0,True,0]),sky(ground=[0,-.001,0]),sky(zenith=[1.000000001,0,0]),
                 sky(horizon_falloff=.099999999),sky(horizon_falloff=16.000001),sky(horizon_falloff='1'),
                 sky(sun_size_degrees=.099999999),sky(sun_size_degrees=20.000001),sky(sun_size_degrees=True),
                 sky(sun_intensity=-.001),sky(sun_intensity=1000000.001),sky(sun_intensity=None)]
        for key in sky():
            missing=sky();missing.pop(key);invalid.append(missing)
        before=self.path.read_bytes();history=c.rpc('world.history')
        for value in invalid:
            c.txn(1,[transform(1,(99,0,0)),sky_environment(1,value)],error=-32602)
            self.assertEqual(self.path.read_bytes(),before);self.assertEqual(c.rpc('world.history'),history)
        for bad in (float('nan'),float('inf'),-float('inf')):
            reply=c.raw(json.dumps({'jsonrpc':'2.0','id':1,'method':'world.transact','params':{'base_revision':1,'request_id':uuid.uuid4().hex,'ops':[sky_environment(1,sky(sun_intensity=bad))]}}))
            self.assertEqual(reply['error']['code'],-32700);self.assertEqual(self.path.read_bytes(),before)
        c.txn(1,[sky_environment(1,sky(horizon_falloff=.1,sun_size_degrees=20,sun_intensity=1e6))])
        current=c.rpc('world.lighting');self.assertTrue(current['sky']['enabled']);self.assertIsNone(current['sky_sun'])
        self.assertAlmostEqual(current['sky']['horizon_falloff'],.1);self.assertEqual(current['sky']['sun_size_degrees'],20)
        c.close();c=self.open();self.assertEqual(c.rpc('world.lighting'),current)
    def test_sky_sun_reference_transactions_resolve_and_undo(self):
        c=self.open();half=math.sqrt(.5)
        c.txn(0,[create(1),create(2),transform(2,q=(0,half,0,half)),light(2,'directional',intensity=8),sky_environment(1,sky(sun=uid(2))),create(3),light(3,'point')])
        initial=c.rpc('world.lighting');self.assertEqual(initial['sky_sun']['id'],uid(2));self.assertEqual(initial['sky_sun']['intensity'],8)
        for actual,expected in zip(initial['sky_sun']['direction'],[1,0,0]):self.assertAlmostEqual(actual,expected)
        before=self.path.read_bytes();history=c.rpc('world.history')
        for ops in ([{'op':'entity.delete','id':uid(2),'recursive':True}], [remove(2,'Light')], [light(2,'point')],
                    [sky_environment(1,sky(sun=uid(999)))],[sky_environment(1,sky(sun=uid(1)))],
                    [sky_environment(1,sky(sun=uid(3),enabled=False))]):
            c.txn(1,ops,error=-32602);self.assertEqual(self.path.read_bytes(),before);self.assertEqual(c.rpc('world.history'),history)
        c.txn(1,[light(2,'directional',enabled=False)])
        self.assertEqual(c.rpc('world.lighting')['sky']['sun'],uid(2));self.assertIsNone(c.rpc('world.lighting')['sky_sun'])
        c.txn(2,[light(2,'directional'),sky_environment(1,sky(sun=uid(2),enabled=False))]);self.assertIsNone(c.rpc('world.lighting')['sky_sun'])
        # Final-state reference validation allows an atomic clear + removal.
        c.txn(3,[{'op':'entity.delete','id':uid(2),'recursive':True},sky_environment(1,sky())])
        self.assertIsNone(c.rpc('world.lighting')['sky']['sun'])
        c.rpc('world.undo',{'base_revision':4,'request_id':uuid.uuid4().hex})
        self.assertEqual(c.rpc('world.lighting')['sky']['sun'],uid(2));self.assertEqual(c.rpc('entity.get',{'id':uid(2)})['value']['components']['Light']['kind'],'directional')
        c.rpc('world.redo',{'base_revision':5,'request_id':uuid.uuid4().hex});self.assertIsNone(c.rpc('world.lighting')['sky']['sun'])
    def test_runtime_sky_settings_are_frozen_but_sun_pose_is_live(self):
        cap=json.loads(subprocess.check_output([BINARY,'capabilities'],text=True))
        if not cap['result']['features']['simulation']:self.skipTest('Simulation disabled in this build')
        c=self.open();session=uid(950)
        c.txn(0,[create(1),transform(1,(0,3,0)),component(1,'CharacterController',{'radius':.3,'height':1.8,'speed':4,'jump_speed':5,'camera':uid(2)}),
                 create(2,uid(1)),component(2,'Camera',{'vertical_fov':60,'near':.1,'far':100}),
                 create(3),sky_environment(3,sky(sun=uid(4))),create(4,uid(1)),light(4,'directional',intensity=8)])
        c.rpc('runtime.start',{'session_id':session,'revision':1});before=c.rpc('runtime.lighting',{'session_id':session})
        c.txn(1,[sky_environment(3,sky(enabled=False,sun_intensity=0,zenith=[1,0,0])),{'op':'entity.delete','id':uid(4),'recursive':True}])
        self.assertEqual(c.rpc('runtime.lighting',{'session_id':session}),before)
        self.assertFalse(c.rpc('world.lighting')['sky']['enabled'])
        c.rpc('runtime.step',{'session_id':session,'request_id':uuid.uuid4().hex,'expected_tick':0,'ticks':1,'inputs':[{'entity':uid(1),'look':[45,0]}]})
        after=c.rpc('runtime.lighting',{'session_id':session,'tick':1})
        self.assertEqual(after['sky'],before['sky']);self.assertEqual(after['sky_sun']['id'],uid(4));self.assertEqual(after['sky_sun']['intensity'],8)
        self.assertNotEqual(after['sky_sun']['direction'],before['sky_sun']['direction'])
        matrix=c.rpc('runtime.entity',{'session_id':session,'id':uid(4),'tick':1})['world_matrix']
        for actual,expected in zip(after['sky_sun']['direction'],matrix[8:11]):self.assertAlmostEqual(actual,expected)
        c.rpc('runtime.stop',{'session_id':session});c.rpc('runtime.start',{'session_id':uid(951),'revision':2})
        self.assertFalse(c.rpc('runtime.lighting',{'session_id':uid(951)})['sky']['enabled'])


if __name__=='__main__':unittest.main(argv=['lighting_contract'],verbosity=2)
