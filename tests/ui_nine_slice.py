#!/usr/bin/env python3
"""Verify sliced and tinted UI art in authoring, layout and native Vulkan output."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
from pathlib import Path
import struct
import subprocess
import zlib

from ui_image_contract import WorldClient, RpcError, box, element, new_id, pixels, sha


COLORS = [
    (200, 48, 70), (38, 160, 72), (22, 128, 220),
    (240, 152, 32), (110, 46, 130), (0, 182, 175),
    (188, 96, 145), (210, 200, 92), (240, 223, 202),
]
INSETS = [6, 7, 8, 9]


def make_sprite(path):
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data)))

    width, height = 41, 36
    scan = []
    for y in range(height):
        row = 0 if y < INSETS[0] else 2 if y >= height - INSETS[2] else 1
        scan.append(b'\0' + b''.join(bytes((*COLORS[3 * row + column], 255)) * size
                    for column, size in enumerate([INSETS[3], width - INSETS[3] - INSETS[1], INSETS[1]])))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(b''.join(scan))) + chunk(b'IEND', b''))


def verify(owner, args, native, output, camera, captures):
    def trx(revision, shapes):
        return owner.call('world.transact', dict(base_revision=revision, request_id=new_id(), ops=shapes))

    def reject(method, code, **params):
        try:
            owner.call(method, params)
        except RpcError as error:
            assert error.code == code, (method, error)
            return
        raise AssertionError(f'{method} accepted invalid slice metadata')

    sprite = output / 'frame.png'
    make_sprite(sprite)
    asset = owner.call('asset.image.import', dict(source=native(sprite), color_space='srgb'))['asset']
    style = dict(asset=asset, fit='nine_slice', slice_insets_px=INSETS)
    declaration = owner.call('world.describe', dict(view='section', name='ui'))['sections']['ui']['element']
    for control in declaration['oneOf']:
        image_schema = control['properties']['style']['properties']['image']
        assert 'nine_slice' in image_schema['properties']['fit']['enum']
        assert image_schema['then']['required'] == ['slice_insets_px']
        assert image_schema['properties']['tint']['type'] == 'string'

    def put(number, x, width, image, height=54, y=32):
        return element(number, None, 'label', box(x, y, width, height),
                       dict(background_color='#000000', border_width=0, image=image))

    shapes = [put(11, 24, 60, style), put(12, 104, 180, style),
              put(13, 24, 10, style, height=10, y=100),
              put(14, 104, 180, dict(asset=asset, fit='stretch'), y=100)]
    for number, x, text in [(15, 24, 'ALPHA BETA'), (16, 124, 'ALPHA\nBETA')]:
        shapes.append(element(number, None, 'label', box(x, 164, 82, 55),
                              dict(background_color='#000000', color='#ffffff',
                                   border_width=0, font_size=20), text))
    tinted = {**style, 'tint': '#ff00ff'}
    shapes.extend([
        put(17, 300, 60, tinted),
        put(18, 370, 100, dict(asset=asset, fit='stretch', tint='#00ffff')),
        put(19, 300, 60, dict(asset=asset, fit='contain', tint='#ffff00'), y=100),
        put(20, 370, 100, dict(asset=asset, fit='stretch', tint='#ffffff00'), y=100),
        put(21, 300, 60, dict(asset=asset, fit='stretch', tint='#ffffff80'), y=164),
    ])
    for invalid in (dict(asset=asset, fit='nine_slice'), {**style, 'fit': 'stretch'},
                    dict(asset=asset, slice_insets_px=INSETS),
                    *({**style, 'tint': tint} for tint in [True, '#FFFFFF', '#123', '#00ff00;']),
                    *({**style, 'slice_insets_px': insets} for insets in
                      ([0, 0, 0, 0], [True, 7, 8, 9], [6.5, 7, 8, 9], [6, 7, -1, 9],
                       [6, 7, 8], [6, 7, 8, 4096]))):
        reject('world.transact', -32602, base_revision=1, request_id=new_id(),
               ops=[put(11, 24, 60, invalid)])
    assert owner.call('world.inspect')['revision'] == 1
    assert trx(1, [put(11, 24, 60, {**style, 'slice_insets_px': [6, 31, 8, 10]})])['revision'] == 2
    reject('world.dependencies', -32050)
    reject('runtime.start', -32050, session_id=new_id(), revision=2)
    assert trx(2, shapes)['revision'] == 3
    assert [item['filename'] for item in owner.call('world.dependencies')['assets']] == [asset + '.pimage']
    assert owner.call('world.ui.list')['elements'][0]['element']['style']['image'] == style
    session = new_id()
    owner.call('runtime.start', dict(session_id=session, revision=3))
    inspected = owner.call('runtime.ui.inspect', dict(session_id=session, tick=0))['elements']
    assert inspected[0]['style']['image'] == style
    assert inspected[6]['style']['image'] == tinted
    for width, height, scale in [(960, 460, 2), (480, 270, 1)]:
        view = owner.call('world.ui.layout', dict(revision=3, width=width, height=height, scale=scale))
        assert [row['id'] for row in view['controls']] == [shape['id'] for shape in shapes]
        for row, shape in zip(view['controls'], shapes):
            x, y, w, h = [shape['element']['layout'][name]['value']
                          for name in ('left', 'top', 'width', 'height')]
            assert row['bounds'] == [x*scale, y*scale, (x+w)*scale, (y+h)*scale]
        if args.gpu is None:
            continue
        path = output / f'ui-sliced-{scale}x.bmp'
        frame = owner.call('runtime.capture', dict(session_id=session, tick=0, ui_revision=0,
            ui_scale=scale, camera=camera, path=native(path), width=width, height=height,
            samples=4, gpu=args.gpu), timeout=120)
        assert frame['hardware'] and frame['capture_written'] and frame['nvrhi_errors'] == 0, frame
        bitmap = pixels(path)
        checks = 0

        def check(x, y, expected):
            nonlocal checks
            actual = bitmap[y*scale][x*scale]
            assert all(abs(a-b) <= 2 for a,b in zip(actual,expected)), (scale,x,y,expected,actual)
            checks += 1

        for x, control_width in [(24, 60), (104, 180)]:
            for row, dy in enumerate((2, 10, 50)):
                for column, dx in enumerate((4, 12, control_width - 4)):
                    check(x + dx, 32 + dy, COLORS[row*3 + column])
        check(26, 102, COLORS[0])  # Corners contract when the center collapses.
        check(32, 108, COLORS[8])
        check(116, 102, COLORS[0])  # At this x a stretched image still shows its wide corner.
        for row, dy in enumerate((2, 10, 50)):
            for column, dx in enumerate((4, 12, 56)):
                red, _, blue = COLORS[row*3 + column]
                check(300 + dx, 32 + dy, (red, 0, blue))
        red, green, blue = COLORS[4]
        check(420, 59, (0, green, blue))
        check(330, 127, (red, green, 0))
        check(420, 127, (0, 0, 0))
        # Center patch of the same sprite at 128/255 opacity over black,
        # composited in linear light and then encoded for the capture.
        check(330, 191, (79, 31, 94))
        wrapped = [row[24*scale:106*scale] for row in bitmap[164*scale:219*scale]]
        explicit = [row[124*scale:206*scale] for row in bitmap[164*scale:219*scale]]
        assert any(pixel != (0,0,0) for row in explicit for pixel in row)
        assert wrapped == explicit, "Automatic wrapping split a word that fits on the next line."
        captures.append(dict(scale=scale, width=width, height=height, pixels_checked=checks,
                             wrapped_text_matches_explicit_break=True, sha256=sha(path)))
    owner.call('runtime.stop', dict(session_id=session))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--gpu', type=int)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)

    def native(path):
        return (subprocess.check_output(['wslpath', '-w', str(path)], text=True, timeout=10).strip()
                if args.windows_interop else str(path))

    binary = args.binary.resolve()
    project = output / 'game'
    subprocess.run([str(binary), 'project', 'create', native(project), '--name', 'UI slice fixture'],
                   check=True, capture_output=True, timeout=30)
    camera = json.loads((project / 'project.json').read_text())['entry']['camera']
    captures = []
    report = dict(passed=False, binary_sha256=sha(binary), captures=captures)
    try:
        with WorldClient.open(str(binary), native(project / 'world.json'),
                              trace_directory=output / 'rpc', close_timeout=15) as owner:
            verify(owner,args,native,output,camera,captures)
        assert owner.transport.returncode == 0
        report['passed'] = True
    finally:
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Verified nine-slice UI with {len(captures)} Vulkan captures.')


if __name__ == '__main__':
    main()
