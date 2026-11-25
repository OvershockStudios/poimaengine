#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify an off-axis doorway route as a kinematic door uncovers it."""

from argparse import ArgumentParser
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import wave

from retained_capture import Fixture, component, expect_error, file_hash, new_id, transform, tree_hashes, uid
from water_listener_audio_contract import FRAMES_PER_TICK, RATE, pcm

PORTAL, GATE, WALL, SOURCE, PANEL = map(uid, (901, 902, 903, 904, 905))
CLOSED, PARTIAL, OPEN = [2.15,2.6,0], [2.15,3.45,0], [2.15,4.6,0]
PROFILE = dict(half_extents=[.45,.6], gain=.9)
MATERIAL = dict(absorption=[.5,.5,.5], transmission=[0,0,0], scattering=.5, enabled=True)


def motion(position):
    return dict(ticks=12, motions=[dict(entity=GATE, position=position, rotation=[0,0,0,1], duration_ticks=12)])


SEQUENCE = [
    dict(ticks=4, sounds=[dict(op='play', emitter=SOURCE)]),
    motion(OPEN), dict(ticks=8), motion(CLOSED), dict(ticks=8),
]
TOTAL_TICKS = sum(segment['ticks'] for segment in SEQUENCE)


def energy(samples, first, last, channel=None):
    start, end = (2*tick*FRAMES_PER_TICK for tick in (first,last))
    region = samples[start:end] if channel is None else samples[start+channel:end:2]
    return sum(value*value for value in region)


class PortalFixture(Fixture):
    def edit(self, client, *ops):
        write = client.call('world.transact', dict(base_revision=self.revision, request_id=new_id(), ops=ops))
        assert write['committed']
        self.revision = write['revision']

    def author(self, client):
        self.revision = client.call('world.inspect')['revision']
        wav = self.project/'Doorway test tone.wav'
        samples = [round(5000*(math.sin(i*.045)+.35*math.sin(i*.39))) for i in range(1789)]
        with wave.open(str(wav), 'wb') as output:
            output.setparams((1, 2, RATE, 0, 'NONE', 'not compressed'))
            output.writeframes(struct.pack(f'<{len(samples)}h', *samples))
        asset = client.call('asset.audio.import', dict(source=self.native(wav)))['asset']
        wav.unlink()
        self.edit(client,
            dict(op='entity.create', id=PORTAL, name='Door aperture'),
            component(PORTAL,'Transform',transform(CLOSED)), component(PORTAL,'AudioPortal',PROFILE),
            dict(op='entity.create', id=SOURCE, name='Source behind the wall'),
            component(SOURCE,'Transform',transform([0,2.6,-2])),
            component(SOURCE,'AudioEmitter',dict(asset=asset,gain=.75,loop=True,enabled=True)))
        for entity, center, extents, kind in (
            (WALL,[-3.1,2.7,0],[4.6,4,.1],'static'), (GATE,CLOSED,[.6,.75,.08],'kinematic')):
            self.edit(client, dict(op='entity.create', id=entity, name=kind+' acoustic blocker'),
                component(entity,'Transform',transform(center)),
                component(entity,'BoxCollider',dict(half_extents=extents,motion=kind,mass=1,friction=.5,restitution=0)),
                component(entity,'AcousticMaterial',MATERIAL))
        self.edit(client, dict(op='entity.create', id=PANEL, parent=GATE, name='Door panel'),
            component(PANEL,'Transform',transform([0,0,0]) | dict(scale=[1.2,1.5,.16])),
            component(PANEL,'MeshRenderer',dict(primitive='box',visible=True,albedo=[.53,.22,.07])))
        assert client.call('world.dependencies')['needs_audio_portals']
        assert client.call('entity.query',dict(component='AudioPortal'))['entities'][0]['id'] == PORTAL
        for invalid in (dict(half_extents=[1]),dict(half_extents=[0,1]),dict(gain=True),dict(gain=2)):
            expect_error(-32602,'world.transact',client,dict(base_revision=self.revision,request_id=new_id(),
                ops=[component(PORTAL,'AudioPortal',PROFILE | invalid)]))
        assert client.call('world.inspect')['revision'] == self.revision

    def observe(self, client, live=None):
        if live:
            session,tick = live
            result = client.call('runtime.audio.inspect',dict(session_id=session,tick=tick,listener=self.camera))
        else:
            result = client.call('world.audio.inspect',dict(revision=self.revision,listener=self.camera))
        source, = result['sources']
        assert source['entity'] == SOURCE and source['direct_visibility'] == 0
        assert result['audio_portals'] == dict(count=1,budget_exceeded=False)
        return source

    def aperture(self, source, visible):
        path = source['portal_path']
        if not visible:
            assert path is None
            return
        assert path['entity'] == PORTAL and path['visible_samples'] == visible
        point = path['point_world']
        expected_distance = math.dist(point, source['source_position']) + math.dist(point,source['listener_position'])
        assert abs(path['distance_m']-expected_distance)<1e-9
        assert abs(path['distance_gain']-1/expected_distance)<1e-7
        assert path['propagation_delay_samples']==round(expected_distance/343*RATE)
        length = math.dist(point,source['listener_position'])
        expected_direction = [(axis-origin)/length for axis,origin in zip(point,source['listener_position'])]
        assert max(abs(a-b) for a,b in zip(path['listener_local_direction'],expected_direction))<1e-8
        for measured, frequency in zip(path['bands'], (400,2500,10000)):
            excess = expected_distance-source['distance_m']
            expected = PROFILE['gain']*math.sqrt(visible/9)/math.sqrt(1+4*frequency*excess/343)
            assert abs(measured-expected)<1e-7
        return path

    def static_controls(self, client):
        self.aperture(self.observe(client),0)
        for position, label, count in ((PARTIAL,'partial',6),(OPEN,'open',9)):
            self.edit(client,component(GATE,'Transform',transform(position)))
            self.data['paths'][label] = self.aperture(self.observe(client),count)
        self.edit(client,component(GATE,'Transform',transform(CLOSED)))
        self.aperture(self.observe(client),0)

    def replay(self, client, session, label):
        path = self.root/(label+'.wav')
        sequence = [segment | dict(inputs=[dict(entity=self.controller)]) for segment in SEQUENCE]
        params = dict(session_id=session,expected_tick=0,request_id=new_id(),listener=self.camera,
                      path=self.native(path),sequence=sequence)
        result = client.call('runtime.audio.replay',params)
        assert result['success'] and result['tick']==TOTAL_TICKS
        assert result['capture']['sha256']==file_hash(path)
        assert result['stream']['voices_started']==1 and result['stream']['portal_handoffs']>=1
        assert not result['stream']['portal_budget_exceeded']
        assert client.call('runtime.audio.replay',params) == result | dict(replayed=True)
        self.aperture(self.observe(client,(session,TOTAL_TICKS)),0)
        self.data['replays'][label] = result
        return pcm(path)

    def saved_portal(self, client, saves):
        client.call('save.configure',dict(request_id=new_id(),expected_generation=0,root=self.native(saves)))
        session = self.mount(client)
        result = client.call('runtime.step',dict(session_id=session,request_id=new_id(),expected_tick=0,**motion(OPEN)))
        assert result['current_tick']==12
        selected = self.aperture(self.observe(client,(session,12)),9)
        self.save(client,session,12,'door-open')
        result = client.call('runtime.step',dict(session_id=session,request_id=new_id(),expected_tick=12,**motion(CLOSED)))
        assert result['current_tick']==24
        self.aperture(self.observe(client,(session,24)),0)
        assert self.restore_portal(client,session,24) == selected
        return selected

    def restore_portal(self, client, session, tick):
        new = new_id()
        loaded = client.call('save.load',dict(request_id=new_id(),configuration_generation=1,
            slot='door-open',expected_generation=1,revision=self.revision,expected_session_id=session,
            expected_tick=tick,expected_gameplay_revision=0,expected_structure_revision=0,new_session_id=new))
        assert loaded['tick']==12 and loaded['session_id']==new
        return self.aperture(self.observe(client,(new,12)),9)

    def player(self, game, expected):
        binary = game.parent/json.loads(game.read_bytes())['engine']['executable']
        raw, replay = (self.root/name for name in ('Player output.raw','Aperture sequence.json'))
        replay.write_text(json.dumps(SEQUENCE)+'\n')
        settings = dict(SDL_AUDIO_DRIVER='disk',SDL_AUDIO_FORMAT='F32',SDL_AUDIO_CHANNELS='2',
                        SDL_AUDIO_FREQUENCY='48000',SDL_AUDIO_DISK_OUTPUT_FILE=self.native(raw))
        env = os.environ | settings
        if self.args.windows_interop:
            env['WSLENV'] = ':'.join(filter(None,[env.get('WSLENV'),*settings]))
        args = [str(binary),'game','run',self.native(game),'--replay',self.native(replay),
                '--gpu',str(self.args.gpu),'--width','320','--height','180']
        proc = subprocess.run(args,env=env,text=True,capture_output=True,timeout=90)
        self.data['player'] = dict(exit=proc.returncode,stdout=proc.stdout,stderr=proc.stderr)
        assert proc.returncode==0,proc
        report = json.loads(proc.stdout)['result']['play']
        output = report['audio']
        assert report['hardware'] and report['tick']==TOTAL_TICKS and report['nvrhi_errors']==0
        assert output['driver']=='disk' and output['stream_drained'] and output['voices_started']==1
        assert output['submitted_frames']==TOTAL_TICKS*FRAMES_PER_TICK
        assert output['portal_handoffs']==self.data['replays']['source']['stream']['portal_handoffs']
        captured = tuple(entry[0] for entry in struct.iter_unpack('<f',raw.read_bytes()))
        first = next(i for i,value in enumerate(expected) if value)
        offset = next(i for i,value in enumerate(captured) if value)-first
        assert offset>=0 and offset%2==0
        assert captured[offset:offset+len(expected)]==expected
        self.data['player_pcm'] = dict(frames=len(expected)//2,sha256=file_hash(raw),leading_silence=offset//2)

    def export(self, pcm_source, saved, saves):
        inspected = self.cli('project','inspect',self.native(self.manifest))
        assert inspected['needs_audio_portals']
        self.cli('project','configure',self.native(self.manifest),'--patch','{"audio":true}',
                 '--expected-manifest-sha256',inspected['manifest_sha256'])
        old_runtime = self.root/'Capability control'
        old_runtime.mkdir()
        descriptor = json.loads((self.args.runtime/'runtime.json').read_bytes())
        assert descriptor['features'].pop('audio_portals') is True
        (old_runtime/'runtime.json').write_text(json.dumps(descriptor)+'\n')
        refused, staged, relocated = (self.root/name for name in ('Refused','Staged export','Moved export'))
        proc = subprocess.run([str(self.args.binary),'project','build',self.native(self.manifest),
            '--runtime',self.native(old_runtime),'--output',self.native(refused)],text=True,capture_output=True,timeout=90)
        self.data['refused_export'] = dict(exit=proc.returncode,output=json.loads(proc.stdout))
        assert proc.returncode!=0 and not refused.exists() and 'Audio portals require' in proc.stdout
        self.cli('project','build',self.native(self.manifest),'--runtime',self.native(self.args.runtime),
                 '--output',self.native(staged))
        staged.rename(relocated)
        self.project.rename(self.root/'Hidden source')
        game = relocated/'game.json'
        assert self.cli('game','inspect',self.native(game))['needs_audio_portals']
        before = tree_hashes(relocated)
        with self.owner('relocated',game=game,saves=saves) as client:
            session = client.call('runtime.status')['session_id']
            assert self.replay(client,session,'relocated') == pcm_source
            assert self.restore_portal(client,session,TOTAL_TICKS) == saved
        if self.args.gpu is not None:
            self.player(game,pcm_source)
        assert tree_hashes(relocated)==before and not self.project.exists()
        self.data['relocated_files'] = len(before)

    def run(self):
        self.cli('project','create',self.native(self.project),'--name','Off-axis doorway')
        entry = json.loads(self.manifest.read_bytes())['entry']
        self.camera,self.controller = entry['camera'],entry['controller']
        saves = self.root/'Saves'
        saves.mkdir()
        with self.owner('door-probe') as client:
            self.author(client)
            self.static_controls(client)
            session = self.mount(client)
            samples = self.replay(client,session,'source')
            client.call('runtime.stop',dict(session_id=session))
            closed, opened, sealed = (energy(samples,*window) for window in ((0,4),(17,24),(38,44)))
            left,right = (energy(samples,17,24,ear) for ear in (0,1))
            assert opened>.001 and opened>1000*max(closed,sealed)
            assert right>1.5*left
            self.data['energy'] = dict(closed=closed,opened=opened,sealed=sealed,left=left,right=right)
            saved = self.saved_portal(client,saves)
            self.data['paths']['warm_restored'] = saved
        if self.args.runtime:
            self.export(samples,saved,saves)


if __name__=='__main__':
    parser = ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--runtime',type=Path)
    parser.add_argument('--gpu',type=int)
    parser.add_argument('--windows-interop',action='store_true')
    args = parser.parse_args()
    if not __debug__ or (args.gpu is not None and not args.runtime):
        parser.error('Assertions must be enabled. GPU playback requires an installed Windows runtime.')
    args.binary = args.binary.resolve()
    root = Path(tempfile.mkdtemp(prefix='audio-portal-',dir=args.output.resolve()))
    data = dict(binary_sha256=file_hash(args.binary),passed=False,cli=[],owners=[],paths={},replays={})
    try:
        PortalFixture(args,root,data).run()
        data['passed'] = True
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(data,indent=2,allow_nan=False)+'\n')
        print(path,flush=True)
