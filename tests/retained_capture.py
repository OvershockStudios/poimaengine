#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check renderer lifetime, frame identity, and export through a guarded runtime."""

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools' / 'python'))
from poima_client import RpcError, WorldClient, new_id

from world_particles_contract import component, transform
from world_terrain_contract import file_hash, success, tree_hashes, uid

CAMERA, RED, BLUE = map(uid, (100, 101, 102))
COUNT = 8


def expect_error(code, method, client, params):
    try:
        client.call(method, params)
    except RpcError as error:
        assert error.code == code, (method, params, error)
        return error.message
    raise AssertionError(('Unexpected success', method, params))


class Fixture:
    def __init__(self, args, root, evidence):
        self.args = args
        self.root = root
        self.data = evidence
        self.project = root / 'Capture Project'
        self.manifest = self.project / 'project.poimaproject'

    def native(self, path):
        if not self.args.windows_interop:
            return str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip()

    def cli(self, *arguments):
        cmd = [str(self.args.binary), *arguments]
        proc = subprocess.run(cmd, text=True, capture_output=True, timeout=90)
        self.data['cli'].append(dict(argv=cmd, exit=proc.returncode, stdout=proc.stdout, stderr=proc.stderr))
        assert proc.returncode == 0, proc
        return success(json.loads(proc.stdout))

    @contextmanager
    def owner(self, label, game=None, saves=None):
        options = dict(cwd=self.root, trace_directory=self.root/(label+'-wire'),
                       trace_limit_bytes=8*1024*1024)
        if game:
            binary = game.parent / json.loads(game.read_bytes())['engine']['executable']
            connection = WorldClient.open_game(binary, self.native(game), save_root=self.native(saves), **options)
        else:
            connection = WorldClient.open(self.args.binary, project=self.native(self.manifest), **options)
        record = dict(label=label, pid=connection.transport.process_id)
        self.data['owners'].append(record)
        try:
            with connection as client:
                yield client
                assert client.call('session.close')['closed']
        finally:
            record.update(exit=connection.transport.returncode, closed=connection.closed,
                          cleanup_error=str(connection.cleanup_error) if connection.cleanup_error else None)
        assert record['exit'] == 0 and record['closed'] and record['cleanup_error'] is None, record

    def author(self, client):
        revision = client.call('world.inspect')['revision']
        ops = [dict(op='entity.create', id=e, name=name)
               for e,name in ((CAMERA,'Recording camera'),(RED,'Near red panel'),(BLUE,'Far blue panel'))]
        ops.extend((component(CAMERA,'Transform',transform([0,0,5])),
                    component(CAMERA,'Camera',dict(vertical_fov=51,near=.1,far=48))))
        for e,offset,color in ((RED,[.3,-.18,0],[.85,.05,.035]),(BLUE,[-1,.28,-1],[.035,.07,.85])):
            ops.extend((component(e,'Transform',transform(offset)),
                        component(e,'MeshRenderer',dict(primitive='box',albedo=color,visible=True))))
        result = client.call('world.transact',dict(base_revision=revision,request_id=new_id(),ops=ops))
        assert result['committed']
        return result['revision']

    def mount(self, client):
        session = new_id()
        result = client.call('runtime.start',dict(session_id=session,revision=self.revision))
        assert result['started'] and result['tick']==0
        return session

    def step(self, client, session, tick, index):
        offset = [index*.125,.03*index,0.]
        result = client.call('runtime.step',dict(session_id=session,request_id=new_id(),expected_tick=tick,ticks=1,
            camera_poses=[dict(entity=CAMERA,world=dict(position=offset,rotation=[0,0,0,1]))]))
        assert result['current_tick']==tick+1
        return offset

    def parameters(self, session, tick, path, **options):
        result = dict(session_id=session,tick=tick,camera=CAMERA,path=self.native(path),
                      width=320,height=180,samples=1,lighting_path=self.args.lighting_path,
                      capture_frames=2)
        if self.args.gpu is not None:
            result['gpu'] = self.args.gpu
        return result | options

    def capture(self, client, session, tick, label, *, reuse=None, **options):
        target = self.root/(label+'.bmp')
        params = self.parameters(session,tick,target,**options)
        if reuse is not None:
            params['reuse_renderer'] = reuse
        start = time.perf_counter()
        report = client.call('runtime.capture',params,timeout=60)
        elapsed = time.perf_counter()-start
        assert report['hardware'] and report['capture_written'] and report['nvrhi_errors']==0
        assert report['tick']==tick and report['session_id']==session and report['camera']==CAMERA
        assert report['frames_presented']==params['capture_frames']
        assert (report['width'], report['height'], report['samples'])==(params['width'],params['height'],1)
        assert ('renderer_reused' in report) is (reuse is True)
        row = dict(image=file_hash(target),time_s=elapsed,report=report)
        self.data['captures'][label] = row
        return row

    def sequence(self, client, session, label, *, reuse):
        result = []
        for i in range(COUNT):
            camera = self.step(client,session,i,i)
            if i==1 and reuse:
                bad = self.root/'stale.bmp'
                params = self.parameters(session,1,bad,reuse_renderer=True)
                self.data['stale'] = expect_error(-32009,'runtime.capture',client,params)
                assert not bad.exists()
                occupied = self.root/'retained-0.bmp'
                original = file_hash(occupied)
                params = self.parameters(session,2,occupied,reuse_renderer=True)
                self.data['occupied'] = expect_error(-32602,'runtime.capture',client,params)
                assert file_hash(occupied)==original
            row = self.capture(client,session,i+1,f'{label}-{i}',reuse=reuse)
            assert row['report']['camera_world'][12:15]==[camera[0],camera[1],camera[2]+5]
            if reuse:
                assert row['report']['renderer_reused'] is (i>0)
                completed = row['report']['render_diagnostics']['completed_submissions']
                assert completed==2*(i+1)
            result.append(row)
        return result

    def release(self, client, session, released):
        result = client.call('runtime.capture.release',dict(session_id=session))
        assert result==dict(session_id=session,released=released),result

    def save(self, client, session, tick, slot):
        return client.call('save.write',dict(request_id=new_id(),configuration_generation=1,
            expected_generation=0,slot=slot,session_id=session,expected_tick=tick,
            expected_gameplay_revision=0,expected_structure_revision=0))

    def retention(self, client):
        session = self.mount(client)
        cold = self.sequence(client,session,'isolated',reuse=False)
        client.call('runtime.stop',dict(session_id=session))
        session = self.mount(client)
        retained = self.sequence(client,session,'retained',reuse=True)
        assert len({x['image'] for x in retained})==COUNT
        assert [x['image'] for x in cold]==[x['image'] for x in retained]
        self.data['identical_images'] = COUNT
        self.data['wall_time_s'] = dict(isolated_median=statistics.median(r['time_s'] for r in cold),
            retained_after_first_median=statistics.median(r['time_s'] for r in retained[1:]))
        saves = self.root/'Source saves'
        saves.mkdir()
        client.call('save.configure',dict(request_id=new_id(),expected_generation=0,root=self.native(saves)))
        before = self.save(client,session,COUNT,'before')
        assert self.capture(client,session,COUNT,'different-width',reuse=True,width=352)['report']['renderer_reused'] is False
        assert self.capture(client,session,COUNT,'same-width',reuse=True,width=352)['report']['renderer_reused'] is True
        self.release(client,session,True)
        self.release(client,session,False)
        row = self.capture(client,session,COUNT,'reopened',reuse=True)
        assert not row['report']['renderer_reused'] and row['image']==retained[-1]['image']
        self.capture(client,session,COUNT,'one-shot-after-retained')
        self.release(client,session,False)
        after = self.save(client,session,COUNT,'after')
        assert before['sha256']==after['sha256'] and before['bytes']==after['bytes']
        self.data['capture_preserved_save'] = before['sha256']
        current_tick = COUNT
        if self.args.fsr:
            for i in range(3):
                self.step(client,session,COUNT+i,COUNT+i)
                frame = self.capture(client,session,COUNT+i+1,f'temporal-{i}',reuse=True,reconstruction='fsr3_native')
                report = frame['report']
                reconstruction = report['render_diagnostics']['reconstruction']
                assert report['renderer_reused'] is (i>0)
                assert reconstruction['active'] and reconstruction['history_sequence']==(i+1)*2
            current_tick += 3
        else:
            self.capture(client,session,COUNT,'before-restore',reuse=True)
        replacement = new_id()
        restored = client.call('save.load',dict(request_id=new_id(),configuration_generation=1,slot='before',
            expected_generation=1,revision=self.revision,expected_session_id=session,expected_tick=current_tick,
            expected_gameplay_revision=0,expected_structure_revision=0,new_session_id=replacement))
        assert restored['session_id']==replacement and restored['tick']==COUNT and not restored['recovered']
        self.release(client,replacement,False)
        restored_picture = self.capture(client,replacement,COUNT,'restored',reuse=True)
        assert not restored_picture['report']['renderer_reused'] and restored_picture['image']==retained[-1]['image']
        client.call('runtime.stop',dict(session_id=replacement))

    def standalone(self):
        staged, relocated, hidden = (self.root/name for name in ('Staged export','Relocated export','Hidden source'))
        self.cli('project','build',self.native(self.manifest),'--output',self.native(staged),
                 '--runtime',self.native(self.args.runtime_root))
        staged.rename(relocated)
        self.project.rename(hidden)
        manifest = relocated/'game.json'
        saves = self.root/'External saves'
        saves.mkdir()
        try:
            initial = tree_hashes(relocated)
            with self.owner('relocated',game=manifest,saves=saves) as client:
                status = client.call('runtime.status')
                assert status['active'] and status['tick']==0
                session = status['session_id']
                target = relocated/'would-write-bundle.bmp'
                self.data['bundle_output'] = expect_error(-32602,'runtime.capture',client,
                    self.parameters(session,0,target,reuse_renderer=True))
                assert not target.exists()
                for i in range(2):
                    self.step(client,session,i,i)
                    row = self.capture(client,session,i+1,f'export-{i}',reuse=True)
                    assert row['report']['renderer_reused'] is (i>0)
                    assert row['image']==self.data['captures'][f'isolated-{i}']['image']
                self.release(client,session,True)
            assert tree_hashes(relocated)==initial and not self.project.exists()
            self.data['export_files'] = len(initial)
        finally:
            hidden.rename(self.project)

    def run(self):
        self.cli('project','create',self.native(self.project),'--name','Retained Capture Fixture')
        with self.owner('authoring-and-runtime') as client:
            schema = client.call('world.describe',dict(view='method',name='runtime.capture'))
            assert schema['schema_revision']>=224
            assert schema['methods']['runtime.capture']['properties']['reuse_renderer']['default'] is False
            self.revision = self.author(client)
            active = self.mount(client)
            invalid = self.root/'invalid.bmp'
            params = self.parameters(active,0,invalid,reuse_renderer=1)
            self.data['invalid_boolean'] = expect_error(-32602,'runtime.capture',client,params)
            assert not invalid.exists()
            self.release(client,active,False)
            if self.args.gpu is None:
                params['reuse_renderer'] = True
                self.data['without_renderer'] = expect_error(-32003,'runtime.capture',client,params)
                assert not invalid.exists()
                self.release(client,active,False)
            client.call('runtime.stop',dict(session_id=active))
            if self.args.gpu is not None:
                self.retention(client)
        if self.args.runtime_root:
            self.standalone()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    parser.add_argument('--runtime-root',type=Path)
    parser.add_argument('--fsr',action='store_true')
    args = parser.parse_args()
    if not __debug__ or (args.gpu is None and (args.fsr or args.runtime_root)):
        parser.error('Run with assertions enabled. FSR and export require --gpu.')
    args.binary = args.binary.resolve()
    args.output.mkdir(exist_ok=True,parents=True)
    root = Path(tempfile.mkdtemp(prefix='retained-capture-',dir=args.output.resolve()))
    result = dict(passed=False,cli=[],owners=[],captures={},runtime_sha256=file_hash(args.binary),
                  lighting_path=args.lighting_path,gpu=args.gpu)
    try:
        Fixture(args,root,result).run()
        result['passed'] = True
    finally:
        receipt = root/'evidence.json'
        receipt.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        print(receipt)


if __name__=='__main__':
    main()
