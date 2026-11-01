#!/usr/bin/env python3
"""Physical optional-prefix publication policy; no compiler/native execution."""
# SPDX-License-Identifier: Apache-2.0
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
spec=importlib.util.spec_from_file_location('poima_publish_physical',Path(__file__).resolve().parents[1]/'scripts/publish_native_gameplay.py')
publish=importlib.util.module_from_spec(spec);spec.loader.exec_module(publish)
class PhysicalRequirements(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory();self.addCleanup(self.directory.cleanup)
        self.path=Path(self.directory.name)/'requirements.json'
        self.baseline=dict(call_version=1,call_bytes=80,services_version=7,services_bytes=176,features=['baseline_v7'])
    def read(self,value):
        self.path.write_text(json.dumps(value));return publish.generated_requirements(self.path)
    def test_independent_optional_feature_and_old_prefixes(self):
        physical=dict(self.baseline,services_bytes=424,features=['baseline_v7','physical_rig_v1'])
        self.assertEqual(self.read(physical),physical)
        for prefix,features in [(176,['baseline_v7']),(400,['baseline_v7','capsule_cast_v1']),(392,['baseline_v7','camera_pose_v1'])]:
            old=dict(self.baseline,services_bytes=prefix,features=features)
            with self.subTest(old=old):self.assertEqual(self.read(old),old)
        for features in [physical['features']+['capsule_cast_v1'],physical['features']+['camera_pose_v1'],physical['features']+['animation_ik_v1']]:
            value=dict(physical,features=features)
            with self.subTest(features=features):self.assertEqual(self.read(value),value)
    def test_extent_never_grants_rights(self):
        physical=dict(self.baseline,services_bytes=424,features=['baseline_v7','physical_rig_v1'])
        for extent in [176,400,408,416,423,425,480]:
            with self.subTest(extent=extent),self.assertRaises(ValueError):self.read(dict(physical,services_bytes=extent))
        for features in [['baseline_v7'],['physical_rig_v1'],physical['features']+['physical_rig_v1'],physical['features']+['unknown_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):self.read(dict(physical,features=features))
        with self.assertRaises(ValueError):self.read(dict(physical,services_version=8))
        with self.assertRaises(ValueError):self.read(dict(physical,services_bytes=424.0))
        with self.assertRaises(ValueError):self.read(dict(physical,services_bytes=True))
    def test_union_still_honors_other_dependencies(self):
        physical=dict(self.baseline,services_bytes=424,features=['baseline_v7','physical_rig_v1'])
        for feature in ['animation_layers_v1','animation_ik_end_orientation_v1','animation_grounding_control_v1','animation_rate_v1']:
            with self.subTest(feature=feature),self.assertRaises(ValueError):self.read(dict(physical,features=physical['features']+[feature]))
        features=['baseline_v7','physical_rig_v1','animation_inertial_v1','animation_layers_v1','animation_rate_v1','animation_ik_v1','animation_ik_end_orientation_v1','animation_grounding_control_v1']
        value=dict(physical,features=features);self.assertEqual(self.read(value),value)
if __name__=='__main__':unittest.main()
