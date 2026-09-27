#!/usr/bin/env python3
"""Exercise real editor actions and Vulkan cache churn in one native window.

Scripted semantic actions are not physical mouse/keyboard qualification.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
from texture_fixture import quad, png
from gltf_fixture import glb
from scene_capture import pixels

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('binary', type=Path)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--gpu', type=int, default=0)
p.add_argument('--windows-interop', action='store_true')
a = p.parse_args()
run = a.output / uuid.uuid4().hex
run.mkdir(parents=True)
world = run / 'world.json'
record = {'passed': False, 'gpu_index': a.gpu,
          'binary_sha256': hashlib.sha256(a.binary.read_bytes()).hexdigest()}

def native(path):
    return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if a.windows_interop else str(path.resolve())

def uid(n):
    return f'{n:032x}'

def rpc(method, params):
    row = {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params}
    r = subprocess.run([str(a.binary.resolve()), 'world', native(world)],
                       input=json.dumps(row)+'\n', text=True, capture_output=True, timeout=60)
    assert r.returncode == 0, r.stdout+r.stderr
    reply = json.loads(r.stdout)
    assert 'result' in reply, reply
    return reply['result']

actions = []
def action(frame, op, **params):
    actions.append({'frame': frame, 'op': op, **params})

try:
    models = []
    for i, color in enumerate(([255, 0, 0, 255], [0, 255, 0, 255])):
        doc, blob = quad([png(1, 1, color)])
        path = run / f'quad-{i}.glb'
        path.write_bytes(glb(doc, blob))
        models.append(rpc('asset.import', {'source': native(path)})['asset'])
    action(0, 'create', kind='cube', id=uid(1), name='Floor', position=[0, -1, 0], scale=[8, .2, 8])
    action(0, 'create', kind='cube', id=uid(2), name='Editable cube', position=[1.5, 0, 0])
    action(0, 'create', kind='light', id=uid(3), name='Key light', position=[0, 4, 4])
    action(0, 'create', kind='empty', id=uid(4), name='Environment')
    action(0, 'import_asset', path=native(run/'quad-0.glb'))
    action(0, 'instantiate_asset', asset=models[0], id=uid(100), name='Imported panel')
    mesh = hashlib.sha256(('poima.instance.v1/'+uid(100)+'/node/0/primitive/0').encode()).hexdigest()[:32]
    action(1, 'component', id=uid(3), type='Light', value={'kind': 'point', 'enabled': True, 'color': [1, .85, .7], 'intensity': 150, 'range': 20, 'shadow': {'enabled': True, 'distance': 20}})
    action(1, 'component', id=uid(4), type='LightingEnvironment', value={'ambient': [.12, .14, .2], 'exposure': 1, 'shadow_resolution': 256})
    # Resolved per-object materials acquire new identities at each revision;
    # this covers their ownership as well as imported mesh/image cache keys.
    action(1, 'component', id=mesh, type='PbrTextures', value={'normal_scale': 1})
    # Repeatedly retire/reload cooked meshes and their textures. Every image is
    # presented before the next mutation, unlike a transaction-only stress test.
    for frame in range(2, 42):
        action(frame, 'component', id=mesh, type='StaticMesh', value={'asset': models[frame % 2], 'primitive': 0, 'visible': True})
        if frame % 5 == 0:
            action(frame, 'component', id=uid(4), type='LightingEnvironment', value={'ambient': [.12, .14, .2], 'exposure': 1, 'shadow_resolution': 512 if frame % 10 else 256})
        if frame % 8 == 0:
            action(frame, 'component', id=uid(3), type='Light', value={'kind': 'point', 'enabled': True, 'color': [1, .85, .7], 'intensity': 150, 'range': 20, 'shadow': {'enabled': frame % 16 == 0, 'distance': 20}})
    action(42, 'select', id=uid(2))
    action(43, 'rename', name='Restored cube')
    action(44, 'delete')
    action(45, 'undo')
    action(46, 'redo')
    action(47, 'undo')
    action(48, 'select', id=uid(2))
    action(49, 'frame_selected')
    action(50, 'play')
    action(50, 'pause')
    action(51, 'step', ticks=60)
    action(52, 'stop')
    action(53, 'camera', position=[5, 3, 8], yaw=30, pitch=-15)
    script = run/'actions.json'
    script.write_text(json.dumps({'actions': actions}, indent=2)+'\n')
    capture, report = run/'editor.bmp', run/'report.json'
    command = [str(a.binary.resolve()), 'editor', native(world), '--gpu', str(a.gpu), '--width', '1440', '--height', '900', '--frames', '56', '--script', native(script), '--capture', native(capture), '--report', native(report)]
    result = subprocess.run(command, text=True, capture_output=True, timeout=180)
    record.update(command=command, exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
    assert result.returncode == 0, result.stdout+result.stderr
    evidence = json.loads(report.read_text())
    record['editor'] = evidence
    assert evidence['success'] and evidence['render']['capture_written'], evidence
    assert evidence['render']['hardware'] and evidence['render']['nvrhi_errors'] == 0, evidence
    assert len(evidence['actions']) == len(actions), evidence
    for item in evidence['actions']:
        assert 'error' not in item, item
    document = json.loads(world.read_text())
    assert document['entities'][uid(2)]['name'] == 'Restored cube', document
    assert not evidence['playing'], evidence
    samples = evidence['resource_samples']
    assert len(samples) >= 40, samples
    assert next(x['result']['tick'] for x in evidence['actions'] if x['op'] == 'step') == 60
    assert document['revision'] == next(x['revision'] for x in samples if x['frame'] == 49)
    for sample in samples:
        assert sample['meshes'] <= 1 and sample['images'] <= 2 and sample['materials'] <= 2, sample
        assert sample['skin_instances'] == 0, sample
    image = pixels(capture)
    assert len(image) == 900 and len(image[0]) == 1440
    colors = {pixel for row in image for pixel in row}
    assert len(colors) > 100, 'Capture is blank or lacks rendered UI/scene content.'
    record.update(passed=True, image={'sha256': hashlib.sha256(capture.read_bytes()).hexdigest(), 'path': str(capture.resolve())},
                  checks=['create/import/instantiate/component edits', 'delete/undo/redo preserves entity identity', '40 presented mesh/texture replacements', 'shadow toggle/resolution changes', 'play/pause/60-tick step/stop', 'bounded live cache entries'])
finally:
    (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n')
    print(run/'evidence.json')
