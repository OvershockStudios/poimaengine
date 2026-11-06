# SPDX-License-Identifier: Apache-2.0
"""Original Knight geometry/depth references for grounded physical handoff.

Reads completed CPU records only; never starts an engine, compiler or graphics
process. Complete saved locals supply independent affine FK and original-source
skinning. Contact/physical correctness remains the separate native workflow's
scope. Reference preparation alone is not rendered evidence.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import sys

sys.dont_write_bytecode = True
import character_depth_oracle as depth
from character_library_intake import LibraryOracle, decode_cooked
from managed_physical_gameplay_contract import strict_json
import world_physical_contract as physical

ROOT = depth.ROOT
SELF = Path(__file__).resolve()
MIB = 1024 * 1024
MAX_JSON = 32 * MIB
MAX_REFERENCE = 4 * MIB
MAX_SELECTED_BYTES = 160 * MIB
ACTOR, OBSERVER, VISIBLE_FLOOR = (f'{n:032x}' for n in (1, 700, 750))
SELECTIONS = (
    ('planted-stationary', 2, False),
    ('planted-active', 4, False),
    ('takeover-first', 5, True),
    ('takeover-future', 9, True),
    ('passive-next', 15, True),
)
need = depth.need


def lookup_path(value):
    """Locate actual WSL records on Windows without rewriting their contents."""
    text = str(value)
    if os.name == 'nt' and text.startswith('/mnt/d/poimaengine/'):
        relative = Path(text[len('/mnt/d/poimaengine/'):])
        need(not relative.is_absolute() and '..' not in relative.parts,
             'Recorded path must remain inside the actual checkout')
        return ROOT / relative
    return depth.lookup_path(text)


def file_identity(path, limit=MAX_JSON):
    path = Path(path)
    before = path.lstat()
    need(stat.S_ISREG(before.st_mode) and not getattr(before, 'st_file_attributes', 0) & 0x400,
         'Ordinary selected file required: ' + str(path))
    need(before.st_size <= limit, 'Selected file exceeds bound: ' + str(path))
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    after = path.lstat()
    need((before.st_size, before.st_mtime_ns, before.st_ino) ==
         (after.st_size, after.st_mtime_ns, after.st_ino), 'Selected file changed while reading')
    return dict(bytes=after.st_size, sha256=digest)


def read_json(path):
    before = file_identity(path)
    result = strict_json(Path(path).read_bytes())
    need(file_identity(path) == before, 'Selected JSON changed during parse')
    return result, before


def matrix_close(actual, expected, tolerance, label):
    need(type(actual) is list and type(expected) is list and len(actual) == len(expected) == 16,
         label + ': complete matrix required')
    need(all(type(v) in (int, float) and math.isfinite(v) for v in actual + expected),
         label + ': finite matrix required')
    need(max(abs(a-b) for a, b in zip(actual, expected)) <= tolerance, label)


def independent_worlds(oracle, locals_, wrapper):
    """Independent TRS hierarchy evaluation; native world matrices are witnesses."""
    need(len(locals_) == len(oracle.nodes) == 57, 'All original node locals required')
    local_matrices = [depth.transform_matrix(**row) for row in locals_]
    result, visiting = {}, set()

    def at(index):
        need(index not in visiting, 'Original hierarchy cycle')
        if index not in result:
            visiting.add(index)
            parent = at(oracle.parents[index]) if index in oracle.parents else wrapper
            result[index] = depth.multiply(parent, local_matrices[index])
            visiting.remove(index)
        return result[index]

    return [at(i) for i in range(57)]


def original_floor(document, saved, observation):
    """Join the raster's bounded y=0 plane to both real floor representations."""
    entities = document['entities']
    collider = entities[physical.FLOOR]['components']
    visible = entities[VISIBLE_FLOOR]['components']
    floor_transform = dict(position=[0., -.25, 0.], rotation=[0., 0., 0., 1.], scale=[1., 1., 1.])
    visible_transform = dict(position=[0., -.25, 0.], rotation=[0., 0., 0., 1.], scale=[24., .5, 24.])
    need(physical.exact(collider['Transform'], floor_transform) and
         collider['BoxCollider']['half_extents'] == [12., .25, 12.] and
         collider['BoxCollider']['motion'] == 'static', 'Actual static bounded y=0 support')
    need(physical.exact(visible['Transform'], visible_transform) and
         visible['MeshRenderer']['primitive'] == 'box' and
         visible['MeshRenderer']['visible'] is True, 'Actual visible floor matches analytic plane')
    need(physical.exact(saved[physical.FLOOR]['local'], floor_transform) and
         physical.exact(saved[VISIBLE_FLOOR]['local'], visible_transform), 'Saved floor geometry changed')
    support = observation['support']
    need(support['id'] == physical.FLOOR and physical.exact(support['local_transform'], floor_transform),
         'Actual native support identity/local transform')
    matrix_close(support['world_matrix'], depth.transform_matrix(**floor_transform), 1e-10,
                 'Actual support world frame must match analytic plane')
    primitive = [key for key, row in entities.items() if 'MeshRenderer' in row['components']]
    need(primitive == [VISIBLE_FLOOR], 'One explicit primitive floor; no unmodeled primitive draws')
    for kind, count in (('SkinnedMesh', 6), ('StaticMesh', 9), ('PbrMaterial', 15)):
        need(sum(kind in row['components'] for row in entities.values()) == count,
             'Complete original material/drawable population: ' + kind)


def archive_input(row, tick, passive, oracle, pins):
    path = lookup_path(row['path'])
    full, pin = read_json(path)
    need(pin == dict(bytes=row['bytes'], sha256=row['sha256']), 'Original whole checkpoint changed')
    pins[str(path)] = pin
    need(full['format'] == 'poima.world-save' and full['version'] == 1, 'Original complete world-save envelope')
    payload, observation = full['snapshot']['payload'], row['observation']
    need(type(payload['tick']) is int and payload['tick'] == observation['tick'] == tick and
         observation['passive'] is passive, 'Exact committed checkpoint/observation phase and tick')
    need(payload['physical']['version'] == 4 and len(payload['physical']['rigs']) == 1,
         'Complete grounded E4 physical extension')
    native_rig = payload['physical']['rigs'][0]
    witness = native_rig['procedural_capture']
    need(witness['version'] == 2 and witness['grounding']['prepared'] is True and
         witness['captured_tick'] == (4 if passive else tick), 'Actual completed grounded decision witness')
    need((physical.exact(witness, native_rig['procedural_terminal']) if passive else
          native_rig['procedural_terminal'] is None), 'Exact current versus historical procedural ownership')
    saved = {r['id']: r for r in payload['entities']}
    need(len(saved) == len(payload['entities']), 'Complete unique saved entity population')
    nodes = observation['all_nodes']
    need(len(nodes) == 57 and all(r['id'] == physical.node(i) and r['tick'] == tick
         for i, r in enumerate(nodes)), 'Complete ordered original node observations')
    for native in nodes + [observation[n] for n in ('actor', 'wrapper', 'camera', 'observer', 'support')]:
        need(native['tick'] == tick and physical.exact(saved[native['id']]['local'], native['local_transform']),
             'Whole saved/native local-transform witness')
    wrapper = observation['wrapper']
    actor = observation['actor']
    need(actor['id'] == ACTOR and wrapper['id'] == physical.RIG, 'Actual linked actor/rig identity')
    matrix_close(actor['world_matrix'], depth.transform_matrix(**actor['local_transform']), 1e-10,
                 'Actual actor root frame')
    wrapper_world = depth.multiply(actor['world_matrix'], depth.transform_matrix(**wrapper['local_transform']))
    matrix_close(wrapper['world_matrix'], wrapper_world, 1e-10, 'Actual direct-child wrapper frame')
    worlds = independent_worlds(oracle, [saved[physical.node(i)]['local'] for i in range(57)], wrapper_world)
    for i, native in enumerate(nodes):
        matrix_close(native['world_matrix'], worlds[i], 3e-7, 'Independent complete corrected FK')
    observer = observation['observer']
    need(observer['id'] == OBSERVER and physical.exact(observer['local_transform'], depth.CAMERA_LOCAL),
         'Frozen actor-relative observer')
    camera = depth.multiply(actor['world_matrix'], depth.transform_matrix(**depth.CAMERA_LOCAL))
    matrix_close(observer['world_matrix'], camera, 1e-10, 'Actual observer world product')
    authored_camera = full['document']['entities'][OBSERVER]['components']['Camera']
    need(authored_camera == dict(vertical_fov=depth.FOV, near=depth.NEAR, far=depth.FAR),
         'Authored observer lens agrees with raster')
    original_floor(full['document'], saved, observation)
    manifest_path = lookup_path(row['manifest_path'])
    manifest, manifest_id = read_json(manifest_path)
    need(manifest_id == row['manifest_identity'] and physical.exact(manifest, row['manifest']),
         'Complete original manifest retained')
    member = manifest['payload']['current']
    need(member['file'] == path.name and dict(bytes=member['bytes'], sha256=member['sha256']) == pin and
         dict(bytes=row['receipt']['bytes'], sha256=row['receipt']['sha256']) == pin,
         'Original archive/manifest/receipt identities join')
    pins[str(manifest_path)] = manifest_id
    return full, observation, worlds, camera, pin


def cooked_vertex_census(oracle, cooked_bytes):
    """Derive draw-instance counts from fully source-validated cooked corners.

    Tangent generation can split a source vertex at a tangent/handedness seam.
    The renderer counts cooked vertex-buffer entries, whereas the independent
    depth raster evaluates the original indexed source triangles. Keep both
    populations explicit; neither comes from a renderer diagnostic response.
    """
    metadata, meshes, _ = decode_cooked(cooked_bytes, oracle)
    need(len(metadata['nodes']) == len(oracle.nodes) == 57,
         'Complete original and cooked node populations')
    rows = []
    for index, (original, cooked_node) in enumerate(zip(oracle.nodes, metadata['nodes'])):
        primitive_ids = [original['mesh']] if 'mesh' in original else []
        need(cooked_node['primitives'] == primitive_ids and
             cooked_node['skin'] == original.get('skin', -1),
             'Original drawable and skin instance membership retained')
        for primitive in primitive_ids:
            source = oracle.meshes[primitive]
            cooked = meshes[primitive]
            description = metadata['primitives'][primitive]
            skinned = 'skin' in original
            need(description['skinned'] is skinned and
                 len(cooked['vertices']) == description['vertices'] and
                 len(cooked['indices']) == len(source['indices']) == description['indices'] and
                 set(cooked['mapping']) == set(range(len(cooked['vertices']))),
                 'Every drawn cooked vertex retains a verified original triangle-corner witness')
            rows.append(dict(node=index, primitive=primitive, skinned=skinned,
                source_vertices=len(source['positions']), cooked_vertices=len(cooked['vertices']),
                triangles=len(cooked['indices'])//3,
                original_corner_witnesses=len(cooked['mapping'])))
    need(len(rows) == 15 and sum(r['skinned'] for r in rows) == 6,
         'All original drawable instances retained')
    return dict(drawables=rows,
        original_vertices=sum(r['source_vertices'] for r in rows),
        cooked_vertices=sum(r['cooked_vertices'] for r in rows),
        original_skinned_vertices=sum(r['source_vertices'] for r in rows if r['skinned']),
        cooked_skinned_vertices=sum(r['cooked_vertices'] for r in rows if r['skinned']),
        triangles=sum(r['triangles'] for r in rows),
        scope='Per-drawable original versus cooked buffer populations; all cooked corners/skin attributes independently joined to original source before counting.')


def make_reference(source, cpu_report, require_stale=()):
    source, cpu_report = Path(source), Path(cpu_report)
    helpers = (SELF, ROOT/'tests/character_depth_oracle.py', ROOT/'tests/character_library_intake.py',
        ROOT/'tests/imported_character_contract.py', ROOT/'tests/managed_physical_gameplay_contract.py',
        ROOT/'tests/world_physical_contract.py')
    helper_pins = {str(p.relative_to(ROOT)): file_identity(p) for p in helpers}
    manifest_path = ROOT/'examples/physical-character-yard/source-manifest.json'
    manifest, manifest_id = read_json(manifest_path)
    model_id = file_identity(source)
    need(model_id == {k: manifest['model'][k] for k in ('bytes', 'sha256')}, 'Unchanged original licensed model')
    cpu, cpu_id = read_json(cpu_report)
    need(cpu['format'] == 'poima.physical-character-grounded-handoff.v1' and
         cpu['passed'] is True and cpu['errors'] == [], 'Completed true grounded-character workflow required')
    need(len(cpu['owners']) == 3 and all(o['natural_exit'] is True and o['forced_cleanup'] is False and
         o['exit_code'] == 0 and o['reader_failure'] is None for o in cpu['owners']),
         'Three actual native writers must close before reference preparation')
    need(len(cpu['archives']) == 16 and len(cpu['whole_pairs']) == 11,
         'Complete declared sixteen-archive/eleven-pair grounded workflow')
    need(set(require_stale) <= {s for s, _, _ in SELECTIONS}, 'Unknown required stale-control case')
    oracle = LibraryOracle(source.read_bytes())
    material = oracle.document['materials'][0]
    need(material.get('doubleSided') is True and material.get('alphaMode', 'OPAQUE') == 'OPAQUE',
         'Original opaque double-sided material')
    pins = {str(source): model_id, str(cpu_report): cpu_id, str(manifest_path): manifest_id}
    cooked_path = lookup_path(cpu['artifacts']['asset_package'])
    cooked_id = file_identity(cooked_path)
    need(cooked_id == cpu['authored_identity']['package'], 'Complete original cooked content unchanged')
    vertex_census = cooked_vertex_census(oracle, cooked_path.read_bytes())
    pins[str(cooked_path)] = cooked_id
    selected = {}
    for slot, tick, passive in SELECTIONS:
        rows = [r for r in cpu['archives'] if r['slot'] == slot and r['tick'] == tick]
        need(len(rows) == 1, 'One original selected saved checkpoint: ' + slot)
        selected[slot] = archive_input(rows[0], tick, passive, oracle, pins)
    pre_takeover_worlds = selected['planted-active'][2]
    result = dict(format='poima.character-grounded-depth-reference.v1', reference_prepared=True,
        native_render_verified=False, source_model=model_id, cpu_report=cpu_id,
        camera=dict(id=OBSERVER, local=depth.CAMERA_LOCAL, width=depth.W, height=depth.H,
                    vertical_fov=depth.FOV, near=depth.NEAR, far=depth.FAR),
        thresholds=copy.deepcopy(depth.THRESHOLDS), cases=[], required_stale_cases=list(require_stale),
        original_skinned_vertices=vertex_census['original_skinned_vertices'],
        cooked_skinned_vertices=vertex_census['cooked_skinned_vertices'],
        cooked_vertex_census=vertex_census,
        scope='Independent original geometry/LBS depth reference from complete saved locals and native frame witnesses. Native grounding/history correctness and actual GPU capture are separate evidence scopes.')
    for slot, tick, passive in SELECTIONS:
        full, observation, worlds, camera, pin = selected[slot]
        stale_worlds = pre_takeover_worlds if passive else oracle.pose(36, 0.)[2]
        geometry = depth.inspect_case(oracle, slot, worlds, observation['wrapper']['world_matrix'],
                                      observation['actor']['world_matrix'], stale_worlds)
        # Reuse only the established pure math, replacing its old scenario-specific description.
        geometry['stale_control'] = ('Frozen corrected planted-active mesh-local palettes' if passive else
            'Original uncorrected Idle/time0 mesh-local palettes') + '; CURRENT drawable transforms, unchanged rigid accessories and complete nearest-depth occlusion.'
        matrix_close(geometry['camera_world'], camera, 1e-10, 'Reference camera composition')
        need(geometry['framing_passed'] and len(geometry['current_original_depth_probes']) == 32,
             'Whole original framing/current depth controls failed: ' + slot)
        feasible = geometry['stale_control_feasible']
        need(slot not in require_stale or feasible,
             'Required control has insufficient depth separation at unchanged thresholds: ' + slot)
        probes = geometry['current_original_depth_probes'] + (geometry['stale_selected_probes'] if feasible else [])
        result['cases'].append(dict(label=slot, slot=slot, tick=tick, passive=passive, archive=pin,
            native_camera_world=observation['observer']['world_matrix'], stale_positive=feasible,
            stale_requirement=slot in require_stale,
            stale_scope='Separated original-source control' if feasible else 'No stale-palette rejection claim; insufficient separation at the selected resolution.',
            oracle=geometry, probes=probes))
    need(sum(pin['bytes'] for pin in pins.values()) <= MAX_SELECTED_BYTES, 'Bounded selected input volume')
    for name, pin in pins.items():
        need(file_identity(name) == pin, 'Selected original input changed during reference preparation')
    result['input_files'] = [dict(path=name, identity=pin) for name, pin in sorted(pins.items())]
    need(all(file_identity(ROOT/name) == pin for name, pin in helper_pins.items()),
         'Reference helper sources changed during preparation')
    result['helper_sources'] = helper_pins
    return result


def verify_capture(reference, case, capture, before_archive, after_archive, image, *, build_version, expected_gpu):
    """Check a real capture and whole-state brackets; starts no native process.

    The external owner must retain the literal request/reply and qualify process,
    GPU/image identity, required positive cases and repeated restored image pairs.
    """
    need(reference['format'] == 'poima.character-grounded-depth-reference.v1' and
         reference['thresholds'] == depth.THRESHOLDS and
         sum(physical.exact(case, row) for row in reference['cases']) == 1, 'Exact declared reference case')
    before_archive, after_archive, image = map(Path, (before_archive, after_archive, image))
    before, before_id = read_json(before_archive)
    after, after_id = read_json(after_archive)
    need(before_id == after_id == case['archive'] and
         before_archive.read_bytes() == after_archive.read_bytes() and physical.exact(before, after),
         'Whole original save equals both capture brackets without excluded state')
    payload = before['snapshot']['payload']
    need(capture['build_version'] == build_version and capture['gpu'] == expected_gpu and
         capture['source'] == 'runtime' and capture['format'] == 'BMP' and
         capture['world_id'] == payload['world_id'] and
         capture['revision'] == payload['authored_revision'] and
         capture['material_revision'] == capture['ui_revision'] == 0 and capture['ui_scale'] == 1 and
         capture['camera'] == reference['camera']['id'] and capture['frames_presented'] == 1 and
         capture['tick'] == case['tick'] and physical.exact(capture['camera_world'], case['native_camera_world']) and
         physical.exact(capture['lens'], dict(vertical_fov=depth.FOV, near=depth.NEAR, far=depth.FAR)),
         'Actual committed camera/frame/lens capture receipt')
    diagnostics = capture['render_diagnostics']
    receipts, execution = diagnostics['scene_receipts'], diagnostics['frame_execution']
    need(physical.exact(receipts['submitted'], receipts['presented']) and
         receipts['presented']['camera_id'] == OBSERVER and
         receipts['presented']['world_id'] == capture['world_id'] and
         receipts['presented']['authored_revision'] == capture['revision'] and
         receipts['presented']['material_revision'] == capture['material_revision'] and
         physical.exact(receipts['presented']['camera_lens'], capture['lens']),
         'Actual submitted/presented immutable scene')
    need(diagnostics['completed_submissions'] == 1 and execution['submitted'] == execution['limit'] ==
         execution['peak_outstanding'] == 1 and execution['outstanding'] == 0, 'Single GPU frame retired')
    draws = diagnostics['last_draws']
    need(draws['skinned_instances'] == 6 and draws['skinned_vertices'] == reference['cooked_skinned_vertices'] and
         draws['camera_draws'] == 16 and draws['camera_triangles'] == 6964,
         'All original weighted/rigid meshes plus the complete floor rendered')
    reconstruction = diagnostics['reconstruction']
    need(reconstruction['mode'] == 'none' and reconstruction['active'] is False and
         reconstruction['jitter_pixels'] == [0., 0.] and diagnostics['scene_products']['view'] == 'color',
         'Unjittered original-material color/depth observation')
    need(all(type(p['depth']) is float for p in diagnostics['scene_products']['probes']),
         'Uncoerced native device-depth values')
    checked = depth.verify_depth_samples(case, capture)
    image_id = file_identity(image, 2 * MIB)
    need(lookup_path(capture['path']).resolve() == image.resolve(), 'Actual capture path equals selected image')
    from scene_capture import pixels
    rows = pixels(image)
    need(len(rows) == depth.H and all(len(row) == depth.W for row in rows), 'Complete BMP dimensions')
    return dict(depth_checks=checked, archive_identity=before_id, image_identity=image_id,
                whole_capture_state_unchanged=True, visual_review_required=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--cpu-report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--require-stale', action='append', default=[], choices=[s for s, _, _ in SELECTIONS],
                        help='Require the unchanged 32-probe stale-control threshold for this selected case.')
    args = parser.parse_args()
    need(not args.output.exists() and args.output.parent.is_dir(), 'Fresh output and existing parent required')
    result = make_reference(args.source, args.cpu_report, args.require_stale)
    raw = (json.dumps(result, indent=2, allow_nan=False)+'\n').encode()
    need(len(raw) <= MAX_REFERENCE, 'Bounded complete reference output')
    with args.output.open('xb') as stream:
        stream.write(raw)
    print(args.output)


if __name__ == '__main__':
    main()
