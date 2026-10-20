#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check installed runtime gameplay metadata against its canonical native header.

Pure file reads and JSON validation: no engine, compiler, SDK or subprocess is
imported or launched. Header-shape changes fail closed for deliberate review.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
KNOWN_FEATURES = frozenset((
    'baseline_v7', 'gameplay_persistence_v1', 'component_collections_v1',
    'animation_inertial_v1', 'animation_layers_v1', 'character_input_v1',
    'hierarchical_instances_v1', 'player_preferences_v1', 'input_actions_v1',
    'hit_regions_v1', 'animation_ik_v1', 'animation_root_motion_v1',
    'camera_lens_v1', 'character_stance_v1', 'animation_ik_end_orientation_v1', 'animation_grounding_control_v1', 'animation_rate_v1', 'camera_pose_v1', 'navigation_query_v1'))
NAVIGATION = 'navigation_query_v1'


def need(value, message):
    if not value:
        raise ValueError(message)


def read_text(path, cap=1024*1024):
    path = Path(path)
    need(path.is_file() and not path.is_symlink() and path.stat().st_size <= cap, 'A bounded regular metadata source is required.')
    return path.read_text(encoding='utf-8-sig')


def read_json(path):
    def pairs(rows):
        result = {}
        for key, value in rows:
            need(key not in result, 'Duplicate JSON member.')
            result[key] = value
        return result
    def invalid(value):
        raise ValueError('Nonfinite JSON metadata.')
    return json.loads(read_text(path), object_pairs_hook=pairs, parse_constant=invalid)


def canonical(header):
    text = read_text(header)
    numbers = {}
    for declaration in re.findall(r'inline constexpr std::uint32_t ([^;]+);', text):
        for name, value in re.findall(r'\b([a-z_]+)\s*=\s*([0-9]+)\b', declaration):
            need(name not in numbers, 'Duplicate native ABI constant.')
            numbers[name] = int(value)
    symbols = dict(re.findall(r'inline constexpr const char\* ([a-z_]+)="([a-z0-9_]+)";', text))
    body = re.search(r'inline Contract available_contract\(bool navigation_bound=true\) \{(.*?)\n\}', text, re.S)
    need(body is not None, 'Unsupported canonical availability function shape.')
    body = body.group(1)
    need('std::vector<std::string> features{baseline_feature};' in text and
         'if(navigation_bound && runtime_navigation_available())result.features.push_back(navigation_feature);' in body,
         'Canonical baseline or navigation availability policy changed.')
    refs = ['baseline_feature', *re.findall(r'result\.features\.push_back\(([a-z_]+)\);', body)]
    need(len(refs) == len(set(refs)) and all(name in symbols for name in refs), 'Canonical native feature symbols are incomplete or duplicated.')
    features = [symbols[name] for name in refs]
    need(set(features) == KNOWN_FEATURES and len(features) == 19 and refs.count('navigation_feature') == 1,
         'Known native availability feature contract changed.')
    extents = re.findall(r'result\.services_bytes=([a-z_]+);', body)
    need(extents and extents[-1] == 'camera_pose_services_bytes' and all(name in numbers for name in extents),
         'Canonical service allocation endpoint changed.')
    need((numbers.get('call_version'), numbers.get('call_bytes'), numbers.get('services_version'),
          numbers.get(extents[-1])) == (1, 80, 7, 392), 'Known native call/epoch/extent contract changed.')
    return dict(call_version=numbers['call_version'], call_bytes=numbers['call_bytes'],
                services_version=numbers['services_version'], services_bytes=numbers[extents[-1]], features=features)


def validate(runtime, header):
    policy = canonical(header)
    need(type(runtime) is dict and runtime.get('format') == 'poima.runtime' and
         type(runtime.get('version')) is int and runtime['version'] == 1, 'Invalid runtime descriptor envelope.')
    for field in ('call_version', 'call_bytes', 'services_version', 'services_bytes'):
        value = runtime.get('gameplay_'+field)
        need(type(value) is int and value == policy[field], 'Runtime gameplay '+field+' differs from canonical native availability.')
    flags = runtime.get('features')
    need(type(flags) is dict and type(flags.get('navigation')) is bool and type(flags.get('simulation')) is bool,
         'Runtime navigation and simulation capabilities must be Boolean.')
    need(not flags['navigation'] or flags['simulation'], 'Navigation requires simulation in the installed runtime.')
    features = runtime.get('gameplay_features')
    need(type(features) is list and all(type(name) is str for name in features) and
         len(features) == len(set(features)), 'Runtime gameplay features must be unique strings.')
    expected = set(policy['features'])
    if not flags['navigation']:
        expected.remove(NAVIGATION)
    need(set(features) == expected and len(features) == len(expected), 'Runtime gameplay feature membership differs from canonical native availability.')
    # Feature order is not rights: CMake preserves its existing conditional
    # navigation append, while the native availability function inserts it earlier.
    return dict(passed=True, services_version=policy['services_version'], services_bytes=policy['services_bytes'],
                call_version=policy['call_version'], call_bytes=policy['call_bytes'],
                feature_count=len(expected), navigation=flags['navigation'])


def generated_fields(cmake, navigation):
    """Read the two packaging declarations and the unchanged navigation branch."""
    text = read_text(cmake)
    extent = re.findall(r'^set\(POIMA_RUNTIME_GAMEPLAY_SERVICES_BYTES ([0-9]+)\)$', text, re.M)
    features = re.findall(r'^set\(POIMA_RUNTIME_GAMEPLAY_FEATURES "(.*?)"\)$', text, re.M)
    need(len(extent) == len(features) == 1, 'Packaging declarations are missing or duplicated.')
    base = json.loads('['+features[0].replace('\\"', '"')+']')
    branch = ('set(POIMA_RUNTIME_NAVIGATION false)\n'
              'if(POIMA_ENABLE_NAVIGATION AND POIMA_ENABLE_SIMULATION)\n'
              '    set(POIMA_RUNTIME_NAVIGATION true)\n'
              '    string(APPEND POIMA_RUNTIME_GAMEPLAY_FEATURES ", \\\"navigation_query_v1\\\"")\n'
              'endif()')
    need(branch in text and NAVIGATION not in base, 'Packaging navigation conditional or static feature scope changed.')
    return dict(gameplay_services_bytes=int(extent[0]), gameplay_features=base+([NAVIGATION] if navigation else []))


def self_test(header, cmake, stale_runtime=None):
    policy = canonical(header)
    seed = dict(format='poima.runtime', version=1, gameplay_call_version=1, gameplay_call_bytes=80,
                gameplay_services_version=7, features=dict(simulation=True, navigation=False))
    cases = 0
    for navigation in (False, True):
        generated = copy.deepcopy(seed);generated['features']['navigation'] = navigation
        generated.update(generated_fields(cmake, navigation))
        result = validate(generated, header)
        need(result['feature_count'] == (19 if navigation else 18), 'Generated descriptor has the wrong feature count.')
        cases += 1
    good = copy.deepcopy(seed);good.update(generated_fields(cmake, False))
    def reject(value):
        nonlocal cases
        try:
            validate(value, header)
        except ValueError:
            cases += 1
            return
        raise AssertionError('Invalid descriptor was accepted.')
    stale = read_json(stale_runtime) if stale_runtime else copy.deepcopy(good)
    if stale_runtime is None:
        stale['gameplay_services_bytes'] = 256
        stale['gameplay_features'] = [name for name in good['gameplay_features'] if name not in
            ('input_actions_v1', 'hit_regions_v1', 'animation_ik_v1', 'animation_root_motion_v1', 'camera_lens_v1', 'character_stance_v1', 'animation_ik_end_orientation_v1', 'animation_grounding_control_v1', 'animation_rate_v1', 'camera_pose_v1')]
    need(stale['gameplay_services_bytes'] == 256, 'The retained regression witness must be the known stale 256-byte descriptor.')
    reject(stale)
    # Reconstruct generated packaging fields on the actual retained metadata,
    # retaining its platform/capability fields; validate the corrected result.
    corrected = copy.deepcopy(stale);corrected.update(generated_fields(cmake, corrected['features']['navigation']))
    validate(corrected, header);cases += 1
    for field, value in (('gameplay_call_version', 2), ('gameplay_call_bytes', 79),
                         ('gameplay_services_version', 8), ('gameplay_services_bytes', 320),
                         ('gameplay_services_bytes', True)):
        bad = copy.deepcopy(good);bad[field] = value;reject(bad)
    for features in (good['gameplay_features'][:-1], good['gameplay_features']+[NAVIGATION],
                     good['gameplay_features']+['unknown_v1'], good['gameplay_features']+[good['gameplay_features'][0]]):
        bad = copy.deepcopy(good);bad['gameplay_features'] = features;reject(bad)
    bad = copy.deepcopy(good);bad['features']['navigation'] = True;reject(bad)
    bad['gameplay_features'].append(NAVIGATION);bad['features']['simulation'] = False;reject(bad)
    need(policy['services_bytes'] == 392, 'Canonical extent changed during file-only tests.')
    return dict(passed=True, file_only_checks=cases, retained_stale_descriptor_rejected=True,
                generated_navigation_false_and_true_checked=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path)
    parser.add_argument('--header', type=Path, default=ROOT/'include/poima/gameplay_compatibility.hpp')
    parser.add_argument('--cmake', type=Path, default=ROOT/'CMakeLists.txt')
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--stale-runtime', type=Path)
    args = parser.parse_args()
    if bool(args.runtime) == args.self_test:
        parser.error('Supply --runtime OR --self-test.')
    if args.stale_runtime and not args.self_test:
        parser.error('--stale-runtime requires --self-test.')
    try:
        result = self_test(args.header, args.cmake, args.stale_runtime) if args.self_test else validate(read_json(args.runtime), args.header)
        print(json.dumps(result, sort_keys=True))
        return 0
    except (ValueError, AssertionError, KeyError, OSError, TypeError) as error:
        print(json.dumps(dict(passed=False, error=str(error)), sort_keys=True))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
