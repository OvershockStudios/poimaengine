# SPDX-License-Identifier: Apache-2.0
"""Pure checks of import-guard output selection and cleanup after work expiry."""
from pathlib import Path
import importlib.util
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tests'),str(ROOT/'tools/python')]
SOURCE=ROOT/'tests/asset_import_guards.py'
spec=importlib.util.spec_from_file_location('candidate_guard_fixture',SOURCE)
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)

class DefaultsAndBudget(unittest.TestCase):
    def test_only_native_posix_default_uses_os_local_parent(self):
        for platform,interop,local in [('posix',False,True),('posix',True,False),('nt',False,False),('nt',True,False)]:
            with self.subTest(platform=platform,interop=interop), \
                 patch.object(f,'os',SimpleNamespace(name=platform)), \
                 patch.object(f.tempfile,'gettempdir',return_value='/owned-os-local'), \
                 patch.object(f.uuid,'uuid4',return_value=SimpleNamespace(hex='a'*32)):
                actual=f.default_output(interop)
                parent=Path('/owned-os-local/poima-asset-import-guards')if local else f.ROOT/'build/asset-import-guards'
                self.assertEqual(actual,parent/('a'*32))

    def owner(self,directory,clock):
        owner=f.ImportGuards('test_matching_and_legacy_package_identity')
        owner.directory=directory;owner.serial=0;owner.client=owner.transport=None
        owner.raw=[];owner.snapshots=0;sent=[];closed=[]
        def transport(argv,folder,budget,accepted):
            return SimpleNamespace(process=SimpleNamespace(pid=123),budget=budget,accepted=accepted,
                                   returncode=0,stderr_tail='',stderr_truncated=False,broken=False)
        class Client:
            cleanup_error=None
            def __init__(self,t):self.t=t
            def call(self,method,params,timeout):
                sent.append(dict(method=method,params=params,timeout=timeout))
                self.t.accepted(dict(jsonrpc='2.0',id=len(sent),result={}))
                return {}
            def close(self):closed.append(self.t.budget.remaining(10))
        with patch.object(f,'RetainedTransport',transport),patch.object(f,'WorldClient',Client), \
             patch.object(f,'ARGS',SimpleNamespace(windows_interop=False)):
            owner.open(Path('/fake/native'),directory/'world.json','fake')
        return owner,sent,closed

    def test_census_expiry_does_not_record_or_send_and_cleanup_keeps_total_budget(self):
        with tempfile.TemporaryDirectory()as folder:
            clock={'now':80.0};record=dict(calls=[],owners=[])
            with patch('time.monotonic',lambda:clock['now']), \
                 patch.object(f,'DEADLINE',100.0),patch.object(f,'WORK_DEADLINE',90.0), \
                 patch.object(f,'RECORD',record):
                owner,sent,closed=self.owner(Path(folder),clock)
                def expensive_census():clock['now']=91.0
                with patch.object(f,'census',expensive_census):
                    with self.assertRaisesRegex(AssertionError,'cleanup budget reserved'):
                        owner.call('entity.query')
                self.assertEqual(sent,[])
                self.assertEqual(record['calls'],[])
                owner.close()
                self.assertEqual(closed,[9.0])
                self.assertEqual(record['owners'][0]['exit_code'],0)
                self.assertIsNone(record['owners'][0]['cleanup_error'])

    def test_native_timeout_is_recomputed_after_census(self):
        with tempfile.TemporaryDirectory()as folder:
            clock={'now':80.0};record=dict(calls=[],owners=[])
            with patch('time.monotonic',lambda:clock['now']), \
                 patch.object(f,'DEADLINE',100.0),patch.object(f,'WORK_DEADLINE',90.0), \
                 patch.object(f,'RECORD',record):
                owner,sent,closed=self.owner(Path(folder),clock)
                def expensive_census():clock['now']=85.0
                with patch.object(f,'census',expensive_census):
                    self.assertEqual(owner.call('entity.query'),{})
                self.assertEqual(sent,[dict(method='entity.query',params={},timeout=5.0)])
                self.assertEqual(record['calls'][0]['rpc_id'],1)
                clock['now']=95.0
                owner.close()
                self.assertEqual(closed,[5.0])

    def test_expired_work_prevents_new_owner_and_new_fixture_files(self):
        with tempfile.TemporaryDirectory()as folder:
            directory=Path(folder)
            with patch('time.monotonic',return_value=91.0),patch.object(f,'WORK_DEADLINE',90.0), \
                 patch.object(f,'RECORD',dict(calls=[],owners=[])),patch.object(f,'ARGS',SimpleNamespace(output=directory)):
                owner=f.ImportGuards('test_matching_and_legacy_package_identity')
                with self.assertRaisesRegex(AssertionError,'cleanup budget reserved'):
                    owner.setUp()
                self.assertEqual(list(directory.iterdir()),[])
                self.assertEqual(f.RECORD['owners'],[])
                owner.client=None;owner.directory=directory;owner.serial=0
                with patch.object(f,'RetainedTransport')as transport,patch.object(f,'WorldClient')as client:
                    with self.assertRaisesRegex(AssertionError,'cleanup budget reserved'):
                        owner.open(Path('/fake/native'),directory/'world.json','expired')
                    transport.assert_not_called()
                    client.assert_not_called()
                self.assertEqual(list(directory.iterdir()),[])
                self.assertEqual(f.RECORD['owners'],[])

if __name__=='__main__':unittest.main()
