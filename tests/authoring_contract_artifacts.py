# SPDX-License-Identifier: Apache-2.0
"""Keep released authoring artifacts, retained aliases and SDK data consistent."""
import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/python'))
from poima_client.responses import response_contract


def load(path):
    return json.loads((ROOT / path).read_text(encoding='utf-8'))


class ContractArtifacts(unittest.TestCase):
    def test_request_scope_is_retained_not_reprojected(self):
        legacy = load('docs/contracts/authoring-core-v1.candidate.json')
        released = load('docs/contracts/authoring-core-v1.json')
        self.assertEqual(legacy.pop('status'), 'candidate')
        self.assertEqual(released.pop('status'), 'stable')
        self.assertEqual(released, legacy)
        self.assertEqual(released['source_schema_revision'], 46)
        self.assertEqual(set(released['components']), {'Transform'})

    def test_response_requirements_and_alias_are_unchanged(self):
        legacy = load('tools/python/poima_client/core_responses.v1.candidate.json')
        released = load('docs/contracts/authoring-core-responses-v1.json')
        self.assertEqual(released, response_contract())
        self.assertEqual((ROOT / 'docs/contracts/authoring-core-responses-v1.json').read_bytes(),
                         (ROOT / 'tools/python/poima_client/core_responses.v1.json').read_bytes())
        self.assertEqual(legacy.pop('status'), 'candidate')
        self.assertEqual(released.pop('status'), 'stable')
        self.assertEqual(released, legacy)

    def test_release_pins_exact_artifacts_and_scope(self):
        release = load('docs/contracts/authoring-core-v1.release.json')
        for name in ('request_baseline', 'response_baseline'):
            item = release[name]
            path = ROOT / 'docs/contracts' / item['file']
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), item['sha256'])
        self.assertEqual(release['methods'], sorted(response_contract()['methods']))
        self.assertEqual(release['methods'], sorted(load('docs/contracts/authoring-core-v1.json')['methods']))
        self.assertEqual(release['components'], ['Transform'])
        self.assertEqual(release['mutation_constraints'], {
            'component.set': {'type': 'Transform'}, 'entity.reparent': {'mode': 'keep_local'}})
        self.assertEqual(release['scopes'], ['standalone', 'shared_editor', 'shared_headless'])
        self.assertFalse(release['modes']['read_only_runtime']['mutations'])

    def test_packaging_keeps_canonical_and_legacy_resources(self):
        configuration = (ROOT / 'tools/python/pyproject.toml').read_text(encoding='utf-8')
        self.assertIn('"core_responses.v1.json"', configuration)
        self.assertIn('"core_responses.v1.candidate.json"', configuration)


if __name__ == '__main__':
    unittest.main()
