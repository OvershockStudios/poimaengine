#!/usr/bin/env python3
"""Original analytic FBX scalar maps; no exporter, downloaded content or engine run.

The explicit 3ds Max metal/rough property dialect is the pinned ufbx mapping,
not a claim that these original files were produced by that application.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib
from fbx_fixture import array, binary_encode, model, scene, texture_objects
from texture_fixture import png

ROUGH = (0, 64, 128, 255)
METAL = (255, 128, 64, 0)
INVALID = ('colored', 'alpha', 'extent', 'wrap', 'uv', 'transform', 'shader',
           'vector_factor', 'factor_high', 'factor_negative', 'glossiness',
           'advanced', 'escaping', 'dimension', 'decoded_budget', 'image_count')


def scalar_png(values, width=2, height=2, alpha=255):
    return png(width, height, [c for value in values for c in (value, value, value, alpha)])


def constant_png(path, width, height, value):
    """Bounded row-at-a-time RGBA8 PNG writer, including budget witnesses."""
    def chunk(stream, kind, data):
        stream.write(struct.pack('>I', len(data)) + kind + data +
                     struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
    with path.open('wb') as stream:
        stream.write(b'\x89PNG\r\n\x1a\n')
        chunk(stream, b'IHDR', struct.pack('>2I5B', width, height, 8, 6, 0, 0, 0))
        compressor = zlib.compressobj()
        row = b'\0' + bytes((value, value, value, 255)) * width
        for _ in range(height):
            data = compressor.compress(row)
            if data:
                chunk(stream, b'IDAT', data)
        chunk(stream, b'IDAT', compressor.flush())
        chunk(stream, b'IEND', b'')


def uv_geometry(identity=200, name='Triangle'):
    return (f'Geometry: {identity}, "Geometry::{name}", "Mesh" {{\n' +
        array('Vertices', [0, 0, 0, 100, 0, 0, 0, 100, 0]) +
        array('PolygonVertexIndex', [0, 1, -3]) +
        'LayerElementNormal: 0 { Version: 101\nName: ""\n'
        'MappingInformationType: "ByPolygonVertex"\nReferenceInformationType: "Direct"\n' +
        array('Normals', [0, 0, 1] * 3) + '}\n'
        'LayerElementUV: 0 { Version: 101\nName: "UVMap"\n'
        'MappingInformationType: "ByPolygonVertex"\nReferenceInformationType: "Direct"\n' +
        array('UV', [.25, .25, .75, .25, .25, .75]) + '}\n'
        'LayerElementMaterial: 0 { Version: 101\nName: ""\n'
        'MappingInformationType: "AllSame"\nReferenceInformationType: "IndexToDirect"\n' +
        array('Materials', [0]) + '}\nLayer: 0 { Version: 100\n'
        'LayerElement: { Type: "LayerElementNormal"\nTypedIndex: 0 }\n'
        'LayerElement: { Type: "LayerElementUV"\nTypedIndex: 0 }\n'
        'LayerElement: { Type: "LayerElementMaterial"\nTypedIndex: 0 }\n}\n}\n')


def pbr_material(identity=600, rough=.5, metal=.25, extra=''):
    properties = ('P: "3dsMax|ClassIDa", "int", "Integer", "", 3490651648\n'
                  'P: "3dsMax|ClassIDb", "int", "Integer", "", 3195528448\n')
    for key, value in (('roughness', rough), ('metalness', metal)):
        if value is not None:
            kind = 'Vector3D' if isinstance(value, tuple) else 'Number'
            values = ','.join(map(str, value)) if isinstance(value, tuple) else str(value)
            properties += f'P: "3dsMax|main|{key}", "{kind}", "", "A", {values}\n'
    return (f'Material: {identity}, "Material::Scalar{identity}", "" {{\nVersion: 102\n'
            'ShadingModel: "PoimaAnalyticPBR"\nMultiLayer: 0\nProperties70: {\n' +
            properties + extra + '}\n}\n')


def scalar_scene(*, rough='rough.png', metal='metal.png', rough_factor=.5,
                 metal_factor=.25, variant='', copies=1, retained=False, skin=False):
    text = scene(skin=skin, takes=('Move', 'Turn') if skin else ())
    # Replace exactly the original geometry object; existing rig/animations remain.
    start = text.index('Geometry: 200,')
    end = start
    depth = 0
    opened = False
    for end in range(start, len(text)):
        if text[end] == '{': depth += 1; opened = True
        if text[end] == '}':
            depth -= 1
            if opened and depth == 0: break
    text = text[:start] + uv_geometry() + text[end+1:]
    extras, links = [], []
    if variant == 'vector_factor': rough_factor = (.5, .5, .5)
    extra = ''
    if variant == 'glossiness': extra = 'P: "3dsMax|main|useGlossiness", "Number", "", "A", 1\n'
    if variant == 'advanced': extra = 'P: "TransparencyFactor", "Number", "", "A", .5\n'
    for i in range(copies):
        material_id = 600 + i
        extras.append(pbr_material(material_id, rough_factor, metal_factor, extra))
        owner = 100 if i == 0 else 10000 + i
        if i:
            extras += [model(owner, f'Mesh{i}', 'Mesh', (100, 200, 300)), uv_geometry(20000+i, f'Triangle{i}')]
            links += [f'C: "OO",{owner},0\n', f'C: "OO",{20000+i},{owner}\n']
        links.append(f'C: "OO",{material_id},{owner}\n')
        for slot, texture in (('roughness', 700), ('metalness', 710)):
            if (rough if slot == 'roughness' else metal) is not None:
                links.append(f'C: "OP",{texture},{material_id},"3dsMax|main|{slot}_map"\n')
        if retained:
            links.append(f'C: "OP",700,{material_id},"3dsMax|main|ao_map"\n')
    for identity, name, filename in ((700, 'Rough', rough), (710, 'Metal', metal)):
        if filename is not None:
            obj = texture_objects(identity, name, filename, variant == 'transform' and identity == 700)
            if identity == 710 and variant == 'wrap':
                obj = obj.replace('Properties70: {', 'Properties70: {\nP: "WrapModeU", "enum", "", "", 1\n', 1)
            if identity == 700 and variant == 'uv': obj = obj.replace('"UVMap"', '"MissingUV"')
            if identity == 700 and variant == 'shader':
                obj = obj.replace('Properties70: {', 'Properties70: {\n'
                    'P: "3dsMax|MaxTexture", "KString", "", "", "OSLMap"\n'
                    'P: "3dsMax|params|OSLShaderName", "KString", "", "", "OSLBitmap"\n'
                    'P: "3dsMax|parameters", "Compound", "", ""\n'
                    'P: "3dsMax|parameters|Filename", "KString", "", "", "rough.png"\n', 1)
            extras.append(obj)
            links.append(f'C: "OO",{identity+1},{identity}\n')
    if variant == 'decoded_budget':
        extras.append(texture_objects(720, 'TinyColor', 'tiny.png'))
        links += ['C: "OO",721,720\n', 'C: "OP",720,600,"3dsMax|main|base_color_map"\n']
    if variant == 'disconnected':
        extras.append(texture_objects(900, 'DormantMissing', 'never-read.png'))
        links.append('C: "OO",901,900\n')
    return text.replace('Objects: {\n', 'Objects: {\n' + ''.join(extras)).replace(
        'Connections: {\n', 'Connections: {\n' + ''.join(links))


def image_count_scene():
    # 86 distinct pairs => 172 transient inputs +86 packed objects=258, beyond256.
    text = scalar_scene(copies=86)
    objects, links = [], []
    for i in range(1, 86):
        for offset, label, filename in ((0, 'Rough', 'rough.png'), (2, 'Metal', 'metal.png')):
            identity = 30000 + i*4 + offset
            objects.append(texture_objects(identity, f'{label}{i}', filename))
            links.append(f'C: "OO",{identity+1},{identity}\n')
            slot = 'roughness' if offset == 0 else 'metalness'
            text = text.replace(f'C: "OP",{700 if offset == 0 else 710},{600+i},"3dsMax|main|{slot}_map"',
                                f'C: "OP",{identity},{600+i},"3dsMax|main|{slot}_map"')
    return text.replace('Objects: {\n', 'Objects: {\n'+''.join(objects)).replace(
        'Connections: {\n', 'Connections: {\n'+''.join(links))


def write_fixtures(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    fixtures = dict(two=scalar_scene(), rough_only=scalar_scene(metal=None),
        metal_only=scalar_scene(rough=None), no_maps=scalar_scene(rough=None, metal=None),
        absent_factors=scalar_scene(rough_factor=None, metal_factor=None),
        untextured_defaults=scalar_scene(rough=None, metal=None, rough_factor=None, metal_factor=None),
        zero_factors=scalar_scene(rough_factor=0, metal_factor=0),
        shared_pair=scalar_scene(copies=2), transient_retained=scalar_scene(retained=True, copies=2),
        disconnected=scalar_scene(rough=None, metal=None, variant='disconnected'),
        animated=scalar_scene(skin=True), missing=scalar_scene(rough='repair.png'),
        colored=scalar_scene(rough='colored.png'), alpha=scalar_scene(rough='alpha.png'),
        extent=scalar_scene(metal='extent.png'), wrap=scalar_scene(variant='wrap'),
        uv=scalar_scene(variant='uv'), transform=scalar_scene(variant='transform'),
        shader=scalar_scene(variant='shader'), vector_factor=scalar_scene(variant='vector_factor'),
        factor_high=scalar_scene(rough_factor=1.01), factor_negative=scalar_scene(metal_factor=-.01),
        glossiness=scalar_scene(variant='glossiness'), advanced=scalar_scene(variant='advanced'),
        escaping=scalar_scene(rough='../outside.png'), dimension=scalar_scene(rough='dimension.png', metal=None),
        budget_boundary=scalar_scene(rough='large-r.png', metal='large-m.png'),
        decoded_budget=scalar_scene(rough='large-r.png', metal='large-m.png', variant='decoded_budget'),
        image_count=image_count_scene())
    for parent, binary in ((directory, False), (directory/'binary', True)):
        parent.mkdir(exist_ok=True)
        for name, text in fixtures.items():
            (parent/(name+'.fbx')).write_bytes(binary_encode(text) if binary else text.encode('utf-8'))
        (parent/'rough.png').write_bytes(scalar_png(ROUGH))
        (parent/'metal.png').write_bytes(scalar_png(METAL))
        (parent/'colored.png').write_bytes(png(2, 2, [1, 2, 3, 255]*4))
        (parent/'alpha.png').write_bytes(scalar_png(ROUGH, alpha=254))
        (parent/'extent.png').write_bytes(scalar_png((0,), 1, 1))
        (parent/'tiny.png').write_bytes(scalar_png((255, 255, 255, 255)))
        constant_png(parent/'large-r.png', 2048, 2048, 64)
        constant_png(parent/'large-m.png', 2048, 2048, 192)
        constant_png(parent/'dimension.png', 2050, 2050, 64)
    manifest = {str(p.relative_to(directory)).replace('\\', '/'): dict(bytes=p.stat().st_size,
        sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in sorted(directory.rglob('*')) if p.is_file()}
    (directory/'fixture-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    return directory


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    write_fixtures(args.output)
