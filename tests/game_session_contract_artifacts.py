#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pure release-artifact joins and tamper checks; no native/behavior qualification."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client.game_session_responses import (game_session_response_contract,
                                               validate_game_session_result)
from poima_client.responses import ResponseContractError, response_contract
from poima_client.workflow_responses import workflow_response_contract

METHODS = ('runtime.component.get', 'runtime.component.query', 'runtime.components',
    'runtime.entity', 'runtime.gameplay.inspect', 'runtime.hit_test', 'runtime.inspect',
    'runtime.observe', 'runtime.raycast', 'runtime.save.result', 'runtime.save.status',
    'runtime.status', 'runtime.step', 'runtime.ui.activate', 'runtime.ui.inspect',
    'save.inspect', 'save.load', 'save.status', 'save.write')
PROFILES = ('source_world', 'source_shared_player', 'readonly_game')
EXPERIMENTAL = [dict(method='runtime.step', parameter=name, when='nonempty-array')
    for name in ('camera_lenses', 'character_stances', 'ik', 'root_motion')]
EXPERIMENTAL += [dict(method='save.load', parameter='upgrade', when='present')]
AUTHORING = {'entity.get', 'entity.query', 'entity.world_transform', 'world.describe',
    'world.history', 'world.inspect', 'world.redo', 'world.transact', 'world.undo'}
CANDIDATE = (set(METHODS)-{'runtime.raycast','runtime.hit_test'}) | {
    'runtime.start','runtime.stop','runtime.gameplay.load_native','save.configure'}


class ArtifactIntegrityError(ValueError):
    pass


def require(condition, reason):
    if not condition:
        raise ArtifactIntegrityError(reason)


def strict_equal(a, b):
    if type(a) is not type(b):
        return False
    if type(a) is dict:
        return a.keys() == b.keys() and all(strict_equal(a[k],b[k]) for k in a)
    if type(a) is list:
        return len(a) == len(b) and all(strict_equal(x,y) for x,y in zip(a,b))
    return a == b


def decode(data):
    require(type(data) is bytes and len(data) <= 2*1048576, 'Bounded artifact bytes required')
    def pairs(rows):
        result = {}
        for key, value in rows:
            require(key not in result, 'Duplicate artifact key')
            result[key] = value
        return result
    try:
        return json.loads(data,object_pairs_hook=pairs,
            parse_constant=lambda _: require(False,'Nonfinite artifact value'))
    except (UnicodeError,json.JSONDecodeError) as error:
        raise ArtifactIntegrityError('Invalid artifact JSON') from error


def validate_artifact_set(release, request_bytes, response_bytes, packaged_bytes):
    """Validate THESE frozen release joins, not arbitrary JSON Schema semantics."""
    require(release.get('format') == 'poima.runtime-game-session-release.v1'
        and release.get('id') == 'poima.runtime-game-session' and
        type(release.get('version')) is int and release['version'] == 1 and
        release.get('status') == 'stable', 'Wrong release identity/version')
    require(release['methods'] == list(METHODS), 'Changed selected nineteen methods')
    require(release['profiles'] == list(PROFILES) and strict_equal(release['qualified_targets'],
        [dict(os='Windows',arch='x86_64')]), 'Wrong qualified context/target')
    for key, filename, data in [('request_baseline','runtime-game-session-v1.json',request_bytes),
                               ('response_baseline','runtime-game-session-responses-v1.json',response_bytes)]:
        item = release[key]
        require(item['file'] == filename and item['sha256'] == hashlib.sha256(data).hexdigest(),
                'Release baseline path/hash mismatch')
    require(packaged_bytes == response_bytes, 'Packaged response bytes differ')
    request, response = decode(request_bytes), decode(response_bytes)
    require(request['format'] == 'poima.runtime-game-session-requests.v1'
        and response['format'] == 'poima.runtime-game-session-responses.v1'
        and response['status'] == 'stable', 'Wrong baseline format/status')
    for value, key in [(request,'version'),(response,'contract_version')]:
        require(value.get('id') == release['id'] and type(value.get(key)) is int and value[key] == 1,
                'Baseline identity/version differs')
        require(sorted(value['methods']) == list(METHODS), 'Baseline method membership differs')
    revision = release['request_baseline']['source_schema_revision']
    require(type(revision) is int and revision == 98 and
        type(request['provenance']['schema_revision']) is int and
        request['provenance']['schema_revision'] == revision, 'Wrong source schema revision')
    require(set(request['profiles']) == set(PROFILES) and response['profiles'] == list(PROFILES),
            'Baseline profiles differ')
    for profile in PROFILES:
        context = request['profiles'][profile]['context']
        shared, readonly = profile != 'source_world', profile == 'readonly_game'
        expected = dict(mode='read_only_runtime' if readonly else 'authoring',read_only=readonly,
            session_scope=dict(present=True,value='shared_headless') if shared else dict(present=False))
        require(strict_equal(context,expected), 'Mislabelled request profile')
        capture_revision = request['provenance']['captures'][profile]['schema_revision']
        require(type(capture_revision) is int and capture_revision == 98, 'Capture schema differs')
    require(strict_equal(release['experimental_branches'],EXPERIMENTAL) and
            strict_equal(request['experimental_branches'],EXPERIMENTAL), 'Experimental boundary differs')
    for branch in EXPERIMENTAL:
        method, parameter = branch['method'], branch['parameter']
        schema = request['methods'][method]
        require(parameter in schema['properties'] and parameter not in schema['required'],
                'Experimental branch is missing or made mandatory')
        require(schema['properties'][parameter]['type'] ==
                ('array' if branch['when'] == 'nonempty-array' else 'object'), 'Wrong branch discriminator')
    step = request['methods']['runtime.step']
    for parameter in ('inputs','motions','sounds','animations'):
        require(parameter not in step['required'] and step['properties'][parameter]['type'] == 'array'
            and not any(b['parameter'] == parameter for b in EXPERIMENTAL), 'Base commands misclassified')
    expected_defaults = {'runtime.component.query.limit':64,'runtime.ui.inspect.limit':64,
        'save.load.allow_recovery':False,'save.write.acknowledge_recovery':False}
    require(strict_equal(release['defaults'],expected_defaults), 'Release defaults changed type/value')
    for name, default in expected_defaults.items():
        method, field = name.rsplit('.',1)
        schema = request['methods'][method]
        require(field not in schema['required'] and strict_equal(schema['properties'][field]['default'],default)
                and schema['properties'][field]['type'] == ('boolean' if type(default) is bool else 'integer'),
                'Request default/omission differs')
    retention = release['retention_minima']
    require(set(retention) == {'runtime.step','runtime.ui.activate','save.load','save.write',
        'gameplay_terminal_ledger'}, 'Unexpected retention pool membership')
    for name, item in retention.items():
        require(type(item['count']) is int and item['count'] == (64 if name == 'gameplay_terminal_ledger' else 32),
                'Invalid retention count')
    require(retention['runtime.step']['lifetime'] == retention['runtime.ui.activate']['lifetime'] ==
        retention['gameplay_terminal_ledger']['lifetime'] == 'owner-local', 'Wrong owner retention lifetime')
    require(retention['save.write']['lifetime'] == 'durable-per-slot' and
        retention['save.write']['outcomes'] == 'committed writes only', 'Wrong durable retry policy')
    require(retention['save.load']['lifetime'] == 'owner-local-shared-outcome-pool' and
        retention['save.load']['shared_with'] == ['save.configure'] and 'save.configure' not in METHODS,
        'Load/configuration shared pool lost or accidentally released')
    return request, response


class GameSessionContractArtifacts(unittest.TestCase):
    def setUp(self):
        root = ROOT/'docs/contracts'
        self.release = decode((root/'runtime-game-session-v1.release.json').read_bytes())
        self.request_bytes = (root/'runtime-game-session-v1.json').read_bytes()
        self.response_bytes = (root/'runtime-game-session-responses-v1.json').read_bytes()
        self.packaged = (ROOT/'tools/python/poima_client/runtime_game_session_responses.v1.json').read_bytes()
        self.request, self.response = decode(self.request_bytes), decode(self.response_bytes)

    def validate(self):
        return validate_artifact_set(self.release,self.request_bytes,self.response_bytes,self.packaged)

    def repin_mutated_baselines(self):
        self.request_bytes = json.dumps(self.request,allow_nan=False).encode()
        self.response_bytes = json.dumps(self.response,allow_nan=False).encode()
        self.packaged = self.response_bytes
        for name, data in [('request_baseline',self.request_bytes),('response_baseline',self.response_bytes)]:
            self.release[name]['sha256'] = hashlib.sha256(data).hexdigest()

    def test_actual_artifacts_join_full_request_result_and_packaging(self):
        request, response = self.validate()
        self.assertTrue(strict_equal(request,self.request))
        self.assertTrue(strict_equal(response,self.response))
        self.assertTrue(strict_equal(response,game_session_response_contract()))

    def test_changed_hash_or_baseline_path_is_rejected(self):
        for kind in ('request_baseline','response_baseline'):
            with self.subTest(kind=kind):
                original = copy.deepcopy(self.release)
                self.release[kind]['sha256'] = '0'*64
                with self.assertRaises(ArtifactIntegrityError): self.validate()
                self.release = copy.deepcopy(original)
                self.release[kind]['file'] = '../'+self.release[kind]['file']
                with self.assertRaises(ArtifactIntegrityError): self.validate()
                self.release = original

    def test_packaged_byte_drift_is_rejected_even_with_identical_json(self):
        self.packaged += b'\n'
        self.assertEqual(decode(self.packaged),self.response)
        with self.assertRaises(ArtifactIntegrityError): self.validate()

    def test_same_count_method_substitution_is_rejected_after_repinning(self):
        self.release['methods'][self.release['methods'].index('runtime.raycast')] = 'runtime.start'
        for document in (self.request,self.response):
            document['methods']['runtime.start'] = document['methods'].pop('runtime.raycast')
        self.repin_mutated_baselines()
        with self.assertRaises(ArtifactIntegrityError): self.validate()

    def test_baseline_omission_is_rejected_after_repinning(self):
        del self.response['methods']['runtime.hit_test']
        self.repin_mutated_baselines()
        with self.assertRaises(ArtifactIntegrityError): self.validate()

    def test_unqualified_target_and_profile_changes_are_rejected(self):
        for target in [dict(os='Linux',arch='x86_64'),dict(os='Windows',arch='arm64')]:
            self.release['qualified_targets'] = [target]
            with self.subTest(target=target), self.assertRaises(ArtifactIntegrityError): self.validate()
        self.release['qualified_targets'] = [dict(os='Windows',arch='x86_64')]
        self.release['profiles'].append('shared_editor')
        with self.assertRaises(ArtifactIntegrityError): self.validate()

    def test_boolean_and_float_versions_do_not_equal_integer_one(self):
        for version in (True,1.0):
            with self.subTest(location='release',version=version):
                self.release['version'] = version
                with self.assertRaises(ArtifactIntegrityError): self.validate()
        self.release['version'] = 1
        for document, field in [(self.request,'version'),(self.response,'contract_version')]:
            for version in (True,1.0):
                with self.subTest(location=field,version=version):
                    document[field] = version
                    self.repin_mutated_baselines()
                    with self.assertRaises(ArtifactIntegrityError): self.validate()
            document[field] = 1

    def test_schema_revision_and_profile_provenance_are_strict(self):
        self.release['request_baseline']['source_schema_revision'] = True
        with self.assertRaises(ArtifactIntegrityError): self.validate()
        self.release['request_baseline']['source_schema_revision'] = 98
        self.request['profiles']['source_world']['context']['session_scope'] = dict(present=True,value='shared_headless')
        self.repin_mutated_baselines()
        with self.assertRaises(ArtifactIntegrityError): self.validate()

    def test_consent_default_cannot_be_numeric_false(self):
        self.request['methods']['save.load']['properties']['allow_recovery']['default'] = 0
        self.repin_mutated_baselines()
        with self.assertRaises(ArtifactIntegrityError): self.validate()

    def test_experimental_marker_cannot_hide_base_animation(self):
        self.release['experimental_branches'][0]['parameter'] = 'animations'
        self.request['experimental_branches'][0]['parameter'] = 'animations'
        self.repin_mutated_baselines()
        with self.assertRaises(ArtifactIntegrityError): self.validate()

    def test_required_experimental_branch_and_pool_relabelling_fail(self):
        self.request['methods']['runtime.step']['required'].append('ik')
        self.repin_mutated_baselines()
        with self.assertRaises(ArtifactIntegrityError): self.validate()
        self.request['methods']['runtime.step']['required'].remove('ik')
        self.repin_mutated_baselines()
        self.release['retention_minima']['save.load']['shared_with'] = []
        with self.assertRaises(ArtifactIntegrityError): self.validate()

    def test_packaged_api_is_detached_and_preserves_original_result_identity(self):
        first = game_session_response_contract()
        first['methods'].clear();first['profiles'].clear();first['id'] = 'tampered'
        self.assertTrue(strict_equal(game_session_response_contract(),self.response))
        result = dict(generation=0,root=None,write_mode='synchronous')
        self.assertIs(validate_game_session_result('save.status',result),result)
        with self.assertRaises(ResponseContractError):
            validate_game_session_result('save.status',dict(result,generation='0'))
        with self.assertRaises(ResponseContractError):
            validate_game_session_result('runtime.start',{})

    def test_historical_candidate_and_authoring_remain_separate(self):
        root = ROOT/'docs/contracts'
        candidate = decode((root/'runtime-save-responses-v1.candidate.json').read_bytes())
        selection = decode((root/'runtime-save-workflow-v1.candidate.json').read_bytes())
        authoring = decode((root/'authoring-core-v1.json').read_bytes())
        self.assertEqual(candidate['status'],'prospective')
        self.assertEqual(selection['status'],'prospective')
        self.assertEqual(set(candidate['methods']),CANDIDATE)
        self.assertEqual(set(selection['methods']),CANDIDATE)
        self.assertTrue(strict_equal(workflow_response_contract(),candidate))
        self.assertEqual(set(authoring['methods']),AUTHORING)
        self.assertEqual(set(response_contract()['methods']),AUTHORING)
        self.assertEqual(authoring['source_schema_revision'],46)
        self.assertTrue(AUTHORING.isdisjoint(METHODS))

    def test_package_data_declares_frozen_resource(self):
        configuration = (ROOT/'tools/python/pyproject.toml').read_text(encoding='utf-8')
        self.assertIn('"runtime_game_session_responses.v1.json"',configuration)
        self.assertIn('"core_responses.v1.json"',configuration)
        self.assertIn('"core_responses.v1.candidate.json"',configuration)


if __name__ == '__main__':
    unittest.main()
