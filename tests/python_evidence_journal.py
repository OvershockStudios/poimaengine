# SPDX-License-Identifier: Apache-2.0
"""Pure append journal regression tests, no engines or subprocesses."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from evidence_journal import EvidenceJournal, JournalError, MAX_BYTES, _encode


def settled(row):
    return "reply" in row


class JournalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def make(self, **kwargs):
        return EvidenceJournal(self.root / "journal", **kwargs)

    def manifest(self, journal):
        return json.loads(journal.metadata_path.read_bytes())

    def decoded(self, journal):
        return [json.loads(line) for line in journal.path.read_bytes().splitlines()]

    def test_pending_then_completed_contiguous_prefix_and_exact_full_reconstruction(self):
        j = self.make()
        first = {"id": 1, "request": {"method": "inspect"}}
        second = {"id": 2, "reply": {"ok": True}}
        record = {"passed": False, "calls": [first, second], "owners": [{"clean": False}]}
        self.assertEqual(j.checkpoint(record, settled)["calls"], 0)
        self.assertEqual(j.path.read_bytes(), b"")
        first["reply"] = {"result": 7}
        self.assertEqual(j.checkpoint(record, settled)["calls"], 2)
        prefix = j.path.read_bytes()
        record["owners"][0]["clean"] = True
        record["passed"] = True
        j.checkpoint(record, settled)
        self.assertEqual(j.path.read_bytes(), prefix)
        self.assertEqual(self.manifest(j)["fields"], {"passed": True, "owners": [{"clean": True}]})
        result = j.finalize(record)
        self.assertTrue(result["passed"])
        self.assertEqual(json.loads(j.evidence_path.read_bytes()), record)
        self.assertEqual(result["journal_sha256"], hashlib.sha256(prefix).hexdigest())
        self.assertEqual(result["evidence_sha256"], hashlib.sha256(j.evidence_path.read_bytes()).hexdigest())
        self.assertEqual(result["aggregate_bytes"], sum(p.stat().st_size for p in j.directory.iterdir()))
        with self.assertRaises(JournalError):
            j.finalize(record)

    def test_final_partial_failure_retains_pending_rows_duplicates_and_order(self):
        j = self.make()
        rows = [{"id": 9, "reply": {"error": -32009}},
                {"id": 9, "request": {"method": "runtime.observe"}},
                {"id": 9, "reply": {"error": -32009}},
                {"id": None, "write_failed": True}]
        record = {"passed": False, "error": "request write failed", "calls": rows}
        self.assertEqual(j.checkpoint(record, settled)["calls"], 1)
        j.finalize(record)
        entries = self.decoded(j)
        self.assertEqual([entry["index"] for entry in entries], list(range(4)))
        self.assertEqual([entry["row"] for entry in entries], rows)
        self.assertEqual(json.loads(j.evidence_path.read_bytes()), record)
        self.assertFalse(json.loads(j.evidence_path.read_bytes())["passed"])

    def test_late_alias_mutation_of_admitted_row_fails_before_final_publish(self):
        for mutation in (lambda row: row["reply"]["values"].append(3),
                         lambda row: row.__setitem__("id", 1.0)):
            with self.subTest(mutation=mutation):
                with tempfile.TemporaryDirectory(dir=self.root) as directory:
                    j = EvidenceJournal(Path(directory) / "j")
                    row = {"id": 1, "reply": {"values": [1, 2]}}
                    record = {"passed": True, "calls": [row]}
                    j.checkpoint(record, settled)
                    mutation(row)
                    with self.assertRaisesRegex(JournalError, "late mutation"):
                        j.finalize(record)
                    self.assertFalse(j.evidence_path.exists())
                    self.assertTrue(j.failed)
                    with self.assertRaises(JournalError):
                        j.checkpoint(record, settled)

    def test_removed_committed_prefix_and_invalid_predicate_fail_closed(self):
        j = self.make()
        record = {"calls": [{"reply": 1}]}
        j.checkpoint(record, settled)
        before = j.metadata_path.read_bytes()
        with self.assertRaises(JournalError):
            j.checkpoint({"calls": []}, settled)
        self.assertEqual(j.metadata_path.read_bytes(), before)
        for predicate in (None, lambda _: 1):
            with tempfile.TemporaryDirectory(dir=self.root) as directory:
                other = EvidenceJournal(Path(directory) / "j")
                with self.assertRaises(JournalError):
                    other.checkpoint(record, predicate)
                self.assertFalse(other.metadata_path.exists())

    def test_append_error_preserves_previous_commit_and_partial_tail(self):
        j = self.make()
        record = {"calls": [{"reply": 1}]}
        j.checkpoint(record, settled)
        previous, prefix = j.metadata_path.read_bytes(), j.path.read_bytes()
        record["calls"].append({"reply": 2})
        def partial(data):
            with j.path.open("ab") as stream:
                stream.write(data[:7])
            raise OSError("injected append failure")
        with patch.object(j, "_append", partial):
            with self.assertRaises(OSError):
                j.checkpoint(record, settled)
        self.assertEqual(j.metadata_path.read_bytes(), previous)
        self.assertEqual(j.path.read_bytes()[:len(prefix)], prefix)
        self.assertGreater(j.path.stat().st_size, len(prefix))
        self.assertFalse(j.evidence_path.exists())
        with self.assertRaises(JournalError):
            j.finalize(record)

    def test_atomic_metadata_replace_error_preserves_prior_commit_and_raw_rows(self):
        j = self.make()
        record = {"calls": [{"reply": 1}], "owners": []}
        j.checkpoint(record, settled)
        previous = j.metadata_path.read_bytes()
        record["calls"].append({"reply": 2})
        with patch("evidence_journal.os.replace", side_effect=OSError("injected replace failure")):
            with self.assertRaises(OSError):
                j.checkpoint(record, settled)
        self.assertEqual(j.metadata_path.read_bytes(), previous)
        self.assertEqual(len(self.decoded(j)), 2)
        self.assertEqual(len(list(j.directory.glob("metadata.pending.*.json"))), 1)
        self.assertTrue(j.failed)
        self.assertFalse(j.evidence_path.exists())

    def test_cap_counts_old_metadata_new_metadata_and_final_reconstruction(self):
        j = self.make(cap_bytes=1000)
        record = {"calls": [], "value": "x" * 30}
        j.checkpoint(record, settled)
        old = j.metadata_path.read_bytes()
        record["calls"].append({"reply": "y" * 750})
        with self.assertRaises(JournalError):
            j.checkpoint(record, settled)
        self.assertEqual(j.metadata_path.read_bytes(), old)
        self.assertLessEqual(j._size(), 1000)
        # Journal+metadata fit, but final reconstruction (temp+final accounted)
        # must fail rather than silently exceed the aggregate cap.
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            other = EvidenceJournal(Path(directory) / "j", cap_bytes=900)
            big = {"calls": [{"reply": "z" * 260}]}
            other.checkpoint(big, settled)
            before = other.metadata_path.read_bytes()
            with self.assertRaises(JournalError):
                other.finalize(big)
            self.assertFalse(other.evidence_path.exists())
            self.assertEqual(self.manifest(other)["journal"], json.loads(before)["journal"])
            self.assertLessEqual(other._size(), 900)

    def test_exact_boundary_and_single_file_no_rollover_or_dedup(self):
        j = self.make()
        record = {"calls": [{"reply": i % 3, "id": 7} for i in range(100)]}
        for stop in (1, 2, 32, 33, 65, 100):
            j.checkpoint({"calls": record["calls"][:stop]}, settled)
        j.finalize(record)
        self.assertEqual(len(self.decoded(j)), 100)
        self.assertEqual([p.name for p in j.directory.iterdir() if p.suffix == ".jsonl"], ["calls.jsonl"])
        # Constructor validates the actual hard aggregate budget, including bool.
        for cap in (True, 0, -1, MAX_BYTES + 1, 1.0):
            with self.assertRaises(ValueError):
                EvidenceJournal(self.root / "invalid", cap_bytes=cap)
        with self.assertRaises(FileExistsError):
            EvidenceJournal(j.directory)

    def test_corrupt_or_reordered_journal_is_not_repaired_or_published(self):
        for damage in (lambda data: data[:-1],
                       lambda data: b"".join(reversed(data.splitlines(keepends=True))),
                       lambda data: data.replace(b'"index":0', b'"index":1', 1)):
            with self.subTest(damage=damage):
                with tempfile.TemporaryDirectory(dir=self.root) as directory:
                    j = EvidenceJournal(Path(directory) / "j")
                    record = {"calls": [{"reply": 0}, {"reply": 1}]}
                    j.checkpoint(record, settled)
                    original = j.path.read_bytes()
                    j.path.write_bytes(damage(original))
                    with self.assertRaises(JournalError):
                        j.finalize(record)
                    self.assertFalse(j.evidence_path.exists())
                    self.assertNotEqual(j.path.read_bytes(), original)

    def test_no_race_overwrite_and_final_link_failure_retains_diagnostic_temp(self):
        j = self.make()
        record = {"calls": [{"reply": {"nested": [True, None, "é"]}}]}
        j.checkpoint(record, settled)
        j.evidence_path.write_bytes(b"independent writer")
        with self.assertRaises(FileExistsError):
            j.finalize(record)
        self.assertEqual(j.evidence_path.read_bytes(), b"independent writer")
        self.assertTrue((j.directory / "evidence.pending.json").exists())
        self.assertTrue(j.failed)

    def test_owned_parent_destination_is_exclusive_and_aggregate_accounted(self):
        destination = self.root / "evidence.json"
        j = self.make(evidence_path=destination)
        record = {"calls": [{"reply": "unicode é"}], "passed": True}
        receipt = j.finalize(record)
        self.assertEqual(json.loads(destination.read_bytes()), record)
        self.assertFalse((j.directory / "evidence.json").exists())
        self.assertEqual(receipt["aggregate_bytes"], destination.stat().st_size +
                         sum(p.stat().st_size for p in j.directory.iterdir()))
        with self.assertRaises(ValueError):
            EvidenceJournal(self.root / "bad", evidence_path=self.root.parent / "evidence.json")
        with self.assertRaises(FileExistsError):
            EvidenceJournal(self.root / "another", evidence_path=destination)

    def test_exact_aggregate_limit_and_one_byte_less_are_distinct(self):
        record = {"calls": [{"reply": "small"}], "passed": False}
        probe = self.make()
        probe.finalize(record)
        # Final publication temporarily holds two names for the exact evidence
        # bytes; conservatively count both even though they are hardlinked.
        limit = probe._size() + probe.evidence_path.stat().st_size
        for cap, succeeds in ((limit, True), (limit - 1, False)):
            with self.subTest(cap=cap), tempfile.TemporaryDirectory(dir=self.root) as directory:
                j = EvidenceJournal(Path(directory) / "j", cap_bytes=cap)
                if succeeds:
                    self.assertTrue(j.finalize(record)["passed"])
                    self.assertLessEqual(j._size(), cap)
                else:
                    with self.assertRaises(JournalError):
                        j.finalize(record)
                    self.assertFalse(j.evidence_path.exists())

    def test_non_ascii_uses_original_ascii_escape_encoding_and_exact_compact_size(self):
        j = self.make()
        record = {"owners": [{"name": "poème 茶"}],
                  "calls": [{"reply": {"text": "é 🎮"}}], "passed": True}
        receipt = j.finalize(record)
        journal, evidence = j.path.read_bytes(), j.evidence_path.read_bytes()
        self.assertIn(b"\\u00e9", journal)
        self.assertIn(b"\\ud83c\\udfae", evidence)
        self.assertNotIn("é".encode("utf-8"), journal + evidence)
        self.assertEqual(json.loads(evidence), record)
        expected = (json.dumps(record, ensure_ascii=True, allow_nan=False,
                               separators=(",", ":")) + "\n").encode("utf-8")
        self.assertEqual(receipt["evidence_bytes"], len(expected))

    def test_encoder_coercions_cannot_pass_exact_decoded_equality(self):
        for row in ({"reply": (1, 2)}, {"reply": {1: "integer key"}}):
            with self.subTest(row=row), tempfile.TemporaryDirectory(dir=self.root) as directory:
                j = EvidenceJournal(Path(directory) / "j")
                with self.assertRaisesRegex(JournalError, "reconstruction differs"):
                    j.finalize({"calls": [row]})
                self.assertFalse(j.evidence_path.exists())

    def test_exact_chunk_boundary_reduces_appends_without_changing_rows(self):
        rows = [{"id": 7, "reply": "same"} for _ in range(10)]
        line_size = len(_encode({"index": 0, "row": rows[0]}) + b"\n")
        j = self.make(chunk_bytes=line_size * 3)
        with patch.object(j, "_append", wraps=j._append) as append:
            j.checkpoint({"calls": rows}, settled)
        self.assertEqual(append.call_count, 4)
        self.assertEqual([entry["row"] for entry in self.decoded(j)], rows)
        self.assertEqual(j.count, 10)
        self.assertEqual(j.journal_bytes, line_size * 10)
        j.finalize({"calls": rows})

    def test_default_one_mib_chunks_have_bounded_number_of_file_appends(self):
        j = self.make()
        rows = [{"reply": "x" * 12000} for _ in range(200)]
        sizes = []
        original = j._append
        def observed(data):
            sizes.append(len(data))
            original(data)
        with patch.object(j, "_append", observed):
            j.checkpoint({"calls": rows}, settled)
        self.assertEqual(len(sizes), 3)
        self.assertTrue(all(0 < size <= 1024 * 1024 for size in sizes))
        self.assertEqual(sum(sizes), j.journal_bytes)
        self.assertEqual(len(self.decoded(j)), 200)
        self.assertEqual(j.hasher.hexdigest(), hashlib.sha256(j.path.read_bytes()).hexdigest())

    def test_oversized_single_row_is_kept_not_split_or_rejected_by_chunk_target(self):
        j = self.make(chunk_bytes=64)
        rows = [{"reply": "before"}, {"reply": "é" * 100}, {"reply": "after"}]
        snapshots = []
        original = j._append
        def observed(data):
            snapshots.append(bytes(data))
            original(data)
        with patch.object(j, "_append", observed):
            j.checkpoint({"calls": rows}, settled)
        self.assertEqual(len(snapshots), 3)
        self.assertGreater(len(snapshots[1]), 64)
        self.assertEqual(len(snapshots[1].splitlines()), 1)
        self.assertEqual(b"".join(snapshots), j.path.read_bytes())
        self.assertEqual([entry["row"] for entry in self.decoded(j)], rows)
        j.finalize({"calls": rows})

    def test_partial_batch_append_never_advances_count_or_hash(self):
        j = self.make(chunk_bytes=1024)
        record = {"calls": [{"reply": 0}]}
        j.checkpoint(record, settled)
        count, size, digest = j.count, j.journal_bytes, j.hasher.hexdigest()
        manifest = j.metadata_path.read_bytes()
        record["calls"].extend({"reply": number} for number in range(1, 8))
        def partial(data):
            with j.path.open("ab") as stream:
                stream.write(data[:len(data) // 2])
            raise OSError("partial multirow batch")
        with patch.object(j, "_append", partial):
            with self.assertRaises(OSError):
                j.checkpoint(record, settled)
        self.assertEqual((j.count, j.journal_bytes, j.hasher.hexdigest()), (count, size, digest))
        self.assertEqual(j.metadata_path.read_bytes(), manifest)
        self.assertTrue(j.failed)
        self.assertGreater(j.path.stat().st_size, size)

    def test_fixed_golden_bytes_metadata_hashes_and_receipt(self):
        # Fixed codec/manifest oracle: duplicate IDs, repeated whole rows and
        # escaped Unicode remain present. No private helper is imported.
        j = self.make(chunk_bytes=64)
        rows = [{"id": 9, "reply": {"text": "é", "number": n}} for n in (0, 1, 0)]
        record = {"passed": False, "calls": rows, "owners": [{"clean": True}]}
        for stop in (0, 1, 3):
            j.checkpoint(dict(record, calls=rows[:stop]), settled)
        receipt = j.finalize(record)
        expected_journal = b'{"index":0,"row":{"id":9,"reply":{"text":"\\u00e9","number":0}}}\n{"index":1,"row":{"id":9,"reply":{"text":"\\u00e9","number":1}}}\n{"index":2,"row":{"id":9,"reply":{"text":"\\u00e9","number":0}}}\n'
        expected_metadata = b'{"format":"poima.evidence-journal.v1","generation":4,"fields":{"passed":false,"owners":[{"clean":true}]},"journal":{"file":"calls.jsonl","count":3,"bytes":192,"sha256":"d1e884005b866807a1ebadbc8c15273a71e3b0610a1c90011ad4df4eb27be602"}}\n'
        expected_evidence = b'{"passed":false,"owners":[{"clean":true}],"calls":[{"id":9,"reply":{"text":"\\u00e9","number":0}},{"id":9,"reply":{"text":"\\u00e9","number":1}},{"id":9,"reply":{"text":"\\u00e9","number":0}}]}\n'
        expected_receipt = {'passed': True, 'calls': 3, 'journal_bytes': 192, 'journal_sha256': 'd1e884005b866807a1ebadbc8c15273a71e3b0610a1c90011ad4df4eb27be602', 'evidence_bytes': 191, 'evidence_sha256': 'a3d686fb89c6d8af9c6eaf5b4f44c39d60cbcfd71ad9792910811f89bb53c69a', 'aggregate_bytes': 620}
        self.assertEqual(j.path.read_bytes(), expected_journal)
        self.assertEqual(j.metadata_path.read_bytes(), expected_metadata)
        self.assertEqual(j.evidence_path.read_bytes(), expected_evidence)
        self.assertEqual(receipt, expected_receipt)
        self.assertEqual(json.loads(expected_evidence), record)

    def test_chunk_target_validation_is_explicit_without_new_row_limit(self):
        for value in (True, 0, -1, 8 * 1024 * 1024 + 1, 1.0):
            with self.assertRaises(ValueError):
                EvidenceJournal(self.root / "invalid", chunk_bytes=value)
        j = self.make(chunk_bytes=8 * 1024 * 1024)
        self.assertEqual(j.chunk_bytes, 8 * 1024 * 1024)

    def test_filesystem_budget_scans_are_per_chunk_not_per_row(self):
        j = self.make()
        record = {"calls": [{"reply": "x" * 12000} for _ in range(200)]}
        with patch.object(j, "_size", wraps=j._size) as scans, \
                patch.object(j, "_append", wraps=j._append) as appends:
            j.checkpoint(record, settled)
        self.assertEqual(appends.call_count, 3)
        # Three pre-append aggregate checks, one atomic metadata budget check,
        # one final checkpoint receipt size. No per-row Path/stat/resolve scan.
        self.assertEqual(scans.call_count, 5)
        self.assertEqual(j.count, 200)
        self.assertEqual(len(self.decoded(j)), 200)

    def test_flush_budget_includes_initial_metadata_before_any_new_append(self):
        j = self.make(cap_bytes=1000)
        record = {"calls": [], "value": "x" * 400}
        j.checkpoint(record, settled)
        previous, prefix = j.metadata_path.read_bytes(), j.path.read_bytes()
        record["calls"].append({"reply": "y" * 400})
        with patch.object(j, "_append", wraps=j._append) as appends:
            with self.assertRaises(JournalError):
                j.checkpoint(record, settled)
        self.assertEqual(appends.call_count, 0)
        self.assertEqual(j.path.read_bytes(), prefix)
        self.assertEqual(j.metadata_path.read_bytes(), previous)
        self.assertEqual(j.count, 0)
        self.assertLessEqual(j._size(), 1000)

    def test_flush_budget_reads_live_retained_bytes_not_a_stale_cached_size(self):
        j = self.make(cap_bytes=1000)
        record = {"calls": [{"reply": 0}]}
        j.checkpoint(record, settled)
        previous, prefix = j.metadata_path.read_bytes(), j.path.read_bytes()
        retained = j.directory / "retained-tail.bin"
        retained.write_bytes(b"z" * (950 - j._size()))
        record["calls"].append({"reply": "y" * 100})
        with patch.object(j, "_append", wraps=j._append) as appends:
            with self.assertRaises(JournalError):
                j.checkpoint(record, settled)
        self.assertEqual(appends.call_count, 0)
        self.assertEqual(j.path.read_bytes(), prefix)
        self.assertEqual(j.metadata_path.read_bytes(), previous)
        self.assertEqual(j.count, 1)
        self.assertEqual(j._size(), 950)

    def test_negative_zero_roundtrip_and_late_sign_mutation_are_distinct(self):
        j = self.make()
        record = {"calls": [{"reply": {"value": -0.0}}]}
        j.finalize(record)
        decoded = json.loads(j.evidence_path.read_bytes())
        self.assertEqual(decoded["calls"][0]["reply"]["value"].hex(), (-0.0).hex())
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            other = EvidenceJournal(Path(directory) / "j")
            row = {"reply": {"value": -0.0}}
            current = {"calls": [row]}
            other.checkpoint(current, settled)
            row["reply"]["value"] = 0.0
            with self.assertRaisesRegex(JournalError, "late mutation"):
                other.finalize(current)
            self.assertFalse(other.evidence_path.exists())

    def test_nonfinite_or_unencodable_data_never_commits(self):
        for bad in (float("nan"), float("inf"), object()):
            with self.subTest(value=bad):
                with tempfile.TemporaryDirectory(dir=self.root) as directory:
                    j = EvidenceJournal(Path(directory) / "j")
                    with self.assertRaises((ValueError, TypeError)):
                        j.checkpoint({"calls": [{"reply": bad}]}, settled)
                    self.assertFalse(j.metadata_path.exists())
                    self.assertFalse(j.evidence_path.exists())


if __name__ == "__main__":
    unittest.main()
