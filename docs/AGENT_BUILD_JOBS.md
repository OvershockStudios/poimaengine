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

## Poll under a finite budget

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

The helper's offline checks exercise literal lifecycle records, timeout/error
propagation and owned-cancellation sequencing. They do not add native process-tree,
toolchain, provider or performance qualification beyond the linked development
job evidence.
