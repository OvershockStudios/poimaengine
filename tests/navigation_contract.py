#!/usr/bin/env python3
"""Static navigation through the real native authored-world protocol."""
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

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient,RpcError
ARGS=None


def uid(value):return f'{value:032x}'
def native(path):
    value=str(Path(path).resolve())
    return subprocess.check_output(['wslpath','-w',value],text=True,timeout=10).strip() if ARGS.windows_interop else value

def fixture():
    ops=[]
    for identity,name,position,half in [(1,'Floor',[0,-.5,0],[10,.5,10]),(2,'Wall',[0,1.5,0],[.5,1.5,5])]:
        ops.extend([dict(op='entity.create',id=uid(identity),name=name,parent=None),
            dict(op='component.set',id=uid(identity),type='Transform',value=dict(position=position,rotation=[0,0,0,1],scale=[1,1,1])),
            dict(op='component.set',id=uid(identity),type='BoxCollider',value=dict(half_extents=half,motion='static',mass=1,friction=.5,restitution=0))])
    return ops


class NavigationContract(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='navigation-',dir=ARGS.output)
        self.world=Path(self.temp.name)/'静态 world.json';self.owners=[]
        self.client=self.open()
        self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=fixture()))
    def tearDown(self):
        failures=[]
        for owner in reversed(self.owners):
            try:owner.close()
            except BaseException as error:failures.append(str(error))
            if owner.transport.returncode!=0:failures.append('Native owner did not exit cleanly')
        self.temp.cleanup()
        self.assertEqual(failures,[])
    def open(self):
        owner=WorldClient.open(str(ARGS.binary.resolve()),native(self.world));self.owners.append(owner);return owner
    def call(self,method,params=None,error=None,owner=None):
        try:result=(owner or self.client).call(method,params or {},timeout=45)
        except RpcError as failure:
            if error is None:raise
            self.assertEqual(failure.code,error);return failure
        self.assertIsNone(error,'Expected rejection: '+str(result));return result
    def view(self):
        return dict(inspect=self.call('world.inspect'),history=self.call('world.history'),bytes=self.world.read_bytes())
    def bake(self):return self.call('world.navigation.bake',dict(revision=1))
    def path(self,asset,revision=1,owner=None,**extra):
        return self.call('world.navigation.path',dict(revision=revision,asset=asset,start=[-4,.2,0],end=[4,.2,0],extents=[.5,.5,.5],**extra),owner=owner)
    def enabled(self):
        if not ARGS.navigation:self.skipTest('Navigation dependency disabled')
    def test_discovery_and_unavailable(self):
        description=self.call('world.describe')
        self.assertEqual(description['navigation']['available'],bool(ARGS.navigation))
        self.assertEqual(description['navigation']['limits']['mesh_polygons'],32767)
        for method in ('bake','inspect','path'):
            name='world.navigation.'+method
            self.assertIn(name,description['methods'])
            self.assertEqual(self.call('world.describe',dict(view='method',name=name))['methods'][name],description['methods'][name])
        if not ARGS.navigation:
            before=self.view()
            self.call('world.navigation.bake',dict(revision=1),error=-32003)
            self.call('world.navigation.inspect',dict(revision=1,asset='a'*64),error=-32003)
            self.call('world.navigation.path',dict(revision=1,asset='a'*64,start=[0,0,0],end=[1,0,0]),error=-32003)
            self.assertEqual(self.view(),before)
            self.assertFalse(Path(str(self.world)+'.assets').exists())
    def test_bake_query_reopen_and_metadata_only_edit(self):
        self.enabled();before=self.view();baked=self.bake();asset=baked['asset']
        self.assertEqual(baked['scope'],'authored');self.assertEqual(self.view(),before)
        package=Path(str(self.world)+'.assets')/(asset+'.pnav');raw=package.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),asset)
        again=self.bake();self.assertEqual(again['asset'],asset);self.assertTrue(again['reused']);self.assertEqual(package.read_bytes(),raw)
        path=self.path(asset)['path'];self.assertEqual(path['status'],'complete');self.assertTrue(path['complete'])
        self.assertGreaterEqual(len(path['corners']),4);self.assertTrue(any(abs(point[2])>5.15 for point in path['corners']))
        self.assertAlmostEqual(path['requested_start'][1],.2,places=6);self.assertGreaterEqual(path['start_projection_distance'],0)
        self.client.close();new=self.open();self.client=new
        self.assertEqual(self.path(asset)['path'],path)
        self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=1,ops=[dict(op='entity.rename',id=uid(2),name='Renamed wall')]))
        self.assertEqual(self.path(asset,revision=2)['path'],path)
        self.assertEqual(self.call('world.navigation.inspect',dict(revision=2,asset=asset))['source_fingerprint'],baked['source_fingerprint'])
    def test_stale_topology_and_failed_queries_are_observational(self):
        self.enabled();asset=self.bake()['asset'];before=self.view()
        self.call('world.navigation.inspect',dict(revision=0,asset=asset),error=-32009)
        for extra in (dict(max_nodes=31),dict(max_polygons=True),dict(max_corners=1),dict(extents=[.5,-1,.5]),dict(start=[0,0]),dict(surprise=True)):
            params=dict(revision=1,asset=asset,start=[-4,.2,0],end=[4,.2,0]);params.update(extra)
            self.call('world.navigation.path',params,error=-32602)
        self.assertEqual(self.view(),before)
        self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=1,ops=[dict(op='component.set',id=uid(2),type='Transform',value=dict(position=[0,1.5,2],rotation=[0,0,0,1],scale=[1,1,1]))]))
        before=self.view();self.call('world.navigation.inspect',dict(revision=2,asset=asset),error=-32009)
        self.call('world.navigation.path',dict(revision=2,asset=asset,start=[-4,0,0],end=[4,0,0]),error=-32009)
        self.assertEqual(self.view(),before)
        fresh=self.call('world.navigation.bake',dict(revision=2));self.assertNotEqual(fresh['asset'],asset)
    def test_budget_partial_and_missing_endpoint(self):
        self.enabled();asset=self.bake()['asset']
        route=self.path(asset,max_polygons=1)['path'];self.assertFalse(route['complete']);self.assertIn(route['status'],('buffer_limit','partial','out_of_nodes'))
        route=self.path(asset,max_corners=2)['path'];self.assertFalse(route['complete']);self.assertEqual(route['status'],'buffer_limit')
        missing=self.call('world.navigation.path',dict(revision=1,asset=asset,start=[-4,.2,0],end=[100,.2,100],extents=[.1,.1,.1]))['path']
        self.assertEqual(missing['status'],'unreachable');self.assertIsNone(missing['projected_end']);self.assertEqual(missing['corners'],[])
    def test_immutable_owner_cache_and_uncached_corruption(self):
        self.enabled();asset=self.bake()['asset'];expected=self.path(asset)['path'];before=self.view()
        package=Path(str(self.world)+'.assets')/(asset+'.pnav');raw=package.read_bytes();package.write_bytes(b'corrupt')
        # A loaded immutable owner keeps the checked original mesh. External
        # writes do not turn it into a live mutable file view.
        self.assertEqual(self.path(asset)['path'],expected);self.assertEqual(self.view(),before)
        self.call('world.navigation.bake',dict(revision=1),error=-32050)
        self.client.close();self.client=self.open()
        self.call('world.navigation.inspect',dict(revision=1,asset=asset),error=-32050)
        package.write_bytes(raw);self.assertEqual(self.path(asset)['path'],expected)
    def test_invalid_profile_geometry_missing_package_no_publication(self):
        self.enabled();before=self.view()
        for profile in (dict(radius=True),dict(height=.2),dict(cell_size=.001),dict(climb=3),dict(unknown=1)):
            self.call('world.navigation.bake',dict(revision=1,profile=profile),error=-32602)
        self.assertEqual(self.view(),before)
        self.assertFalse(Path(str(self.world)+'.assets').exists())
        self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=1,ops=[dict(op='entity.create',id=uid(3),name='Missing collision package'),dict(op='component.set',id=uid(3),type='MeshCollider',value=dict(asset='a'*64,primitive=0,friction=.5,restitution=0))]))
        before=self.view();self.call('world.navigation.bake',dict(revision=2),error=-32050);self.assertEqual(self.view(),before)


def main():
    global ARGS
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path)
    parser.add_argument('--navigation',type=int,choices=(0,1),required=True);parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--output',type=Path,default=ROOT/'build/navigation-contract')
    ARGS,remaining=parser.parse_known_args();ARGS.output.mkdir(parents=True,exist_ok=True)
    if sys.flags.optimize:parser.error('Assertions must be enabled')
    return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(NavigationContract)).wasSuccessful() else 1
if __name__=='__main__':raise SystemExit(main())
