# Observe and cancel agent build jobs

Use this recipe after an authorized `development.compile`, `development.publish`
or `development.export` call has returned a job ID. These methods require an
authoring host and their own permissions/toolchain configuration. A compilation
receipt identifies accepted work; it does not establish completion or load code.
See [development jobs](DEVELOPMENT_JOBS.md),
[configured publishing/export profiles](DEVELOPMENT_PROFILES.md) and
[game iteration](AGENT_ITERATION.md).

Keep the receipt, request parameters and connection to that same live host.
Job IDs and receipts are session-local. Do not reconnect to a replacement host
and assume that an equal numeric ID identifies the original work. This recipe
does not submit, retry, reconnect, load artifacts, forget jobs or shut down hosts.

## Use the Python client's bounded wait

The client provides single-job inspection and waiting for a host advertising
`development.inspect.include_output` (schema revision 79):

```python
from poima_client import JobWaitExpired

try:
    status = client.wait_for_development(accepted_job_id, timeout=30, poll_interval=0.1)
except JobWaitExpired as error:
    print(error.last_status)  # detached last observation, or None before the first read
    raise

if status["state"] == "succeeded":
    full_status = client.development_status(accepted_job_id)
    diagnostics = client.call("development.diagnostics", {
        "job_id": accepted_job_id, "limit": 16
    }, timeout=5)
```

Waiting uses compact `development.inspect` responses by default and returns
every native terminal outcome, including failure and cancellation. It does not
fetch logs at terminal automatically. `development_status` returns full output
by default; pass `include_output=False` for one compact read. Full reads omit the
optional flag, preserving older hosts' default behavior. Compact reads require
native support; a rejection propagates without a discovery call or fallback.

Both helpers check the requested safe integer ID, literal lifecycle state,
terminal flag and nullable integer exit code; success requires exit code zero.
They preserve the remaining native metadata and verified result. Malformed
replies raise `DevelopmentJobError` with `job_id` and `status`; they do not
invalidate an otherwise usable transport or establish that a job stopped.

The total wait budget covers polling sleeps and supplies the remaining time to
each RPC. `timeout` and `poll_interval` must be finite, positive and at most
86,400 seconds. A trusted terminal response remains authoritative when decoding
finishes after the local deadline. Local expiry raises `JobWaitExpired`, with a
detached last validated observation, and leaves native work untouched. RPC and
transport errors and interruptions propagate unchanged. No implicit submission,
retry, cancellation, reconnection, loading or forgetting occurs. See the
[client recovery contract](PYTHON_CLIENT.md#failure-and-deliberate-recovery).

## Catalog polling for older hosts

`development.jobs` returns retained job statuses without raw stdout/stderr.
The function below selects one accepted job and returns its native terminal
status. It validates the small lifecycle fields it uses; it is not a validator
for every build-result field. An absent or duplicate ID is an error, not proof
of completion.

```python
import time

class JobWaitExpired(TimeoutError):
    def __init__(self, job_id, last_status):
        super().__init__(f"Job {job_id} did not reach terminal state in the polling budget")
        self.job_id = job_id
        self.last_status = last_status

def wait_for_job(client, job_id, *, seconds=30.0):
    if type(job_id) is not int or not 1 <= job_id <= 9007199254740991:
        raise ValueError("job_id must be a positive safe JSON integer")
    if type(seconds) not in (int, float) or not 0 < seconds <= 86400:
        raise ValueError("seconds must be finite, positive and at most 86400")

    terminal_states = {"succeeded", "failed", "cancelled", "timed_out", "launch_failed"}
    deadline = time.monotonic() + seconds
    last_status = None
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise JobWaitExpired(job_id, last_status)
        reply = client.call("development.jobs", timeout=remaining)
        if not isinstance(reply, dict) or not isinstance(reply.get("jobs"), list):
            raise RuntimeError("Unexpected development.jobs response")
        jobs = reply["jobs"]
        if any(not isinstance(item, dict) or type(item.get("job_id")) is not int
               or not 1 <= item["job_id"] <= 9007199254740991 for item in jobs):
            raise RuntimeError("Unexpected development job identifiers")
        identifiers = [item["job_id"] for item in jobs]
        if len(identifiers) != len(set(identifiers)):
            raise RuntimeError("Development job identifiers are duplicated")
        matches = [item for item in jobs if item["job_id"] == job_id]
        if len(matches) != 1:
            raise RuntimeError("Accepted job is absent or duplicated; inspect host lifetime")
        last_status = matches[0]
        state = last_status.get("state")
        if (not isinstance(state, str) or state not in terminal_states | {"queued", "running"}
                or type(last_status.get("terminal")) is not bool
                or last_status["terminal"] != (state in terminal_states)):
            raise RuntimeError("Unexpected development job lifecycle fields")
        if state == "succeeded" and (type(last_status.get("exit_code")) is not int
                                     or last_status["exit_code"] != 0):
            raise RuntimeError("Successful job lacks a zero integer exit code")
        if last_status["terminal"]:
            return last_status
        time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
```

The budget bounds polling sleeps and supplies the remaining time to each RPC.
It is not a hard deadline on Python argument preparation or transport cleanup.
A trusted terminal reply is returned even if decoding or validation finishes
after the polling budget; this is an operational recipe, not a timing benchmark.
`RpcError`, `OutcomeUnknown`, `TransportError` and interruption propagate; the
function does not reinterpret them as a stopped job. A transport deadline can
disconnect the client even though the host's job continues. Follow the
[failure and recovery contract](PYTHON_CLIENT.md#failure-and-deliberate-recovery).

## Inspect one job without repeated logs

For a host advertising `development.inspect.include_output` (schema revision 79),
use compact single-job reads under the same finite budget and lifecycle checks:

```python
progress = client.call("development.inspect", {
    "job_id": accepted_job_id, "include_output": False
}, timeout=5)
```

This keeps the worker error and verified result as well as state, elapsed,
byte/truncation counters and compiler-use metadata. It avoids cataloging other
jobs and retrieving raw logs on each poll. At terminal, obtain the evidence needed
for the operation deliberately:

```python
full_status = client.call("development.inspect", {"job_id": accepted_job_id}, timeout=5)
diagnostics = client.call("development.diagnostics", {
    "job_id": accepted_job_id, "limit": 16
}, timeout=5)
```

Preserve the same host identity and transport-error behavior described above.
The new flag is strictly boolean and defaults to full inspection; unsupported
older hosts require their discovered operations. Raw tails remain bounded, and
`more`/`incomplete` diagnostics still limit the conclusion you can draw.

## Cancel deliberately, then observe again

Only cancel work you own or have explicit permission to stop. A local polling
timeout does not cancel native work. For an owned job, use a separate observation
budget after requesting cancellation:

```python
try:
    status = wait_for_job(client, accepted_job_id, seconds=30)
except JobWaitExpired:
    cancellation = client.call("development.cancel", {"job_id": accepted_job_id}, timeout=5)
    status = wait_for_job(client, accepted_job_id, seconds=10)
```

This example assumes cancellation is authorized. Do not catch a second
`JobWaitExpired` and report successful cleanup: retain its last status and report
that terminal completion remains unobserved. A transport failure during
`development.cancel` has an uncertain outcome; recover deliberately rather than
automatically resubmitting or disconnecting the host.

`requested: true` means cancellation was accepted, not that cleanup finished.
`requested: false` can mean completion won the race; inspect the resulting state.
Cancellation during success verification suppresses the result, but the verifier
itself is not interruptible. A native subprocess timeout does not interrupt
success verification either. Cancellation does not undo output files.

## Use the actual outcome

| Terminal state | Next action |
| --- | --- |
| `succeeded` | Inspect the output and operation-specific verification result. Compilation does not load gameplay; publishing does not prove game behavior. |
| `failed` | Read bounded structured diagnostics or raw inspection when needed. A verifier can fail after a zero child exit. |
| `cancelled` | Preserve partial output separately. Do not load it as a completed artifact. |
| `timed_out` | Record the native deadline outcome and diagnostics. It differs from the client's polling budget expiring. |
| `launch_failed` | Inspect the launch error and configured executable/environment before another authorized attempt. |

For selected compiler feedback, call `development.diagnostics` with a bounded
`limit`; `more` or `incomplete` limits what its records establish. For raw tails
and the worker error, call `development.inspect` only when needed. After recording
the result, `development.forget` can release retained-job capacity; it accepts
terminal jobs only and does not make the old receipt ID reusable.

On a host advertising `include_process`, read
`client.development_status(job_id, include_output=False, include_process=True)`
to retain raw leader-exit and cleanup facts before forgetting the job. Null
observations remain unknown; Windows codes and Job counters do not establish
an exit cause. Keep this inner runner record separate from the external
supervisor's closure evidence. See
[process observation semantics](DEVELOPMENT_JOBS.md#process-and-cleanup-observations).

Windows builds from 0.0.180 can finish with no cleanup request. Check the
retained `final_group_accounting` for the completed Job's raw counters instead
of requiring a termination entry. An absent/null observation is unknown;
`total_terminated_processes:0` alone does not prove that no termination was
requested. Keep actual cleanup requests even when the leader and artifact
verification succeeded.

The SDK's offline checks exercise lifecycle records, deadlines, detached timeout
context and unchanged error propagation. The explicit
[native fixture](../tests/python_development_jobs_native.py) builds package-free
success, syntax failure and repair through the public helpers; Windows execution
requires an external process-tree owner. It does not qualify gameplay, reload,
rendering or iteration performance.
Add `--include-process` to check both default responses and opt-in native
metadata during those same actual compiler jobs.
