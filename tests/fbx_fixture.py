#!/usr/bin/env python3
"""Original analytic ASCII/binary FBX 7.4 fixtures; no downloaded asset content.

Default coordinates are centimeters; explicit variants use meters or Z-up.
The skin has a mesh translated 200cm in X,
a root joint at that same point, and a child 100cm above the root. Vertex
weights are root-only, half/half, child-only. Move translates the child 100cm
in X over one second; Turn rotates the root 90 degrees about Z.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
from pathlib import Path

SECOND = 46186158000


def array(name, values):
    return f'{name}: *{len(values)} {{ a: ' + ','.join(map(str, values)) + ' }\n'


def matrix(x=0, y=0, z=0):
    return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1]


def prop(name, kind, values):
    return f'P: "{name}", "{kind}", "", "A",' + ','.join(map(str, values)) + '\n'


def model(identity, name, kind, position=(0, 0, 0), geometry=(0, 0, 0), rotation=(0, 0, 0)):
    return f'Model: {identity}, "Model::{name}", "{kind}" {{\nVersion: 232\nProperties70: {{\n' + \
        prop('Lcl Translation', 'Lcl Translation', position) + \
        prop('Lcl Rotation', 'Lcl Rotation', rotation) + \
        prop('Lcl Scaling', 'Lcl Scaling', (1, 1, 1)) + \
        prop('GeometricTranslation', 'Vector3D', geometry) + '}\nShading: T\nCulling: "CullingOff"\n}\n'


def scene(*, skin=False, donor=False, takes=(), z_up=False, geometric=False,
          mismatch=False, dq=False, overflow=False, blend=False, unit_cm=1,
          canonical_z_donor=False, rotated_bind=False, start_seconds=0):
    objects, links = [], []
    scale = 1 / unit_cm
    def distance(values):
        return tuple(value * scale for value in values)
    top_rotation = (90, 0, 0) if canonical_z_donor else (0, 0, 0)
    def connect(child, parent, property_name=None):
        links.append(f'C: "{"OP" if property_name else "OO"}",{child},{parent}' +
                     (f',"{property_name}"' if property_name else '') + '\n')
    objects.append(model(100, 'Mesh', 'Mesh' if not donor else 'Null',
                         distance((200, 0, 0) if skin or donor else (100, 200, 300)),
                         distance((20, 30, 40) if geometric else (0, 0, 0)), top_rotation))
    connect(100, 0)
    if not donor:
        objects.append('Geometry: 200, "Geometry::Triangle", "Mesh" {\n' +
                       array('Vertices', distance([0, 0, 0, 100, 0, 0, 0, 100, 0])) +
                       array('PolygonVertexIndex', [0, 1, -3]) +
                       'LayerElementNormal: 0 {\nVersion: 101\nName: ""\nMappingInformationType: "ByPolygonVertex"\n'
                       'ReferenceInformationType: "Direct"\n' + array('Normals', [0, 0, 1] * 3) + '}\n'
                       'Layer: 0 { Version: 100\nLayerElement: { Type: "LayerElementNormal"\nTypedIndex: 0 }\n}\n}\n')
        connect(200, 100)
    if skin or donor:
        objects += [model(101, 'Root', 'LimbNode', distance((200, 0, 0)), rotation=top_rotation),
                    model(102, 'Child', 'LimbNode', distance((0, 101 if mismatch else 100, 0)),
                          rotation=(0, 0, 90) if rotated_bind else (0, 0, 0))]
        connect(101, 0); connect(102, 101)
    if skin:
        objects.append('Deformer: 300, "Deformer::Skin", "Skin" {\nVersion: 101\nLink_DeformAcuracy: 50\n'
                       f'SkinningType: "{"DualQuaternion" if dq else "Linear"}"\n}}\n')
        connect(300, 200)
        child_bind = matrix(200*scale, 100*scale)
        if rotated_bind:
            child_bind[0:8] = [0, 1, 0, 0, -1, 0, 0, 0]
        joints = [(101, [0, 1], [1, .5], matrix(200*scale)),
                  (102, [1, 2], [.5, 1], child_bind)]
        if overflow:
            for i in range(3):
                identity = 103 + i
                objects.append(model(identity, f'Extra{i}', 'LimbNode', distance((200, 0, 0))))
                connect(identity, 0)
                joints.append((identity, [1], [.25], matrix(200*scale)))
        for i, (joint, indices, weights, bind) in enumerate(joints):
            identity = 310 + i
            inverse_bind = matrix(200*scale-bind[12], -bind[13], -bind[14])
            if rotated_bind and joint == 102:
                inverse_bind = [0,-1,0,0, 1,0,0,0, 0,0,1,0, -100*scale,0,0,1]
            objects.append(f'Deformer: {identity}, "SubDeformer::Joint{i}", "Cluster" {{\nVersion: 100\n' +
                           array('Indexes', indices) + array('Weights', weights) +
                           # FBX Cluster.Transform is the mesh-node-to-bone bind
                           # transform, not the mesh's world translation.
                           array('Transform', inverse_bind) +
                           array('TransformLink', bind) + '}\n')
            connect(identity, 300); connect(joint, identity)
    if blend:
        objects.append('Deformer: 390, "Deformer::Morph", "BlendShape" { Version: 100 }\n')
        connect(390, 200)
    for i, take in enumerate(takes):
        stack, layer, curve_node, curve = (400 + i * 10 + n for n in range(4))
        turn = take == 'Turn'
        objects.append(f'AnimationStack: {stack}, "AnimStack::{take}", "" {{\nProperties70: {{\n' +
                       prop('LocalStart', 'KTime', [start_seconds*SECOND]) + prop('LocalStop', 'KTime', [(start_seconds+1)*SECOND]) +
                       prop('ReferenceStart', 'KTime', [start_seconds*SECOND]) + prop('ReferenceStop', 'KTime', [(start_seconds+1)*SECOND]) + '}\n}\n')
        objects.append(f'AnimationLayer: {layer}, "AnimLayer::Base", "" {{ }}\n')
        objects.append(f'AnimationCurveNode: {curve_node}, "AnimCurveNode::{"R" if turn else "T"}", "" {{\nProperties70: {{\n' +
                       prop('d|X', 'Number', [0]) + prop('d|Y', 'Number', [0 if turn else 100*scale]) +
                       prop('d|Z', 'Number', [0]) + '}\n}\n')
        objects.append(f'AnimationCurve: {curve}, "AnimCurve::Value", "" {{\nDefault: 0\nKeyVer: 4008\n' +
                       array('KeyTime', [start_seconds*SECOND, (start_seconds+1)*SECOND]) + array('KeyValueFloat', [0, 90 if turn else 100*scale]) +
                       array('KeyAttrFlags', [24836]) + array('KeyAttrDataFloat', [0, 0, 0, 0]) +
                       array('KeyAttrRefCount', [2]) + '}\n')
        connect(layer, stack); connect(curve_node, layer)
        connect(curve_node, 101 if turn else 102, 'Lcl Rotation' if turn else 'Lcl Translation')
        connect(curve, curve_node, 'd|Z' if turn else 'd|X')
    axes = (2, 1, 1, -1) if z_up or canonical_z_donor else (1, 1, 2, 1)
    settings = ''.join(prop(name, 'int', [value]) for name, value in zip(
        ['UpAxis', 'UpAxisSign', 'FrontAxis', 'FrontAxisSign', 'CoordAxis', 'CoordAxisSign'], (*axes, 0, 1)))
    return '; FBX 7.4.0 project file\n; Original Poima analytic fixture, SPDX-License-Identifier: Apache-2.0\n' \
        'FBXHeaderExtension: { FBXHeaderVersion: 1003\nFBXVersion: 7400\nCreator: "Poima analytic test"\n}\n' \
        'GlobalSettings: { Version: 1000\nProperties70: {\n' + settings + \
        prop('UnitScaleFactor', 'double', [unit_cm]) + prop('OriginalUnitScaleFactor', 'double', [unit_cm]) + \
        '}\n}\nObjects: {\n' + ''.join(objects) + '}\nConnections: {\n' + ''.join(links) + '}\n'


def write_ascii_fixtures(directory):
    directory = Path(directory); directory.mkdir(parents=True, exist_ok=True)
    cases = {
        'static': {}, 'geometric': dict(geometric=True), 'z_up': dict(z_up=True),
        'skin': dict(skin=True), 'animated': dict(skin=True, takes=('Move', 'Turn')),
        'move_donor': dict(donor=True, takes=('Move',)),
        'turn_donor': dict(donor=True, takes=('Turn',)),
        'meter_move_donor': dict(donor=True, takes=('Move',), unit_cm=100),
        'z_up_move_donor': dict(donor=True, takes=('Move',), canonical_z_donor=True),
        'rotated_bind': dict(skin=True, takes=('Move', 'Turn'), rotated_bind=True),
        'shifted_takes': dict(skin=True, takes=('Move', 'Turn'), start_seconds=2),
        'rest_mismatch': dict(donor=True, mismatch=True, takes=('Move',)),
        'dual_quaternion': dict(skin=True, dq=True), 'influence_overflow': dict(skin=True, overflow=True),
        'blend_shape': dict(blend=True),
    }
    for name, options in cases.items():
        (directory / (name + '.fbx')).write_text(scene(**options), encoding='utf-8', newline='\n')
    excessive = scene().replace('Objects: {\n', 'Objects: {\n' + ''.join(
        f'Model: {1000+i}, "Model::Extra{i}", "Null" {{ }}\n' for i in range(10001)))
    excessive = excessive.replace('Connections: {\n', 'Connections: {\n' + ''.join(
        f'C: "OO",{1000+i},0\n' for i in range(10001)))
    (directory / 'node_overflow.fbx').write_text(excessive, encoding='utf-8', newline='\n')
    excessive_position = scene().replace('"A",100.0,200.0,300.0', '"A",100000000000000,200,300')
    (directory / 'position_overflow.fbx').write_text(excessive_position, encoding='utf-8', newline='\n')
    (directory / 'truncated.fbx').write_bytes(b'Kaydara FBX Binary  \x00\x1a\x00')
    return directory



# Restricted original FBX7.4 binary serializer; no third-party exporter code.
from dataclasses import dataclass
import re
import struct
from texture_fixture import png

@dataclass
class Array:
    kind: str
    values: list


@dataclass
class Node:
    name: str
    properties: list
    children: list


ARRAY_KINDS = {
    'Colors': 'd', 'ColorIndex': 'i', 'UV': 'd', 'Materials': 'i', 'Vertices': 'd', 'Normals': 'd', 'Weights': 'd',
    'Transform': 'd', 'TransformLink': 'd',
    'PolygonVertexIndex': 'i', 'Indexes': 'i',
    'KeyTime': 'l', 'KeyValueFloat': 'f', 'KeyAttrFlags': 'i',
    'KeyAttrDataFloat': 'f', 'KeyAttrRefCount': 'i',
}
TOKEN = re.compile(r'\s+|;[^\n]*|"[^"\n]*"|[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?|[A-Za-z_][A-Za-z_0-9]*|[{}:,*]')


def parse(source):
    tokens, offset = [], 0
    while offset < len(source):
        match = TOKEN.match(source, offset)
        if not match:
            raise ValueError(f'Unsupported fixture syntax at {offset}')
        value = match.group()
        if not value.isspace() and not value.startswith(';'):
            tokens.append(value)
        offset = match.end()
    index = 0

    def pop(expected=None):
        nonlocal index
        if index >= len(tokens):
            raise ValueError('Unexpected fixture end')
        value = tokens[index]; index += 1
        if expected is not None and value != expected:
            raise ValueError(f'Expected {expected}, got {value}')
        return value

    def scalar():
        value = pop()
        if value.startswith('"'):
            return value[1:-1]
        if value in ('T', 'F'):
            return value == 'T'
        return float(value) if any(c in value for c in '.eE') else int(value)

    def nodes(nested=False):
        result = []
        while index < len(tokens) and tokens[index] != '}':
            name = pop(); pop(':'); properties = []; children = []
            if index < len(tokens) and tokens[index] == '*':
                pop('*'); count = int(pop()); pop('{'); pop('a'); pop(':')
                values = []
                while tokens[index] != '}':
                    values.append(scalar())
                    if tokens[index] != '}':
                        pop(',')
                pop('}')
                if len(values) != count or name not in ARRAY_KINDS:
                    raise ValueError('Unsupported/mismatched fixture array')
                properties.append(Array(ARRAY_KINDS[name], values))
            else:
                # A following Name: is the next sibling, even on the same line.
                if index < len(tokens) and tokens[index] not in ('{', '}') and not (
                        index+1 < len(tokens) and tokens[index+1] == ':'):
                    properties.append(scalar())
                    while index < len(tokens) and tokens[index] == ',':
                        pop(','); properties.append(scalar())
                if index < len(tokens) and tokens[index] == '{':
                    pop('{'); children = nodes(True); pop('}')
            result.append(Node(name, properties, children))
        if not nested and index != len(tokens):
            raise ValueError('Unexpected root closing brace')
        return result
    return nodes()


def property_bytes(value):
    if isinstance(value, Array):
        fmt = {'d': 'd', 'f': 'f', 'i': 'i', 'l': 'q'}[value.kind]
        payload = struct.pack('<' + fmt*len(value.values), *value.values)
        return value.kind.encode('ascii') + struct.pack('<III', len(value.values), 0, len(payload)) + payload
    if isinstance(value, bool):
        return b'C' + bytes([int(value)])
    if isinstance(value, int):
        return b'L' + struct.pack('<q', value)
    if isinstance(value, float):
        return b'D' + struct.pack('<d', value)
    if isinstance(value, str):
        # FBX binary uses name\0\1type where ASCII uses type::name.
        if '::' in value:
            kind, name = value.split('::', 1)
            value = name + '\0\1' + kind
        encoded = value.encode('utf-8')
        return b'S' + struct.pack('<I', len(encoded)) + encoded
    raise TypeError(type(value))


def binary_encode(source):
    def node_bytes(node, begin):
        name = node.name.encode('ascii')
        if len(name) > 255:
            raise ValueError('Node name too long')
        props = b''.join(property_bytes(value) for value in node.properties)
        position = begin + 13 + len(name) + len(props)
        children = []
        for child in node.children:
            data = node_bytes(child, position); children.append(data); position += len(data)
        if children:
            children.append(bytes(13)); position += 13
        return struct.pack('<IIIB', position, len(node.properties), len(props), len(name)) + name + props + b''.join(children)

    header = b'Kaydara FBX Binary  \0\x1a\0' + struct.pack('<I', 7400)
    result = bytearray(header)
    for node in parse(source):
        result.extend(node_bytes(node, len(result)))
    result.extend(bytes(13))
    return bytes(result)



def texture_objects(identity, name, filename, transformed=False):
    # Identity's next integer belongs to its video/source image object.
    return f'''
Texture: {identity}, "Texture::{name}", "" {{
    Type: "TextureVideoClip"
    Version: 202
    TextureName: "Texture::{name}"
    FileName: "{filename}"
    RelativeFilename: "{filename}"
    ModelUVTranslation: 0,0
    ModelUVScaling: 1,1
    Texture_Alpha_Source: "None"
    Cropping: 0,0,0,0
    Properties70: {{
        P: "UVSet", "KString", "", "", "UVMap"
        P: "Translation", "Vector", "", "A", {0.125 if transformed else 0},0,0
        P: "Scaling", "Vector", "", "A", 1,1,1
        P: "Rotation", "Vector", "", "A", 0,0,0
    }}
}}
Video: {identity+1}, "Video::{name}", "Clip" {{
    Type: "Clip"
    Filename: "{filename}"
    RelativeFilename: "{filename}"
}}
'''


def textured(*, color='color.png', normal=False, transformed=False, normal_filename='normal.png'):
    text = scene()
    layers = '''LayerElementUV: 0 {
Version: 101
Name: "UVMap"
MappingInformationType: "ByPolygonVertex"
ReferenceInformationType: "Direct"
''' + array('UV', [.25, .25, .75, .25, .25, .75]) + '''}
LayerElementMaterial: 0 {
Version: 101
Name: ""
MappingInformationType: "AllSame"
ReferenceInformationType: "IndexToDirect"
''' + array('Materials', [0]) + '''}
'''
    marker = 'Layer: 0 { Version: 100\n'
    text = text.replace(marker, layers + marker +
                        'LayerElement: { Type: "LayerElementUV"\nTypedIndex: 0 }\n'
                        'LayerElement: { Type: "LayerElementMaterial"\nTypedIndex: 0 }\n')
    material = '''Material: 600, "Material::Analytic", "" {
Version: 102
ShadingModel: "lambert"
MultiLayer: 0
Properties70: {
    P: "DiffuseColor", "Color", "", "A", 1,1,1
    P: "DiffuseFactor", "Number", "", "A", 1
    P: "TransparencyFactor", "Number", "", "A", 0
}}
'''
    material += texture_objects(610, 'Color', color, transformed)
    links = 'C: "OO",600,100\nC: "OP",610,600,"DiffuseColor"\nC: "OO",611,610\n'
    if normal:
        material += texture_objects(620, 'NormalOpenGL', normal_filename)
        links += 'C: "OP",620,600,"NormalMap"\nC: "OO",621,620\n'
    text = text.replace('Objects: {\n', 'Objects: {\n' + material)
    return text.replace('Connections: {\n', 'Connections: {\n' + links)



def colored(values=None, *, extra_layer=False, invalid_index=False):
    text = scene()
    values = [1]*12 if values is None else values
    layer = '''LayerElementColor: 0 {
Version: 101
Name: "Original vertex colors"
MappingInformationType: "ByPolygonVertex"
ReferenceInformationType: "REF"
'''.replace('REF', 'IndexToDirect' if invalid_index else 'Direct')
    layer += array('Colors', values)
    if invalid_index:
        layer += array('ColorIndex', [0, 1, 999])
    layer += '}\n'
    if extra_layer:
        layer += layer.replace('LayerElementColor: 0', 'LayerElementColor: 1')
        layer += 'Layer: 1 { Version: 100\nLayerElement: { Type: "LayerElementColor"\nTypedIndex: 1 }\n}\n'
    marker = 'Layer: 0 { Version: 100\n'
    text = text.replace(marker, layer+marker+'LayerElement: { Type: "LayerElementColor"\nTypedIndex: 0 }\n')
    return text


def parser_policy_cases():
    # Inactive material properties must not change the cooked response.
    material = '''Material: 600, "Material::InactiveNormal", "" {
Version: 102
ShadingModel: "lambert"
Properties70: {
P: "DiffuseColor", "Color", "", "A", 1,1,1
P: "DiffuseFactor", "Number", "", "A", 1
P: "NormalMap", "Vector3D", "", "A", 1,2,3
}}
'''
    inactive = scene().replace('Objects: {\n', 'Objects: {\n'+material)
    inactive = inactive.replace('Connections: {\n', 'Connections: {\nC: "OO",600,100\n')
    active = textured(normal=True).replace('    P: "DiffuseColor"',
        '    P: "NormalMap", "Vector3D", "", "A", 1,2,3\n    P: "DiffuseColor"')
    duplicate = scene().replace('Objects: {\n', 'Objects: {\n'+model(100, 'Duplicate', 'Null'))
    # Two genuine faces, six valid normals/UVs, but only one ByPolygon material.
    # Pinned ufbx repairs this by repeating the last material and warns.
    truncated = textured().replace(array('PolygonVertexIndex', [0,1,-3]),
                                    array('PolygonVertexIndex', [0,1,-3,0,1,-3]))
    truncated = truncated.replace(array('Normals', [0,0,1]*3), array('Normals', [0,0,1]*6))
    truncated = truncated.replace(array('UV', [.25,.25,.75,.25,.25,.75]),
                                  array('UV', [.25,.25,.75,.25,.25,.75]*2))
    truncated = truncated.replace('MappingInformationType: "AllSame"',
                                  'MappingInformationType: "ByPolygon"')
    return dict(inactive_normal=inactive, active_normal_response=active,
                duplicate_object_id=duplicate, truncated_materials=truncated)


def collapsed_uv(*, mapped=False):
    text = textured(normal=mapped)
    # A genuine YZ triangle with +X normals. Mikk's default +X fallback
    # for completely collapsed UVs is not a valid tangent to this plane.
    text = text.replace(array('Vertices', [0.,0.,0.,100.,0.,0.,0.,100.,0.]),
                        array('Vertices', [0,0,0,0,100,0,0,0,100]))
    text = text.replace(array('Normals', [0,0,1]*3), array('Normals', [1,0,0]*3))
    text = text.replace(array('UV', [.25,.25,.75,.25,.25,.75]), array('UV', [.25,.25]*3))
    if not mapped:
        text = text.replace('C: "OP",610,600,"DiffuseColor"\n', '')
    return text



def write_fixtures(directory):
    directory = write_ascii_fixtures(directory)
    color = png(2, 2, [255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255])
    normal = png(2, 2, [128,204,230,255]*4)
    (directory / 'color.png').write_bytes(color)
    (directory / 'normal.png').write_bytes(normal)
    (directory.parent / 'outside.png').write_bytes(color)
    for name, options in {
        'texture': {}, 'normal_map': dict(normal=True),
        'shared_normal_map': dict(normal=True, normal_filename='color.png'),
        'missing_texture': dict(color='absent.png'),
        'escaping_texture': dict(color='../outside.png'),
        'uv_transform': dict(transformed=True),
    }.items():
        (directory / (name + '.fbx')).write_text(textured(**options), encoding='utf-8', newline='\n')
    tinted = [1]*12; tinted[4] = .25
    alpha = [1]*12; alpha[11] = .5
    color_cases = {
        'white_color': colored(), 'nonwhite_color': colored(tinted),
        'nonidentity_alpha': colored(alpha), 'multiple_colors': colored(extra_layer=True),
        'invalid_color_index': colored(invalid_index=True),
    }
    for name, text in color_cases.items():
        (directory/(name+'.fbx')).write_text(text, encoding='utf-8', newline='\n')
    for name, text in parser_policy_cases().items():
        (directory/(name+'.fbx')).write_text(text, encoding='utf-8', newline='\n')
    for name, mapped in [('collapsed_uv', False), ('mapped_collapsed_uv', True)]:
        (directory/(name+'.fbx')).write_text(collapsed_uv(mapped=mapped), encoding='utf-8', newline='\n')
    # Valid binary framing with a genuine IEEE quiet NaN in the color array.
    valid = binary_encode(color_cases['white_color'])
    prefix = b'Colorsd'+struct.pack('<III', 12, 0, 96)
    assert valid.count(prefix) == 1
    invalid = valid.replace(prefix+struct.pack('<d', 1), prefix+struct.pack('<d', float('nan')), 1)
    assert len(invalid) == len(valid) and invalid != valid
    (directory/'nonfinite_color.fbx').write_bytes(invalid)
    binary = directory / 'binary'; binary.mkdir(exist_ok=True)
    (binary / 'color.png').write_bytes(color)
    (binary / 'normal.png').write_bytes(normal)
    (directory / 'outside.png').write_bytes(color)
    for source in sorted(directory.glob('*.fbx')):
        content = source.read_bytes()
        data = content if content.startswith(b'Kaydara FBX Binary') else binary_encode(content.decode('utf-8'))
        (binary / source.name).write_bytes(data)
    return directory


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    write_fixtures(parser.parse_args().directory)
