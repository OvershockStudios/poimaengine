#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Diagnostic reveal history, geometric edge distance, and matched-static residuals.
No quality threshold or overall qualification is inferred from these measurements.
"""
import argparse
import hashlib
import json
import math
import sys
from pathlib import Path

import reconstruction_quality as oracle

require = oracle.require


def edge_distance(point, polygons):
    best = math.inf
    for polygon in polygons:
        for a, b in zip(polygon, polygon[1:] + polygon[:1]):
            dx, dy = b[0]-a[0], b[1]-a[1]
            length = dx*dx + dy*dy
            require(length > 0, 'Degenerate polygon edge')
            t = max(0, min(1, ((point[0]-a[0])*dx+(point[1]-a[1])*dy)/length))
            best = min(best, math.hypot(point[0]-a[0]-t*dx, point[1]-a[1]-t*dy))
    return None if math.isinf(best) else best


class RevealTracker:
    """Mixed coverage does not erase the last unambiguous visible identity."""
    def __init__(self, count):
        self.last = [None]*count
        self.reveals = [None]*count

    def update(self, sequence, owners):
        for i, owner in enumerate(owners):
            if owner == -2:
                continue
            before = self.last[i]
            if before is not None and before >= 0 and (owner == -1 or owner > before):
                self.reveals[i] = (sequence, before, owner)
            elif before != owner:
                self.reveals[i] = None
            self.last[i] = owner
        return self.reveals


def age_bin(age):
    if age is None: return 'not_revealed'
    if age <= 2: return str(age)
    if age <= 4: return '3-4'
    if age <= 8: return '5-8'
    if age <= 16: return '9-16'
    return '17+'


def distance_bin(distance):
    if distance is None: return 'no_edges'
    if distance < 1: return '<1'
    if distance < 2: return '1-2'
    if distance < 4: return '2-4'
    return '>=4'


def accumulate(table, key, residual, location):
    row = table.setdefault(key, {'pixels': 0, 'absolute_sum': 0., 'squared_sum': 0.,
                                'max': -1., 'worst': None})
    errors = [abs(v) for v in residual]
    row['pixels'] += 1
    row['absolute_sum'] += sum(errors)
    row['squared_sum'] += sum(v*v for v in residual)
    if max(errors) > row['max']:
        row['max'] = max(errors)
        row['worst'] = dict(location, residual_rgb=list(residual))


def finalize(table):
    result = []
    for key, row in sorted(table.items()):
        row = dict(row)
        count = row['pixels']*3
        row['mae'] = row.pop('absolute_sum')/count
        row['rmse'] = math.sqrt(row.pop('squared_sum')/count)
        result.append(dict(bin=key, **row))
    return result


def geometry(frame, width, height):
    layers = []
    last_depth = 0
    for layer in frame['layers']:
        polygon, depth = oracle.project(layer['vertices_world'], frame['camera'], width, height)
        require(depth[0] >= last_depth-1e-8, 'Layers must be depth-separated front-to-back')
        last_depth = depth[1]
        layers.append(dict(polygon=polygon, radiance=layer['radiance']))
    return layers


def evaluate(path, static_path=None):
    from scene_capture import pixels
    document = json.loads(path.read_text())
    require(document['format'] == 'poima.temporal-quality-input.v1' and document.get('complete'), 'Incomplete/unknown manifest')
    frames = document['frames']; w, h = document['width'], document['height']
    require(1 <= len(frames) <= 128, 'Frame budget exceeded')
    modes = set(frames[0]['captures'])
    count = len(frames[0]['layers'])
    stationary = [all(f['layers'][i] == frames[0]['layers'][i] for f in frames) for i in range(count)]
    static = json.loads(static_path.read_text()) if static_path else None
    if static:
        require(static['format'] == document['format'] and static.get('complete') and
                (static['width'], static['height']) == (w, h), 'Static manifest mismatch')
        static_frames = {f['sequence']: f for f in static['frames']}
        require(len(static_frames) == len(static['frames']), 'Duplicate static sequences')
    tracker = RevealTracker(w*h)
    tables = {mode: {} for mode in modes}; matched = {mode: {} for mode in modes}
    contamination = {mode: {} for mode in modes}; hashes = {}; last_sequence = None
    for frame in frames:
        seq = frame['sequence']
        require(type(seq) is int and (last_sequence is None or seq == last_sequence+1), 'Nonconsecutive sequence')
        last_sequence = seq
        require(set(frame['captures']) == modes and len(frame['layers']) == count, 'Modes/layer identities changed')
        layers = geometry(frame, w, h); owners = []
        reference, _, _ = oracle.reference(w, h, layers, frame['background'], frame['exposure'], owners)
        reveals = tracker.update(seq, owners)
        static_owners = None; static_frame = None
        if static:
            require(seq in static_frames, 'Missing matched static frame')
            static_frame = static_frames[seq]
            require(all(static_frame[k] == frame[k] for k in ('camera', 'background', 'exposure')), 'Static camera/background/exposure differs')
            require(set(static_frame['captures']) == modes, 'Static modes differ')
            offset = count-len(static_frame['layers'])
            require(offset >= 0 and frame['layers'][offset:] == static_frame['layers'], 'Static layers must match unchanged farther suffix')
            static_owners = []
            oracle.reference(w, h, geometry(static_frame, w, h), frame['background'], frame['exposure'], static_owners)
        def load(base, name):
            image_path = base/name; data = image_path.read_bytes()
            hashes[str(image_path)] = hashlib.sha256(data).hexdigest()
            image = pixels(image_path)
            require(len(image) == h and all(len(row) == w for row in image), 'Capture extent differs')
            return [tuple(c/255 for c in pixel) for row in image for pixel in row]
        images = {mode: load(path.parent, name) for mode, name in frame['captures'].items()}
        static_images = {mode: load(static_path.parent, name) for mode, name in static_frame['captures'].items()} if static_frame else {}
        for i, owner in enumerate(owners):
            if owner == -2: continue
            x, y = i % w, i // w
            reveal = reveals[i]; age = seq-reveal[0] if reveal else None
            point = (x+.5, y+.5)
            distances = {name: edge_distance(point, [l['polygon'] for j, l in enumerate(layers) if select(j)])
                         for name, select in (('all', lambda j: True), ('stationary', lambda j: stationary[j]), ('moving', lambda j: not stationary[j]))}
            key = '|'.join(('background' if owner == -1 else 'surface', 'age='+age_bin(age)) + tuple(name+'='+distance_bin(value) for name, value in distances.items()))
            location = dict(sequence=seq, x=x, y=y, owner=owner, reveal_age=age, edge_distance_pixels=distances)
            for mode, image in images.items():
                accumulate(tables[mode], key, [a-b for a,b in zip(image[i], reference[i])], location)
                if static_owners is not None and (owner == -1 and static_owners[i] == -1 or owner >= offset and static_owners[i] == owner-offset):
                    residual = [a-b for a,b in zip(image[i], static_images[mode][i])]
                    accumulate(matched[mode], key, residual, location)
                    if reveal:
                        departed = [oracle.display(c, frame['exposure']) for c in layers[reveal[1]]['radiance']]
                        direction = [a-b for a,b in zip(departed, reference[i])]
                        norm = sum(c*c for c in direction)
                        if norm > 1e-12:
                            projection = sum(a*b for a,b in zip(residual, direction))/norm
                            # Signed fractional contrast toward the departed surface, not a pass gate.
                            row = contamination[mode].setdefault(key, {'pixels':0, 'signed_sum':0., 'max_signed':-math.inf, 'worst':None})
                            row['pixels'] += 1; row['signed_sum'] += projection
                            if projection > row['max_signed']:
                                row['max_signed'] = projection; row['worst'] = location
    result = {'quality_qualified':False, 'status':'diagnostic only; no acceptance thresholds',
              'limitations':['Distances are pixel-center distances to all authored polygon edges, including occluded edges.',
                             'Mixed pixels are excluded; age starts at first fully revealed sample and bridges mixed ownership.',
                             'Matched-static differences include temporal filter history and are not solely stale color.',
                             'Layer indices must retain stable identity throughout the manifest.'],
              'source_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              'manifest_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
              'static_manifest_sha256':hashlib.sha256(static_path.read_bytes()).hexdigest() if static_path else None,
              'capture_hashes':hashes, 'modes':{}}
    for mode in sorted(modes):
        projected = []
        for key,row in sorted(contamination[mode].items()):
            row = dict(row); row['mean_signed'] = row.pop('signed_sum')/row['pixels']; projected.append(dict(bin=key, **row))
        result['modes'][mode] = {'reference_error_bins':finalize(tables[mode]), 'matched_static_residual_bins':finalize(matched[mode]), 'departed_color_contrast_bins':projected}
    return result


def self_test():
    t = RevealTracker(1)
    require(t.update(1,[0]) == [None], 'First visibility is not a reveal')
    t.update(2,[-2]); require(t.update(3,[1])[0] == (3,0,1), 'Mixed ownership lost far-surface reveal')
    t.update(4,[-2]); require(t.update(5,[1])[0] == (3,0,1), 'Mixed boundary incorrectly restarted age')
    require(t.update(6,[0]) == [None], 'Occlusion must reset reveal')
    t.update(7,[-2]); require(t.update(8,[-1])[0] == (8,0,-1), 'Background reveal lost')
    square = [(0,0),(2,0),(2,2),(0,2)]
    require(edge_distance((1,1),[square]) == 1 and abs(edge_distance((3,3),[square])-math.sqrt(2)) < 1e-12, 'Segment distance differs')
    require(edge_distance((3,1),[square]) == 1 and edge_distance((1,1),[]) is None, 'Distance endpoint/empty case differs')
    table = {}; accumulate(table,'control',[.1,-.2,.3],{'sequence':4,'x':2,'y':1})
    row = finalize(table)[0]
    require(abs(row['mae']-.2)<1e-12 and row['worst']['sequence']==4, 'Residual aggregation differs')
    return {'passed':True, 'scope':'CPU synthetic tracking/distance/aggregation controls only; no GPU quality qualification'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true'); parser.add_argument('--evaluate', type=Path)
    parser.add_argument('--static-manifest', type=Path); parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    require(not sys.flags.optimize, 'Shared BMP reader requires assertions')
    require(args.self_test != bool(args.evaluate), 'Choose self-test or evaluate')
    require(not args.static_manifest or args.evaluate, 'Static comparison requires evaluation')
    result = self_test() if args.self_test else evaluate(args.evaluate.resolve(), args.static_manifest.resolve() if args.static_manifest else None)
    text = json.dumps(result, indent=2, allow_nan=False)+'\n'
    if args.output: args.output.write_text(text)
    print(text, end='')


if __name__ == '__main__': main()
