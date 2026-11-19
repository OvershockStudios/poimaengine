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
    def test_drive_named_dependency_and_exact_prefix(self):
        drive=dict(self.baseline,services_bytes=448,features=['baseline_v7','physical_rig_drive_v1','physical_rig_v1'])
        self.assertEqual(self.read(drive),drive)
        for extent in [176,400,424,432,440,447,449,480,448.0,True]:
            with self.subTest(extent=extent),self.assertRaises(ValueError):self.read(dict(drive,services_bytes=extent))
        for features in [['baseline_v7'],['baseline_v7','physical_rig_v1'],['baseline_v7','physical_rig_drive_v1'],drive['features']+['physical_rig_drive_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):self.read(dict(drive,features=features))
        combined=dict(drive,features=drive['features']+['capsule_cast_v1'])
        self.assertEqual(self.read(combined),combined)
    def test_recovery_independent_permission_and_unions(self):
        recovery=dict(self.baseline,services_bytes=512,features=['baseline_v7','physical_recovery_v1','physical_rig_v1'])
        self.assertEqual(self.read(recovery),recovery)
        for extent in [176,424,448,496,504,511,513,512.0,True]:
            with self.subTest(extent=extent),self.assertRaises(ValueError):self.read(dict(recovery,services_bytes=extent))
        for names in [['baseline_v7','physical_recovery_v1'],['baseline_v7','physical_rig_v1'],recovery['features']+['physical_recovery_v1']]:
            with self.subTest(names=names),self.assertRaises(ValueError):self.read(dict(recovery,features=names))
        for added in [['physical_rig_drive_v1'],['physical_rig_drive_v1','physical_rig_animation_v1'],['camera_pose_v1']]:
            value=dict(recovery,features=recovery['features']+added)
            self.assertEqual(self.read(value),value)
        with self.assertRaises(ValueError):self.read(dict(recovery,features=recovery['features']+['physical_rig_animation_v1']))
    def test_item_attachment_named_independent_prefix(self):
        attachment=dict(self.baseline,services_bytes=528,features=['baseline_v7','item_attachment_v1'])
        self.assertEqual(self.read(attachment),attachment)
        combined=dict(attachment,features=attachment['features']+['physical_rig_v1','physical_recovery_v1'])
        self.assertEqual(self.read(combined),combined)
        for invalid in [dict(attachment,services_bytes=512),dict(attachment,features=['baseline_v7']),
                        dict(attachment,features=attachment['features']+['physical_recovery_v1'])]:
            with self.assertRaises(ValueError):self.read(invalid)
    def test_attachment_contact_uses_existing_extent_and_requires_attachment(self):
        guard='item_attachment_contact_v1'
        attachment='item_attachment_v1'
        contact=dict(self.baseline,services_bytes=528,features=['baseline_v7',attachment,guard])
        self.assertEqual(self.read(contact),contact)
        for invalid in [dict(contact,services_bytes=544),dict(contact,features=['baseline_v7',guard])]:
            with self.assertRaises(ValueError):self.read(invalid)
    def test_surface_contact_requires_frame_contact(self):
        attachment,guard,surface='item_attachment_v1','item_attachment_contact_v1','item_attachment_surface_v1'
        value=dict(self.baseline,services_bytes=528,features=['baseline_v7',attachment,guard,surface])
        self.assertEqual(self.read(value),value)
        for feature in (attachment,guard):
            with self.subTest(feature=feature),self.assertRaises(ValueError):
                self.read(dict(value,features=[name for name in value['features'] if name!=feature]))
    def test_recoverable_grip_requires_surface_contact(self):
        features=['baseline_v7','item_attachment_v1','item_attachment_contact_v1',
                  'item_attachment_surface_v1','item_attachment_recovery_v1']
        value=dict(self.baseline,services_bytes=528,features=features)
        self.assertEqual(self.read(value),value)
        with self.assertRaises(ValueError):
            self.read(dict(value,features=[name for name in features if name!='item_attachment_surface_v1']))
if __name__=='__main__':unittest.main()
