#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare indirect rigid draws with native direct captures and a relocated replay."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import traceback
import uuid

from rigid_mesh_lod_capture import (
    BASE_INDICES, BASE_POINTS, Fixture, WorldClient, component, digest, inventory, transform, uid,
)
from solar_lighting_contract import VALIDATION_FAILURES, VALIDATION_LAYER


PLAYER = uid(1)
CAMERAS = {label: uid(index) for index, label in enumerate(('coast', 'inland', 'empty'), 10)}
CAPTURE_SIZE = (320, 180)
GROUPS = 4
INSTANCES = 128
SEQUENCE = [dict(ticks=5), dict(ticks=3, move=[.25, 0], look=[1, 0])]


class VisibilityFixture(Fixture):
    def author(self, client):
        broad = self.imported(client, 'Leaf', BASE_POINTS, BASE_INDICES, 0)
        narrow = self.imported(client, 'Reed', BASE_POINTS[:4], [0, 1, 2, 0, 2, 3], 1)
        ops = [dict(op='entity.create', id=PLAYER, name='Listener'),
               component(PLAYER, 'Transform', transform()),
               component(PLAYER, 'CharacterController', dict(radius=.3, height=1.8, speed=4,
                                                              jump_speed=5, camera=CAMERAS['coast']))]
        for (label, camera), x in zip(CAMERAS.items(), (0, 40, 160), strict=True):
            ops.extend((dict(op='entity.create', id=camera, name=label, parent=PLAYER if label=='coast' else None),
                        component(camera, 'Transform', transform((x, 0, 0))),
                        component(camera, 'Camera', dict(vertical_fov=60, near=.1, far=100))))
        box = uid(20)
        ops.extend((dict(op='entity.create', id=box, name='Direct control'),
                    component(box, 'Transform', transform((0, -3.5, -12))),
                    component(box, 'MeshRenderer', dict(primitive='box', albedo=[.2, .3, .4], visible=True,
                                                        cast_shadows=False, receive_shadows=False))))
        self.revision = self.transaction(client, 0, ops)['revision']

        for group in range(GROUPS):
            asset = (broad, narrow)[group % 2]
            for start in (0, 64):
                operations = []
                for index in range(start, start + 64):
                    entity = uid(100 + group * INSTANCES + index)
                    cell = index // 2
                    x = 40 * (index % 2) + (.64 * (cell % 8 - 3.5)) + (group % 2 - .5) * 5
                    y = .64 * (cell // 8 - 3.5) + (group // 2 - .5) * 5
                    mesh = dict(asset=asset, primitive=0, visible=True, cast_shadows=False)
                    if group % 2:
                        mesh['foliage_motion'] = dict(root_y=-1, tip_y=1,
                            wind=dict(direction=[1, .25], tip_m=.05, period_ticks=60, wavelength_m=8))
                    operations.extend((dict(op='entity.create', id=entity, name=f'Plant {group} {index}'),
                        component(entity, 'Transform', transform((x, y, -14)) | dict(scale=[.18, .24, .18])),
                        component(entity, 'StaticMesh', mesh)))
                    if index % 11 == 0:
                        operations.append(component(entity, 'PbrMaterial', dict(base_color=[.3, .5, .1],
                            metallic=0, roughness=.8, emissive=[.05, .2, .8], double_sided=False)))
                self.revision = self.transaction(client, self.revision, operations)['revision']
        schema = self.call(client, 'world.describe', view='method', name='world.capture')
        option = schema['methods']['world.capture']['properties']['gpu_visibility']
        assert option['type'] == 'boolean' and option['default'] is False, option
        self.call(client, 'world.capture', error=-32602, revision=self.revision, camera=CAMERAS['coast'],
                  path=self.native(self.root / 'invalid.bmp'), gpu_visibility='true')
        assert not (self.root / 'invalid.bmp').exists()

    def capture(self, client, label, camera, **flags):
        path = self.root / (label + '.bmp')
        result = self.call(client, 'world.capture', revision=self.revision, camera=CAMERAS[camera],
                           path=self.native(path), width=CAPTURE_SIZE[0], height=CAPTURE_SIZE[1],
                           samples=self.args.samples, gpu=self.args.gpu, lighting_path=self.args.lighting_path,
                           reconstruction=self.args.reconstruction, **flags)
        assert result['capture_written'] and result['hardware'] and result['nvrhi_errors'] == 0, result
        self.evidence['images'][label] = digest(path)
        return result

    def compare(self, direct, indirect, visible):
        assert direct['hardware'] and indirect['hardware']
        direct = direct['render_diagnostics']
        indirect = indirect['render_diagnostics']
        counts = indirect['gpu_visibility']
        assert counts['supported'] and counts['active'] and counts['reason']=='active', counts
        assert (counts['candidates'], counts['visible']) == (GROUPS*INSTANCES, visible), counts
        assert counts['indirect_batches'] < 32 and counts['slot_bytes'] > 0, counts
        a, b = direct['last_draws'], indirect['last_draws']
        for field in ('camera_draws', 'camera_culled', 'camera_triangles', 'shadow_draws', 'shadow_triangles'):
            assert a[field] == b[field], (field, a, b)
        if visible:
            assert b['camera_batches'] < a['camera_batches'], (a, b)
        assert 0 < counts['indirect_batches'] <= b['camera_batches']
        return dict(direct_commands=a['camera_batches'], indirect_commands=b['camera_batches'],
                    visible=b['camera_draws'], culled=b['camera_culled'], gpu_visibility=counts)

    def compare_cameras(self, client):
        outputs = self.evidence['images']
        for name in CAMERAS:
            cpu = self.capture(client, name+'-cpu', name)
            gpu = self.capture(client, name+'-gpu', name, gpu_visibility=True)
            assert outputs[name+'-cpu'] == outputs[name+'-gpu'], name
            self.evidence['comparisons'][name] = self.compare(cpu, gpu, 0 if name=='empty' else GROUPS*INSTANCES//2)
        assert outputs['coast-gpu'] != outputs['inland-gpu'] != outputs['empty-gpu']
        for label, options in (('culling_disabled', dict(culling=False)), ('instancing_disabled', dict(draw_instancing=False))):
            cpu = self.capture(client, label+'-cpu', 'coast', **options)
            gpu = self.capture(client, label+'-gpu', 'coast', gpu_visibility=True, **options)
            assert outputs[label+'-cpu'] == outputs[label+'-gpu']
            assert gpu['render_diagnostics']['gpu_visibility']['reason']==label, gpu
            assert cpu['render_diagnostics']['last_draws']==gpu['render_diagnostics']['last_draws']

    def run_export(self):
        self.manifest.write_text(json.dumps(dict(format='poima.project', version=1, project_id=uid(1234),
            name='GPU visibility', entry=dict(world=self.world.name, controller=PLAYER, camera=CAMERAS['coast']),
            audio=False)), encoding='utf-8')
        bundle = self.root / 'Exported game'
        build = [str(self.args.binary.resolve()), 'project', 'build', self.native(self.manifest),
                 '--output', self.native(bundle), '--runtime', self.native(self.args.runtime_root)]
        self.cli(build)
        relocated = self.root / 'Relocated game'
        bundle.rename(relocated)
        self.project.rename(self.root / 'Detached project')
        before = inventory(relocated)
        replay = self.root / 'replay.json'
        replay.write_text(json.dumps(SEQUENCE), encoding='utf-8')
        exe = relocated / 'runtime' / 'bin' / 'poima.exe'
        reports, entities = [], []
        for mode in ('cpu', 'gpu'):
            output = self.root / ('relocated-' + mode + '.bmp')
            receipt = self.cli([str(exe), 'game', 'run', self.native(relocated/'game.json'),
                '--replay', self.native(replay), '--capture', self.native(output), '--gpu', str(self.args.gpu),
                '--samples', str(self.args.samples), '--lighting-path', self.args.lighting_path,
                '--width', str(CAPTURE_SIZE[0]), '--height', str(CAPTURE_SIZE[1]),
                '--reconstruction', self.args.reconstruction,
                '--gpu-visibility', 'true' if mode=='gpu' else 'false'])
            report = receipt['result']['play']
            assert report['success'] and report['tick']==8 and report['capture_written'], report
            assert report['frames_presented']==report['render_diagnostics']['completed_submissions']==9, report
            reports.append(report)
            entities.append({entity: {key: value for key, value in state.items() if key!='session_id'}
                             for entity, state in receipt['result']['entities'].items()})
            self.evidence['images']['relocated-'+mode] = digest(output)
        assert self.evidence['images']['relocated-cpu']==self.evidence['images']['relocated-gpu']
        assert entities[0]==entities[1]
        self.evidence['relocated_entities'] = entities[0]
        self.evidence['comparisons']['relocated'] = self.compare(*reports, GROUPS*INSTANCES//2)
        assert inventory(relocated)==before

    def cli(self, command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=120, encoding='utf-8')
        self.evidence.setdefault('processes', []).append(dict(argv=command, exit_code=result.returncode,
                                                              stdout=result.stdout, stderr=result.stderr))
        assert result.returncode==0 and all(marker not in result.stderr for marker in VALIDATION_FAILURES), result
        reply = json.loads(result.stdout)
        assert reply['status']=='ok', reply
        return reply

    def run(self):
        with WorldClient.open(str(self.args.binary.resolve()), self.native(self.world),
                              trace_directory=self.root/'transport') as client:
            self.author(client)
            self.compare_cameras(client)
        stderr = (self.root/'transport'/'stderr.log').read_text(encoding='utf-8')
        assert VALIDATION_LAYER in stderr and all(marker not in stderr for marker in VALIDATION_FAILURES)
        if self.args.runtime_root:
            self.run_export()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--runtime-root', type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int, required=True)
    parser.add_argument('--samples', type=int, choices=(1, 4), default=1)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    parser.add_argument('--reconstruction', choices=('none', 'fsr3_native'), default='none')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='gpu-visibility-', dir=args.output.resolve()))
    evidence = dict(format='poima.gpu-visibility.v1', passed=False, requests=[], images={}, comparisons={},
                    fixture_sha256=digest(Path(__file__)), executable_sha256=digest(args.binary),
                    gpu=args.gpu, samples=args.samples, lighting_path=args.lighting_path, reconstruction=args.reconstruction)
    try:
        VisibilityFixture(args, root, evidence).run()
        evidence['passed'] = True
    except BaseException:
        evidence['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(path)


if __name__ == '__main__':
    main()
