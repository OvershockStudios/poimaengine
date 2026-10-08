#!/usr/bin/env python3
"""Check generated feature declarations before NativeAOT payload publication."""
# SPDX-License-Identifier: Apache-2.0
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('poima_publish',Path(__file__).resolve().parents[1]/'scripts/publish_native_gameplay.py')
publish=importlib.util.module_from_spec(spec);spec.loader.exec_module(publish)

class Requirements(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory();self.addCleanup(self.directory.cleanup)
        self.path=Path(self.directory.name)/'requirements.json'
        self.baseline=dict(call_version=1,call_bytes=80,services_version=7,services_bytes=176,features=['baseline_v7'])
    def read(self,data):
        self.path.write_text(json.dumps(data));return publish.generated_requirements(self.path)
    def test_baseline_and_named_extension(self):
        self.assertEqual(self.read(self.baseline),self.baseline)
        extended=dict(self.baseline,services_bytes=192,features=['animation_inertial_v1','baseline_v7'])
        self.assertEqual(self.read(extended),extended)
    def test_prefix_cannot_grant_undeclared_feature(self):
        for extent in [175,180,192,193,240]:
            with self.subTest(extent=extent),self.assertRaises(ValueError):self.read(dict(self.baseline,services_bytes=extent))
        for extent in [176,184,191,193]:
            with self.subTest(extent=extent),self.assertRaises(ValueError):self.read(dict(self.baseline,services_bytes=extent,features=['baseline_v7','animation_inertial_v1']))
    def test_closed_shape_and_numeric_boundaries(self):
        for field in ['call_version','call_bytes','services_version','services_bytes']:
            for value in [True,-1,1.5,2**32,None,'192']:
                with self.subTest(field=field,value=value),self.assertRaises(ValueError):self.read(dict(self.baseline,**{field:value}))
            invalid=dict(self.baseline);del invalid[field]
            with self.assertRaises(ValueError):self.read(invalid)
        with self.assertRaises(ValueError):self.read(dict(self.baseline,extra=1))
    def test_required_feature_validation(self):
        for features in [[],['animation_inertial_v1'],['baseline_v7','baseline_v7'],['baseline_v7','unknown_v1'],['baseline_v7',None],['baseline_v7','BAD'],{},'baseline_v7']:
            with self.subTest(features=features),self.assertRaises(ValueError):self.read(dict(self.baseline,features=features))
        for field,value in [('call_version',2),('call_bytes',88),('services_version',8)]:
            with self.subTest(field=field),self.assertRaises(ValueError):self.read(dict(self.baseline,**{field:value}))
    def test_duplicate_and_oversized_input(self):
        self.path.write_text('{"call_version":1,"call_version":2,"call_bytes":80,"services_version":7,"services_bytes":176,"features":["baseline_v7"]}')
        with self.assertRaises(ValueError):publish.generated_requirements(self.path)
        self.path.write_text(' '*16385)
        with self.assertRaises(ValueError):publish.generated_requirements(self.path)

if __name__=='__main__':unittest.main()
