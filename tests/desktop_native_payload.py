#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Portable native-install selection/staging checks; no compiler or GUI needed."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('desktop_payload_under_test', ROOT/'scripts/build_desktop.py')
desktop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(desktop)
VERSION = '0.0.191'


def installed(root, *, audio=True):
    (root/'bin').mkdir(parents=True)
    (root/'share/poima').mkdir(parents=True)
    document = {'format': 'poima.runtime', 'version': 1, 'engine_version': VERSION,
                'target_os': 'Windows', 'target_arch': 'x86_64', 'executable': 'bin/poima.exe',
                'features': {'simulation': True, 'renderer': True, 'managed': True,
                             'audio': audio, 'editor': False}}
    (root/'runtime.json').write_text(json.dumps(document))
    for name in ['poima.exe', 'poima_desktop.dll'] + (['phonon.dll'] if audio else []):
        (root/'bin'/name).write_bytes(('installed '+name).encode())
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md', 'TRADEMARKS.md'):
        (root/'share/poima'/name).write_text('notice '+name)
    return document


class InstalledPayloadTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.install = self.root/'installed'
        self.document = installed(self.install)
        self.stage = self.root/'stage'
        self.stage.mkdir()

    def rewrite(self):
        (self.install/'runtime.json').write_text(json.dumps(self.document))

    def test_uses_install_bin_even_with_different_flat_artifacts(self):
        # Both a build-root and install-root flat file are irrelevant. A target
        # output subdirectory is CMake's responsibility, not a Python guess.
        build = self.root/'build'
        build.mkdir()
        for name in ('poima.exe', 'poima_desktop.dll', 'phonon.dll'):
            (build/name).write_bytes(b'stale build artifact')
            (self.install/name).write_bytes(b'wrong install-root artifact')
        result = desktop.copy_installed_native(self.install, self.stage, VERSION)
        self.assertEqual(set(result['files']), {'poima.exe', 'poima_desktop.dll', 'phonon.dll'})
        for name, identity in result['files'].items():
            self.assertEqual((self.stage/name).read_bytes(), (self.install/'bin'/name).read_bytes())
            self.assertEqual(identity['sha256'], desktop.digest(self.stage/name))
        self.assertEqual((build/'poima.exe').read_bytes(), b'stale build artifact')

    def test_valid_install_needs_no_flat_build_files(self):
        result = desktop.installed_native_payload(self.install, VERSION)
        self.assertEqual(result['engine_version'], VERSION)
        self.assertEqual(len(result['files']), 3)
        self.assertFalse((self.root/'build').exists())

    def test_missing_native_file_never_falls_back_to_flat_file(self):
        for name in ('poima.exe', 'poima_desktop.dll', 'phonon.dll'):
            with self.subTest(name=name):
                original = (self.install/'bin'/name).read_bytes()
                (self.install/'bin'/name).unlink()
                (self.install/name).write_bytes(b'cannot replace missing installed binary')
                with self.assertRaisesRegex(RuntimeError, 'Missing installed native payload'):
                    desktop.copy_installed_native(self.install, self.stage, VERSION)
                self.assertEqual(list(self.stage.iterdir()), [])
                (self.install/'bin'/name).write_bytes(original)

    def test_audio_false_does_not_require_audio_library(self):
        self.document['features']['audio'] = False
        self.rewrite()
        (self.install/'bin/phonon.dll').unlink()
        result = desktop.copy_installed_native(self.install, self.stage, VERSION)
        self.assertEqual(set(result['files']), {'poima.exe', 'poima_desktop.dll'})

    def test_version_must_match_selected_source(self):
        with self.assertRaisesRegex(RuntimeError, 'selected source version'):
            desktop.installed_native_payload(self.install, '0.0.190')
        (self.root/'CMakeLists.txt').write_text('project(Poima VERSION 0.0.191 LANGUAGES C CXX)\n')
        with mock.patch.object(desktop, 'ROOT', self.root):
            self.assertEqual(desktop.source_engine_version(), VERSION)

    def test_required_feature_types_and_enabled_flags(self):
        for feature, value in [('simulation', False), ('renderer', False), ('managed', False),
                               ('audio', 1), ('renderer', 1), ('navigation', 'true')]:
            with self.subTest(feature=feature, value=value):
                current = dict(self.document['features'])
                self.document['features'][feature] = value
                self.rewrite()
                with self.assertRaisesRegex(RuntimeError, 'Boolean feature flags'):
                    desktop.installed_native_payload(self.install, VERSION)
                self.document['features'] = current

    def test_layout_platform_and_descriptor_types(self):
        for key, value in [('executable', '../poima.exe'), ('executable', 'poima.exe'),
                           ('target_os', 'Linux'), ('target_arch', 'arm64'),
                           ('version', True), ('format', 'other')]:
            with self.subTest(key=key, value=value):
                original = self.document[key]
                self.document[key] = value
                self.rewrite()
                with self.assertRaisesRegex(RuntimeError, 'Windows x86_64 layout'):
                    desktop.installed_native_payload(self.install, VERSION)
                self.document[key] = original

    def test_required_installed_notice_missing_is_preflight_failure(self):
        (self.install/'share/poima/LICENSE').unlink()
        with self.assertRaisesRegex(RuntimeError, 'Missing installed native payload'):
            desktop.copy_installed_native(self.install, self.stage, VERSION)
        self.assertEqual(list(self.stage.iterdir()), [])

    def test_existing_stage_native_is_not_overwritten_or_partly_populated(self):
        target = self.stage/'poima_desktop.dll'
        target.write_bytes(b'preexisting managed-stage native file')
        before = desktop.inventory(self.stage)
        with self.assertRaisesRegex(RuntimeError, 'already contains a native payload'):
            desktop.copy_installed_native(self.install, self.stage, VERSION)
        self.assertEqual(desktop.inventory(self.stage), before)
        self.assertEqual(target.read_bytes(), b'preexisting managed-stage native file')

    def test_change_since_preflight_is_rejected_before_copy(self):
        expected = desktop.installed_native_payload(self.install, VERSION)
        (self.install/'bin/poima.exe').write_bytes(b'changed after selection')
        with self.assertRaisesRegex(RuntimeError, 'changed after preflight'):
            desktop.copy_installed_native(self.install, self.stage, VERSION, expected)
        self.assertEqual(list(self.stage.iterdir()), [])

    def test_main_rejects_bad_install_before_managed_publish_or_activation(self):
        checkout = self.root/'checkout'
        output = checkout/'build/desktop'
        output.mkdir(parents=True)
        (checkout/'CMakeLists.txt').write_text('project(Poima VERSION 0.0.191 LANGUAGES C CXX)\n')
        sdk = checkout/'.cache/toolchains/dotnet-10.0.401/dotnet'
        sdk.parent.mkdir(parents=True)
        sdk.write_bytes(b'not executed')
        (output/'Poima.Editor.exe').write_bytes(b'previous public candidate')
        manifest = {'format': 'poima.desktop-build', 'version': 1,
                    'files': desktop.inventory(output)}
        (output/desktop.MANIFEST).write_text(json.dumps(manifest))
        pointer = checkout/'build/desktop-current.txt'
        pointer.write_bytes(b'previous launcher pointer\n')
        before = desktop.inventory(output)
        calls = []

        def command(argv, **kwargs):
            calls.append(list(map(str, argv)))
            self.assertEqual(argv[:2], ['cmake', '--install'])
            self.assertEqual(argv[-1], '--strip')
            prefix = Path(argv[argv.index('--prefix')+1])
            installed(prefix)
            (prefix/'bin/poima_desktop.dll').unlink()

        with mock.patch.object(desktop, 'ROOT', checkout), \
                mock.patch.object(sys, 'argv', ['build_desktop.py', '--skip-native', '--output', str(output)]), \
                mock.patch.object(desktop.subprocess, 'run', side_effect=command):
            with self.assertRaisesRegex(RuntimeError, 'Missing installed native payload'):
                desktop.main()
        self.assertEqual(len(calls), 1)
        self.assertEqual(desktop.inventory(output), before)
        self.assertEqual(pointer.read_bytes(), b'previous launcher pointer\n')


if __name__ == '__main__':
    unittest.main()
