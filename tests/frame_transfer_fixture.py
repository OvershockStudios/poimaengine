#!/usr/bin/env python3
"""Original FBX bone-frame gauges and independent analytic motion/skin oracles.

No retargeter output or importer matrix helpers enter the expected results.
Centimeter FBX source: Root at (200,0,0), Child at world (200,100,0).
Donor Root axes rotate 90 degrees around Z; Child local axes cancel that roll.
The motion mixes Root Z rotation with Child X rotation and local translation.
"""
# SPDX-License-Identifier: Apache-2.0
import math
from pathlib import Path

from fbx_fixture import SECOND, array, model, prop, scene, write_ascii_fixtures


def multiply(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)]
            for i in range(4)]


def translation(x, y, z):
    return [[1, 0, 0, x], [0, 1, 0, y], [0, 0, 1, z], [0, 0, 0, 1]]


def rotation_x(degrees):
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    return [[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]]


def rotation_z(degrees):
    c, s = math.cos(math.radians(degrees)), math.sin(math.radians(degrees))
    return [[c, -s, 0, 0], [s, c, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]


def column_major(matrix):
    return [matrix[row][column] for column in range(4) for row in range(4)]


def point(matrix, value):
    return [sum(matrix[row][k] * value[k] for k in range(3)) + matrix[row][3]
            for row in range(3)]


def expected_motion(time):
    """Closed-form physical motion, independently of any frame conversion.

    Original base inverse bind: root identity, child translation (0,-1,0).
    Mesh local vertices are (0,0,0),(1,0,0),(0,1,0), with root-only,
    half root/child, and child-only weights, respectively.
    """
    root = multiply(translation(2, 0, 0), rotation_z(60 * time))
    child = multiply(multiply(root, translation(.3 * time, 1 + .2 * time, 0)),
                     rotation_x(40 * time))
    root_vertex = point(root, [1, 0, 0])
    child_vertex = point(child, [1, -1, 0])
    return dict(root=column_major(root), child=column_major(child),
                vertices=[point(root, [0, 0, 0]),
                          [(a + b) / 2 for a, b in zip(root_vertex, child_vertex)],
                          point(child, [0, 0, 0])])


def _animation(take_index, name, root_angle):
    stack, layer = 500 + take_index * 100, 501 + take_index * 100
    objects = [f'AnimationStack: {stack}, "AnimStack::{name}", "" {{\nProperties70: {{\n' +
               prop('LocalStart', 'KTime', [0]) + prop('LocalStop', 'KTime', [SECOND]) +
               prop('ReferenceStart', 'KTime', [0]) + prop('ReferenceStop', 'KTime', [SECOND]) + '}\n}\n',
               f'AnimationLayer: {layer}, "AnimLayer::Base", "" {{ }}\n']
    links = [f'C: "OO",{layer},{stack}\n']
    motion = name == 'Motion'
    tracks = [(101, 'R', {'X': (0, 0), 'Y': (0, 0),
                          'Z': (root_angle, root_angle + (60 if motion else 0))}),
              (102, 'R', {'X': (0, 40 if motion else 0), 'Y': (0, 0), 'Z': (-90, -90)}),
              (102, 'T', {'X': (100, 120 if motion else 100),
                          'Y': (0, -30 if motion else 0), 'Z': (0, 0)})]
    for track_index, (target, channel, axes) in enumerate(tracks):
        node = stack + 10 + track_index * 10
        property_name = 'Lcl Rotation' if channel == 'R' else 'Lcl Translation'
        objects.append(f'AnimationCurveNode: {node}, "AnimCurveNode::{channel}{target}", "" {{\nProperties70: {{\n' +
                       ''.join(prop('d|' + axis, 'Number', [values[0]]) for axis, values in axes.items()) + '}\n}\n')
        links.extend([f'C: "OO",{node},{layer}\n', f'C: "OP",{node},{target},"{property_name}"\n'])
        for axis_index, (axis, values) in enumerate(axes.items()):
            curve = node + axis_index + 1
            objects.append(f'AnimationCurve: {curve}, "AnimCurve::Value", "" {{\nDefault: {values[0]}\nKeyVer: 4008\n' +
                           array('KeyTime', [0, SECOND]) + array('KeyValueFloat', values) +
                           array('KeyAttrFlags', [24836]) + array('KeyAttrDataFloat', [0, 0, 0, 0]) +
                           array('KeyAttrRefCount', [2]) + '}\n')
            links.append(f'C: "OP",{curve},{node},"d|{axis}"\n')
    return ''.join(objects), ''.join(links)


def donor_scene(*, sampled_reference=False, aligned=False, proportion=False,
                nonuniform=False):
    text = scene(donor=True)
    original_root = model(101, 'Root', 'LimbNode', (200.0, 0.0, 0.0))
    original_child = model(102, 'Child', 'LimbNode', (0.0, 100.0, 0.0))
    angle = 0 if aligned else 90
    root_position = (0, -200, 0) if aligned else (200, 0, 0)
    root_default = angle - 20 if sampled_reference else angle
    child_default = (75, 0, 0) if sampled_reference else (110 if proportion else 100, 0, 0)
    child_rotation = -60 if sampled_reference else -90
    root = model(101, 'Root', 'LimbNode', root_position, rotation=(0, 0, root_default))
    child = model(102, 'Child', 'LimbNode', child_default, rotation=(0, 0, child_rotation))
    if nonuniform:
        child = child.replace(prop('Lcl Scaling', 'Lcl Scaling', (1, 1, 1)),
                              prop('Lcl Scaling', 'Lcl Scaling', (1, 1.1, 1)))
    if text.count(original_root) != 1 or text.count(original_child) != 1:
        raise RuntimeError('Original FBX fixture hierarchy changed')
    text = text.replace(original_root, root).replace(original_child, child)
    objects, links = '', ''
    for index, name in enumerate(('Reference', 'Motion')):
        more_objects, more_links = _animation(index, name, angle)
        objects += more_objects
        links += more_links
    marker = '}\nConnections: {\n'
    if text.count(marker) != 1:
        raise RuntimeError('Original FBX object/connection delimiter changed')
    return text.replace(marker, objects + marker + links, 1)


def write_fixtures(directory):
    directory = write_ascii_fixtures(Path(directory))
    # A target reference sampled from its first original Move take at zero is
    # deliberately the same physical stance as its imported default pose.
    (directory / 'base_with_reference.fbx').write_text(scene(skin=True, takes=('Move', 'Turn')),
                                                       encoding='utf-8', newline='\n')
    variants = dict(gauge_rest={}, gauge_sample=dict(sampled_reference=True),
                    gauge_aligned=dict(aligned=True), gauge_proportion=dict(proportion=True),
                    gauge_scale=dict(nonuniform=True))
    for name, options in variants.items():
        (directory / (name + '.fbx')).write_text(donor_scene(**options), encoding='utf-8', newline='\n')
    return directory
