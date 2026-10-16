# World storage diagnostics

Windows staging and snapshot-publication failures include internal operation
and stage details on the native service's stderr. JSON-RPC keeps the existing
response:

```json
{"code":-32000,"message":"World storage failure; inspect stderr."}
```

Capture stderr separately from protocol stdout. For example:

```text
poima world scene.json > responses.ndjson 2> storage.log
```

A recorded controlled failure can look like this:

```text
World operation failed: Cannot publish world snapshot. [storage operation=MoveFileExW stage=primary_publish win32_code=5 retry_attempts=5 first_win32_code=5 last_win32_code=5 scheduled_wait_ms=30 retry_elapsed_us=80251]
```

The number is the actual thread error captured immediately after the failed
Windows call. It is not a diagnosis of which process or filesystem condition
caused the failure. The example does not identify the cause of historical
intermittent failures. The fields contain no paths or authored content.

| Field | Meaning |
| --- | --- |
| `operation` | Failed `CreateFileW`, `WriteFile`, `FlushFileBuffers`, `CloseHandle` or `MoveFileExW`, or a successful `WriteFile` with a short result (`short_write`). |
| `stage` | Known world-commit boundary: `primary_staging`, `previous_staging`, `previous_publish` or `primary_publish`. Omitted when the shared helper's caller supplies no stage. |
| `win32_code` | Captured failed-call error, including a literal zero if that was observed. Absent for a successful short write, which has no failed-call error. |
| `requested`, `written` | Requested byte count and the actual `WriteFile` output count. These distinguish incomplete output from the requested payload; they do not establish durability. |
| `close_win32_code` | A separate failed close following a primary write/short-write/flush failure. It does not overwrite the first failure. |

For authored-world publication retries, `retry_attempts` counts actual calls,
including a successful final call. `first_win32_code` and `last_win32_code`
retain actual failed calls; success does not replace the last error with zero.
`scheduled_wait_ms` sums requested waits and `retry_elapsed_us` measures elapsed
time from the first captured failure.

Numbers use decimal formatting. The original exception messages remain the
same for other helper users such as asset and preference storage. Writer-lock
startup failures and unrelated I/O exceptions do not gain these fields. POSIX
syscall behavior and diagnostics remain unchanged. These internal fields are
not a new JSON-RPC response schema or a frozen application ABI.

## Bounded Windows publication retries

Starting with 0.0.173, only authored-world `previous_publish` and
`primary_publish` opt into retries. First-attempt success performs no retry
guard reads, waits or diagnostic output. Shared asset/settings/editor callers
retain one-attempt replacement; POSIX rename behavior is unchanged.

After an actual Windows error 5, 32 or 33, the same rename may be attempted again.
There are at most six calls including the first, with requested waits of 2, 4,
8, 16 and 32 ms, totaling at most 62 ms. A cooperative 100 ms eligibility window
starts after the first failed call and includes observation, waits and guard
work. It does not interrupt synchronous filesystem calls or guarantee a 100 ms
request duration.

Before each additional call, the engine verifies primary existence, its complete
precommit bytes and the prepared source bytes. Reads stream through a 64 KiB
buffer under the 16 MiB document limit. Changed bytes or inability to prove these
conditions return the existing `-32009` conflict. The edit is not rebased and
permissions are not changed. These path checks do not eliminate races with an
uncooperative writer between checking and renaming.

An actual first eligible failure emits `World publication retry pending`.
A successful retried rename emits `World publication recovered`; exhaustion
retains `World operation failed` with the original public storage error.
Pending does not guarantee another attempt or a committed edit. Recovery of the
backup rename alone does not prove the transaction committed: primary
publication may still fail. Use the JSON-RPC reply and complete state/receipt
observations to reconcile the transaction.

Retry pending/recovery logging contains stream exceptions. These synchronous diagnostics still require
clients to drain stderr as well as stdout; they do not provide a nonblocking
logging or I/O deadline guarantee.

## Publication and recovery

Commit prepares and flushes the candidate `.pending`, prepares the previous
primary in `.previous.pending`, publishes `.previous`, then replaces the
primary. The session publishes its new in-memory world and history only after
primary replacement. Retries preserve this ordering and repeat only the failed rename. The
transaction and its history/receipt updates are not reapplied.

A failed primary replacement can leave the new candidate in `.pending` and
an updated `.previous` containing the unchanged primary. A failed backup
replacement can leave `.previous.pending` as well. These preparation files do
not prove that a transaction committed. Startup ignores them when opening the
valid primary.

For an acknowledged storage error, retain stderr and the complete primary and
sidecars before changing anything. Inspect the current revision and state.
Once the blocking condition is resolved and the revision is still appropriate,
the same request ID and identical parameters can be resubmitted. A completed
request's receipt prevents applying it twice. If the response was lost, inspect the world and reconcile the original request
before submitting further edits. The controlled checks below do not qualify
automatic retries after a transport timeout.

## Verification boundary

`world_storage_native` exercises the formatter and ordinary POSIX writes on
Linux. On Windows it additionally produces real staging-creation and blocked
primary/backup replacement failures using its own files and handles. Constructed
formatter cases do not qualify actual short writes, flush failures or close
failures. The standalone native fixture retains its output and prints its
location; `--output FRESH_DIRECTORY` chooses an explicit location.

The retained 0.0.172 `tests/world_storage_protocol.py` fixture requires an
externally owned Windows Job and its original diagnostic format.
It blocks replacement at both world-commit publication stages, compares complete
primary/native/history/receipt state and the expected sidecar preparations,
then releases the handle and checks identical-request recovery, receipt replay
and cold reopen. Its optional baseline compares the original public error
bytes. A separate current-only write cohort records unexpected failures without
retrying to obtain a pass.

The 0.0.173 `tests/world_storage_retry_protocol.py` fixture exercises six cases
through twelve native owners: persistent and released blockers at each
publication stage, plus changed primary and prepared bytes. Its 116 raw
request/reply pairs and 58 complete four-file forecasts pass, including actual
same-request recovery, warm/cold receipt replay and conflicts preserving external
bytes.
The fixture directs raw stderr into an owned file to bind observed failures to
the same in-flight call; its observation I/O is not a performance comparison.

A separate fixed cohort passes twenty fresh runs of the unchanged original
130-commit method. Its post-EOF decoded stderr records 24 recovered renames:
fifteen primary and nine backup replacements. Each observed failed-call code is
5. All planned runs are retained; none is replaced by a retry run. The broader
Windows CPU campaign passes all twenty selected stages, and all 23 selected
Linux headless CTests pass. See the
[current evidence](evidence/m2-world-storage-retries.json).

The retained 0.0.172 Windows campaign passes eighteen of nineteen stages; the original
receipt-eviction test still fails. A separate minimally instrumented run of its
unchanged 130-commit workload captures `MoveFileExW`, `previous_publish` and
Windows error 5. That identifies the failed operation, not the process or
filesystem condition responsible. Its original test remains failed and the
native owner closes with code zero.

The new protocol fixture separately passes 243 request/reply pairs across four
native owners, controlled failures at both publication stages, recovery and
cold reopening, followed by 131 additional current-build commits. Those extra
commits use a different observation pattern from the original stress test.
They do not qualify a fix for its failure. See the
[checkpoint evidence](evidence/m2-world-storage-diagnostics.json).

These checks do not establish power-loss directory durability or interruption
at every filesystem instruction. Windows error references and immediate error
capture are documented by Microsoft for
[MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
and [GetLastError](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-getlasterror).
