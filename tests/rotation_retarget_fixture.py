#!/usr/bin/env python3
"""Original proportion-changing FBX fixtures and closed-form target skin oracle."""
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
from frame_transfer_fixture import (column_major, multiply, point, rotation_x,
                                    rotation_z, translation, write_fixtures as frame_fixtures)
from fbx_fixture import model, prop, scene


def write_fixtures(directory):
    directory = frame_fixtures(Path(directory))
    original = model(102, 'Child', 'LimbNode', (0.0, 100.0, 0.0))
    scaled = original.replace(prop('Lcl Scaling', 'Lcl Scaling', (1, 1, 1)),
                              prop('Lcl Scaling', 'Lcl Scaling', (1, 1.25, 1)))
    target = scene(skin=True)
    if target.count(original) != 1:
        raise RuntimeError('Original target fixture changed')
    (directory/'target_scale.fbx').write_text(target.replace(original, scaled), encoding='utf-8', newline='\n')
    return directory


def expected_motion(time, *, delta_scale=None, child_y=1, child_x=0,
                    child_scale=(1, 1, 1), stationary=False):
    """Independent handwritten target FK using the ORIGINAL target inverse binds.

    Motion orientation is Root Rz(60*t), Child Rx(40*t), regardless of donor
    proportions/scales. Target-reference translations stay fixed. Explicit
    Child delta maps donor X/Y into target Y/-X, yielding (.3*t,.2*t,0).
    """
    root = multiply(translation(2, 0, 0), rotation_z(0 if stationary else 60*time))
    p = translation(child_x + (0 if delta_scale is None else .3*time*delta_scale),
                    child_y + (0 if delta_scale is None else .2*time*delta_scale), 0)
    scale = [[child_scale[0], 0, 0, 0], [0, child_scale[1], 0, 0],
             [0, 0, child_scale[2], 0], [0, 0, 0, 1]]
    child = multiply(multiply(multiply(root, p), rotation_x(0 if stationary else 40*time)), scale)
    root_vertex, child_vertex = point(root, [1, 0, 0]), point(child, [1, -1, 0])
    return dict(root=column_major(root), child=column_major(child),
                vertices=[point(root, [0, 0, 0]),
                          [(a+b)/2 for a, b in zip(root_vertex, child_vertex)],
                          point(child, [0, 0, 0])])
