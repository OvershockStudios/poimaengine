#!/usr/bin/env python3
"""Check cooked UI assets through authoring, native saves, and optional Vulkan/export."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import uuid
import zlib

from scene_capture import pixels

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client import RpcError, WorldClient


def uid(value):
    return f'{value:032x}'


def new_id():
    return uuid.uuid4().hex


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sprite(path, primary):
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data)))
    def scan(left, right):
        return b'\x00' + bytes(left) * 4 + bytes(right) * 4
    top = scan(primary, (216, 120, 32, 128))
    bottom = scan((255, 0, 255, 0), (22, 162, 201, 255))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', 8, 4, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(top * 2 + bottom * 2)) + chunk(b'IEND', b''))


def box(x, y, width, height):
    return {'position': 'absolute', 'left': {'unit': 'dp', 'value': x},
            'top': {'unit': 'dp', 'value': y}, 'width': {'unit': 'dp', 'value': width},
            'height': {'unit': 'dp', 'value': height}, 'padding': [0, 0, 0, 0]}


def element(number, parent, kind, layout, style, text='', visible=True):
    return dict(op='ui.element.set', id=uid(number), element=dict(
        name=f'Textured {kind}', parent=uid(parent) if parent else None, kind=kind,
        text=text, action='confirm' if kind == 'button' else None, visible=visible,
        enabled=True, layout=layout, style=style))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int)
    parser.add_argument('--runtime', type=Path, help='Matching installed Windows export runtime.')
    args = parser.parse_args()
    if args.runtime and args.gpu is None:
        parser.error('--runtime needs --gpu for packaged Vulkan verification')
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.output.resolve()
    binary = args.binary.resolve()
    report = {'passed': False, 'engine_sha256': sha(binary), 'rpc': [], 'commands': [],
              'captures': {}, 'owner_exits': []}
    project = root / 'source-project'
    world = project / 'world.json'
    saves = root / 'saves'
    saves.mkdir()
    client = None

    def native(path):
        value = str(path.resolve())
        if args.windows_interop:
            return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=10).strip()
        return value

    def cli(*argv):
        command = [str(binary), *argv]
        run = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=180)
        result = json.loads(run.stdout)
        report['commands'].append(dict(argv=command, response=result, exit_code=run.returncode))
        assert run.returncode == 0 and result.get('status') == 'ok', (result, run.stderr)
        return result['result']

    def call(method, code=None, **params):
        try:
            result = client.call(method, params, timeout=120 if method.endswith('.capture') else 25)
        except RpcError as error:
            report['rpc'].append(dict(method=method, params=params,
                                      error={'code': error.code, 'message': str(error)}))
            assert error.code == code, (method, error)
            return None
        report['rpc'].append(dict(method=method, params=params, result=result))
        assert code is None, (method, params, result)
        return result

    def edit(revision, ops, code=None):
        return call('world.transact', code, base_revision=revision, request_id=new_id(), ops=ops)

    def close():
        nonlocal client
        owner, client = client, None
        owner.close()
        report['owner_exits'].append(owner.transport.returncode)
        assert owner.transport.returncode == 0, owner.transport.stderr_tail

    def capture(name, session):
        if args.gpu is None:
            return None
        path = root / (name + '.bmp')
        result = call('runtime.capture', session_id=session, tick=0, ui_revision=0,
                      ui_scale=1, camera=camera, path=native(path), width=640, height=480,
                      samples=4, gpu=args.gpu)
        assert result['hardware'] and result['capture_written'] and result['nvrhi_errors'] == 0, result
        report['captures'][name] = {'sha256': sha(path), 'gpu': result['gpu'], 'ui_revision': result['ui_revision']}
        return pixels(path)

    def match(image, x, y, rgb, tolerance=2):
        if image is not None:
            assert all(abs(a-b) <= tolerance for a,b in zip(image[y][x],rgb)), (x,y,image[y][x],rgb)

    def saved_configuration():
        result = call('save.configure', request_id=new_id(), expected_generation=0, root=native(saves))
        assert result['generation'] == 1

    try:
        manifest = project / 'project.json'
        cli('project', 'create', native(project), '--name', 'Cooked HUD fixture')
        camera = json.loads(manifest.read_text(encoding='utf-8'))['entry']['camera']
        client = WorldClient.open(str(binary), native(world), close_timeout=10)
        world_id = call('world.inspect')['world_id']
        first, second = (root / name for name in ('first.png', 'second.png'))
        red, green = (180, 64, 16, 255), (33, 192, 84, 255)
        sprite(first, red)
        sprite(second, green)
        original = call('asset.image.import', source=native(first), color_space='srgb')['asset']
        alternate = call('asset.image.import', source=native(second), color_space='srgb')['asset']
        linear = call('asset.image.import', source=native(first), color_space='linear')['asset']
        assert len({original,alternate,linear}) == 3

        panel = element(10, None, 'panel', {**box(40, 40, 176, 144), 'overflow': 'hidden'},
                        {'background_color': '#241c24', 'border_width': 0})
        badge = element(11, 10, 'label', box(0, 0, 64, 64),
                        {'background_color': '#000000', 'image': {'asset': original}})
        confirm = element(12, 10, 'button', box(0, 72, 128, 40),
                          {'background_color': '#000000', 'image': {'asset': original,'fit': 'stretch'}}, 'Continue')
        unbound = element(11, 10, 'label', box(0, 0, 64, 64),
                          {'image': {'asset': 'https://example.invalid/image.png'}})
        edit(1,[panel,unbound],-32602)
        assert call('world.inspect')['revision'] == 1
        edit(1,[panel,badge,confirm])
        assert call('world.ui.get', id=uid(11), revision=2)['element'] == badge['element']
        owner = dict(kind='world', id=world_id)
        page = call('world.asset.references', owner=owner, revision=2, limit=1)
        assert len(page['edges']) == 1 and page['next_after'] is not None
        cursor = page['next_after']
        assert cursor == {'owner': owner, 'component':'ui', 'path':f'/ui/{uid(11)}/style/image/asset'}
        remaining = call('world.asset.references', owner=owner, revision=2, after=cursor)
        assert len(remaining['edges']) == 1 and remaining['edges'][0]['path'] == f'/ui/{uid(12)}/style/image/asset'
        assert call('world.asset.references', asset=original)['edges'] == page['edges'] + remaining['edges']

        first_id = original + '.pimage'
        second_id = alternate + '.pimage'
        dependencies = call('world.dependencies')
        assert [a['filename'] for a in dependencies['assets']] == [first_id]
        layout = call('world.ui.layout', revision=2, width=640, height=480, scale=1)
        rows = {control['id']:control for control in layout['controls']}
        assert rows[uid(11)]['bounds'] == [40,40,104,104]
        assert rows[uid(12)]['bounds'] == [40,112,168,152] and rows[uid(12)]['hittable']
        session = new_id()
        call('runtime.start', session_id=session, revision=2)
        original_ui = call('runtime.ui.inspect', session_id=session, tick=0)
        assert original_ui['elements'][1]['style']['image'] == {'asset':original}
        before = capture('original', session)
        match(before,52,66,red[:3])
        # The letterbox and the transparent bottom-left texels leave black.
        match(before,52,45,(0,0,0))
        match(before,52,82,(0,0,0))
        # Half-alpha is blended in linear light over the black label background.
        def blend(channel):
            source = (channel / 255 + .055) / 1.055
            return round((1.055 * (source ** 2.4 * 128/255) ** (1/2.4) - .055) * 255)
        match(before,93,66,tuple(blend(c) for c in (216,120,32)))
        match(before,149,145,(22,162,201))  # stretched image behind the button text

        saved_configuration()
        call('save.write', configuration_generation=1, slot='hud', expected_generation=0,
             session_id=session, expected_tick=0, expected_ui_revision=0, expected_control_sequence=0,
             expected_gameplay_revision=0, request_id=new_id())
        invalid = element(11,10,'label',box(0,0,64,64),
                          {'background_color':'#000000','image':{'asset':linear}})
        edit(2,[invalid])
        call('world.dependencies', -32050)
        call('world.ui.layout', -32050, revision=3, width=640, height=480)
        assert call('runtime.ui.inspect', session_id=session, tick=0) == original_ui
        missing = element(11,10,'label',box(0,0,64,64),{'image':{'asset':'f'*64}})
        edit(3,[missing])
        call('world.dependencies', -32050)
        updated = element(11,10,'label',box(0,0,64,64),
                          {'background_color':'#000000','image':{'asset':alternate}})
        edit(4,[updated])
        assert sorted(a['filename'] for a in call('world.dependencies')['assets']) == sorted((first_id,second_id))
        package = world.with_suffix('.json.assets') / first_id
        intact = package.read_bytes()
        package.write_bytes(intact[:-1] + bytes([intact[-1] ^ 1]))
        try:
            call('world.dependencies', -32050)
            assert len(call('world.asset.references', asset=original)['edges']) == 1
        finally:
            package.write_bytes(intact)
        assert capture('frozen',session) == before
        close()

        moved_inputs = root / 'original-inputs'
        moved_inputs.mkdir()
        for input_file in (first,second):
            input_file.rename(moved_inputs / input_file.name)
        client = WorldClient.open(str(binary), native(world), close_timeout=10)
        saved_configuration()
        restored = new_id()
        call('save.load', configuration_generation=1, slot='hud', expected_generation=1, revision=5,
             expected_session_id=None, expected_tick=None, expected_gameplay_revision=None,
             new_session_id=restored, request_id=new_id())
        restored_ui = call('runtime.ui.inspect',session_id=restored,tick=0)
        assert restored_ui['elements'] == original_ui['elements']
        assert capture('cold-save',restored) == before
        call('runtime.stop',session_id=restored)
        new_session = new_id()
        call('runtime.start',session_id=new_session,revision=5)
        after = capture('fresh-runtime',new_session)
        match(after,52,66,green[:3])
        if args.gpu is not None:
            assert before != after
        close()

        if args.runtime:
            output = root / 'exported'
            cli('project', 'build', native(manifest), '--output', native(output), '--runtime', native(args.runtime))
            moved = root / 'relocated'
            output.rename(moved)
            project.rename(root / 'source-offline')
            game = moved / 'game.json'
            packaged_images = sorted(p.name for p in moved.rglob('*.pimage'))
            assert packaged_images == sorted((first_id,second_id))
            packaged = moved / 'runtime' / 'bin' / 'poima.exe'
            client = WorldClient.open_game(str(packaged), native(game), close_timeout=10)
            state = call('runtime.status')
            assert capture('relocated-game',state['session_id']) == after
            close()
        report['passed'] = True
    except BaseException as error:
        report['failure'] = repr(error)
        raise
    finally:
        if client is not None:
            close()
        (root/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(f"Verified cooked HUD images in {len(report['rpc'])} calls and {len(report['captures'])} captures.")


if __name__ == '__main__':
    main()
