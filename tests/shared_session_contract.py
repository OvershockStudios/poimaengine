#!/usr/bin/env python3
"""Cross-process authoring against one compiled same-user world host."""
# SPDX-License-Identifier: Apache-2.0
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

BINARY=str(Path(sys.argv[1]).resolve());sys.argv=[sys.argv[0]]
def uid():return uuid.uuid4().hex
def request(method,params=None,id=1):return {'jsonrpc':'2.0','id':id,'method':method,'params':params or {}}

class SharedWorld(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory();self.world=Path(self.directory.name)/'world.json';self.endpoint='test-'+uid()
        self.host=subprocess.Popen([BINARY,'serve',str(self.world),'--endpoint',self.endpoint],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        deadline=time.monotonic()+15
        while time.monotonic()<deadline:
            probe=self.client([request('world.inspect')],check=False,timeout=100)
            if probe.returncode==0:break
            if self.host.poll() is not None:self.fail(self.host.communicate()[1])
            time.sleep(.02)
        else:self.host.kill();self.host.wait();self.fail('Shared host did not become ready.')
    def tearDown(self):
        if self.host.poll() is None:
            try:self.client([request('host.shutdown')],check=False)
            except Exception:pass
            try:self.host.wait(timeout=5)
            except subprocess.TimeoutExpired:self.host.kill();self.host.wait(timeout=5)
        self.host.communicate();self.directory.cleanup()
    def client(self,requests,check=True,timeout=3000):
        lines=''.join((r if isinstance(r,str) else json.dumps(r))+'\n' for r in requests)
        p=subprocess.run([BINARY,'connect',self.endpoint,'--timeout-ms',str(timeout)],input=lines,text=True,capture_output=True,timeout=10)
        if not check:return p
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        return [json.loads(line) for line in p.stdout.splitlines()]
    def result(self,method,params=None):
        row=self.client([request(method,params)])[0];self.assertIn('result',row,row);return row['result']
    def create(self,id=None,revision=0,request_id=None):
        return {'base_revision':revision,'request_id':request_id or uid(),'ops':[{'op':'entity.create','id':id or uid(),'name':'Shared entity'}]}
    def test_discovery_notifications_and_detach_do_not_close_owner(self):
        schema=self.result('world.describe');self.assertEqual(schema['session_scope'],'shared_headless')
        self.assertIn('host.shutdown',schema['methods']);self.assertNotIn('session.close',schema['methods'])
        self.assertFalse(self.result('runtime.status')['active'])
        rows=self.client([{'jsonrpc':'2.0','method':'world.inspect'},'{','',request('session.close'),request('world.inspect')])
        self.assertEqual(len(rows),4);self.assertEqual(rows[0]['error']['code'],-32700)
        self.assertEqual(rows[1]['error']['code'],-32700);self.assertEqual(rows[2]['error']['code'],-32080)
        self.assertEqual(rows[3]['result']['revision'],0);self.assertIsNone(self.host.poll());self.assertFalse(self.world.exists())
    def test_separate_clients_share_history_and_durable_receipts(self):
        entity=uid();params=self.create(entity)
        self.assertEqual(self.result('world.transact',params)['revision'],1)
        self.assertTrue(self.result('world.transact',params)['replayed'])
        self.assertEqual(self.result('world.history')['undo_count'],1)
        undo={'base_revision':1,'request_id':uid()};self.assertEqual(self.result('world.undo',undo)['revision'],2)
        self.assertTrue(self.result('world.undo',undo)['replayed'])
        self.assertEqual(self.result('world.inspect')['entity_count'],0)
        self.result('world.redo',{'base_revision':2,'request_id':uid()})
        self.assertEqual(self.result('entity.get',{'id':entity})['value']['name'],'Shared entity')
        self.assertEqual(json.loads(self.world.read_text())['revision'],3)
    def test_concurrent_revision_conflict_is_atomic(self):
        transactions=[self.create(),self.create()]
        with ThreadPoolExecutor(max_workers=2) as workers:
            responses=list(workers.map(lambda p:self.client([request('world.transact',p)])[0],transactions))
        self.assertEqual(sum('result' in r for r in responses),1,responses)
        self.assertEqual(next(r['error']['code'] for r in responses if 'error' in r),-32009)
        self.assertEqual(self.result('world.inspect')['entity_count'],1)
        self.assertEqual(self.result('world.history')['undo_count'],1)
    def test_preview_and_failed_validation_leave_other_client_state(self):
        preview={**self.create(),'preview':True};self.assertFalse(self.result('world.transact',preview)['committed'])
        invalid=self.create();invalid['ops'][0]['name']=''
        self.assertIn('error',self.client([request('world.transact',invalid)])[0])
        self.assertEqual(self.result('world.inspect')['revision'],0);self.assertEqual(self.result('world.history')['undo_count'],0)
    def test_exclusive_owner_and_endpoint_do_not_rewrite_world(self):
        self.result('world.transact',self.create());before=self.world.read_bytes()
        duplicate=subprocess.run([BINARY,'serve',str(self.world),'--endpoint',self.endpoint],capture_output=True,text=True,timeout=5)
        self.assertNotEqual(duplicate.returncode,0)
        direct=subprocess.run([BINARY,'world',str(self.world)],input='',capture_output=True,text=True,timeout=5)
        self.assertNotEqual(direct.returncode,0);self.assertEqual(before,self.world.read_bytes())
        self.assertEqual(self.result('world.inspect')['revision'],1)
    def test_oversized_line_and_duplicate_fields_do_not_poison_bridge(self):
        rows=self.client([' '*1048577,'{"jsonrpc":"2.0","id":1,"method":"world.inspect","method":"host.shutdown"}',request('world.inspect')])
        self.assertEqual([r['error']['code'] for r in rows[:2]],[-32700,-32700])
        self.assertEqual(rows[2]['result']['revision'],0);self.assertIsNone(self.host.poll())
    def test_clean_shutdown_releases_endpoint_and_world_for_restart(self):
        params=self.create();self.result('world.transact',params)
        self.result('host.shutdown');self.assertEqual(self.host.wait(timeout=5),0)
        self.host.communicate()
        self.host=subprocess.Popen([BINARY,'serve',str(self.world),'--endpoint',self.endpoint],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        result=self.result('world.transact',params);self.assertTrue(result['replayed'])
        self.assertEqual(self.result('world.history')['undo_count'],0)

    @unittest.skipUnless(sys.platform.startswith('linux'), 'Linux pathname socket publication contract')
    def test_socket_interim_permissions_wait_without_connecting(self):
        endpoint='test-'+uid()
        path=Path('/tmp')/('poima-'+str(os.geteuid()))/(endpoint+'.sock')
        listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        try:
            listener.bind(str(path));path.chmod(0o700);listener.listen(1)
            listener.settimeout(.15)
            command=[BINARY,'connect',endpoint,'--timeout-ms','3000']
            with ThreadPoolExecutor(max_workers=1) as workers:
                pending=workers.submit(subprocess.run,command,input=json.dumps(request('world.inspect'))+'\n',
                                       text=True,capture_output=True,timeout=5)
                with self.assertRaises(socket.timeout):listener.accept()
                self.assertFalse(pending.done(), 'Client rejected the temporary mode instead of waiting for publication.')
                path.chmod(0o600);listener.settimeout(3)
                connection,_=listener.accept()
                with connection:
                    connection.settimeout(3)
                    def receive(size):
                        data=b''
                        while len(data)<size:
                            part=connection.recv(size-len(data))
                            self.assertTrue(part, 'Client disconnected before complete frame.')
                            data+=part
                        return data
                    size=struct.unpack('<I',receive(4))[0]
                    self.assertLessEqual(size,1048576)
                    row=json.loads(receive(size))
                    reply=json.dumps({'jsonrpc':'2.0','id':row['id'],'result':{'ready':True}}).encode()
                    connection.sendall(struct.pack('<I',len(reply))+reply)
                result=pending.result()
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertTrue(json.loads(result.stdout)['result']['ready'])
            # A mode that never becomes safe remains unavailable; retrying it
            # must not weaken the final mode check or the connection deadline.
            path.chmod(0o700);listener.settimeout(.05)
            result=subprocess.run([BINARY,'connect',endpoint,'--timeout-ms','100'],input='{}\n',
                                  text=True,capture_output=True,timeout=3)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('timed out',result.stderr)
            with self.assertRaises(socket.timeout):listener.accept()
        finally:
            listener.close()
            if path.exists():path.unlink()

if __name__=='__main__':unittest.main(verbosity=2)
