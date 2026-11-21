#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check rendered shadow clearance from a live seam stamp on Windows Vulkan."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import traceback

from snow_compaction_contract import (
    BOX, CAMERA, CAMERA_POSITION, CENTER, EAST, LENS, LIGHT, MATERIAL,
    REGULAR_DEPTH, REGULAR_RADIUS, SnowContract, Lattice, WEST, near, unit, vector_near,
)
from world_particles_contract import component, transform
from world_terrain_contract import CAPTURE_SIZE, file_hash, projection, uid


LAMP, RECEIVER = map(uid, (605, 606))
LAMP_POSITION = [1, .211125, .35]
RECEIVER_CENTER = [1.175, -.075, -.12]
RECEIVER_SCALE = [.67, .02, .08]
RECEIVER_TOP = RECEIVER_CENTER[1]+RECEIVER_SCALE[1]/2
TARGETS = [('west', .947, WEST), ('east', 1.053, EAST), ('quiet', 1.40, EAST)]
HDR_MINIMUM = .15


def lamp(enabled):
    return dict(kind='spot', color=[1, 1, 1], intensity=2, enabled=True, range=2, inner_angle=40,
                outer_angle=50, shadow=dict(enabled=enabled, near=.05, distance=2, bias=.0001, normal_bias=.001))


def toward(origin, target):
    return [end-start for start, end in zip(origin, target, strict=True)]


class SnowShadowContract(SnowContract):
    def add_test_light(self, revision):
        direction = unit(toward(LAMP_POSITION, [1.18, RECEIVER_TOP, RECEIVER_CENTER[2]]))
        aim = unit([direction[1], -direction[0], 0, 1-direction[2]])
        ops = [
            component(LIGHT, 'Light', dict(kind='directional', color=[1, 1, 1], intensity=0, enabled=False)),
            component(uid(4), 'LightingEnvironment',
                      dict(ambient=[.02]*3, exposure=1, shadow_resolution=2048, shadow_filter='pcf3_point')),
            dict(op='entity.create', id=LAMP, name='Oblique snow shadow light'),
            component(LAMP, 'Transform', transform(LAMP_POSITION, list(aim))),
            component(LAMP, 'Light', lamp(False)),
            dict(op='entity.create', id=RECEIVER, name='Fixed snow shadow receiver plate'),
            component(RECEIVER, 'Transform', transform(RECEIVER_CENTER) | dict(scale=RECEIVER_SCALE)),
            component(RECEIVER, 'MeshRenderer', dict(primitive='box', albedo=[.6]*3, visible=True,
                                                     cast_shadows=False, receive_shadows=True)),
            component(RECEIVER, 'PbrMaterial', MATERIAL | dict(base_color=[.6]*3)),
            component(RECEIVER, 'BoxCollider', BOX),
        ]
        reply = self.world_call('world.transact', request_id=uid(610), base_revision=revision, ops=ops)
        assert reply['committed'] and reply['revision'] == revision+1, reply
        return reply['revision']

    def collect(self, client, session, tick, casting, lattice, description):
        camera = client.call('runtime.entity', dict(session_id=session, tick=tick, id=CAMERA))['world_matrix']
        width, height = CAPTURE_SIZE
        focal = height/(2*math.tan(math.radians(LENS['vertical_fov'])/2))
        camera_depth = CAMERA_POSITION[1]-RECEIVER_TOP
        near_plane, far_plane = LENS['near'], LENS['far']
        device_depth = far_plane/(far_plane-near_plane)*(1-near_plane/camera_depth)
        expected_contacts = []
        probes = []
        for name, target_x, blocker in TARGETS:
            p = [target_x, RECEIVER_TOP, RECEIVER_CENTER[2]]
            u, v = map(math.floor, projection(camera, LENS, p))
            # The center of the raster sample, rather than the arbitrary
            # center of the authored target, is the corresponding light ray.
            point = [CAMERA_POSITION[0]+(u+.5-width/2)*camera_depth/focal, RECEIVER_TOP,
                     CAMERA_POSITION[2]+(v+.5-height/2)*camera_depth/focal]
            ray = toward(LAMP_POSITION, point)
            hit = client.call('runtime.raycast', dict(session_id=session, tick=tick,
                origin=LAMP_POSITION, direction=unit(ray), distance=math.hypot(*ray)+.02))['hit']
            expected_contact = blocker if tick == 0 or name == 'quiet' else RECEIVER
            assert hit is not None and hit['entity'] == expected_contact, (name, point, hit, expected_contact)
            if expected_contact == RECEIVER:
                assert hit['triangle'] is None, hit
                vector_near(hit['position'], point, 3e-5)
                vector_near(hit['normal'], [0, 1, 0], 3e-5)
            else:
                x, y, z = hit['position']
                expected_height, normal, face = lattice.surface(x, z, blocker)
                assert 0 < z < .04 and hit['triangle'] == face, (name, hit, face)
                near(y, expected_height, 3e-5)
                vector_near(hit['normal'], normal, .002)
            probes.append(dict(x=u, y=v))
            expected_contacts.append(dict(name=name, receiver_point_m=point, native_ray_hit=hit))
        path = self.root/(description+'.bmp')
        receipt = client.call('runtime.capture', dict(session_id=session, tick=tick, structure_revision=0,
            camera=CAMERA, path=self.native(path), width=width, height=height, gpu=self.args.gpu,
            lighting_path=self.args.lighting_path, reconstruction='none', samples=1, capture_frames=2,
            scene_product_probes=probes))
        assert receipt['hardware'] is True and receipt['capture_written'] is True, receipt
        assert receipt['nvrhi_errors'] == 0 and receipt['frames_presented'] > 0, receipt
        assert receipt['tick'] == tick and receipt['camera_world'] == camera, receipt
        diagnostics = receipt['render_diagnostics']
        assert diagnostics['lighting_path'] == self.args.lighting_path, diagnostics
        draws = diagnostics['last_draws']
        assert draws['shadow_views'] == int(casting), draws
        assert (draws['shadow_draws'] > 0) is casting, draws
        products = diagnostics['scene_products']['probes']
        cases = {}
        for probe, expected, actual in zip(probes, expected_contacts, products, strict=True):
            assert actual['surface_valid'] is True and actual['x'] == probe['x'] and actual['y'] == probe['y'], actual
            near(actual['depth'], device_depth, 2e-6)
            vector_near(actual['shading_normal'][:3], [0, 1, 0], .002)
            cases[expected['name']] = expected | dict(sample=actual)
        self.measurements.setdefault('captures', {})[description] = dict(
            path=str(path), sha256=file_hash(path), raw_receipt=receipt, cases=cases)
        return cases

    def shadow_pass(self, revision, casting, label):
        lattice = Lattice()
        session = uid(611+int(casting))
        with self.owner(label) as (client, _):
            started = client.call('runtime.start', dict(session_id=session, revision=revision))
            assert started['tick'] == 0 and started['session_id'] == session, started
            pristine = self.collect(client, session, 0, casting, lattice, label+' pristine')
            command = self.stamp(client, session, 0, 0, REGULAR_RADIUS, REGULAR_DEPTH)
            self.update(client, session, 0, 613, command)
            assert lattice.stamp(CENTER, REGULAR_RADIUS, REGULAR_DEPTH)
            self.snow(client, session, 1, lattice)
            modified = self.collect(client, session, 1, casting, lattice, label+' modified')
        return pristine, modified

    def run(self):
        revision = self.add_test_light(self.author())
        without_shadow = self.shadow_pass(revision, False, 'shadow disabled')
        frozen = self.world.read_bytes()
        reply = self.world_call('world.transact', request_id=uid(614), base_revision=revision,
                                ops=[component(LAMP, 'Light', lamp(True))])
        assert reply['committed'] and reply['revision'] == revision+1, reply
        comparison = json.loads(self.world.read_bytes())
        assert comparison['entities'][LAMP]['components']['Light']['shadow']['enabled'] is True
        # Hold inputs fixed across this authored A/B except the lamp's shadow
        # enable bit and transactional metadata.
        before = json.loads(frozen)
        comparison['entities'][LAMP]['components']['Light']['shadow']['enabled'] = False
        assert comparison['entities'] == before['entities'], (before['entities'], comparison['entities'])
        with_shadow = self.shadow_pass(reply['revision'], True, 'shadow enabled')
        ratios = self.measurements['enabled_over_disabled_hdr'] = {}
        for name, _, _ in TARGETS:
            off_pristine, off_modified = (phase[name]['sample']['raw_hdr'] for phase in without_shadow)
            for prior, current in zip(off_pristine, off_modified, strict=True):
                assert prior >= HDR_MINIMUM, (name, off_pristine, off_modified)
                near(current, prior, max(.002, abs(prior)*.005))
            samples = ratios[name] = []
            for on, off in zip(with_shadow, without_shadow, strict=True):
                assert on[name]['native_ray_hit'] == off[name]['native_ray_hit'], (name, on[name], off[name])
                enabled = on[name]['sample']['raw_hdr']
                disabled = off[name]['sample']['raw_hdr']
                samples.append([a/b for a, b in zip(enabled, disabled, strict=True)])
            assert all(0 <= ratio <= .2 for ratio in samples[0]), (name, samples)
            if name == 'quiet':
                assert all(0 <= ratio <= .2 for ratio in samples[1]), (name, samples)
                vector_near(samples[0], samples[1], .02)
            else:
                assert all(.9 <= ratio <= 1.01 for ratio in samples[1]), (name, samples)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path, help='Windows native runtime executable.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true', help='Translate paths when launched from WSL.')
    parser.add_argument('--gpu', type=int, default=0)
    parser.add_argument('--lighting-path', choices=('forward', 'deferred'), default='forward')
    args = parser.parse_args()
    if not __debug__:
        parser.error('Run with assertions enabled.')
    if not 0 <= args.gpu <= 4095:
        parser.error('--gpu requires a device index from 0 through 4095.')
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='snow-shadow-', dir=args.output.resolve()))
    contract = SnowShadowContract(args, root)
    result = dict(format='poima.snow-shadow-contract.v1', passed=False, gpu=args.gpu,
                  lighting_path=args.lighting_path, executable_sha256=file_hash(args.binary),
                  harness_sha256=file_hash(Path(__file__)),
                  snow_fixture_sha256=file_hash(Path(__file__).with_name('snow_compaction_contract.py')),
                  calls=contract.calls, measurements=contract.measurements)
    try:
        contract.run()
        result['passed'] = True
    except BaseException:
        result['failure'] = traceback.format_exc()
        raise
    finally:
        path = root/'evidence.json'
        path.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
        print(f'Evidence file {path}')


if __name__ == '__main__':
    main()
