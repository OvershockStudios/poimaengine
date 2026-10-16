# SPDX-License-Identifier: Apache-2.0
"""Bounded observation of accepted native development jobs; never submits work."""
import copy
import math
import time

_TERMINAL = frozenset(("succeeded", "failed", "cancelled", "timed_out", "launch_failed"))
_STATES = _TERMINAL | {"queued", "running"}


class DevelopmentJobError(RuntimeError):
    """A reply cannot establish the requested job's lifecycle."""

    def __init__(self, job_id, status):
        super().__init__("Unexpected development job lifecycle or identity")
        self.job_id = job_id
        self.status = status


class JobWaitExpired(TimeoutError):
    """Local observation expired; the native job has not been cancelled."""

    def __init__(self, job_id, last_status):
        super().__init__("Job {} did not reach terminal state in the polling budget".format(job_id))
        self.job_id = job_id
        self.last_status = last_status


def _arguments(job_id, include_output, timeout, include_process):
    if type(job_id) is not int or not 1 <= job_id <= 9007199254740991:
        raise ValueError("job_id must be a positive safe JSON integer")
    if type(include_output) is not bool:
        raise ValueError("include_output must be a boolean")
    if type(include_process) is not bool:
        raise ValueError("include_process must be a boolean")
    _seconds(timeout, "timeout")


def _seconds(value, name):
    if (type(value) not in (int, float) or not 0 < value <= 86400
            or not math.isfinite(value)):
        raise ValueError(name + " must be finite, positive and at most 86400 seconds")


def _status(client, job_id, include_output, timeout, include_process):
    # Omit the optional flag for full inspection, preserving older hosts' default.
    # Compact inspection requires a host advertising include_output; a native
    # rejection propagates without a hidden discovery call or full-log fallback.
    params = {"job_id": job_id}
    if not include_output:
        params["include_output"] = False
    if include_process:
        params["include_process"] = True
    result = client.call("development.inspect", params, timeout=timeout)
    valid = isinstance(result, dict) and type(result.get("job_id")) is int
    if valid:
        state = result.get("state")
        exit_code = result.get("exit_code")
        valid = (result["job_id"] == job_id and isinstance(state, str)
                 and state in _STATES and type(result.get("terminal")) is bool
                 and result["terminal"] == (state in _TERMINAL)
                 and "exit_code" in result
                 and (exit_code is None or type(exit_code) is int)
                 and (state != "succeeded" or type(exit_code) is int and exit_code == 0))
    if not valid:
        raise DevelopmentJobError(job_id, result)
    return result


def inspect_job(client, job_id, *, include_output=True, timeout=30.0, include_process=False):
    _arguments(job_id, include_output, timeout, include_process)
    return _status(client, job_id, include_output, timeout, include_process)


def wait_for_job(client, job_id, *, timeout=30.0, poll_interval=0.1, include_output=False,
                 include_process=False):
    _arguments(job_id, include_output, timeout, include_process)
    _seconds(poll_interval, "poll_interval")
    deadline = time.monotonic() + timeout
    last_status = None
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise JobWaitExpired(job_id, last_status)
        result = _status(client, job_id, include_output, remaining, include_process)
        # A valid terminal reply is authoritative even if decoding/validation
        # finished after the local deadline. Failure is also a terminal outcome.
        if result["terminal"]:
            return result
        last_status = copy.deepcopy(result)
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(poll_interval, remaining))
