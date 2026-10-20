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
    def test_grounding_control_named_prefix(self):
        features=['animation_grounding_control_v1','animation_ik_end_orientation_v1','animation_ik_v1','baseline_v7']
        required=dict(self.baseline,services_bytes=368,features=features)
        self.assertEqual(self.read(required),required)
        for extent in [176,288,336,352,360,367,369]:
            with self.subTest(extent=extent),self.assertRaises(ValueError):self.read(dict(required,services_bytes=extent))
        for invalid in [features[1:],features[:1]+features[2:],features[:2]+features[3:],features+[features[0]]]:
            with self.subTest(features=invalid),self.assertRaises(ValueError):self.read(dict(required,features=invalid))
        # The preexisting352 orientation requirement remains accepted unchanged.
        old=dict(self.baseline,services_bytes=352,features=features[1:])
        self.assertEqual(self.read(old),old)
    def test_ik_end_orientation_named_prefix_and_point_dependency(self):
        value=dict(self.baseline,services_bytes=352,features=['animation_ik_end_orientation_v1','animation_ik_v1','baseline_v7'])
        self.assertEqual(self.read(value),value)
        for extent in [176,288,304,320,336,344,351,353]:
            with self.subTest(extent=extent),self.assertRaises(ValueError):self.read(dict(value,services_bytes=extent))
        for features in [['baseline_v7','animation_ik_end_orientation_v1'],
                         ['baseline_v7','animation_ik_v1'],
                         value['features']+['animation_ik_end_orientation_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):self.read(dict(value,features=features))
    def test_baseline_and_named_extension(self):
        self.assertEqual(self.read(self.baseline),self.baseline)
        extended=dict(self.baseline,services_bytes=192,features=['animation_inertial_v1','baseline_v7'])
        self.assertEqual(self.read(extended),extended)
        layers=dict(self.baseline,services_bytes=208,features=['animation_inertial_v1','animation_layers_v1','baseline_v7'])
        self.assertEqual(self.read(layers),layers)
    def test_input_actions_independent_and_every_feature_combination(self):
        optional=['animation_inertial_v1','animation_layers_v1','character_input_v1',
                  'navigation_query_v1','hierarchical_instances_v1','player_preferences_v1']
        for mask in range(1 << len(optional)):
            features=sorted(['baseline_v7','input_actions_v1']+
                            [name for index,name in enumerate(optional) if mask & (1 << index)])
            required=dict(self.baseline,services_bytes=264,features=features)
            with self.subTest(features=features):
                if 'animation_layers_v1' in features and 'animation_inertial_v1' not in features:
                    with self.assertRaises(ValueError):self.read(required)
                else:self.assertEqual(self.read(required),required)
            for extent in [176,192,208,216,224,232,256,263,265,272]:
                with self.subTest(features=features,extent=extent),self.assertRaises(ValueError):
                    self.read(dict(required,services_bytes=extent))
        for features in [['baseline_v7'],['input_actions_v1'],
                         ['baseline_v7','input_actions_v1','input_actions_v1'],
                         ['baseline_v7','input_actions_v1','future_action_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):
                self.read(dict(self.baseline,services_bytes=264,features=features))
    def test_hit_regions_independent_and_all_nine_feature_names(self):
        optional=['animation_inertial_v1','animation_layers_v1','character_input_v1',
                  'navigation_query_v1','hierarchical_instances_v1','player_preferences_v1','input_actions_v1']
        for mask in range(1 << len(optional)):
            features=sorted(['baseline_v7','hit_regions_v1']+
                            [name for index,name in enumerate(optional) if mask & (1 << index)])
            required=dict(self.baseline,services_bytes=272,features=features)
            with self.subTest(features=features):
                if 'animation_layers_v1' in features and 'animation_inertial_v1' not in features:
                    with self.assertRaises(ValueError):self.read(required)
                else:self.assertEqual(self.read(required),required)
            for extent in [176,192,208,216,224,232,256,264,271,273]:
                with self.subTest(features=features,extent=extent),self.assertRaises(ValueError):
                    self.read(dict(required,services_bytes=extent))
        for features in [['baseline_v7'],['hit_regions_v1'],
                         ['baseline_v7','hit_regions_v1','hit_regions_v1'],
                         ['baseline_v7','hit_regions_v1','future_hit_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):
                self.read(dict(self.baseline,services_bytes=272,features=features))
    def test_character_independence_and_combinations(self):
        for animation in [[], ['animation_inertial_v1'], ['animation_inertial_v1', 'animation_layers_v1']]:
            features=sorted(['baseline_v7', 'character_input_v1']+animation)
            required=dict(self.baseline,services_bytes=216,features=features)
            with self.subTest(features=features):
                self.assertEqual(self.read(required),required)
            for extent in [176,192,208,215,217,240]:
                with self.subTest(features=features,extent=extent),self.assertRaises(ValueError):
                    self.read(dict(required,services_bytes=extent))
        for features in [['baseline_v7'], ['baseline_v7','animation_inertial_v1'],
                         ['baseline_v7','animation_inertial_v1','animation_layers_v1'],
                         ['baseline_v7','animation_layers_v1','character_input_v1'],
                         ['baseline_v7','character_input_v1','character_input_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):
                self.read(dict(self.baseline,services_bytes=216,features=features))
    def test_navigation_independence_and_every_named_combination(self):
        for character in [False, True]:
            for animation in [[], ['animation_inertial_v1'], ['animation_inertial_v1', 'animation_layers_v1']]:
                features=sorted(['baseline_v7', 'navigation_query_v1']+(['character_input_v1'] if character else [])+animation)
                required=dict(self.baseline,services_bytes=224,features=features)
                with self.subTest(features=features):self.assertEqual(self.read(required),required)
                for extent in [176,192,208,216,223,225,240]:
                    with self.subTest(features=features,extent=extent),self.assertRaises(ValueError):
                        self.read(dict(required,services_bytes=extent))
        for features in [['baseline_v7'],['navigation_query_v1'],['baseline_v7','navigation_query_v1','navigation_query_v1'],
                         ['baseline_v7','navigation_query_v1','animation_layers_v1'],
                         ['baseline_v7','navigation_query_v1','future_feature_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):
                self.read(dict(self.baseline,services_bytes=224,features=features))

    def test_instance_independence_and_combinations(self):
        for navigation in [False,True]:
            for character in [False,True]:
                for animation in [[],['animation_inertial_v1'],['animation_inertial_v1','animation_layers_v1']]:
                    features=sorted(['baseline_v7','hierarchical_instances_v1']+animation+
                                    (['navigation_query_v1'] if navigation else [])+(['character_input_v1'] if character else []))
                    required=dict(self.baseline,services_bytes=232,features=features)
                    with self.subTest(features=features):self.assertEqual(self.read(required),required)
                    for extent in [176,192,208,216,224,231,233,256]:
                        with self.subTest(features=features,extent=extent),self.assertRaises(ValueError):
                            self.read(dict(required,services_bytes=extent))
        for features in [['baseline_v7'],['hierarchical_instances_v1'],
                         ['baseline_v7','hierarchical_instances_v1','hierarchical_instances_v1'],
                         ['baseline_v7','hierarchical_instances_v1','animation_layers_v1'],
                         ['baseline_v7','hierarchical_instances_v1','future_feature_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):
                self.read(dict(self.baseline,services_bytes=232,features=features))

    def test_layer_dependency_and_named_prefix(self):
        features=['baseline_v7','animation_inertial_v1','animation_layers_v1']
        for extent in [176,192,200,207,209,240]:
            with self.subTest(extent=extent),self.assertRaises(ValueError):
                self.read(dict(self.baseline,services_bytes=extent,features=features))
        for incomplete in [['baseline_v7'],['baseline_v7','animation_layers_v1'],
                           ['baseline_v7','animation_inertial_v1'],features+['animation_layers_v1']]:
            with self.subTest(features=incomplete),self.assertRaises(ValueError):
                self.read(dict(self.baseline,services_bytes=208,features=incomplete))
    def test_preferences_independent_and_every_existing_feature_combination(self):
        optional=['animation_inertial_v1','animation_layers_v1','character_input_v1',
                  'navigation_query_v1','hierarchical_instances_v1']
        for mask in range(1 << len(optional)):
            features=sorted(['baseline_v7','player_preferences_v1']+
                            [name for index,name in enumerate(optional) if mask & (1 << index)])
            required=dict(self.baseline,services_bytes=256,features=features)
            if 'animation_layers_v1' in features and 'animation_inertial_v1' not in features:
                with self.subTest(features=features),self.assertRaises(ValueError):self.read(required)
                continue
            with self.subTest(features=features):self.assertEqual(self.read(required),required)
            for extent in [176,192,208,216,224,232,248,255,257,272]:
                with self.subTest(features=features,extent=extent),self.assertRaises(ValueError):
                    self.read(dict(required,services_bytes=extent))
        for features in [['baseline_v7'],['player_preferences_v1'],
                         ['baseline_v7','player_preferences_v1','player_preferences_v1'],
                         ['baseline_v7','player_preferences_v1','future_feature_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):
                self.read(dict(self.baseline,services_bytes=256,features=features))
    def test_ik_independent_all_named_combinations(self):
        optional=['animation_inertial_v1','animation_layers_v1','character_input_v1',
                  'navigation_query_v1','hierarchical_instances_v1','player_preferences_v1','input_actions_v1','hit_regions_v1']
        for mask in range(256):
            features=sorted(['baseline_v7','animation_ik_v1']+[name for i,name in enumerate(optional) if mask & (1 << i)])
            required=dict(self.baseline,services_bytes=288,features=features)
            if 'animation_layers_v1' in features and 'animation_inertial_v1' not in features:
                with self.subTest(features=features),self.assertRaises(ValueError):self.read(required)
                continue
            with self.subTest(features=features):self.assertEqual(self.read(required),required)
            for extent in [176,192,208,216,224,232,256,264,272,280,287,289]:
                with self.subTest(features=features,extent=extent),self.assertRaises(ValueError):self.read(dict(required,services_bytes=extent))
        for features in [['baseline_v7'],['animation_ik_v1'],['baseline_v7','animation_ik_v1','animation_ik_v1'],['baseline_v7','animation_ik_v1','future_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):self.read(dict(self.baseline,services_bytes=288,features=features))
    def test_root_motion_independent_all_named_combinations(self):
        optional=['animation_inertial_v1','animation_layers_v1','character_input_v1',
                  'navigation_query_v1','hierarchical_instances_v1','player_preferences_v1','input_actions_v1','hit_regions_v1','animation_ik_v1']
        for mask in range(512):
            features=sorted(['baseline_v7','animation_root_motion_v1']+[name for i,name in enumerate(optional) if mask & (1 << i)])
            required=dict(self.baseline,services_bytes=304,features=features)
            if 'animation_layers_v1' in features and 'animation_inertial_v1' not in features:
                with self.subTest(features=features),self.assertRaises(ValueError):self.read(required)
                continue
            with self.subTest(features=features):self.assertEqual(self.read(required),required)
            for extent in [176,192,208,216,224,232,256,264,272,288,296,303,305]:
                with self.subTest(features=features,extent=extent),self.assertRaises(ValueError):self.read(dict(required,services_bytes=extent))
        for features in [['baseline_v7'],['animation_root_motion_v1'],['baseline_v7','animation_root_motion_v1','animation_root_motion_v1'],['baseline_v7','animation_root_motion_v1','future_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):self.read(dict(self.baseline,services_bytes=304,features=features))
    def test_independent_camera_lens_prefix_and_unions(self):
        optional=['animation_inertial_v1','animation_layers_v1','character_input_v1','navigation_query_v1','hierarchical_instances_v1','player_preferences_v1','input_actions_v1','hit_regions_v1','animation_ik_v1','animation_root_motion_v1']
        for mask in range(1024):
            features=sorted(['baseline_v7','camera_lens_v1']+[name for i,name in enumerate(optional) if mask & (1 << i)])
            required=dict(self.baseline,services_bytes=320,features=features)
            if mask & 2 and not mask & 1:
                with self.subTest(mask=mask),self.assertRaises(ValueError):self.read(required)
            else:self.assertEqual(self.read(required),required)
        for extent in [176,304,312,319,321]:
            with self.subTest(extent=extent),self.assertRaises(ValueError):self.read(dict(self.baseline,services_bytes=extent,features=['baseline_v7','camera_lens_v1']))
        for features in [['baseline_v7'],['camera_lens_v1'],['baseline_v7','camera_lens_v1','camera_lens_v1'],['baseline_v7','camera_lens_v1','unknown_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):self.read(dict(self.baseline,services_bytes=320,features=features))
    def test_character_stance_independent_tail(self):
        optional=['animation_inertial_v1','animation_layers_v1','character_input_v1','navigation_query_v1','hierarchical_instances_v1','player_preferences_v1','input_actions_v1','hit_regions_v1','animation_ik_v1','animation_root_motion_v1','camera_lens_v1']
        for mask in range(2048):
            features=sorted(['baseline_v7','character_stance_v1']+[name for i,name in enumerate(optional) if mask&(1<<i)])
            required=dict(self.baseline,services_bytes=336,features=features)
            if mask&2 and not mask&1:
                with self.subTest(mask=mask),self.assertRaises(ValueError):self.read(required)
            else:self.assertEqual(self.read(required),required)
        for extent in [176,320,328,335,337]:
            with self.subTest(extent=extent),self.assertRaises(ValueError):self.read(dict(self.baseline,services_bytes=extent,features=['baseline_v7','character_stance_v1']))
        for features in [['baseline_v7'],['character_stance_v1'],['baseline_v7','character_stance_v1','character_stance_v1']]:
            with self.subTest(features=features),self.assertRaises(ValueError):self.read(dict(self.baseline,services_bytes=336,features=features))
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
