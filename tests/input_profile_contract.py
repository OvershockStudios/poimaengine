#!/usr/bin/env python3
"""Persistent player input settings and native evaluation through JSON-RPC."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import os
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
ROOT=Path(__file__).resolve().parents[1];SCRATCH=ROOT/'build/input-profile-contract'
SCRATCH.mkdir(parents=True,exist_ok=True)
DEFAULT={'bindings':{'forward':['key.w'],'backward':['key.s'],'left':['key.a'],'right':['key.d'],'jump':['key.space'],'use':['key.e']},
         'sensitivity_x':.1,'sensitivity_y':.1,'invert_x':False,'invert_y':False}
REPORTS=[]
def rpc(method,**params):return {'jsonrpc':'2.0','method':method,'params':params}

class InputProfile(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=SCRATCH);self.root=Path(self.tmp.name)
        self.world=self.root/'world.json';self.profile=self.root/'player.poima-input.json'
        self.result(self.requests(rpc('world.transact',request_id=uuid.uuid4().hex,base_revision=0,
            ops=[{'op':'entity.create','id':'1'*32,'name':'Unchanged authored world'}]))[0])
        self.world_bytes=self.world.read_bytes()
    def tearDown(self):
        self.assertEqual(self.world.read_bytes(),self.world_bytes,'Input settings changed authored world bytes.')
        self.tmp.cleanup()
    def native(self,path):
        return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip() if args.windows_interop else str(path.resolve())
    def requests(self,*requests,world=None):
        command=[BINARY,'world',self.native(world or self.world)]
        p=subprocess.run(command,input=''.join(json.dumps(dict(r,id=i))+'\n' for i,r in enumerate(requests)),
                         text=True,encoding='utf-8',capture_output=True,timeout=30)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        out=[json.loads(line) for line in p.stdout.splitlines()];self.assertEqual(len(out),len(requests));return out
    def result(self,response):self.assertIn('result',response,response);return response['result']
    def error(self,response,code=None):
        self.assertIn('error',response,response)
        if code is not None:self.assertEqual(response['error']['code'],code,response)
    def transaction(self,profile=None,revision=0,request_id=None,**kw):
        return rpc('input.transact',path=self.native(self.profile),request_id=request_id or uuid.uuid4().hex,
                   expected_revision=revision,profile=copy.deepcopy(profile if profile is not None else DEFAULT),**kw)
    def inspect(self):return self.result(self.requests(rpc('input.inspect',path=self.native(self.profile)))[0])
    def remapped(self):
        profile=copy.deepcopy(DEFAULT);profile['bindings']['forward']=['key.up'];return profile
    def test_discovery_and_missing_profile(self):
        schema,description=[self.result(r) for r in self.requests(rpc('world.describe'),rpc('input.describe'))]
        self.assertGreaterEqual(schema['schema_revision'],17)
        for method in ['input.describe','input.inspect','input.transact','input.evaluate']:self.assertIn(method,schema['methods'])
        for key in ['input_profile','input_revision']:self.assertIn(key,schema['methods']['runtime.play']['properties'])
        self.assertEqual(description['defaults'],DEFAULT)
        controls={control['id']:control for control in description['controls']}
        for name in ['key.w','key.up','mouse.3','key.f1']:self.assertIn(name,controls)
        for name in ['key.escape','key.tab']:self.assertIn(name,description['reserved_controls'])
        missing=self.inspect();self.assertEqual(missing['profile'],DEFAULT);self.assertEqual(missing['revision'],0)
        self.assertFalse(missing['persisted']);self.assertFalse(self.profile.exists())
    def test_preview_restart_and_shared_profile(self):
        request=self.transaction(self.remapped(),preview=True)
        preview=self.result(self.requests(request)[0]);self.assertTrue(preview['preview']);self.assertEqual(preview['revision'],0)
        self.assertEqual(preview['proposed_revision'],1);self.assertFalse(preview['persisted']);self.assertFalse(self.profile.exists())
        request['params']['preview']=False
        committed=self.result(self.requests(request)[0]);self.assertEqual(committed['revision'],1)
        self.assertTrue(committed['persisted']);self.assertEqual(committed['profile'],self.remapped())
        raw=self.profile.read_bytes();inspection=self.inspect()
        self.assertEqual(inspection['profile'],self.remapped());self.assertEqual(inspection['content_hash'],committed['content_hash'])
        other=self.root/'another-world.json'
        shared=self.result(self.requests(rpc('input.inspect',path=self.native(self.profile)),world=other)[0])
        self.assertEqual(shared,inspection);self.assertFalse(other.exists());self.assertEqual(self.profile.read_bytes(),raw)
        REPORTS.append({'shared_between_worlds':True,'content_hash':inspection['content_hash'],'application':inspection['application']})
    def test_receipt_replay_conflicts_and_noop_revision(self):
        request=self.transaction(self.remapped());first=self.result(self.requests(request)[0]);raw=self.profile.read_bytes()
        retry=copy.deepcopy(request);retry['params']['preview']=False
        self.assertEqual(self.result(self.requests(retry)[0]),dict(first,replayed=True));self.assertEqual(raw,self.profile.read_bytes())
        changed=copy.deepcopy(request);changed['params']['profile']['invert_y']=True
        self.error(self.requests(changed)[0],-32010);self.assertEqual(raw,self.profile.read_bytes())
        self.error(self.requests(self.transaction(revision=0))[0],-32009);self.assertEqual(raw,self.profile.read_bytes())
        no_op=self.result(self.requests(self.transaction(self.remapped(),revision=1))[0])
        self.assertEqual(no_op['revision'],2);self.assertFalse(no_op['changed']);self.assertEqual(no_op['content_hash'],first['content_hash'])
        current=self.profile.read_bytes()
        self.assertEqual(self.result(self.requests(request)[0]),dict(first,replayed=True));self.assertEqual(current,self.profile.read_bytes())
        self.assertEqual(self.inspect()['revision'],2)
    def test_receipt_retention_boundary_and_previous_snapshot(self):
        requests=[]
        for revision in range(35):
            profile=self.remapped();profile['sensitivity_x']=.1+revision*.01
            requests.append(self.transaction(profile,revision=revision))
        results=[self.result(r) for r in self.requests(*requests)]
        self.assertEqual([r['revision'] for r in results],list(range(1,36)))
        raw=self.profile.read_bytes();previous=Path(str(self.profile)+'.previous')
        previous_raw=previous.read_bytes();self.assertEqual(json.loads(previous_raw)['revision'],34)
        self.assertEqual(self.inspect()['revision'],35)
        # Requests 0..2 fall outside the retained 32-entry receipt window.
        # Index 3 is the oldest surviving receipt, not merely the latest one.
        for index in [3,34]:
            self.assertEqual(self.result(self.requests(requests[index])[0]),dict(results[index],replayed=True))
            self.assertEqual(self.profile.read_bytes(),raw);self.assertEqual(previous.read_bytes(),previous_raw)
        for index in [0,2]:
            self.error(self.requests(requests[index])[0],-32009)
            self.assertEqual(self.profile.read_bytes(),raw);self.assertEqual(previous.read_bytes(),previous_raw)
        REPORTS.append({'receipt_window':32,'commits':35,'evicted_receipts_rejected':True,'previous_revision':34})
    def test_invalid_profiles_preserve_last_good_bytes(self):
        self.result(self.requests(self.transaction())[0]);before=self.profile.read_bytes()
        variants=[]
        def altered(change):
            value=copy.deepcopy(DEFAULT);change(value);variants.append(value)
        altered(lambda p:p['bindings'].update(forward=['key.escape']))
        altered(lambda p:p['bindings'].update(forward=['key.tab']))
        altered(lambda p:p['bindings'].update(forward=['key.unknown']))
        altered(lambda p:p['bindings'].update(forward=['key.a']))
        altered(lambda p:p['bindings'].update(forward=['key.w','key.w']))
        altered(lambda p:p['bindings'].update(forward='key.w'))
        altered(lambda p:p['bindings'].update(forward=['key.w','key.up','key.i','key.t','key.f1']))
        altered(lambda p:p['bindings'].update(fly=['key.f']))
        altered(lambda p:p['bindings'].pop('jump'))
        for key,value in [('sensitivity_x',-1),('sensitivity_y',11),('sensitivity_x',True),('sensitivity_y','0.1'),('invert_x',1),('invert_y','false'),('extra',0)]:
            altered(lambda p,k=key,v=value:p.update({k:v}))
        variants.extend([[],None,42])
        for value in variants:
            request=self.transaction(revision=1);request['params']['profile']=value
            with self.subTest(profile=value):self.error(self.requests(request)[0],-32602);self.assertEqual(before,self.profile.read_bytes())
        good=self.result(self.requests(self.transaction(self.remapped(),revision=1))[0]);self.assertEqual(good['revision'],2)
    def test_profile_file_failures_never_overwrite(self):
        self.result(self.requests(self.transaction())[0]);valid=self.profile.read_bytes();document=json.loads(valid)
        newer=copy.deepcopy(document);newer['format']='poima.input.v999'
        invalid=copy.deepcopy(document);invalid['profile']['bindings']['forward']=['key.escape']
        for raw in [b'{broken',json.dumps(newer).encode(),json.dumps(invalid).encode()]:
            self.profile.write_bytes(raw)
            for request in [rpc('input.inspect',path=self.native(self.profile)),self.transaction(revision=1),rpc('input.evaluate',path=self.native(self.profile),events=[])]:
                self.error(self.requests(request)[0]);self.assertEqual(self.profile.read_bytes(),raw)
        self.profile.write_bytes(valid);self.assertEqual(self.inspect()['revision'],1)
        for path in [self.root/'wrong.json',self.root/'missing-directory'/'profile.poima-input.json']:
            request=self.transaction();request['params']['path']=self.native(path)
            self.error(self.requests(request)[0]);self.assertFalse(path.exists())
    def test_oversized_and_duplicate_key_files_preserve_bytes(self):
        self.result(self.requests(self.transaction())[0]);valid=self.profile.read_bytes()
        duplicate_top=b'{"revision":1,'+valid[1:]
        duplicate_nested=valid.replace(b'"invert_x":false',b'"invert_x":false,"invert_x":false',1)
        self.assertNotEqual(duplicate_nested,valid)
        for raw in [valid+b' '*(65537-len(valid)),duplicate_top,duplicate_nested]:
            self.profile.write_bytes(raw)
            for request in [rpc('input.inspect',path=self.native(self.profile)),self.transaction(revision=1),
                            rpc('input.evaluate',path=self.native(self.profile),events=[])]:
                self.error(self.requests(request)[0],-32070);self.assertEqual(self.profile.read_bytes(),raw)
        self.profile.write_bytes(valid)
    def test_reserved_world_and_asset_paths_preserve_bytes(self):
        # A world with the profile suffix proves rejection is not just a suffix
        # check. Its own writer remains the only owner of its data and sidecars.
        other=self.root/'authored.poima-input.json'
        create=rpc('world.transact',request_id=uuid.uuid4().hex,base_revision=0,
                   ops=[{'op':'entity.create','id':'2'*32,'name':'Reserved world'}])
        self.result(self.requests(create,world=other)[0]);raw=other.read_bytes()
        request=self.transaction();request['params']['path']=self.native(other)
        self.error(self.requests(request,world=other)[0],-32602);self.assertEqual(other.read_bytes(),raw)
        cache=Path(str(self.world)+'.assets');cache.mkdir(exist_ok=True)
        target=cache/'reserved.poima-input.json';target.write_bytes(b'immutable asset sentinel')
        before=target.read_bytes();request['params']['path']=self.native(target)
        for operation in [request,rpc('input.inspect',path=self.native(target))]:
            self.error(self.requests(operation)[0],-32602);self.assertEqual(target.read_bytes(),before)
        self.assertEqual(sorted(p.name for p in cache.iterdir()),[target.name])
    def check_linked_paths(self,hard):
        if args.windows_interop:self.skipTest('Link creation exercised on native Linux; Windows interoperability link semantics differ.')
        self.result(self.requests(self.transaction())[0]);valid=self.profile.read_bytes()
        for index,suffix in enumerate(['','.lock','.pending','.previous','.previous.pending']):
            folder=self.root/('link-case-'+str(index));folder.mkdir()
            path=folder/'profile.poima-input.json';path.write_bytes(valid)
            target=folder/'must-remain-unchanged';target.write_bytes(valid if not suffix else b'linked sidecar sentinel')
            linked=Path(str(path)+suffix)
            if not suffix:linked.unlink()
            try:
                if hard:os.link(target,linked)
                else:linked.symlink_to(target)
            except (NotImplementedError,OSError) as error:self.skipTest('Filesystem does not support this link setup: '+str(error))
            before=path.read_bytes();target_bytes=target.read_bytes();inode=target.stat().st_ino
            # Preserve the link pathname: resolve() would test its target instead.
            linked_path=str(path.absolute())
            request=self.transaction(self.remapped(),revision=1);request['params']['path']=linked_path
            for operation in [request,rpc('input.inspect',path=linked_path),rpc('input.evaluate',path=linked_path,events=[])]:
                self.error(self.requests(operation)[0]);self.assertEqual(path.read_bytes(),before)
                self.assertEqual(target.read_bytes(),target_bytes);self.assertEqual(target.stat().st_ino,inode)
            self.assertTrue(linked.is_symlink() if not hard else linked.stat().st_ino==inode)
    def test_symlink_profiles_and_sidecars_preserve_targets(self):self.check_linked_paths(False)
    def test_hardlink_profiles_and_sidecars_preserve_targets(self):self.check_linked_paths(True)
    def test_invalid_transaction_envelopes_are_atomic(self):
        self.result(self.requests(self.transaction())[0]);raw=self.profile.read_bytes()
        for change in [{'expected_revision':-1},{'expected_revision':'1'},{'expected_revision':True},
                       {'expected_revision':9007199254740992},{'request_id':'invalid'},
                       {'preview':1},{'unknown':True},{'path':42}]:
            request=self.transaction(revision=1);request['params'].update(change)
            with self.subTest(change=change):self.error(self.requests(request)[0],-32602);self.assertEqual(self.profile.read_bytes(),raw)
    def test_profile_preview_preserves_existing_file_and_empty_binding(self):
        self.result(self.requests(self.transaction())[0]);raw=self.profile.read_bytes()
        profile=self.remapped();profile['bindings']['jump']=[];profile['bindings']['use']=['mouse.3','key.f1']
        preview=self.result(self.requests(self.transaction(profile,revision=1,preview=True))[0])
        self.assertEqual(preview['profile'],profile);self.assertEqual(preview['revision'],1);self.assertEqual(preview['proposed_revision'],2)
        self.assertEqual(self.profile.read_bytes(),raw)
        self.assertEqual(self.result(self.requests(self.transaction(profile,revision=1))[0])['profile'],profile)
    def test_saved_bindings_native_evaluator_edges_and_look(self):
        profile=self.remapped();profile.update(sensitivity_x=.25,sensitivity_y=.5,invert_x=True,invert_y=True)
        committed=self.result(self.requests(self.transaction(profile))[0]);before=self.profile.read_bytes()
        events=[{'control':'key.w','down':True},{'consume':True},{'control':'key.up','down':True},{'consume':True},
                {'control':'key.space','down':True},{'motion':[8,-6]},{'consume':True},{'consume':True},
                {'clear':True},{'consume':True}]
        result=self.result(self.requests(rpc('input.evaluate',path=self.native(self.profile),events=events))[0])
        frames=result['frames'];self.assertEqual([f['event'] for f in frames],[1,3,6,7,9])
        self.assertEqual(frames[0]['move'],[0,0]);self.assertEqual(frames[1]['move'],[0,1])
        self.assertTrue(frames[2]['jump']);self.assertFalse(frames[3]['jump'])
        self.assertEqual(frames[2]['look'],[2,-3]);self.assertEqual(frames[3]['look'],[0,0])
        self.assertEqual(frames[4]['move'],[0,0]);self.assertFalse(frames[4]['jump'])
        self.assertEqual(result['input_profile']['source'],'profile');self.assertEqual(result['input_profile']['revision'],1)
        self.assertEqual(result['input_profile']['content_hash'],committed['content_hash']);self.assertEqual(self.profile.read_bytes(),before)
        default=self.result(self.requests(rpc('input.evaluate',events=[{'control':'key.w','down':True},{'consume':True}]))[0])
        self.assertEqual(default['frames'][0]['move'],[0,1]);self.assertEqual(default['input_profile']['source'],'defaults')
    def test_invalid_evaluator_events(self):
        for events in [[{'control':'key.unknown','down':True}],[{'control':'key.w','down':1}],[{'motion':[1]}],
                       [{'motion':['1',2]}],[{'consume':1}],[{'clear':False}],[{'consume':True,'clear':True}],
                       [{'control':'key.w','down':True,'motion':[1,2]}],[{'mystery':True}],[{'consume':True}]*257]:
            with self.subTest(events=events[:2]):self.error(self.requests(rpc('input.evaluate',events=events))[0],-32602)
        self.error(self.requests(rpc('input.evaluate',path=self.native(self.profile),events=[]))[0]);self.assertFalse(self.profile.exists())

if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(InputProfile))
    if args.evidence:args.evidence.write_text(json.dumps({'tests':result.testsRun,'passed':result.wasSuccessful(),
        'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'reports':REPORTS},indent=2)+'\n')
    raise SystemExit(not result.wasSuccessful())
