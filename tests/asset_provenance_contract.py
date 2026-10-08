#!/usr/bin/env python3
"""Immutable local provenance, authored mappings and frozen saved-source closure."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from collections import Counter
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid
from texture_fixture import png,quad
from gltf_fixture import glb

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient,RpcError
ARGS=None
CALLS=[]
OWNERS=[]
NOTES=[]
def sha(data):return hashlib.sha256(data).hexdigest()
def native(path):
    value=str(Path(path).resolve())
    return subprocess.check_output(['wslpath','-w',value],text=True,timeout=10).strip() if ARGS.windows_interop else value
def tree(root):return {p.relative_to(root).as_posix():sha(p.read_bytes()) for p in sorted(root.rglob('*')) if p.is_file()}
def pose(x=0):return dict(position=[x,0,0],rotation=[0,0,0,1],scale=[1,1,1])
def mapping(asset,records):return dict(op='asset.provenance.set',asset=asset,records=records)
def normalized(value):
    if isinstance(value,dict):return {k:normalized(v) for k,v in value.items() if k!='session_id'}
    if isinstance(value,list):return [normalized(v) for v in value]
    return value

class ProvenanceContract(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='provenance-',dir=ARGS.output)
        self.directory=Path(self.temp.name);self.world=self.directory/'world.json';self.assets=Path(str(self.world)+'.assets')
        self.owners=[];self.client=self.open();self.revision=0;self.session=None;self.tick=0
        self.entity='1'*32
        self.edit([dict(op='entity.create',id=self.entity,name='Provenance witness'),
                   dict(op='component.set',id=self.entity,type='Transform',value=pose())])
        self.image=self.directory/'original.png';self.image.write_bytes(png(1,1,[50,100,150,255]))
        self.asset=self.call('asset.image.import',dict(source=native(self.image),color_space='srgb'))['asset']
    def tearDown(self):
        failures=[]
        for owner in reversed(self.owners):
            try:owner.close()
            except BaseException as error:failures.append(str(error))
            code=owner.transport.returncode;OWNERS.append(dict(test=self.id(),exit_code=code))
            if code!=0:failures.append('Native owner exit was not zero')
        self.temp.cleanup();self.assertEqual(failures,[])
    def open(self):
        owner=WorldClient.open(str(ARGS.binary.resolve()),native(self.world));self.owners.append(owner);return owner
    def reopen(self):self.client.close();self.client=self.open()
    def call(self,method,params=None,error=None):
        params=params or {};entry=dict(test=self.id(),method=method,params=params);CALLS.append(entry)
        try:result=self.client.call(method,params,timeout=45)
        except RpcError as failure:
            entry['error']=dict(code=failure.code,message=str(failure));self.assertIsNotNone(error,str(failure));self.assertEqual(failure.code,error);return failure
        entry['result']=result;self.assertIsNone(error,'Expected RPC rejection');return result
    def edit(self,ops,**extra):
        result=self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=self.revision,ops=ops,**extra))
        if not extra.get('preview',False):self.revision=result['revision']
        return result
    def metadata(self,**extra):
        row=dict(format='poima.asset-provenance',version=1,asset=self.asset,kind='image',title='Original fixture',creator='Poima tests',
                 license=dict(identifier='LicenseRef-Test',notice='Original test fixture; retained attribution.'))
        row.update(extra);return row
    def create(self,row=None):return self.call('asset.provenance.create',dict(record=self.metadata() if row is None else row))
    def query(self,**extra):return self.call('world.asset.provenance',dict(revision=self.revision,**extra))
    def state(self):return dict(world=self.world.read_bytes(),inspect=self.call('world.inspect'),history=self.call('world.history'))
    def record_path(self,identity):return self.assets/(identity+'.pprov')

    def test_record_identity_origins_and_legacy_unchanged(self):
        before=self.state();self.assertNotIn('asset_provenance',json.loads(before['world']))
        self.assertEqual(self.call('world.dependencies')['assets'],[])
        same=self.directory/'renamed.png';same.write_bytes(self.image.read_bytes())
        self.assertEqual(self.call('asset.image.import',dict(source=native(same),color_space='srgb'))['asset'],self.asset)
        rows=[self.metadata(source='https://example.org/first.png'),self.metadata(source='https://example.org/second.png'),
              self.metadata(source='https://example.org/first.png',license=dict(identifier='LicenseRef-Test',notice='Changed attribution.'))]
        records=[]
        for row in rows:
            created=self.create(row);identity=created['record'];records.append(identity)
            self.assertTrue(created['created']);self.assertEqual((created['asset'],created['kind']),(self.asset,'image'))
            raw=self.record_path(identity).read_bytes();self.assertEqual(sha(raw),identity);self.assertEqual(created['bytes'],len(raw))
            inspected=self.call('asset.provenance.inspect',dict(record=identity))
            self.assertEqual(inspected,dict(record=identity,bytes=len(raw),metadata=row))
            self.assertEqual(self.create(row),dict(created,created=False))
        self.assertEqual(len(set(records)),3);self.assertEqual(self.state(),before)
        self.assertEqual(self.query()['assets'],[]);self.assertEqual(self.call('world.dependencies')['assets'],[])
        self.assertEqual(sha((self.assets/(self.asset+'.pimage')).read_bytes()),self.asset)
        path=self.record_path(records[0]);raw=path.read_bytes();path.write_bytes(b'corrupt existing immutable record')
        self.call('asset.provenance.create',dict(record=rows[0]),error=-32050)
        self.assertEqual(path.read_bytes(),b'corrupt existing immutable record','Create repaired/overwrote existing corrupt record.')
        path.write_bytes(raw)

    def test_mapping_preview_stale_retry_atomic_history_reopen(self):
        a=self.create()['record'];b=self.create(self.metadata(title='Second record'))['record'];records=sorted([a,b]);before=self.state()
        preview=self.edit([mapping(self.asset,list(reversed(records)))],preview=True)
        self.assertEqual(preview['changed_world_fields'],['asset_provenance']);self.assertEqual(self.state(),before)
        params=dict(request_id=uuid.uuid4().hex,base_revision=self.revision,ops=[mapping(self.asset,list(reversed(records)))])
        accepted=self.call('world.transact',params);self.revision=accepted['revision']
        self.assertEqual(accepted['changed_world_fields'],['asset_provenance'])
        self.assertEqual(json.loads(self.world.read_bytes())['asset_provenance'],{self.asset:records})
        unchanged=self.state();self.assertEqual(self.call('world.transact',params),dict(accepted,replayed=True));self.assertEqual(self.state(),unchanged)
        self.call('world.transact',dict(params,request_id=uuid.uuid4().hex),error=-32009)
        self.call('world.asset.provenance',dict(revision=self.revision-1),error=-32009)
        for bad in ([a,a],['f'*64],True):
            code=-32050 if bad==['f'*64] else -32602
            self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=self.revision,ops=[
                dict(op='component.set',id=self.entity,type='Transform',value=pose(7)),mapping(self.asset,bad)]),error=code)
            self.assertEqual(self.state(),unchanged)
        self.revision=self.call('world.undo',dict(request_id=uuid.uuid4().hex,base_revision=self.revision))['revision']
        self.assertNotIn('asset_provenance',json.loads(self.world.read_bytes()))
        self.revision=self.call('world.redo',dict(request_id=uuid.uuid4().hex,base_revision=self.revision))['revision']
        self.assertEqual(self.query()['assets'],[dict(asset=self.asset,records=records)])
        self.reopen();self.assertEqual(self.query(asset=self.asset)['assets'],[dict(asset=self.asset,records=records)])
        self.edit([mapping(self.asset,[])]);self.assertNotIn('asset_provenance',json.loads(self.world.read_bytes()))
        self.assertEqual(self.call('world.dependencies')['assets'],[])

    def test_unused_mapping_fresh_closure_corruption_and_pagination(self):
        a=self.create()['record'];self.edit([mapping(self.asset,[a])])
        second=self.directory/'second.png';second.write_bytes(png(1,1,[200,30,60,255]))
        other=self.call('asset.image.import',dict(source=native(second),color_space='srgb'))['asset']
        b=self.create(self.metadata(asset=other))['record'];self.edit([mapping(other,[b])])
        ordered=sorted([self.asset,other]);page=self.query(limit=1)
        self.assertEqual([e['asset'] for e in page['assets']],ordered[:1]);self.assertTrue(page['has_more']);self.assertEqual(page['next_after'],ordered[0])
        last=self.query(after=page['next_after'],limit=1);self.assertEqual([e['asset'] for e in last['assets']],ordered[1:]);self.assertFalse(last['has_more']);self.assertIsNone(last['next_after'])
        self.call('world.asset.provenance',error=-32602)
        self.edit([dict(op='entity.rename',id=self.entity,name='New paging revision')])
        self.call('world.asset.provenance',dict(revision=page['revision'],after=page['next_after'],limit=1),error=-32009)
        for extra in (dict(limit=0),dict(limit=65),dict(after='A'*64),dict(asset=self.asset,after=self.asset)):
            self.call('world.asset.provenance',dict(revision=self.revision,**extra),error=-32602)
        before=self.state()
        self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=self.revision,ops=[mapping(self.asset,[b])]),error=-32602)
        self.assertEqual(self.state(),before)
        expected=sorted([dict(filename=r+'.pprov',sha256=r,bytes=self.record_path(r).stat().st_size) for r in (a,b)],key=lambda e:e['filename'])
        dependencies=self.call('world.dependencies');self.assertEqual(dependencies['assets'],[]);self.assertEqual(dependencies['provenance_records'],expected)
        self.assertRegex(dependencies['asset_credits_sha256'],r'^[0-9a-f]{64}$')
        # Provenance for unused content is retained; unused cooked bytes are not
        # accidentally pulled into the dependency closure or needed to inspect.
        (self.assets/(self.asset+'.pimage')).unlink();(self.assets/(other+'.pimage')).unlink()
        self.assertEqual(self.call('world.dependencies'),dependencies)
        self.call('asset.provenance.inspect',dict(record=a))
        path=self.record_path(a);raw=path.read_bytes()
        for corrupt in (True,False):
            if corrupt:path.write_bytes(raw+b' ')
            else:path.unlink()
            self.assertEqual(self.query()['assets'],[dict(asset=x,records=[a if x==self.asset else b]) for x in ordered])
            # Pagination is metadata-only even while a mapped record is absent.
            head=self.query(limit=1);tail=self.query(after=head['next_after'],limit=1)
            self.assertEqual(head['assets']+tail['assets'],[dict(asset=x,records=[a if x==self.asset else b]) for x in ordered])
            self.assertTrue(head['has_more']);self.assertEqual(head['next_after'],ordered[0])
            self.assertFalse(tail['has_more']);self.assertIsNone(tail['next_after'])
            self.call('asset.provenance.inspect',dict(record=a),error=-32050)
            self.call('world.dependencies',error=-32050);self.assertEqual(self.state(),before)
            path.write_bytes(raw)
        outside=self.directory/'record-copy.pprov';outside.write_bytes(raw);path.unlink()
        try:path.symlink_to(outside)
        except OSError as error:NOTES.append(dict(test=self.id(),symlink_unavailable=str(error)));path.write_bytes(raw)
        else:
            try:self.call('asset.provenance.inspect',dict(record=a),error=-32050);self.call('world.dependencies',error=-32050)
            finally:path.unlink();path.write_bytes(raw)
        self.assertEqual(self.call('world.dependencies'),dependencies)

    def test_forged_record_kind_rejected_by_active_asset_closure(self):
        # The checked creation API cannot manufacture this record. Inject a
        # correctly hashed/canonical declaration to test independent closure
        # verification rather than merely creation-time cooked-kind admission.
        model_path=self.directory/'quad.glb'
        document,blob=quad([],material={'pbrMetallicRoughness':{'baseColorFactor':[1,1,1,1]}})
        model_path.write_bytes(glb(document,blob))
        model=self.call('asset.import',dict(source=native(model_path)))['asset']
        self.edit([dict(op='component.set',id=self.entity,type='StaticMesh',value=dict(asset=model,primitive=0,visible=True)),
                   dict(op='component.set',id=self.entity,type='PbrTextures',value=dict(base_color=dict(asset=self.asset)))])
        assets=self.call('world.dependencies')['assets']
        self.assertIn(self.asset+'.pimage',[entry['filename'] for entry in assets])
        self.call('asset.provenance.create',dict(record=self.metadata(kind='model')),error=-32050)
        self.client.close()
        metadata=self.metadata(kind='model')
        raw=json.dumps(metadata,sort_keys=True,separators=(',',':'),ensure_ascii=False).encode('utf-8')
        identity=sha(raw);self.record_path(identity).write_bytes(raw)
        authored=json.loads(self.world.read_bytes());authored['asset_provenance']={self.asset:[identity]}
        self.world.write_text(json.dumps(authored,indent=2)+'\n',encoding='utf-8')
        before=self.world.read_bytes();inventory=tree(self.assets);self.client=self.open()
        self.assertEqual(self.call('asset.provenance.inspect',dict(record=identity)),dict(record=identity,bytes=len(raw),metadata=metadata))
        self.assertEqual(self.query(asset=self.asset)['assets'],[dict(asset=self.asset,records=[identity])])
        history=self.call('world.history')
        self.call('world.dependencies',error=-32050)
        self.assertEqual(self.world.read_bytes(),before);self.assertEqual(tree(self.assets),inventory);self.assertEqual(self.call('world.history'),history)

    def test_invalid_records_and_size_bounds_are_nonmutating(self):
        original=self.metadata();bad=[]
        for field,value in [('version',True),('version',2),('asset','A'*64),('asset','0'*63),('kind','unknown'),('title',''),
                            ('title','x'*1025),('creator','x'*1025),('creator','bad\x00name'),
                            ('source','file:///secret'),('source','https://user:password@example.org/asset'),
                            ('source','https://example.org/a b'),('source','https://example.org/\\path'),
                            ('source','https://example.org/'+('x'*4096)),('unknown',1)]:
            row=copy.deepcopy(original);row[field]=value;bad.append(row)
        for license in ({'identifier':'','notice':'x'},{'identifier':'x'*257,'notice':'x'},
                        {'identifier':'ok','notice':''},{'identifier':'ok','notice':'x'*32769},
                        {'identifier':'ok','notice':'x','extra':True}):
            row=copy.deepcopy(original);row['license']=license;bad.append(row)
        for inputs in ([dict(sha256='a'*64,bytes=-1)],[dict(sha256='A'*64,bytes=1)],
                       [dict(sha256='a'*64,bytes=True)],[dict(sha256='a'*64,bytes=9007199254740992)],
                       [dict(sha256='a'*64,bytes=1)]*2,
                       [dict(sha256=f'{i:064x}',bytes=1) for i in range(33)]):
            row=copy.deepcopy(original);row['inputs']=inputs;bad.append(row)
        before=self.state();inventory=tree(self.assets)
        for row in bad:
            self.call('asset.provenance.create',dict(record=row),error=-32602)
            self.assertEqual(tree(self.assets),inventory);self.assertEqual(self.state(),before)
        self.call('asset.provenance.create',dict(record=self.metadata(asset='f'*64)),error=-32050)
        self.call('asset.provenance.create',dict(record=self.metadata(kind='audio')),error=-32050)
        # UTF-8 byte limits, escaped canonical JSON budget, and valid max input count.
        row=self.metadata(title='é'*512,creator='é'*512,inputs=[dict(sha256=f'{i+1:064x}',bytes=9007199254740991) for i in range(32)])
        self.create(row);row['title']+='é';self.call('asset.provenance.create',dict(record=row),error=-32602)
        row=self.metadata(license=dict(identifier='ok',notice='\x01'*32768))
        self.call('asset.provenance.create',dict(record=row),error=-32602)
        records=[self.create(self.metadata(title=f'Record {i}'))['record'] for i in range(9)]
        self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=self.revision,ops=[mapping(self.asset,sorted(records))]),error=-32602)
        self.assertEqual(self.state(),before)

    def test_malformed_wire_utf8_and_duplicate_keys(self):
        # WorldClient deliberately rejects invalid Unicode before transport.
        # A separate bounded raw owner tests the actual native parser instead.
        before=self.state();inventory=tree(self.assets);self.client.close()
        request=dict(jsonrpc='2.0',id=1,method='asset.provenance.create',params=dict(record=self.metadata()))
        wire=json.dumps(request,separators=(',',':')).encode()
        needle=b'"title":"Original fixture"'
        cases=[wire.replace(needle,b'"title":"first","title":"second"'),
               wire.replace(needle,b'"title":"\\ud800"'),wire.replace(needle,b'"title":"\xff"')]
        process=subprocess.Popen([str(ARGS.binary.resolve()),'world',native(self.world)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:stdout,stderr=process.communicate(b'\n'.join(cases)+b'\n',timeout=30)
        except BaseException:
            process.kill();process.communicate();raise
        finally:OWNERS.append(dict(test=self.id(),exit_code=process.returncode,transport='raw invalid JSON'))
        self.assertEqual(process.returncode,0,stderr.decode(errors='replace'))
        replies=[json.loads(line) for line in stdout.splitlines()];self.assertEqual(len(replies),len(cases))
        for payload,reply in zip(cases,replies):
            CALLS.append(dict(test=self.id(),method='asset.provenance.create',raw_request_hex=payload.hex(),reply=reply))
            self.assertIn('error',reply);self.assertIn(reply['error']['code'],(-32700,-32602))
        self.client=self.open();self.assertEqual(self.world.read_bytes(),before['world']);self.assertEqual(self.call('world.inspect'),before['inspect']);self.assertEqual(tree(self.assets),inventory)

    def test_saved_source_freezes_a_and_fresh_restore_checks_a(self):
        if not ARGS.runtime:self.skipTest('Simulation disabled')
        a=self.create(self.metadata(title='Frozen A'))['record'];b=self.create(self.metadata(title='Current B'))['record']
        self.edit([mapping(self.asset,[a])]);frozen_revision=self.revision
        saves=self.directory/'saves';saves.mkdir()
        def configure():return self.call('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(saves)))['generation']
        generation=configure();self.session=uuid.uuid4().hex
        self.call('runtime.start',dict(revision=self.revision,session_id=self.session))
        def entity():return self.call('runtime.entity',dict(session_id=self.session,tick=self.tick,id=self.entity))
        saved=entity()
        self.call('save.write',dict(request_id=uuid.uuid4().hex,configuration_generation=generation,slot='provenance',expected_generation=0,
            session_id=self.session,expected_tick=self.tick,expected_gameplay_revision=0))
        self.edit([mapping(self.asset,[b]),dict(op='component.set',id=self.entity,type='Transform',value=pose(9))])
        authored=self.state();saved_files=tree(saves)
        def load(error=None):
            new=uuid.uuid4().hex
            params=dict(request_id=uuid.uuid4().hex,configuration_generation=generation,slot='provenance',expected_generation=1,
                revision=self.revision,expected_session_id=self.session,expected_tick=self.tick if self.session else None,
                expected_gameplay_revision=0 if self.session else None,new_session_id=new)
            result=self.call('save.load',params,error=error)
            if error is None:self.session=new;self.tick=result['tick']
            return result
        path=self.record_path(a);raw=path.read_bytes();path.write_bytes(b'corrupted frozen provenance')
        self.call('world.dependencies') # Today's B closure is valid.
        load(error=-32070);self.assertEqual(entity(),saved);self.assertEqual(self.state(),authored);self.assertEqual(tree(saves),saved_files)
        self.reopen();self.session=None;self.tick=0;generation=configure()
        load(error=-32070);self.assertFalse(self.call('runtime.status')['active']);self.assertEqual(self.state()['world'],authored['world'])
        path.write_bytes(raw);restored=load();self.assertTrue(restored['source_stale']);self.assertEqual(restored['authored_revision'],frozen_revision)
        self.assertEqual(normalized(entity()),normalized(saved));self.assertEqual(self.state()['world'],authored['world']);self.assertEqual(tree(saves),saved_files)
        self.assertEqual(self.query()['assets'],[dict(asset=self.asset,records=[b])])

def main():
    global ARGS
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('binary',type=Path)
    parser.add_argument('--runtime',type=int,choices=(0,1),default=0);parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--output',type=Path);parser.add_argument('--evidence',type=Path);ARGS=parser.parse_args()
    if sys.flags.optimize:parser.error('Assertions must be enabled')
    if ARGS.output is None:ARGS.output=ROOT/'build/asset-provenance-contract'/uuid.uuid4().hex
    if ARGS.output.exists():parser.error('Output must be new')
    ARGS.output.mkdir(parents=True)
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ProvenanceContract))
    evidence=dict(passed=result.wasSuccessful(),tests=result.testsRun,skipped=len(result.skipped),rpc_calls=len(CALLS),
        methods=dict(sorted(Counter(c['method'] for c in CALLS).items())),owners=OWNERS,calls=CALLS,notes=NOTES,
        binary_sha256=sha(ARGS.binary.read_bytes()),test_sha256=sha(Path(__file__).read_bytes()))
    path=ARGS.evidence or ARGS.output/'local-evidence.json';path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(evidence,ensure_ascii=True,indent=2)+'\n',encoding='utf-8')
    return 0 if result.wasSuccessful() else 1
if __name__=='__main__':raise SystemExit(main())
