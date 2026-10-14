# SPDX-License-Identifier: Apache-2.0
"""Synthetic filesystem tests only; never selects a real checkout build tree."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/build_storage.py'
spec = importlib.util.spec_from_file_location('build_storage', SCRIPT)
storage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(storage)


def elf(kind=1):
    return (b'\x7fELF\x02\x01\x01' + bytes(9) +
            struct.pack('<HHIQQQIHHHHHH', kind, 62, 1, 0, 0, 64, 0, 64, 0, 0, 64, 1, 0) + bytes(64))


def coff(big=False):
    if big:
        header = (struct.pack('<HHHHI', 0, 65535, 2, 0x8664, 0) +
                  struct.pack('<IHH8B', 0xD1BAA1C7, 0xBAEE, 0x4BA9, 0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8) + struct.pack('<IIIIIII', 0, 0, 0, 0, 1, 0, 0))
    else:
        header = struct.pack('<HHIIIHH', 0x8664, 1, 0, 0, 0, 0, 0)
    section = struct.pack('<8sIIIIIIHHI', b'.text', 0, 0, 4, len(header)+40, 0, 0, 0, 0, 0x60500020)
    return header + section + b'\x90\x90\x90\xc3'


class RetiredBuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='poima-retired-build-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root/'CMakeLists.txt').write_text('project(Poima)\n')
        self.tree = self.root/'build/old'
        (self.tree/'CMakeFiles/game.dir').mkdir(parents=True)
        (self.tree/'CMakeCache.txt').write_text('CMAKE_PROJECT_NAME:STATIC=Poima\nCMAKE_HOME_DIRECTORY:INTERNAL='+str(self.root)+'\n')
        (self.tree/'build.ninja').write_text('# CMAKE generated file: DO NOT EDIT!\n')
        self.object = self.write('CMakeFiles/game.dir/main.cpp.o', elf())
        self.source = self.write('CMakeFiles/game.dir/keep.cpp', b'int source;')
        self.data = self.write('CMakeFiles/game.dir/not-object.o', b'licensed mesh or other data, not a compiler object')
        self.binary = self.write('bin/game.exe', b'MZ\0shipped-binary')
        self.report = self.write('reports/owner.json', b'{"passed":true}')

    def write(self, label, data):
        path = self.tree/label
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        if path.suffix.lower() in ('.o','.obj','.pch','.gch'):
            with (self.tree/'build.ninja').open('a') as graph:
                graph.write('build '+label.replace(' ', '$ ')+': CXX_COMPILER__game_RelWithDebInfo input.cpp\n')
        return path

    def plan(self, protect=()):
        return storage.make_plan(self.root, ['build/old'], protect)

    def frozen(self):
        plan = self.plan()
        path = self.root/'reviewed-plan.json'
        path.write_text(json.dumps(plan))
        return path, hashlib.sha256(path.read_bytes()).hexdigest()

    def test_report_preserves_sources_assets_binaries_and_nonobject_o(self):
        before = {p: p.read_bytes() for p in (self.object,self.source,self.data,self.binary,self.report)}
        plan = self.plan()
        self.assertEqual([x['path'] for x in plan['trees'][0]['candidates']], ['build/old/CMakeFiles/game.dir/main.cpp.o'])
        self.assertEqual(plan['candidate_bytes'], len(elf()))
        self.assertEqual(before, {p:p.read_bytes() for p in before})
        self.assertTrue(any(x['path'].endswith('not-object.o') for x in plan['trees'][0]['skipped']))

    def test_supported_coff_bigobj_and_bounded_cmake_pch(self):
        self.write('obj/ordinary.obj', coff())
        self.write('CMakeFiles/game.dir/big.obj', coff(True))
        self.write('CMakeFiles/game.dir/cmake_pch.hxx.pch', b'CPCH\x01\x08\0\0'+bytes(56))
        self.write('CMakeFiles/game.dir/cmake_pch.hxx.gch', b'gpch+014'+bytes(56))
        self.write('obj/custom.pch', b'CPCH\x01\x08\0\0'+bytes(56))
        kinds = {x['kind'] for x in self.plan()['trees'][0]['candidates']}
        self.assertEqual(kinds, {'ELF-relocatable','COFF-relocatable','GCC-CMake-PCH','Clang-CMake-PCH'})
        self.assertNotIn('build/old/obj/custom.pch', [x['path'] for x in self.plan()['trees'][0]['candidates']])

    def test_nonintermediate_and_executable_elf_not_selected(self):
        self.write('assets/mesh.o', elf())
        self.write('CMakeFiles/game.dir/actual-executable.o', elf(2))
        self.write('CMakeFiles/game.dir/truncated.o', b'\x7fELF\2\1\1'+bytes(45))
        self.assertEqual(len(self.plan()['trees'][0]['candidates']), 1)

    def test_prebuilt_objects_and_source_vendor_evidence_are_preserved(self):
        for label in ('licensed-inputs/obj/mesh.obj', '_deps/vendor-src/obj/mesh.obj',
                      'evidence/CMakeFiles/sample.dir/retained.o', 'sdk/CMakeFiles/prebuilt.dir/source.obj',
                      'owned-sources/CMakeFiles/source.dir/item.o', 'obj/licensed-inputs/mesh.obj',
                      'CMakeFiles/game.dir/assets/data.o'):
            self.write(label, coff())  # Even explicitly named compiler output cannot override preservation.
        hidden = self.tree/'CMakeFiles/game.dir/unlisted.o';hidden.write_bytes(elf())
        self.assertEqual([x['path'] for x in self.plan()['trees'][0]['candidates']],
                         ['build/old/CMakeFiles/game.dir/main.cpp.o'])

    def test_non_ninja_root_and_unfamiliar_rules_preserve_intermediates(self):
        (self.tree/'build.ninja').write_text('# CMAKE generated file: DO NOT EDIT!\nbuild CMakeFiles/game.dir/main.cpp.o: CUSTOM_COMMAND input\n')
        self.assertEqual(self.plan()['candidate_bytes'],0)
        (self.tree/'build.ninja').unlink()
        self.assertEqual(self.plan()['candidate_bytes'],0)

    def test_escaped_literal_ninja_output_is_owned(self):
        output = self.write('CMakeFiles/game.dir/two words.cpp.o', coff())
        self.assertIn(output.relative_to(self.root).as_posix(), [x['path'] for x in self.plan()['trees'][0]['candidates']])

    def test_malformed_or_truncated_headers_are_preserved(self):
        broken = [b'', b'\x7fELF', b'\x7fELF\2\1\1'+bytes(45), coff()[:30],
                  coff(True)[:55], b'MZ'+bytes(126), elf()[:80], b'CPCH\1\10\0\0']
        for number, data in enumerate(broken):
            with self.subTest(number=number):
                file = self.write('CMakeFiles/game.dir/broken'+str(number)+'.o', data)
                self.assertIsNone(storage.object_kind(file))
        self.assertEqual(len(self.plan()['trees'][0]['candidates']), 1)

    def test_link_inserted_after_report_blocks_apply(self):
        path, digest = self.frozen()
        outside = self.root/'retained-object.o'; outside.write_bytes(elf())
        self.object.unlink()
        try:self.object.symlink_to(outside)
        except OSError as error:self.skipTest(str(error))
        with self.assertRaisesRegex(storage.Refused, 'Linked'):storage.apply_plan(path, digest)
        self.assertEqual(outside.read_bytes(), elf())

    def test_apply_removes_only_planned_objects_and_retains_plan(self):
        path, digest = self.frozen()
        result = storage.apply_plan(path, digest)
        self.assertTrue(result['passed'])
        self.assertEqual(result['deleted'], ['build/old/CMakeFiles/game.dir/main.cpp.o'])
        self.assertFalse(self.object.exists())
        self.assertTrue(all(p.exists() for p in (self.source,self.data,self.binary,self.report,path)))
        with self.assertRaises(storage.Refused):
            storage.apply_plan(path, digest)

    def test_all_files_preflight_before_any_deletion(self):
        second = self.write('CMakeFiles/game.dir/z.cpp.o', coff())
        path, digest = self.frozen()
        second.write_bytes(coff()+b'changed')
        with self.assertRaises(storage.Refused):
            storage.apply_plan(path, digest)
        self.assertTrue(self.object.exists())

    def test_hash_detects_same_size_same_mtime_changes(self):
        path, digest = self.frozen()
        old = self.object.stat()
        data = bytearray(self.object.read_bytes());data[40] ^= 1
        self.object.write_bytes(data)
        os.utime(self.object, ns=(old.st_atime_ns, old.st_mtime_ns))
        with self.assertRaises(storage.Refused):
            storage.apply_plan(path, digest)
        self.assertTrue(self.object.exists())

    def test_plan_hash_and_freshness_required(self):
        path, digest = self.frozen()
        with self.assertRaises(storage.Refused):storage.apply_plan(path, '0'*64)
        with self.assertRaises(storage.Refused):storage.apply_plan(path, digest, now_ns=time.time_ns()+901*10**9)
        with self.assertRaises(storage.Refused):storage.apply_plan(path, digest, now_ns=0)
        path.write_text(path.read_text()+' ')
        with self.assertRaises(storage.Refused):storage.apply_plan(path, digest)
        self.assertTrue(self.object.exists())

    def test_whole_root_thin_archive_rejection_even_outside_objects(self):
        self.write('assets/hidden-reference.a', b'!<thin>\nrest')
        with self.assertRaisesRegex(storage.Refused, 'Thin archive'):self.plan()

    def test_lock_anywhere_rejects_and_new_lock_blocks_apply(self):
        path, digest = self.frozen()
        self.write('nested/.ninja_lock', b'')
        with self.assertRaisesRegex(storage.Refused, 'lock'):storage.apply_plan(path, digest)
        self.assertTrue(self.object.exists())

    def test_explicit_protected_retention_path_rejects_overlap(self):
        for protect in (['build/old'], ['build/old/reports'], ['build']):
            with self.assertRaisesRegex(storage.Refused, 'protected'):self.plan(protect)
        self.assertTrue(self.object.exists())

    def test_current_preset_is_protected_and_changed_presets_invalidate_plan(self):
        path, digest = self.frozen()
        (self.root/'CMakePresets.json').write_text(json.dumps({'version':5,'configurePresets':[{'name':'dev','binaryDir':'${sourceDir}/build/old'}]}))
        with self.assertRaisesRegex(storage.Refused, 'protected'):self.plan()
        with self.assertRaises(storage.Refused):storage.apply_plan(path, digest)
        self.assertTrue(self.object.exists())

    def test_unknown_preset_configuration_fails_closed(self):
        (self.root/'CMakePresets.json').write_text(json.dumps({'include':['other.json']}))
        with self.assertRaisesRegex(storage.Refused, 'includes'):self.plan()

    def test_noncmake_external_or_checkout_build_container_rejected(self):
        for labels in (['build'], ['../elsewhere'], ['C:/outside'], ['build/absent']):
            with self.assertRaises(storage.Refused):storage.make_plan(self.root, labels)
        (self.root/'build/another').mkdir()
        with self.assertRaises(storage.Refused):storage.make_plan(self.root, ['build/another'])
        (self.tree/'CMakeCache.txt').write_text('CMAKE_PROJECT_NAME:STATIC=Other\nCMAKE_HOME_DIRECTORY:INTERNAL='+str(self.root))
        with self.assertRaisesRegex(storage.Refused, 'not this'):self.plan()

    def test_linked_file_and_parent_rejected(self):
        try:
            (self.tree/'link').symlink_to(self.source)
        except OSError as error:self.skipTest(str(error))
        with self.assertRaisesRegex(storage.Refused, 'Linked'):self.plan()

    def test_linked_selection_ancestor_rejected(self):
        try:(self.root/'build/alias').symlink_to(self.tree, target_is_directory=True)
        except OSError as error:self.skipTest(str(error))
        with self.assertRaisesRegex(storage.Refused, 'Linked'):storage.make_plan(self.root, ['build/alias'])

    def test_reparse_attribute_rejected(self):
        original = Path.lstat
        target = self.tree
        def flagged(path, *args, **kwargs):
            if path == target:return SimpleNamespace(st_mode=stat.S_IFDIR, st_file_attributes=0x400)
            return original(path, *args, **kwargs)
        with patch.object(Path, 'lstat', flagged):
            with self.assertRaisesRegex(storage.Refused, 'reparse'):self.plan()

    def test_hardlinked_object_rejected(self):
        try:os.link(self.object, self.tree/'duplicate.o')
        except OSError as error:self.skipTest(str(error))
        with self.assertRaisesRegex(storage.Refused, 'Hardlinked'):self.plan()

    def test_late_change_stops_after_recorded_partial_application(self):
        second = self.write('CMakeFiles/game.dir/z.cpp.o', coff())
        path, digest = self.frozen()
        def replace_later(target):
            os.unlink(target)
            second.write_bytes(b'newer output must stay')
        result = storage.apply_plan(path, digest, unlink=replace_later)
        self.assertFalse(result['passed'])
        self.assertEqual(result['deleted'], ['build/old/CMakeFiles/game.dir/main.cpp.o'])
        self.assertEqual(second.read_bytes(), b'newer output must stay')

    def test_report_cli_never_deletes_and_plan_is_exclusive(self):
        from contextlib import redirect_stdout
        from io import StringIO
        path = self.root/'reviewed-plan.json'
        args = ['--checkout', str(self.root), '--retired-build','build/old','--write-plan',str(path)]
        with redirect_stdout(StringIO()):
            self.assertEqual(storage.main(args),0)
            self.assertEqual(storage.main(args),1)
        self.assertTrue(self.object.exists())


if __name__ == '__main__':
    unittest.main()
