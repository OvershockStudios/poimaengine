#!/usr/bin/env python3
"""Native provenance export/verification with metadata-only runtime fixtures.

Imports real PNG assets and provenance records through the host. The runtime
payload is deliberately non-executable; this does not qualify player deployment.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
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
import zlib

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

def tree(root):return {p.relative_to(root).as_posix():digest(p) for p in sorted(Path(root).rglob('*')) if p.is_file()}
def png(color):
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'+bytes(color)))+chunk(b'IEND',b'')

class ProvenanceProjects(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='provenance-project-',dir=ARGS.output)
        self.root=Path(self.temp.name);self.project=self.root/'Source project';self.sequence=0;self.client=None
        self.cli('project','create',native(self.project),'--name','Provenance package contract')
        self.manifest=self.project/'project.json';self.spec=json.loads(self.manifest.read_text(encoding='utf-8'))
        self.world=self.project/self.spec['entry']['world'];self.version=self.cli('version')['version']
        self.target='Windows' if ARGS.windows_interop or os.name=='nt' else 'Linux'
    def tearDown(self):
        try:self.close()
        finally:self.temp.cleanup()
    def cli(self,*args,success=True):
        proc=subprocess.run([str(ARGS.binary.resolve()),*args],capture_output=True,text=True,encoding='utf-8',timeout=90)
        try:reply=json.loads(proc.stdout)
        except ValueError:self.fail(f'Non-JSON CLI reply: {proc.returncode} {proc.stdout} {proc.stderr}')
        CALLS.append(dict(test=self.id(),interface='cli',arguments=list(args),exit_code=proc.returncode,reply=reply,stderr=proc.stderr))
        self.assertEqual(proc.returncode==0,success,reply);self.assertEqual(reply['status'],'ok' if success else 'error',reply)
        if not success:self.assertTrue(reply['diagnostics'],reply)
        return reply.get('result')
    def rpc(self,method,params=None):
        if self.client is None:self.client=WorldClient.open(str(ARGS.binary.resolve()),native(self.world))
        result=self.client.call(method,params or {},timeout=45)
        CALLS.append(dict(test=self.id(),interface='world',method=method,params=params or {},result=result));return result
    def close(self):
        if self.client is not None:
            owner=self.client;self.client=None;owner.close();code=owner.transport.returncode
            OWNERS.append(dict(test=self.id(),exit_code=code));self.assertEqual(code,0,'Owned host did not exit cleanly')
    def tx(self,ops):
        revision=self.rpc('world.inspect')['revision']
        return self.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,ops=ops))
    def asset(self,color):
        source=self.root/(uuid.uuid4().hex+'.png');source.write_bytes(png(color))
        asset=self.rpc('asset.image.import',dict(source=native(source),color_space='srgb'))['asset'];source.unlink();return asset
    def record(self,asset,title,creator,notice):
        metadata=dict(format='poima.asset-provenance',version=1,asset=asset,kind='image',title=title,creator=creator,
            license=dict(identifier='CC0-1.0',notice=notice))
        result=self.rpc('asset.provenance.create',dict(record=metadata));record=result['record']
        inspected=self.rpc('asset.provenance.inspect',dict(record=record));self.assertEqual(inspected['metadata'],metadata)
        package=Path(str(self.world)+'.assets')/(record+'.pprov');self.assertEqual(digest(package),record)
        return record,package.read_bytes()
    def fixtures(self):
        used=self.asset([80,120,160,255]);unused=self.asset([10,20,30,255])
        a,ab=self.record(used,'First texture','First creator','First notice exact.')
        b,bb=self.record(used,'Second origin','Second creator','Second notice exact.')
        c,cb=self.record(unused,'Unused texture','Unused creator','Unused notice exact.')
        maps=dict(base_color=dict(asset=used),emissive=None,metallic_roughness=None,normal=None,occlusion=None)
        self.tx([dict(op='component.set',id='0'*31+'1',type='PbrTextures',value=maps),
            dict(op='asset.provenance.set',asset=used,records=[b,a]),dict(op='asset.provenance.set',asset=unused,records=[c])])
        self.close();return used,unused,{a:ab,b:bb,c:cb}
    def runtime(self,provenance=True):
        folder=self.root/'Runtime fixture';(folder/'bin').mkdir(parents=True);notices=folder/'share/poima';notices.mkdir(parents=True)
        for name in ('LICENSE','THIRD_PARTY_NOTICES.md'):(notices/name).write_text('Non-executable test distribution.\n',encoding='utf-8')
        executable='bin/poima.exe' if self.target=='Windows' else 'bin/poima'
        (folder/executable).write_bytes(b'Non-executable metadata-only fixture; do not launch.\n');(folder/executable).chmod(0o755)
        descriptor=dict(format='poima.runtime',version=1,engine_version=self.version,target_os=self.target,target_arch='x86_64',executable=executable,
            gameplay_services_version=7,gameplay_call_version=1,gameplay_call_bytes=80,gameplay_services_bytes=176,gameplay_features=['baseline_v7'],
            features=dict(simulation=True,renderer=True,audio=False,managed=False,editor=False,native_gameplay=False))
        if provenance:descriptor['features']['asset_provenance']=True
        (folder/'runtime.json').write_text(json.dumps(descriptor,indent=2)+'\n',encoding='utf-8');return folder
    def export(self,runtime):
        self.close();self.sequence+=1;bundle=self.root/('Bundle '+str(self.sequence));before=tree(self.project)
        self.cli('project','build',native(self.manifest),'--runtime',native(runtime),'--output',native(bundle))
        self.assertEqual(tree(self.project),before);self.verify(bundle);return bundle
    def verify(self,bundle,success=True):
        before=tree(bundle);self.cli('game','inspect',native(bundle/'game.json'),success=success);self.assertEqual(tree(bundle),before)
    def refresh(self,bundle,path,remove=False):
        doc=json.loads((bundle/'game.json').read_text(encoding='utf-8'))
        if remove:doc['files']=[row for row in doc['files'] if row['path']!=path]
        else:
            row=next(row for row in doc['files'] if row['path']==path);row.update(size=(bundle/path).stat().st_size,sha256=digest(bundle/path))
        (bundle/'game.json').write_text(json.dumps(doc,indent=2)+'\n',encoding='utf-8')
    def test_relocated_closure_all_origins_unused_mapping_and_reopen(self):
        used,unused,records=self.fixtures();runtime=self.runtime();summary=self.cli('project','inspect',native(self.manifest))
        self.assertEqual({row['filename'] for row in summary['provenance_records']},{key+'.pprov' for key in records})
        self.assertIn('asset_credits',summary);bundle=self.export(runtime);doc=json.loads((bundle/'game.json').read_text(encoding='utf-8'))
        for record,data in records.items():
            path='content/world.json.assets/'+record+'.pprov';self.assertEqual((bundle/path).read_bytes(),data)
            self.assertEqual(next(row for row in doc['files'] if row['path']==path),dict(path=path,size=len(data),sha256=record,role='asset_provenance'))
        credits=(bundle/'content/ASSET_CREDITS.txt').read_bytes()
        for text in ('First creator','Second creator','Unused creator','First notice exact.','Second notice exact.','Unused notice exact.'):
            self.assertIn(text.encode(),credits)
        self.assertTrue(any(row['path'].endswith(used+'.pimage') for row in doc['files']))
        self.assertFalse(any(row['path'].endswith(unused+'.pimage') for row in doc['files']),'Unused mapped image was cooked into closure')
        relocated=self.root/'Relocated bundle';shutil.move(bundle,relocated);shutil.rmtree(self.project);self.verify(relocated)
    def test_credit_tamper_updated_inventory_still_rejected(self):
        self.fixtures();bundle=self.export(self.runtime());path='content/ASSET_CREDITS.txt'
        (bundle/path).write_bytes(b'Altered notice, with refreshed inventory.\n');self.refresh(bundle,path);self.verify(bundle,False)
    def test_missing_and_corrupt_record_with_refreshed_inventory_rejected(self):
        _,_,records=self.fixtures();runtime=self.runtime();record=next(iter(records));path='content/world.json.assets/'+record+'.pprov'
        with self.subTest(case='corrupt'):
            bundle=self.export(runtime);(bundle/path).write_bytes(b'{}\n');self.refresh(bundle,path);self.verify(bundle,False)
        with self.subTest(case='missing'):
            bundle=self.export(runtime);(bundle/path).unlink();self.refresh(bundle,path,remove=True);self.verify(bundle,False)
    def test_unmapped_imported_records_preserve_legacy_bundle_shape(self):
        runtime=self.runtime(provenance=False);first=self.export(runtime);original=(first/'game.json').read_bytes();world=(first/'content/world.json').read_bytes()
        asset=self.asset([100,110,120,255]);self.record(asset,'Unmapped','Unmapped creator','Unmapped notice.');self.close()
        summary=self.cli('project','inspect',native(self.manifest));self.assertNotIn('provenance_records',summary);self.assertNotIn('asset_credits',summary)
        second=self.export(runtime);self.assertEqual((second/'game.json').read_bytes(),original);self.assertEqual((second/'content/world.json').read_bytes(),world)
        self.assertFalse(any(row['role'] in ('asset_provenance','asset_credits') for row in json.loads(original)['files']))
    def test_runtime_content_capability_required_and_freshly_verified(self):
        self.fixtures();runtime=self.runtime();descriptor_path=runtime/'runtime.json'
        descriptor=json.loads(descriptor_path.read_text(encoding='utf-8'))
        for value in (None,False,'true',1,[],{}):
            with self.subTest(feature=value):
                current=json.loads(json.dumps(descriptor))
                if value is None:current['features'].pop('asset_provenance')
                else:current['features']['asset_provenance']=value
                descriptor_path.write_text(json.dumps(current,indent=2)+'\n',encoding='utf-8')
                destination=self.root/('Rejected capability '+uuid.uuid4().hex);before=tree(self.project)
                self.cli('project','build',native(self.manifest),'--runtime',native(runtime),'--output',native(destination),success=False)
                self.assertEqual(tree(self.project),before);self.assertFalse(destination.exists())
        descriptor_path.write_text(json.dumps(descriptor,indent=2)+'\n',encoding='utf-8');bundle=self.export(runtime)
        path='runtime/runtime.json';current=json.loads((bundle/path).read_text(encoding='utf-8'))
        current['features']['asset_provenance']=False
        (bundle/path).write_text(json.dumps(current,indent=2)+'\n',encoding='utf-8');self.refresh(bundle,path);self.verify(bundle,False)
    def test_source_record_corruption_rejects_export_without_publication(self):
        _,_,records=self.fixtures();runtime=self.runtime();record=next(iter(records))
        (Path(str(self.world)+'.assets')/(record+'.pprov')).write_bytes(b'{}\n');destination=self.root/'Rejected bundle'
        before=tree(self.project);self.cli('project','build',native(self.manifest),'--runtime',native(runtime),'--output',native(destination),success=False)
        self.assertEqual(tree(self.project),before);self.assertFalse(destination.exists())

def main():
    global ARGS
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--output',type=Path)
    ARGS=parser.parse_args()
    if sys.flags.optimize:parser.error('Assertions must remain enabled')
    if ARGS.output is None:ARGS.output=ROOT/'build/asset-provenance-project'/uuid.uuid4().hex
    if ARGS.output.exists():parser.error('--output must be new')
    ARGS.output.mkdir(parents=True)
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ProvenanceProjects))
    report=dict(passed=result.wasSuccessful(),tests=result.testsRun,failures=len(result.failures),errors=len(result.errors),binary_sha256=digest(ARGS.binary),test_sha256=digest(__file__),
        cli_calls=sum(row['interface']=='cli' for row in CALLS),world_calls=sum(row['interface']=='world' for row in CALLS),world_owners=OWNERS,
        scope='Actual image/provenance imports and native bundle export/verification; non-executable metadata-only runtime. No player deployment or legal policy qualification.',calls=CALLS)
    (ARGS.output/'evidence.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');return 0 if result.wasSuccessful() else 1
if __name__=='__main__':raise SystemExit(main())
