#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify camera focus tracking through authoring, ticks, saves and Vulkan."""

import argparse
import json
import math
from pathlib import Path
import tempfile

from depth_of_field_capture import FocusFixture, LENS, PROFILE, SIZE, samples
from retained_capture import CAMERA, component, expect_error, file_hash, new_id, transform, tree_hashes, uid

SUPPORT, TARGET, RECIPE = map(uid, (650, 651, 652))
YAW = [0, math.sqrt(.5), 0, math.sqrt(.5)]
FALLBACK = 3.25
MOVE_TICKS = 12
FOCUS = dict(entity=TARGET, local_offset_m=[-.5, .05, .05])
CAMERA_OPTIONS = LENS | dict(viewmodel=dict(vertical_fov=65, near=.015, far=10))
PROBES = samples(1)[26:36] + samples(2)[26:36]


class TrackingFixture(FocusFixture):
    def transact(self, client, revision, operations):
        return client.call('world.transact', dict(request_id=new_id(), base_revision=revision, ops=operations))

    def author_tracking(self, client):
        revision = self.author(client)
        revision = self.select(client, revision, PROFILE | dict(focus_distance_m=FALLBACK))
        revision = self.transact(client, revision, [
            dict(op='entity.create', id=SUPPORT, name='Moving focus rig'),
            component(SUPPORT, 'Transform', transform([.15, .2, -4.5], YAW)),
            component(SUPPORT, 'BoxCollider', dict(motion='kinematic', half_extents=[.05]*3,
                                                   mass=1, friction=.5, restitution=0)),
            dict(op='entity.create', id=TARGET, name='Focus mark', parent=SUPPORT),
            component(TARGET, 'Transform', transform([0, 0, 0])),
        ])['revision']
        for invalid in (dict(entity=CAMERA), dict(entity=uid(999)), dict(entity=uid(0)),
                        FOCUS | dict(local_offset_m=[0, 0, True]), FOCUS | dict(local_offset_m=[1001, 0, 0])):
            params = dict(request_id=new_id(), base_revision=revision,
                          ops=[component(CAMERA, 'Camera', CAMERA_OPTIONS | dict(focus_target=invalid))])
            expect_error(-32602, 'world.transact', client, params)
            assert client.call('world.inspect')['revision'] == revision
        recipe = dict(op='template.set', id=RECIPE, name='Camera and mark', root=CAMERA,
            entities={
                CAMERA: dict(name='Rig camera', parent=None, components=dict(
                    Transform=transform([0, 0, 0]), Camera=LENS | dict(focus_target=dict(entity=TARGET)))),
                TARGET: dict(name='Rig mark', parent=CAMERA, components=dict(Transform=transform([0, 0, -6]))),
            })
        return self.transact(client, revision, [recipe, component(CAMERA, 'Camera', CAMERA_OPTIONS | dict(focus_target=FOCUS))])['revision']

    def focus(self, client, session, tick, distance, world_z, *, camera=CAMERA, target=TARGET, in_range=True):
        result = client.call('runtime.camera_focus', dict(session_id=session, tick=tick, entity=camera))
        assert result['tick'] == tick and result['session_id'] == session
        state = result['state']
        assert state['entity'] == camera and abs(state['distance_m']-distance) < .00001, state
        tracking = state['tracking']
        assert tracking['target'] == target and tracking['in_range'] is in_range
        assert math.isclose(tracking['world_position_m'][2], world_z, abs_tol=.00001)
        self.data['observations'].append(result)
        return tracking

    def move(self, client, session, tick, z):
        result = client.call('runtime.step', dict(session_id=session, request_id=new_id(), expected_tick=tick,
            ticks=MOVE_TICKS, motions=[dict(entity=SUPPORT, position=[.15, .2, z], rotation=YAW, duration_ticks=MOVE_TICKS)]))
        assert result['current_tick'] == tick+MOVE_TICKS

    def check_frame(self, client, session, tick, label, expected, group, **render):
        frame = self.capture(client, session, tick, label, width=SIZE[0], height=SIZE[1],
                             scene_product_probes=PROBES, **render)
        focus = frame['report']['render_diagnostics']['depth_of_field']
        assert focus['active'] and focus['tracking'] == expected, focus
        numerical_focus = frame['report']['render_diagnostics']['scene_products']['probes']
        if not render.get('reconstruction'):
            assert len(numerical_focus) == len(PROBES)
            for pixel in numerical_focus[10*(group-1):10*group]:
                delta = math.dist(pixel['raw_hdr'], pixel['resolved_hdr'])
                if expected['in_range']:
                    assert delta == 0, (label, pixel)
        return frame

    def exercise(self, client, session, label):
        existing = self.focus(client, session, 0, 4, -4)
        assert math.dist(existing['world_position_m'], [.2, .25, -4]) < .00001
        other = client.call('runtime.camera_focus', dict(session_id=session, tick=0, entity=uid(3)))['state']
        assert other['distance_m'] == FALLBACK and other['tracking'] is None
        assert client.call('runtime.camera_focus', dict(session_id=session, tick=0, entity=TARGET))['state'] is None
        if self.args.gpu is not None:
            start = self.check_frame(client, session, 0, label+'-near', existing, 1, reuse=True)
            assert not start['report']['renderer_reused']
            self.data[label+'-near'] = start['image']
        self.move(client, session, 0, -8.7)
        middle = self.focus(client, session, MOVE_TICKS, 8.2, -8.2)
        self.data['stale_tick'] = expect_error(-32009, 'runtime.camera_focus', client,
            dict(session_id=session, tick=0, entity=CAMERA))
        saved = self.save(client, session, MOVE_TICKS, 'focus')
        if self.args.gpu is not None:
            changed = self.check_frame(client, session, MOVE_TICKS, label+'-far', middle, 2, reuse=True)
            assert changed['report']['renderer_reused'] and changed['image'] != start['image']
            self.data[label+'-far'] = changed['image']
            if self.args.fsr:
                fsr = self.check_frame(client, session, MOVE_TICKS, label+'-fsr', middle, 2, reconstruction='fsr3_native')
                assert fsr['report']['render_diagnostics']['reconstruction']['active']
        self.move(client, session, MOVE_TICKS, .5)
        fallback = self.focus(client, session, MOVE_TICKS*2, FALLBACK, 1, in_range=False)
        if self.args.gpu is not None:
            frame = self.check_frame(client, session, MOVE_TICKS*2, label+'-fallback', fallback, 1)
            self.data[label+'-fallback'] = frame['image']
        replacement = new_id()
        restored = client.call('save.load', dict(request_id=new_id(), configuration_generation=1, slot='focus',
            expected_generation=1, revision=self.revision, expected_session_id=session, expected_tick=MOVE_TICKS*2,
            expected_gameplay_revision=0, expected_structure_revision=0, new_session_id=replacement))
        assert restored['session_id'] == replacement and restored['tick'] == MOVE_TICKS
        assert self.focus(client, replacement, MOVE_TICKS, 8.2, -8.2) == middle
        if self.args.gpu is not None:
            restored_frame = self.check_frame(client, replacement, MOVE_TICKS, label+'-restored', middle, 2, reuse=True)
            assert not restored_frame['report']['renderer_reused'] and restored_frame['image'] == changed['image']
        self.data[label+'-save_sha256'] = saved['sha256']
        return replacement

    def check_instance(self, client, session):
        result = client.call('runtime.structure.transact', dict(session_id=session, request_id=new_id(),
            expected_tick=MOVE_TICKS, expected_structure_revision=0, spawns=[dict(template_id=RECIPE)]))
        root = result['spawned'][0]
        instance = client.call('runtime.instance', dict(session_id=session, id=root, tick=MOVE_TICKS))
        assert instance['nodes'][CAMERA] == root and root != CAMERA and instance['nodes'][TARGET] != TARGET
        self.focus(client, session, MOVE_TICKS, 6, -6, camera=root, target=instance['nodes'][TARGET])
        self.data['spawned_camera_focus_remapped'] = True

    def run(self):
        self.cli('project', 'create', self.native(self.project), '--name', 'Camera Focus Fixture')
        saves = self.root/'Source saves'
        saves.mkdir()
        with self.owner('source') as client:
            self.revision = self.author_tracking(client)
            assert client.call('world.dependencies')['needs_depth_of_field']
            if self.args.gpu is not None:
                path = self.root/'Authored focus.bmp'
                report = client.call('world.capture', dict(revision=self.revision, camera=CAMERA, path=self.native(path),
                    width=SIZE[0], height=SIZE[1], samples=1, gpu=self.args.gpu, lighting_path=self.args.lighting_path,
                    capture_frames=2), timeout=60)
                assert abs(report['render_diagnostics']['depth_of_field']['tracking']['distance_m']-4) < .00001
                self.data['authored_capture_sha256'] = file_hash(path)
            client.call('save.configure', dict(request_id=new_id(), expected_generation=0, root=self.native(saves)))
            active = self.exercise(client, self.mount(client), 'source')
            self.check_instance(client, active)
            if self.args.gpu is not None:
                assert self.data['authored_capture_sha256'] == self.data['source-near']
        if self.args.runtime_root:
            self.check_relocated()

    def check_relocated(self):
        staged, relocated, hidden = (self.root/name for name in ('Build', 'Relocated', 'Hidden source'))
        self.cli('project', 'build', self.native(self.manifest), '--output', self.native(staged),
                 '--runtime', self.native(self.args.runtime_root))
        staged.rename(relocated)
        self.project.rename(hidden)
        game = relocated/'game.json'
        binary = relocated/json.loads(game.read_bytes())['engine']['executable']
        if self.args.windows_interop:
            binary.chmod(binary.stat().st_mode | 0o100)
        saved = self.root/'Export saves'
        saved.mkdir()
        before = tree_hashes(relocated)
        with self.owner('relocated', game=game, saves=saved) as client:
            session = client.call('runtime.status')['session_id']
            self.exercise(client, session, 'export')
        with self.owner('fresh-save', game=game, saves=saved) as client:
            status = client.call('runtime.status')
            assert status['active'] and status['tick'] == 0
            session = new_id()
            restored = client.call('save.load', dict(request_id=new_id(), configuration_generation=1, slot='focus',
                expected_generation=1, revision=self.revision, expected_session_id=status['session_id'], expected_tick=0,
                expected_gameplay_revision=0, expected_structure_revision=0, new_session_id=session))
            assert restored['session_id'] == session and restored['tick'] == MOVE_TICKS
            focus = self.focus(client, session, MOVE_TICKS, 8.2, -8.2)
            frame = self.check_frame(client, session, MOVE_TICKS, 'fresh-save', focus, 2)
            assert frame['image'] == self.data['source-far']
        assert tree_hashes(relocated) == before and not self.project.exists()
        assert all(self.data['source-'+name] == self.data['export-'+name] for name in ('near', 'far', 'fallback'))
        self.data['immutable_export_files'] = len(before)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--runtime-root', type=Path)
    parser.add_argument('--fsr', action='store_true')
    args = parser.parse_args()
    if not __debug__ or (args.gpu is None and (args.fsr or args.runtime_root)):
        parser.error('Run with assertions enabled. Rendering options require a selected GPU.')
    args.binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(dir=args.output, prefix='camera-focus-'))
    evidence = dict(passed=False, binary_sha256=file_hash(args.binary), captures={}, cli=[], owners=[], observations=[])
    try:
        TrackingFixture(args, root, evidence).run()
        evidence['passed'] = True
    finally:
        receipt = root/'evidence.json'
        receipt.write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n')
        print(receipt, flush=True)


if __name__ == '__main__':
    main()
