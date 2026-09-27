#!/usr/bin/env python3
"""Qualify docked/floating/hidden Scene composition and external layout storage.

Scripted actions use the editor's actual docking and layout implementation;
this is not physical mouse-drag qualification.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import uuid
from scene_capture import pixels

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('binary', type=Path)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--gpu', type=int, default=0)
p.add_argument('--samples', type=int, choices=(1, 4), default=4)
p.add_argument('--windows-interop', action='store_true')
args = p.parse_args()
run = args.output/uuid.uuid4().hex
project = run/'project'
project.mkdir(parents=True)
world = project/'world.json'
record = {'passed': False, 'gpu': args.gpu, 'samples': args.samples, 'runs': [], 'checks': [],
          'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
          'qualification': 'Scripted real docking/layout actions and GPU pixels; no physical input claim.'}

def native(path):
    return subprocess.check_output(['wslpath', '-w', str(path.resolve())], text=True).strip() if args.windows_interop else str(path.resolve())

def tree(path):
    return {str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest() for p in path.rglob('*') if p.is_file()}

def red_count(image, rect=None, maximum_x=None):
    left, top, right, bottom = 0, 0, len(image[0]), len(image)
    if rect:
        x, y, w, h = rect
        left, top = max(0, int(x)), max(0, int(y))
        right, bottom = min(right, int(x+w)), min(bottom, int(y+h))
    if maximum_x is not None:
        right = min(right, int(maximum_x))
    return sum(r > 65 and r > g*1.7 and r > b*1.7 for row in image[top:bottom] for r, g, b in row[left:right])

def editor(label, actions=None, layout=None, expect_success=True):
    capture, report = run/(label+'.bmp'), run/(label+'.json')
    command = [str(args.binary.resolve()), 'editor', native(world), '--gpu', str(args.gpu), '--width', '1440',
               '--height', '900', '--samples', str(args.samples), '--frames', '8', '--capture', native(capture), '--report', native(report)]
    if actions is not None:
        script = run/(label+'-actions.json')
        script.write_text(json.dumps({'actions': actions}), encoding='utf-8')
        command += ['--script', native(script)]
    command += ['--layout', native(layout)] if layout else ['--no-layout']
    result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=90)
    entry = {'label': label, 'command': command, 'exit_code': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
    record['runs'].append(entry)
    assert (result.returncode == 0) == expect_success, entry
    envelope = json.loads(result.stdout)
    assert envelope['status'] == ('ok' if expect_success else 'error'), envelope
    data = json.loads(report.read_text(encoding='utf-8'))
    entry['report'] = data
    if not expect_success:
        assert not data['success'] and not capture.exists(), data
        return data, None
    assert data['success'] and data['render']['hardware'] and data['render']['nvrhi_errors'] == 0, data
    assert data['render']['capture_written'], data
    state = data['editor_state']['layout']
    assert state['font'] == 'Source Sans 3' and state['docking'] and not state['native_multi_window'], state
    image = pixels(capture)
    entry['capture_sha256'] = hashlib.sha256(capture.read_bytes()).hexdigest()
    return state, image

camera = {'frame': 0, 'op': 'camera', 'position': [0, 0, 8], 'yaw': 0, 'pitch': 0}
reset = {'frame': 0, 'op': 'layout_reset'}
floating = {'frame': 1, 'op': 'layout_float', 'panel': 'Scene', 'position': [10, 150], 'size': [500, 400]}

try:
    entity = '1'*32
    camera_entity = '3'*32
    request = {'jsonrpc': '2.0', 'id': 1, 'method': 'world.transact', 'params': {'request_id': '2'*32, 'base_revision': 0, 'ops': [
        {'op': 'entity.create', 'id': entity, 'name': 'Visible red cube'},
        {'op': 'component.set', 'id': entity, 'type': 'Transform', 'value': {'position': [0, 0, 0], 'rotation': [0, 0, 0, 1], 'scale': [3, 3, 3]}},
        {'op': 'component.set', 'id': entity, 'type': 'MeshRenderer', 'value': {'primitive': 'box', 'albedo': [.9, .02, .01], 'visible': True}},
        {'op': 'entity.create', 'id': camera_entity, 'name': 'Reference camera'},
        {'op': 'component.set', 'id': camera_entity, 'type': 'Transform', 'value': {'position': [0, 0, 8], 'rotation': [0, 0, 0, 1], 'scale': [1, 1, 1]}},
        {'op': 'component.set', 'id': camera_entity, 'type': 'Camera', 'value': {'vertical_fov': 60, 'near': .1, 'far': 1000}}]}}
    authored = subprocess.run([str(args.binary.resolve()), 'world', native(world)], input=json.dumps(request)+'\n',
        capture_output=True, text=True, encoding='utf-8', timeout=30)
    assert authored.returncode == 0 and 'result' in json.loads(authored.stdout), authored.stdout+authored.stderr
    before = tree(project)
    reference_path = run/'world-reference.bmp'
    reference_request = {'jsonrpc': '2.0', 'id': 2, 'method': 'world.capture', 'params': {
        'revision': 1, 'camera': camera_entity, 'path': native(reference_path),
        'gpu': args.gpu, 'samples': args.samples, 'width': 960, 'height': 540}}
    reference_command = [str(args.binary.resolve()), 'world', native(world)]
    reference_run = subprocess.run(reference_command, input=json.dumps(reference_request)+'\n',
        capture_output=True, text=True, encoding='utf-8', timeout=90)
    record['reference'] = {'command': reference_command, 'request': reference_request,
        'exit_code': reference_run.returncode, 'stdout': reference_run.stdout, 'stderr': reference_run.stderr}
    assert reference_run.returncode == 0 and 'result' in json.loads(reference_run.stdout), record['reference']
    reference_image = pixels(reference_path)
    record['reference']['capture_sha256'] = hashlib.sha256(reference_path.read_bytes()).hexdigest()
    baseline, base_image = editor('default', [reset, camera])
    windows = baseline['windows']
    assert all(windows[name]['dock_id'] for name in ['Hierarchy', 'Inspector', 'Scene', 'Project', 'Console']), windows
    assert windows['Project']['dock_id'] == windows['Console']['dock_id'], windows
    assert windows['Hierarchy']['rect'][0] < windows['Scene']['rect'][0] < windows['Inspector']['rect'][0], windows
    assert windows['Inspector']['rect'][3] > windows['Scene']['rect'][3], windows
    assert red_count(base_image, baseline['scene_viewport']) > 1000
    record['checks'].append('default split hierarchy/scene/inspector and bottom Project/Console tab group')
    # Both cameras look at the same flat, unlit cube face. Interior samples avoid
    # silhouettes/MSAA coverage and isolate the Scene compositor's sRGB handling.
    x, y, width, height = baseline['scene_viewport']
    editor_center = (int(x+width/2), int(y+height/2))
    reference_center = (len(reference_image[0])//2, len(reference_image)//2)
    comparisons = []
    for dy in range(-2, 3):
        for dx in range(-2, 3):
            expected = reference_image[reference_center[1]+dy][reference_center[0]+dx]
            actual = base_image[editor_center[1]+dy][editor_center[0]+dx]
            assert expected[0] > 65 and expected[0] > expected[1]*1.7 and expected[0] > expected[2]*1.7, expected
            delta = [abs(a-b) for a, b in zip(actual, expected)]
            comparisons.append({'offset': [dx, dy], 'reference_rgb': expected, 'editor_rgb': actual, 'delta': delta})
    record['srgb_comparison'] = {'reference_center': reference_center, 'editor_center': editor_center,
        'tolerance_per_channel': 1, 'samples': comparisons}
    assert all(max(sample['delta']) <= 1 for sample in comparisons), record['srgb_comparison']
    record['checks'].append('Scene compositor interior RGB matches ordinary world capture within one code value per channel')

    floated, float_image = editor('floating', [reset, camera, floating])
    assert floated['windows']['Scene']['dock_id'] == 0 and floated['windows']['Scene']['visible'], floated
    hierarchy_right = floated['windows']['Hierarchy']['rect'][0]+floated['windows']['Hierarchy']['rect'][2]
    overlap = red_count(float_image, floated['scene_viewport'], maximum_x=hierarchy_right-5)
    assert overlap > 100, f'Floating Scene did not composite red geometry over underlying Hierarchy: {overlap}'
    record['checks'].append('floating Scene composites actual 3D pixels above an underlying panel')

    docked, _ = editor('retabbed', [reset, camera, floating,
        {'frame': 3, 'op': 'layout_dock', 'panel': 'Scene', 'target': 'Hierarchy'}])
    assert docked['windows']['Scene']['dock_id'] == docked['windows']['Hierarchy']['dock_id'] != 0, docked
    hidden, hidden_image = editor('hidden', [reset, camera, {'frame': 1, 'op': 'layout_show', 'panel': 'Scene', 'visible': False}])
    assert not hidden['windows']['Scene']['open'] and hidden['scene_viewport'][2:] == [0, 0], hidden
    assert red_count(hidden_image) == 0, 'Hidden Scene leaked previously rendered geometry into the editor.'
    restored, _ = editor('reset', [reset, camera, floating,
        {'frame': 2, 'op': 'layout_show', 'panel': 'Scene', 'visible': False}, {'frame': 3, 'op': 'layout_reset'}])
    assert restored['windows']['Scene']['dock_id'] != 0 and restored['windows']['Scene']['visible'], restored
    assert restored['scene_viewport'] == baseline['scene_viewport'], (restored, baseline)
    record['checks'].append('real re-tab, hidden Scene zero viewport, and reset restore the default geometry')

    layout_file = run/'User layout.ini'
    saved, _ = editor('persist-save', [reset, camera, floating], layout=layout_file)
    assert saved['persistent'] and not saved['loaded'] and 0 < layout_file.stat().st_size <= 65536, saved
    reopened, _ = editor('persist-load', layout=layout_file)
    assert reopened['persistent'] and reopened['loaded'], reopened
    assert reopened['windows']['Scene']['dock_id'] == 0, reopened
    assert reopened['windows']['Scene']['rect'] == saved['windows']['Scene']['rect'], (saved, reopened)
    oversized = run/'Oversized layout.ini'
    oversized.write_bytes(b'x'*65537)
    editor('oversized-rejected', layout=oversized, expect_success=False)
    assert tree(project) == before, 'Editor layout actions or persistence changed project files.'
    assert not (project/'imgui.ini').exists()
    record['checks'].append('bounded external layout saves/loads across restart and never writes project content')
    record['passed'] = True
except BaseException as error:
    record['failure'] = repr(error)
    raise
finally:
    (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    (args.output/f'editor-layout-gpu-{args.gpu}.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(run/'evidence.json')
