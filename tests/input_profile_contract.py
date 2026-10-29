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
        self.assertGreaterEqual(schema['schema_revision'],119)
        for method in ['input.describe','input.inspect','input.transact','input.evaluate']:self.assertIn(method,schema['methods'])
        inspection=schema['methods']['input.inspect']
        self.assertEqual(inspection['required'],['path']);self.assertFalse(inspection['additionalProperties'])
        self.assertEqual(inspection['properties']['read_only']['type'],'boolean')
        self.assertIs(inspection['properties']['read_only']['default'],False)
        for key in ['input_profile','input_revision']:self.assertIn(key,schema['methods']['runtime.play']['properties'])
        self.assertEqual(description['defaults'],DEFAULT)
        controls={control['id']:control for control in description['controls']}
        for name in ['key.w','key.up','mouse.3','key.f1']:self.assertIn(name,controls)
        for name in ['key.escape','key.tab']:self.assertIn(name,description['reserved_controls'])
        missing=self.inspect();self.assertEqual(missing['profile'],DEFAULT);self.assertEqual(missing['revision'],0)
        self.assertFalse(missing['persisted']);self.assertFalse(self.profile.exists())
    def directory_state(self,folder):
        # Include every directory and every complete file; no lock/sidecar exclusions.
        return {p.relative_to(folder).as_posix():('directory',None) if p.is_dir() else ('file',p.read_bytes())
                for p in folder.rglob('*')}
    def test_read_only_missing_and_valid_exact_response_and_membership(self):
        missing=self.inspect()
        self.result(self.requests(self.transaction(self.remapped()))[0]);expected=self.inspect()
        self.assertEqual(set(expected),{'format','profile','revision','persisted','content_hash','application'})
        folder=self.root/'immutable-inspection';folder.mkdir();(folder/'empty-directory').mkdir()
        (folder/'sentinel.bin').write_bytes(b'whole-directory sentinel\x00\xff')
        target=folder/'existing.poima-input.json';target.write_bytes(self.profile.read_bytes())
        absent=folder/'absent.poima-input.json';before=self.directory_state(folder)
        for path,wanted in [(target,expected),(absent,missing)]:
            # Both ordinary native absolute paths and world-relative paths use the same guard path.
            for selected in [self.native(path),path.relative_to(self.root).as_posix()]:
                with self.subTest(path=selected):
                    response=self.result(self.requests(rpc('input.inspect',path=selected,read_only=True))[0])
                    self.assertEqual(response,wanted)
                    self.assertEqual(self.directory_state(folder),before)
        REPORTS.append({'read_only_inspection':True,'complete_response_equal':True,
            'missing_defaults_equal':True,'complete_directory_bytes_and_membership_equal':True})
    def test_read_only_corruption_preserves_complete_directory(self):
        self.result(self.requests(self.transaction())[0]);valid=self.profile.read_bytes()
        newer=json.loads(valid);newer['format']='poima.input.v999'
        duplicate=b'{"revision":1,'+valid[1:]
        for index,raw in enumerate([b'{broken',json.dumps(newer).encode(),duplicate]):
            folder=self.root/('corrupt-read-only-'+str(index));folder.mkdir()
            target=folder/'player.poima-input.json';target.write_bytes(raw)
            (folder/'sentinel.bin').write_bytes(b'preserved');before=self.directory_state(folder)
            request=rpc('input.inspect',path=self.native(target),read_only=True)
            observed=self.requests(request)[0];self.error(observed,-32070)
            self.assertEqual(self.directory_state(folder),before)
            # Retain the ordinary locked error result too; the new mode changes no error schema.
            ordinary=self.requests(rpc('input.inspect',path=self.native(target)))[0]
            self.error(ordinary,-32070);self.assertEqual(observed['error'],ordinary['error'])
            self.assertEqual(target.read_bytes(),raw)
            after=dict(before);after[target.name+'.lock']=('file',b'')
            self.assertEqual(self.directory_state(folder),after)
    def test_read_only_strict_boolean_rejected_before_path_or_storage(self):
        self.result(self.requests(self.transaction())[0])
        folder=self.root/'invalid-read-only';folder.mkdir()
        valid=folder/'valid.poima-input.json';valid.write_bytes(self.profile.read_bytes())
        corrupt=folder/'corrupt.poima-input.json';corrupt.write_bytes(b'{broken')
        before=self.directory_state(folder)
        for value in [0,1,-1,0.0,1.0,'true','false',None,[],{},[True]]:
            for path in [self.native(valid),self.native(corrupt),self.native(folder/'absent.poima-input.json'),42]:
                with self.subTest(value=value,path=path):
                    response=self.requests(rpc('input.inspect',path=path,read_only=value))[0]
                    self.error(response,-32602)
                    self.assertEqual(response['error']['message'],'read_only must be boolean.')
                    self.assertEqual(self.directory_state(folder),before)
    def test_default_and_explicit_false_retain_lock_sidecar(self):
        self.result(self.requests(self.transaction(self.remapped()))[0]);expected=self.inspect()
        for name,extra in [('default',{}),('explicit-false',{'read_only':False})]:
            folder=self.root/name;folder.mkdir()
            target=folder/'player.poima-input.json';target.write_bytes(self.profile.read_bytes())
            before=self.directory_state(folder)
            observed=self.result(self.requests(rpc('input.inspect',path=self.native(target),**extra))[0])
            self.assertEqual(observed,expected)
            after=dict(before);after[target.name+'.lock']=('file',b'')
            self.assertEqual(self.directory_state(folder),after)
        REPORTS.append({'default_and_explicit_false_preserve_cooperative_lock_creation':True})
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
        for operation in [request,rpc('input.inspect',path=self.native(target)),
                          rpc('input.inspect',path=self.native(target),read_only=True)]:
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
            for operation in [request,rpc('input.inspect',path=linked_path),
                              rpc('input.inspect',path=linked_path,read_only=True),
                              rpc('input.evaluate',path=linked_path,events=[])]:
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
    def test_v3_named_states_roundtrip_and_format_boundary(self):
        old=self.root/'old.poima-input.json'
        legacy=self.transaction();legacy['params']['path']=self.native(old)
        prior=self.result(self.requests(legacy)[0]);old_bytes=old.read_bytes()
        profile=copy.deepcopy(DEFAULT);profile['actions']={'reload':['key.r'],'fire':['mouse.1','key.f'],'aim':['mouse.3'],'empty':[]}
        convert=self.transaction(profile,revision=1);convert['params']['path']=self.native(old)
        for preview in [False,True]:
            convert['params']['preview']=preview
            self.error(self.requests(convert)[0],-32602);self.assertEqual(old.read_bytes(),old_bytes)
        self.assertEqual(self.result(self.requests(legacy)[0]),dict(prior,replayed=True))
        request=self.transaction(profile);committed=self.result(self.requests(request)[0]);raw=self.profile.read_bytes()
        self.assertEqual(json.loads(raw)['format'],'poima.input.v3');self.assertEqual(self.inspect()['profile'],profile)
        canonical=json.dumps(profile,sort_keys=True,separators=(',',':')).encode()
        self.assertEqual(committed['content_hash'],hashlib.sha256(canonical).hexdigest())
        self.assertEqual(self.result(self.requests(request)[0]),dict(committed,replayed=True))
        conflict=copy.deepcopy(request);conflict['params']['profile']['actions']['fire']=[]
        self.error(self.requests(conflict)[0],-32010);self.assertEqual(self.profile.read_bytes(),raw)
        events=[{'control':'mouse.1','down':True},{'consume':True},{'consume':True},
            {'control':'key.f','down':True},{'control':'mouse.1','down':False},{'consume':True},
            {'control':'key.f','down':False},{'consume':True},
            {'control':'key.r','down':True},{'control':'key.r','down':False},{'consume':True},
            {'control':'mouse.3','down':True},{'clear':True},{'consume':True}]
        result=self.result(self.requests(rpc('input.evaluate',path=self.native(self.profile),events=events))[0])
        self.assertEqual(result['input_profile']['format'],'poima.input.v3')
        frames=result['frames']
        for frame in frames:self.assertEqual([a['name'] for a in frame['actions']],['aim','empty','fire','reload'])
        fire=[next(a for a in f['actions'] if a['name']=='fire') for f in frames]
        self.assertEqual(fire[:4],[dict(name='fire',held=h,pressed=p,released=r) for h,p,r in
            [(True,True,False),(True,False,False),(True,False,False),(False,False,True)]])
        self.assertEqual(next(a for a in frames[4]['actions'] if a['name']=='reload'),
            dict(name='reload',held=False,pressed=True,released=True))
        for row in frames[5]['actions']:self.assertEqual(row,dict(name=row['name'],held=False,pressed=False,released=False))
        self.assertEqual(self.profile.read_bytes(),raw);self.assertEqual(old.read_bytes(),old_bytes)
        empty=copy.deepcopy(profile);empty['actions']={}
        self.result(self.requests(self.transaction(empty,revision=1))[0]);self.assertEqual(self.inspect()['format'],'poima.input.v3')
        self.error(self.requests(self.transaction(DEFAULT,revision=2))[0],-32602)
        REPORTS.append({'v3_named_actions':4,'complete_sorted_states':True,'quick_tap_press_and_release':True,
            'legacy_conversion_requires_new_destination':True,'old_receipt_replay_unchanged':True})
    def test_v3_strict_names_controls_limits_and_corrupt_receipts(self):
        profile=copy.deepcopy(DEFAULT);profile['actions']={'fire':['mouse.1']}
        self.result(self.requests(self.transaction(profile))[0]);good=self.profile.read_bytes()
        variants=[]
        for actions in [None,[],True,{'':[]},{'Fire':[]},{'1fire':[]},{'fire aim':[]},{'f/aim':[]},{'f\u00e9':[]},
            {'a'*65:[]},{'fire':'mouse.1'},{'fire':[True]},{'fire':['mouse.1','mouse.1']},
            {'fire':['key.w']},{'fire':['key.escape']},{'fire':['key.tab']},{'fire':['gamepad.start']},
            {'fire':['gamepad.east']},{'fire':['key.unknown']},{'fire':['key.f','key.g','key.h','key.i','key.j']},
            {f'action.{i}':[] for i in range(33)},{'fire':['mouse.1'],'aim':['mouse.1']}]:
            value=copy.deepcopy(profile);value['actions']=actions;variants.append(value)
        for value in variants:
            with self.subTest(actions=value['actions']):
                self.error(self.requests(self.transaction(value,revision=1))[0],-32602);self.assertEqual(self.profile.read_bytes(),good)
        accepted=copy.deepcopy(profile);accepted['actions']={f'action.{i}':[] for i in range(31)}
        accepted['actions']['a'*64]=['key.f','key.g','key.h','key.i']
        self.result(self.requests(self.transaction(accepted,revision=1))[0]);valid=self.profile.read_bytes()
        doc=json.loads(valid);wrong=copy.deepcopy(doc);wrong['format']='poima.input.v1'
        mixed=copy.deepcopy(doc);mixed['receipts'][0]['params']['profile'].pop('actions')
        duplicate=valid.replace(b'"actions":{',b'"actions":{"duplicate":[],"duplicate":[],',1)
        self.assertNotEqual(valid,duplicate)
        for raw in [json.dumps(wrong).encode(),json.dumps(mixed).encode(),duplicate]:
            self.profile.write_bytes(raw)
            self.error(self.requests(rpc('input.inspect',path=self.native(self.profile)))[0],-32070)
            self.assertEqual(self.profile.read_bytes(),raw)
        self.profile.write_bytes(valid)

if __name__=='__main__':
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(InputProfile))
    if args.evidence:args.evidence.write_text(json.dumps({'tests':result.testsRun,'passed':result.wasSuccessful(),
        'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'reports':REPORTS},indent=2)+'\n')
    raise SystemExit(not result.wasSuccessful())
