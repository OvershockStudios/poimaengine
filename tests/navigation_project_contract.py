#!/usr/bin/env python3
"""Real navigation asset closure and metadata-only project/bundle compatibility.

The host actually bakes and binds .pnav content. Runtime executables and native
library headers in this test are non-executable fixtures; no renderer, shipped
NativeAOT image or player lifecycle is qualified here. `game inspect` invokes
native bundle verification, including content closure and requirements.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient
ARGS=None
CALLS=[]
OWNERS=[]


def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def native(path):
    value=str(Path(path).resolve())
    return subprocess.check_output(['wslpath','-w',value],text=True,timeout=10).strip() if ARGS.windows_interop else value

def tree(root):
    return {p.relative_to(root).as_posix():digest(p) for p in sorted(Path(root).rglob('*')) if p.is_file()}

def image(target):
    # Intentionally only the image-format header checked by artifact inspection.
    # It has no runnable entrypoint, export or compiled managed gameplay.
    data=bytearray(256)
    if target=='Linux':
        data[:6]=b'\x7fELF\x02\x01';struct.pack_into('<HH',data,16,3,62)
    else:
        data[:2]=b'MZ';struct.pack_into('<I',data,60,128);data[128:132]=b'PE\0\0'
        struct.pack_into('<H',data,132,0x8664);struct.pack_into('<H',data,150,0x2000);struct.pack_into('<H',data,152,0x20b)
    return bytes(data)


class NavigationProjects(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='navigation-project-',dir=ARGS.output)
        self.root=Path(self.temp.name);self.project=self.root/'Source project'
        self.cli('project','create',native(self.project),'--name','Navigation package contract')
        self.manifest=self.project/'project.json';self.spec=json.loads(self.manifest.read_text(encoding='utf-8'))
        self.world=self.project/self.spec['entry']['world'];self.version=self.cli('version')['version']
        self.target='Windows' if ARGS.windows_interop or os.name=='nt' else 'Linux'
        self.client=None;self.asset=None;self.sequence=0
    def tearDown(self):
        try:self.close()
        finally:self.temp.cleanup()
    def cli(self,*arguments,success=True):
        proc=subprocess.run([str(ARGS.binary.resolve()),*arguments],capture_output=True,text=True,encoding='utf-8',timeout=90)
        try:reply=json.loads(proc.stdout)
        except ValueError:self.fail(f'Non-JSON native CLI reply: {proc.returncode} {proc.stdout} {proc.stderr}')
        CALLS.append(dict(test=self.id(),interface='cli',arguments=list(arguments),exit_code=proc.returncode,reply=reply,stderr=proc.stderr))
        self.assertEqual(proc.returncode==0,success,reply)
        self.assertEqual(reply['status'],'ok' if success else 'error',reply)
        if not success:self.assertTrue(reply['diagnostics'],reply)
        return reply.get('result')
    def rpc(self,method,params=None):
        if self.client is None:self.client=WorldClient.open(str(ARGS.binary.resolve()),native(self.world))
        result=self.client.call(method,params or {},timeout=45)
        CALLS.append(dict(test=self.id(),interface='world',method=method,params=params or {},result=result))
        return result
    def close(self):
        if self.client is not None:
            owner=self.client;self.client=None;owner.close()
            code=owner.transport.returncode;OWNERS.append(dict(test=self.id(),exit_code=code));self.assertEqual(code,0,'Owned world process did not exit cleanly')
    def bake(self):
        source=self.world.read_bytes();revision=json.loads(source)['revision']
        before=self.rpc('world.inspect');history=self.rpc('world.history')
        baked=self.rpc('world.navigation.bake',dict(revision=revision,profile=dict(climb=0)))
        self.assertEqual(self.world.read_bytes(),source,'Bake changed authored document')
        self.assertEqual(self.rpc('world.inspect'),before);self.assertEqual(self.rpc('world.history'),history)
        self.asset=baked['asset'];self.package=Path(str(self.world)+'.assets')/(self.asset+'.pnav')
        self.assertEqual(digest(self.package),self.asset)
        return baked
    def bind(self):
        if self.asset is None:self.bake()
        revision=self.rpc('world.inspect')['revision']
        self.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,ops=[dict(op='navigation.set',asset=self.asset)]))
        self.assertEqual(json.loads(self.world.read_text(encoding='utf-8'))['navigation'],dict(asset=self.asset))
        self.close()
    def inspect(self,success=True):
        self.close();before=tree(self.project)
        result=self.cli('project','inspect',native(self.manifest),success=success)
        self.assertEqual(tree(self.project),before,'Project inspection changed source tree')
        return result
    def runtime(self,navigation=False,license=True,native_gameplay=False):
        self.sequence+=1;root=self.root/('Runtime fixture '+str(self.sequence));(root/'bin').mkdir(parents=True)
        notices=root/'share/poima';notices.mkdir(parents=True)
        for name in ('LICENSE','THIRD_PARTY_NOTICES.md'):(notices/name).write_text('Original non-executable test distribution.\n',encoding='utf-8')
        executable='bin/poima.exe' if self.target=='Windows' else 'bin/poima'
        (root/executable).write_bytes(b'Non-executable metadata-only runtime; do not launch.\n');(root/executable).chmod(0o755)
        descriptor=dict(format='poima.runtime',version=1,engine_version=self.version,target_os=self.target,target_arch='x86_64',executable=executable,
            gameplay_services_version=7,gameplay_call_version=1,gameplay_call_bytes=80,gameplay_services_bytes=224 if navigation else 216,
            gameplay_features=['baseline_v7','navigation_query_v1'] if navigation else ['baseline_v7'],
            features=dict(simulation=True,renderer=True,audio=False,managed=False,editor=False,native_gameplay=native_gameplay,navigation=navigation))
        if navigation and license:
            path=notices/'licenses/RecastNavigation/License.txt';path.parent.mkdir(parents=True)
            path.write_text('Original license-presence test fixture; not upstream license text.\n',encoding='utf-8')
        self.write_runtime(root,descriptor)
        return root,descriptor
    def write_runtime(self,root,descriptor):(root/'runtime.json').write_text(json.dumps(descriptor,indent=2)+'\n',encoding='utf-8')
    def export(self,runtime,success=True):
        self.close();self.sequence+=1;destination=self.root/('Bundle '+str(self.sequence));source=tree(self.project);distribution=tree(runtime)
        result=self.cli('project','build',native(self.manifest),'--runtime',native(runtime),'--output',native(destination),success=success)
        self.assertEqual(tree(self.project),source,'Export altered source project');self.assertEqual(tree(runtime),distribution,'Export altered runtime fixture')
        if not success:self.assertFalse(destination.exists(),'Rejected preflight published partial bundle')
        else:
            self.assertIn(result['publication'],('atomic_directory','manifest_last'));self.assertFalse((destination/'.poima-incomplete').exists())
            before=tree(destination);self.cli('game','inspect',native(destination/'game.json'));self.assertEqual(tree(destination),before,'Bundle verification altered payload')
        return destination
    def assert_rejected_bundle(self,bundle):
        before=tree(bundle);self.cli('game','inspect',native(bundle/'game.json'),success=False);self.assertEqual(tree(bundle),before,'Failed verification repaired/altered bundle')
    def refresh_inventory(self,bundle,path,remove=False):
        manifest=bundle/'game.json';doc=json.loads(manifest.read_text(encoding='utf-8'))
        if remove:doc['files']=[row for row in doc['files'] if row['path']!=path]
        else:
            row=next(row for row in doc['files'] if row['path']==path);row.update(size=(bundle/path).stat().st_size,sha256=digest(bundle/path))
        manifest.write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8')
    def artifact(self,features=None):
        features=features or ['baseline_v7','navigation_query_v1'];folder=self.project/'gameplay';folder.mkdir(exist_ok=True)
        library='Poima.NativeGame.dll' if self.target=='Windows' else 'Poima.NativeGame.so';(folder/library).write_bytes(image(self.target))
        notice=folder/'NOTICE.txt';notice.write_text('Original non-executable native image-header fixture.\n',encoding='utf-8')
        identity='poima.test.navigation-metadata';schema=dict(identity=identity,bytes=4,fields=[dict(name='Counter',kind='int32',offset=0,bytes=4)])
        descriptor=dict(format='poima.native-gameplay',version=2,engine_version=self.version,target_os=self.target,target_arch='x86_64',
            call_version=1,call_bytes=80,services_version=7,minimum_services_bytes=224,required_features=features,
            entry='poima_gameplay_entry',library=library,identity=identity,type='Poima.Test.NavigationMetadata',schema=schema,
            files=[dict(path=p.name,size=p.stat().st_size,sha256=digest(p),role=role) for p,role in [(folder/library,'library'),(notice,'notice')]])
        (folder/'native-gameplay.json').write_text(json.dumps(descriptor,indent=2)+'\n',encoding='utf-8')
        self.spec.update(version=2,gameplay=dict(descriptor='gameplay/native-gameplay.json',values=dict(Counter=7)))
        self.manifest.write_text(json.dumps(self.spec,indent=2)+'\n',encoding='utf-8')
        return descriptor
    def test_unbound_world_omits_baked_unused_navigation_from_closure(self):
        baseline=self.inspect();self.assertNotIn('needs_navigation',baseline)
        off,_=self.runtime();before_bundle=self.export(off);before=json.loads((before_bundle/'game.json').read_text(encoding='utf-8'))
        self.bake();self.close();after=self.inspect()
        self.assertEqual(after,baseline,'Unbound baked artifact changed project content summary')
        second=self.export(off);doc=json.loads((second/'game.json').read_text(encoding='utf-8'))
        self.assertEqual(doc,before,'Unbound bake changed bundle identity/inventory')
        self.assertEqual((second/'content/world.json').read_bytes(),(before_bundle/'content/world.json').read_bytes())
        self.assertFalse(any(row['path'].endswith('.pnav') for row in doc['files']))
        self.assertTrue(self.package.is_file(),'Unreferenced bake was not actually present on disk')
    def test_bound_project_exact_navigation_asset_and_license_closure(self):
        baseline=self.inspect();self.bind();view=self.inspect();self.assertTrue(view['needs_navigation'])
        expected=sorted(baseline['assets']+[dict(filename=self.asset+'.pnav',sha256=self.asset,bytes=self.package.stat().st_size)],key=lambda row:row['filename'])
        self.assertEqual(view['assets'],expected)
        runtime,descriptor=self.runtime(navigation=True);bundle=self.export(runtime);doc=json.loads((bundle/'game.json').read_text(encoding='utf-8'))
        navpath='content/world.json.assets/'+self.asset+'.pnav';navrows=[row for row in doc['files'] if row['path'].endswith('.pnav')]
        self.assertEqual(navrows,[dict(path=navpath,size=self.package.stat().st_size,sha256=self.asset,role='asset')])
        self.assertEqual((bundle/navpath).read_bytes(),self.package.read_bytes())
        self.assertEqual(json.loads((bundle/'content/world.json').read_text(encoding='utf-8'))['navigation'],dict(asset=self.asset))
        self.assertEqual(next(row for row in doc['files'] if row['path']=='runtime/share/poima/licenses/RecastNavigation/License.txt')['role'],'runtime')
        self.assertEqual(descriptor['gameplay_features'],['baseline_v7','navigation_query_v1'])
        self.assertNotIn('gameplay',doc,'No module should be introduced by a navigation binding')
    def test_missing_disabled_and_nonboolean_navigation_capabilities_reject(self):
        self.bind()
        for value in (None,False,'true',1,[],{}):
            with self.subTest(navigation=value):
                runtime,descriptor=self.runtime(navigation=True)
                if value is None:descriptor['features'].pop('navigation')
                else:descriptor['features']['navigation']=value
                self.write_runtime(runtime,descriptor);self.export(runtime,False)
                self.assertIn('Boolean' if value is not None and type(value) is not bool else 'navigation-enabled',json.dumps(CALLS[-1]['reply']))
        runtime,_=self.runtime(navigation=True,license=False);self.export(runtime,False)
        self.assertIn('RecastNavigation',json.dumps(CALLS[-1]['reply']))
    def test_missing_tampered_and_coherently_rehashed_navigation_package_reject(self):
        self.bind();runtime,_=self.runtime(navigation=True);bundle=self.export(runtime)
        path='content/world.json.assets/'+self.asset+'.pnav';package=bundle/path;raw=package.read_bytes()
        package.write_bytes(raw+b'tampered');self.assert_rejected_bundle(bundle);package.write_bytes(raw)
        package.unlink();self.assert_rejected_bundle(bundle);package.write_bytes(raw)
        coherent=self.root/'Coherently altered bundle';shutil.copytree(bundle,coherent)
        (coherent/path).write_bytes(raw+b'tampered');self.refresh_inventory(coherent,path);self.assert_rejected_bundle(coherent)
        dropped=self.root/'Coherently dropped bundle';shutil.copytree(bundle,dropped)
        (dropped/path).unlink();self.refresh_inventory(dropped,path,remove=True);self.assert_rejected_bundle(dropped)
        self.cli('game','inspect',native(bundle/'game.json'))
    def test_coherent_descriptor_and_license_removal_recheck_content_requirements(self):
        self.bind();runtime,descriptor=self.runtime(navigation=True);bundle=self.export(runtime)
        descriptor['features']['navigation']=False;self.write_runtime(bundle/'runtime',descriptor);self.refresh_inventory(bundle,'runtime/runtime.json')
        self.assert_rejected_bundle(bundle);self.assertIn('navigation-enabled',json.dumps(CALLS[-1]['reply']))
        descriptor['features']['navigation']=True;self.write_runtime(bundle/'runtime',descriptor);self.refresh_inventory(bundle,'runtime/runtime.json')
        license='runtime/share/poima/licenses/RecastNavigation/License.txt';(bundle/license).unlink();self.refresh_inventory(bundle,license,remove=True)
        self.assert_rejected_bundle(bundle);self.assertIn('RecastNavigation',json.dumps(CALLS[-1]['reply']))
    def test_navigation_only_224_artifact_negotiates_named_features_independently(self):
        self.bind();self.artifact();view=self.inspect();requirements=view['gameplay']['requirements']
        self.assertEqual(requirements,dict(call_version=1,call_bytes=80,services_version=7,minimum_services_bytes=224,required_features=['baseline_v7','navigation_query_v1']))
        for extent,names in ((216,['baseline_v7','navigation_query_v1']),(224,['baseline_v7']),(224,['navigation_query_v1'])):
            with self.subTest(extent=extent,features=names):
                runtime,descriptor=self.runtime(navigation=True,native_gameplay=True)
                descriptor['gameplay_services_bytes']=extent;descriptor['gameplay_features']=names;self.write_runtime(runtime,descriptor);self.export(runtime,False)
        for names in (['baseline_v7','navigation_query_v1'],['baseline_v7','navigation_query_v1','future_optional_v1']):
            runtime,descriptor=self.runtime(navigation=True,native_gameplay=True);descriptor['gameplay_features']=names;self.write_runtime(runtime,descriptor)
            bundle=self.export(runtime);doc=json.loads((bundle/'game.json').read_text(encoding='utf-8'));self.assertEqual(doc['version'],2)
            self.assertEqual(doc['gameplay']['values'],dict(Counter=7))
        # A larger allocation and nav name do not grant unrequested animation or
        # character services to a module that explicitly requires those names.
        self.artifact(['animation_inertial_v1','animation_layers_v1','baseline_v7','character_input_v1','navigation_query_v1'])
        runtime,_=self.runtime(navigation=True,native_gameplay=True);self.export(runtime,False)


def main():
    global ARGS
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path);parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--output',type=Path,help='New retained evidence directory (default: temporary build directory)')
    ARGS=parser.parse_args()
    if sys.flags.optimize:parser.error('Assertions must remain enabled')
    temporary=None
    if ARGS.output is None:
        parent=ROOT/'build/navigation-project-contract';parent.mkdir(parents=True,exist_ok=True);temporary=tempfile.TemporaryDirectory(prefix='run-',dir=parent);ARGS.output=Path(temporary.name)
    else:
        if ARGS.output.exists():parser.error('--output must be new')
        ARGS.output.mkdir(parents=True)
    try:
        result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(NavigationProjects))
        report=dict(passed=result.wasSuccessful(),tests=result.testsRun,failures=len(result.failures),errors=len(result.errors),
            binary_sha256=digest(ARGS.binary),test_sha256=digest(__file__),cli_calls=sum(row['interface']=='cli' for row in CALLS),
            world_calls=sum(row['interface']=='world' for row in CALLS),world_owners=OWNERS,
            scope='Actual native .pnav bake/bind/content closure; metadata-only non-executable runtime and image-header fixtures. No player, renderer or published NativeAOT lifecycle.',calls=CALLS)
        (ARGS.output/'evidence.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        return 0 if result.wasSuccessful() else 1
    finally:
        if temporary is not None:temporary.cleanup()

if __name__=='__main__':raise SystemExit(main())
