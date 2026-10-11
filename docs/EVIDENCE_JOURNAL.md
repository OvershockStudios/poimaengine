# Evidence journals

`tools/evidence_journal.py` stores a Python evidence record without rewriting its
entire `calls` array at every checkpoint. It appends newly settled rows to indexed
JSONL, replaces a small metadata checkpoint, then reconstructs the original
`evidence.json` once. It uses only the Python standard library. It does not submit
engine requests, run processes, change a verification oracle or manage shutdown.

Use one writer and a fresh journal directory. Keep the complete record in memory,
including duplicate request IDs, raw and interpreted records, errors and pending
requests. Choose a settlement predicate for your record format: a row is settled
only after its reply and all of its recorded context are final.

```python
import sys
from pathlib import Path

# In a checkout script, replace this with your actual checkout root.
checkout = Path("/path/to/poima")
sys.path.insert(0, str(checkout / "tools"))
from evidence_journal import EvidenceJournal

# This recipe applies to raw rows with request/response and interpreted rows
# with method/reply. Adapt the predicate to the format you actually record.
def settled(row):
    if "request" in row:
        return "response" in row
    if "method" in row:
        return "reply" in row
    return False

output = Path("evidence-run")
output.mkdir()  # Choose a fresh owned output; never adopt an earlier run.
record = {"passed": False, "calls": [], "owners": []}
journal = EvidenceJournal(output / "journal",
                          evidence_path=output / "evidence.json")

raw = {"request": {"id": 1, "method": "world.inspect", "params": {}}}
record["calls"].append(raw)
journal.checkpoint(record, settled)  # The request is still pending.

# Your transport supplies the actual reply; no synthetic reply is needed.
# raw["response"] = actual_reply
# journal.checkpoint(record, settled)

# Finish requests, owner cleanup, phase metadata and error reporting first.
# Set record["passed"] from the experiment's own verification, not this helper.
receipt = journal.finalize(record)
```

Checkpoint admission stops at the first pending row. Later settled rows wait in
memory behind it. Once admitted, a row must not change; metadata outside `calls`
may continue to change until finalization. Finalization retains every remaining
row, including incomplete transport requests and rows recording failed operations. It independently
decodes the journal, verifies its hash and indices, and compares the entire
reconstructed record with the current in-memory record. Array order, scalar types
and the sign of floating-point zero must match. Integer object keys and tuples
cannot silently pass JSON's coercions. Nonfinite values cannot be encoded.

The receipt's `passed` means that persistence and exact reconstruction succeeded.
It does not mean the recorded experiment passed. A record with `passed: false`
remains false in the published evidence. Call `finalize` exactly once, after all
final metadata is stable; it exclusively creates `evidence.json` and refuses an
existing final file. Its destination is confined to the journal directory or its
immediate owned parent. The helper does not redact evidence; choose publication
content in a separate reviewed step without removing the retained original.

The default aggregate limit is 512 MiB. It counts journal files, committed
metadata, temporary metadata, the reconstructed evidence temporary file and its
final name. Both final names are conservatively counted during their brief
coexistence. A smaller positive integer `cap_bytes` is supported; the hard maximum
is 512 MiB. `chunk_bytes` defaults to 1 MiB and accepts positive integers up to
8 MiB. A row larger than that chunk target is kept alone, subject to the same
aggregate cap. The cap bounds these stored files; it is not a bound on the
caller's in-memory record or on final reconstruction memory.

Each checkpoint syncs its appended prefix before atomically replacing metadata
containing all non-call fields and the admitted count, byte length and SHA-256.
Append, encoding, capacity, replacement, publication or comparison failures raise
and latch failure. Preserve the exception and the files in a separate owner
report. Do not retry the failed writer or claim success: earlier metadata and an
uncommitted tail may remain for inspection. There is no recovery or adoption API,
concurrent-writer protocol or guarantee of filesystem crash recovery.

Run the pure regression suite from a checkout:

```sh
python3 tests/python_evidence_journal.py
```

These tests exercise record preservation, pending replies, fixed byte and hash
oracles, capacity boundaries and injected failures. They do not qualify engine
behavior or establish a performance improvement for your workload.

All 25 tests pass on Linux and Windows for 0.0.109.
[Qualification and measured-helper lineage](evidence/m2-evidence-journal.json)
record the public helper's equivalence to the separately measured implementation.
