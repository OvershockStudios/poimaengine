# SPDX-License-Identifier: Apache-2.0
"""Append-only evidence persistence; no evidence or timing is omitted.

One owner, one fresh directory, no concurrent writers. Rows may be mutable before
settlement; modifying an admitted row is caught by final reconstruction. Failed
writes latch failure and preserve the prior metadata commit and diagnostic tail.
"""
import hashlib
import json
import os
from pathlib import Path

MAX_BYTES = 512 * 1024 * 1024
DEFAULT_CHUNK_BYTES = 1024 * 1024
MAX_CHUNK_BYTES = 8 * 1024 * 1024


class JournalError(RuntimeError):
    pass


def _encode(value):
    return json.dumps(value, ensure_ascii=True, allow_nan=False,
                      separators=(",", ":")).encode("utf-8")


def _same(left, right):
    # JSON objects are unordered; scalar type distinctions and array order matter.
    # Compare decoded types as well, so tuples/int object keys cannot masquerade
    # as an unchanged native JSON record through encoder coercion.
    if type(left) is not type(right):
        return False
    if type(left) is dict:
        return (left.keys() == right.keys() and
                all(_same(left[key], right[key]) for key in left))
    if type(left) is list:
        return len(left) == len(right) and all(_same(a, b) for a, b in zip(left, right))
    if type(left) is float:
        return left.hex() == right.hex()  # Preserve signed zero and exact value.
    return type(left) in (str, int, bool, type(None)) and left == right


def _load(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise JournalError("Duplicate JSON object key")
            result[key] = value
        return result
    return json.loads(data, object_pairs_hook=unique,
                      parse_constant=lambda _: (_ for _ in ()).throw(
                          JournalError("Nonfinite journal data")))


class EvidenceJournal:
    def __init__(self, directory, *, cap_bytes=MAX_BYTES, evidence_path=None,
                 chunk_bytes=DEFAULT_CHUNK_BYTES):
        if type(cap_bytes) is not int or not 1 <= cap_bytes <= MAX_BYTES:
            raise ValueError("cap_bytes must be 1..512 MiB")
        if type(chunk_bytes) is not int or not 1 <= chunk_bytes <= MAX_CHUNK_BYTES:
            raise ValueError("chunk_bytes must be 1..8 MiB")
        self.chunk_bytes = chunk_bytes
        self.directory = Path(directory)
        self.evidence_path = (self.directory / "evidence.json" if evidence_path is None
                              else Path(evidence_path))
        if (self.evidence_path.name != "evidence.json" or
                self.evidence_path.parent.resolve() not in
                (self.directory.resolve(), self.directory.parent.resolve())):
            raise ValueError("Final evidence must be evidence.json in the journal or owned parent")
        if self.evidence_path.exists() or self.evidence_path.is_symlink():
            raise FileExistsError("Final evidence already exists")
        self.directory.mkdir()  # Existing paths are never overwritten/adopted.
        self.path = self.directory / "calls.jsonl"
        self.metadata_path = self.directory / "metadata.json"
        self.path.open("xb").close()
        self.cap_bytes = cap_bytes
        self.count = 0
        self.journal_bytes = 0
        self.hasher = hashlib.sha256()
        self.generation = 0
        self.failed = False
        self.finalized = False

    def _active(self):
        if self.failed or self.finalized:
            raise JournalError("Journal is failed or already finalized")

    def _record(self, record):
        if type(record) is not dict or type(record.get("calls")) is not list:
            raise JournalError("Record must be an object with a calls array")
        if len(record["calls"]) < self.count:
            raise JournalError("Committed call prefix was removed")

    def _size(self):
        total = sum(p.stat().st_size for p in self.directory.iterdir() if p.is_file())
        if self.evidence_path.parent.resolve() != self.directory.resolve() and self.evidence_path.is_file():
            total += self.evidence_path.stat().st_size
        return total

    def _budget(self, additional):
        if self._size() + additional > self.cap_bytes:
            raise JournalError("Aggregate journal/checkpoint/reconstruction cap exceeded")

    def _append(self, data):
        # A failed/short append is retained for diagnosis; it cannot be committed.
        with self.path.open("ab") as stream:
            if stream.write(data) != len(data):
                raise JournalError("Short journal append")
            stream.flush()

    def _metadata(self, metadata):
        data = _encode(metadata) + b"\n"
        self._budget(len(data))  # Old committed file and new temp coexist.
        temporary = self.directory / ("metadata.pending.%d.json" % self.generation)
        with temporary.open("xb") as stream:
            if stream.write(data) != len(data):
                raise JournalError("Short metadata write")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, self.metadata_path)

    def checkpoint(self, record, settled):
        """Append the newly settled contiguous prefix at an existing persist site.

        ``settled(row)`` is the caller's native-reply settlement predicate. False
        stops admission; later rows stay in memory even if independently settled.
        Metadata is replaced even when no new row was admitted. No rows deduplicate.
        """
        self._active()
        try:
            self._record(record)
            if not callable(settled):
                raise JournalError("Settlement predicate must be callable")
            chunk = bytearray()
            chunk_rows = 0
            next_index = self.count

            def flush():
                nonlocal chunk_rows
                if not chunk_rows:
                    return
                self._budget(len(chunk))
                self._append(chunk)
                # No partial append can advance either counter or running hash.
                self.hasher.update(chunk)
                self.journal_bytes += len(chunk)
                self.count += chunk_rows
                chunk.clear()
                chunk_rows = 0

            while next_index < len(record["calls"]):
                item = record["calls"][next_index]
                ready = settled(item)
                if type(ready) is not bool:
                    raise JournalError("Settlement predicate must return bool")
                if not ready:
                    break
                data = _encode({"index": next_index, "row": item}) + b"\n"
                if chunk and len(chunk) + len(data) > self.chunk_bytes:
                    flush()
                # Buffer admission is pure arithmetic. The actual aggregate disk
                # footprint is rechecked at every flush BEFORE its append.
                if len(chunk) + len(data) > self.cap_bytes:
                    raise JournalError("Buffered chunk exceeds aggregate cap")
                chunk.extend(data)
                chunk_rows += 1
                next_index += 1
                # An individual row bigger than the target is retained alone;
                # target size never introduces a new evidence-row rejection.
                if len(chunk) >= self.chunk_bytes:
                    flush()
            flush()
            # Sync the newly appended prefix once, before metadata can commit it.
            with self.path.open("ab") as stream:
                os.fsync(stream.fileno())
            self.generation += 1
            fields = {key: value for key, value in record.items() if key != "calls"}
            metadata = dict(format="poima.evidence-journal.v1", generation=self.generation,
                            fields=fields, journal=dict(file="calls.jsonl", count=self.count,
                            bytes=self.journal_bytes, sha256=self.hasher.hexdigest()))
            self._metadata(metadata)
            return dict(calls=self.count, journal_bytes=self.journal_bytes,
                        journal_sha256=self.hasher.hexdigest(), aggregate_bytes=self._size())
        except BaseException:
            self.failed = True
            raise

    def finalize(self, record):
        """Retain every remaining row, including unfinished/failed transport rows.

        Independently decode/hash the journal and compare the reconstructed full
        native record before exclusive publication. This is the only full rewrite.
        A successful result describes persistence, not whether the experiment passed.
        """
        self._active()
        try:
            self.checkpoint(record, lambda _: True)
            metadata = _load(self.metadata_path.read_bytes())
            manifest = metadata["journal"]
            actual_hash = hashlib.sha256()
            actual_bytes = 0
            calls = []
            with self.path.open("rb") as stream:
                for line in stream:
                    actual_hash.update(line)
                    actual_bytes += len(line)
                    if not line.endswith(b"\n"):
                        raise JournalError("Incomplete final journal row")
                    entry = _load(line)
                    if (type(entry) is not dict or set(entry) != {"index", "row"}
                            or type(entry["index"]) is not int
                            or entry["index"] != len(calls)):
                        raise JournalError("Journal order/index differs")
                    calls.append(entry["row"])
            if manifest != dict(file="calls.jsonl", count=len(calls), bytes=actual_bytes,
                                sha256=actual_hash.hexdigest()):
                raise JournalError("Retained journal differs from committed manifest")
            reconstructed = dict(metadata["fields"], calls=calls)
            if not _same(reconstructed, record):
                raise JournalError("Full reconstruction differs: late mutation or missing evidence")
            data = _encode(reconstructed) + b"\n"
            # Temp plus exclusively linked final name coexist briefly; both are
            # accounted even on filesystems where the two names share data blocks.
            self._budget(2 * len(data))
            temporary = self.directory / "evidence.pending.json"
            with temporary.open("xb") as stream:
                if stream.write(data) != len(data):
                    raise JournalError("Short final evidence write")
                stream.flush()
                os.fsync(stream.fileno())
            os.link(temporary, self.evidence_path)  # Atomic no-overwrite publication.
            temporary.unlink()
            self.finalized = True
            return dict(passed=True, calls=len(calls), journal_bytes=actual_bytes,
                        journal_sha256=actual_hash.hexdigest(), evidence_bytes=len(data),
                        evidence_sha256=hashlib.sha256(data).hexdigest(),
                        aggregate_bytes=self._size())
        except BaseException:
            self.failed = True
            raise
