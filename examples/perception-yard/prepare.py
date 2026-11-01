#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare Perception Yard: a fresh manifest, then one guarded native authoring transaction.

Authors only ordinary native world operations: entities, transforms, primitive
boxes/colliders, a player controller/camera, a light and UI controls. No download,
compilation, gameplay injection, rendering, art, audio or automatic retry occurs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[2]

# Intruder station torso centres (x, z); the head box sits 0.75 m above the torso.
STATIONS = {1: (0.0, -2.0), 2: (3.0, -2.0), 3: (-3.0, -2.0), 4: (-7.0, 4.0), 5: (0.0, -8.0)}
TORSO_Y, HEAD_Y = .8, 1.55


def entity(value):
    return f'{value:032x}'


def operations():
    ops = []

    def create(n, name, position, scale=(1, 1, 1), parent=None, rotation=(0, 0, 0, 1)):
        ops.extend([dict(op='entity.create', id=entity(n), name=name, parent=None if parent is None else entity(parent)),
                    dict(op='component.set', id=entity(n), type='Transform',
                         value=dict(position=list(position), rotation=list(rotation), scale=list(scale)))])

    def component(n, kind, value):
        ops.append(dict(op='component.set', id=entity(n), type=kind, value=value))

    def box(n, name, centre, extents, color, motion='static'):
        # Unit collider half extents plus scale keep visual and collision boxes identical.
        create(n, name, centre, tuple(2*x for x in extents))
        component(n, 'MeshRenderer', dict(primitive='box', visible=True, albedo=color))
        component(n, 'BoxCollider', dict(half_extents=[.5, .5, .5], motion=motion, mass=1, friction=.5, restitution=0))

    box(1, 'Yard floor', (0, -.5, 0), (12, .5, 12), [.2, .21, .2])
    # Spectator controller well outside every sentry sight line, yawed 45 degrees toward the yard.
    create(2, 'Yard spectator', (9, .02, 9), rotation=(0, .3826834323650898, 0, .9238795325112867))
    component(2, 'CharacterController', dict(radius=.3, height=1.8, speed=4, jump_speed=5, camera=entity(3), locomotion={}))
    create(3, 'Yard camera', (0, 6, 0), parent=2, rotation=(-.3007057995042731, 0, 0, .9537169507482269))
    component(3, 'Camera', dict(vertical_fov=70, near=.1, far=100))
    # Sentry eye (0, 1.6, 6) lies inside the post; the visor lies on every forward sight line.
    box(10, 'Sentry post', (0, 1, 6), (.3, 1, .3), [.3, .32, .4])
    box(11, 'Sentry visor', (0, 1.6, 5.6), (.25, .08, .1), [.85, .7, .2])
    x, z = STATIONS[2]
    box(20, 'Intruder torso', (x, TORSO_Y, z), (.3, .5, .2), [.6, .2, .2], 'kinematic')
    box(21, 'Intruder head', (x, HEAD_Y, z), (.15, .15, .15), [.8, .5, .4], 'kinematic')
    box(30, 'Tall wall', (1.7, 1.25, 2), (.8, 1.25, .1), [.45, .47, .5])
    box(31, 'Low wall', (-1.5, .7, 2), (.8, .7, .1), [.5, .45, .38])
    create(40, 'Yard sun', (0, 0, 0), rotation=(-.3826834323650898, 0, 0, .9238795325112867))
    component(40, 'Light', dict(kind='directional', color=[1, .94, .86], intensity=3, enabled=True, shadow=dict(enabled=True, distance=60)))
    create(41, 'Yard ambient', (0, 0, 0))
    component(41, 'LightingEnvironment', dict(ambient=[.07, .08, .1], exposure=1, shadow_resolution=512))

    def ui(n, kind, text, top, action=None):
        element = dict(parent=None if n == 500 else entity(500), name=text, kind=kind, text='' if kind == 'panel' else text,
                       action=action, visible=True, enabled=True)
        element['layout'] = dict(position='absolute', left=dict(unit='dp', value=0 if n == 500 else 24),
                                 top=dict(unit='dp', value=0 if n == 500 else top),
                                 width=dict(unit='dp', value=448 if n == 500 else 400),
                                 height=dict(unit='dp', value=500 if n == 500 else 62 if n == 502 else 36))
        element['style'] = dict(background_color='#182024dd' if n == 500 else '#00000000' if kind == 'label' else '#722f37',
                                color='#f7e8eb', font_size=16)
        ops.append(dict(op='ui.element.set', id=entity(n), element=element))

    ui(500, 'panel', 'Perception yard', 0)
    ui(501, 'label', 'SENTRY SIGHT AND MEMORY', 16)
    ui(502, 'label', 'Waiting for the first Tick', 58)
    for n, text, action in [(510, 'Intruder: open ground', 'open'), (511, 'Intruder: behind tall wall', 'hide'),
                            (512, 'Intruder: head over low wall', 'low'), (513, 'Intruder: flank, outside view', 'flank'),
                            (514, 'Intruder: beyond range', 'far'), (515, 'Fault on second future Tick', 'fault'),
                            (516, 'Clear pending fault', 'clear-fault'), (517, 'Forget target, rest gaze', 'reset')]:
        ui(n, 'button', text, 140+(n-510)*42, action)
    return ops


def identity(path):
    with Path(path).open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(bytes=Path(path).stat().st_size, sha256=digest)


def prepare(output):
    """Create a fresh project manifest only; the world is authored separately through the native service."""
    output = Path(output).absolute()
    if output.exists() or output.is_symlink() or not output.parent.is_dir():
        raise ValueError('Fresh output with an existing parent required.')
    output.mkdir()
    project = dict(format='poima.project', version=1, project_id=uuid.uuid4().hex, name='Perception Yard',
                   entry=dict(world='world.json', camera=entity(3), controller=entity(2)), audio=False)
    (output/'project.json').write_text(json.dumps(project, indent=2)+'\n', encoding='utf-8')
    record = dict(format='poima.perception-yard-source.v1', project=identity(output/'project.json'), world_present=False,
                  operations_sha256=hashlib.sha256(json.dumps(operations(), sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
                  scope='New manifest only. Author operations() through the native service before gameplay; no build/native/render execution here.')
    (output/'perception-source.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    binary, output = args.binary.resolve(strict=True), args.output.absolute()
    if output.exists() or not output.parent.is_dir():
        parser.error('Fresh output with an existing parent required.')
    output.mkdir()
    sys.path.insert(0, str(ROOT/'tools/python'))
    from poima_client import WorldClient
    record = dict(passed=False, calls=[], errors=[], binary=identity(binary),
                  scope='Fresh Perception Yard authoring only; not gameplay, physics or sight qualification.')
    client, deadline = None, time.monotonic()+30
    try:
        record['preparation'] = prepare(output/'project')
        world = output/'project/world.json'
        client = WorldClient.open(str(binary), str(world), trace_directory=str(output/'stdio'), close_timeout=5)

        def call(method, params=None):
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Thirty-second authoring deadline.')
            row = dict(method=method, params=params or {})
            record['calls'].append(row)
            row['result'] = client.call(method, row['params'], timeout=min(remaining, 10))
            return row['result']
        before = call('world.inspect')
        if before['revision'] != 0 or before['entity_count'] != 0:
            raise ValueError('Expected a new empty native world.')
        changed = call('world.transact', dict(request_id=uuid.uuid4().hex, base_revision=0, ops=operations()))
        after = call('world.inspect')
        if changed['committed'] is not True or after['revision'] != changed['revision'] or not world.is_file():
            raise RuntimeError('Guarded yard was not persisted.')
        record['world'] = identity(world)
        record['passed'] = True
    except BaseException:
        record['errors'].append(traceback.format_exc())
    finally:
        if client is not None:
            try:
                client.close()
            except BaseException:
                record['passed'] = False
                record['errors'].append(traceback.format_exc())
            record['close'] = dict(closed=client.closed, pid=client.transport.process_id, exit_code=client.transport.returncode,
                                   cleanup_error=None if client.cleanup_error is None else str(client.cleanup_error))
            if not client.closed or client.transport.returncode != 0 or client.cleanup_error is not None:
                record['passed'] = False
        (output/'authoring.json').write_text(json.dumps(record, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    print(json.dumps(dict(passed=record['passed'], project=str(output/'project/project.json'), record=str(output/'authoring.json'))))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
