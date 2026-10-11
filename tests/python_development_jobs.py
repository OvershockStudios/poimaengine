# SPDX-License-Identifier: Apache-2.0
"""Pure job inspection/wait contracts; no engine, compiler or child processes.

Run: python3 tests/python_development_jobs.py [--installed]
Scripted replies and an explicit fake clock qualify the Python helper only.
"""
import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

if "--installed" in sys.argv:
    sys.argv.remove("--installed")
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "python"))

from poima_client import (DevelopmentJobError, JobWaitExpired, OutcomeUnknown,
                          RpcError, TransportError, WorldClient)

JOB = 17
STATES = ("queued", "running", "succeeded", "failed", "cancelled", "timed_out", "launch_failed")
TERMINAL = STATES[2:]


def status(state="running", **updates):
    value = dict(job_id=JOB, state=state, terminal=state in TERMINAL,
                 exit_code=0 if state == "succeeded" else None,
                 untouched={"result": [1, None], "compiler_server_used": None})
    value.update(updates)
    return value


class Clock:
    def __init__(self):
        self.now = 100.0
        self.sleeps = []

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        if seconds <= 0:
            raise AssertionError("Nonpositive wait")
        self.sleeps.append(seconds)
        self.now += seconds


class ScriptedTransport:
    def __init__(self, replies, clock=None):
        self.replies = list(replies)
        self.clock = clock
        self.calls = []
        self.closed = False

    def request(self, method, params=None, timeout=30.0):
        self.calls.append((method, copy.deepcopy(params), timeout))
        if method != "development.inspect":
            raise AssertionError("Unexpected hidden request: " + method)
        if not self.replies:
            raise AssertionError("Unexpected retry/poll")
        reply = self.replies.pop(0)
        if isinstance(reply, tuple):
            reply, elapsed = reply
            self.clock.now += elapsed
        if isinstance(reply, BaseException):
            raise reply
        return reply

    def close(self):
        self.closed = True


class DevelopmentContracts(unittest.TestCase):
    def client(self, *replies, clock=None):
        transport = ScriptedTransport(replies, clock)
        return WorldClient(transport), transport

    def test_direct_default_explicit_full_and_compact_preserve_native_object(self):
        for options, params in (({}, {"job_id": JOB}),
                                ({"include_output": True}, {"job_id": JOB}),
                                ({"include_output": False}, {"job_id": JOB, "include_output": False})):
            with self.subTest(options=options):
                reply = status("succeeded", future_field={"opaque": [1, "a"]})
                client, transport = self.client(reply)
                self.assertIs(client.development_status(JOB, timeout=2.5, **options), reply)
                self.assertEqual(transport.calls, [("development.inspect", params, 2.5)])
                self.assertFalse(transport.closed)

    def test_wait_defaults_to_compact_but_full_is_explicit_and_unchanged(self):
        for options, params in (({}, {"job_id": JOB, "include_output": False}),
                                ({"include_output": True}, {"job_id": JOB}),
                                ({"include_output": False}, {"job_id": JOB, "include_output": False})):
            with self.subTest(options=options):
                clock = Clock()
                reply = status("succeeded", stdout="raw when present", unknown_identity="opaque")
                client, transport = self.client(reply, clock=clock)
                with patch("poima_client.development.time", clock):
                    self.assertIs(client.wait_for_development(JOB, timeout=7, **options), reply)
                self.assertEqual(transport.calls, [("development.inspect", params, 7)])
                self.assertEqual(clock.sleeps, [])

    def test_all_seven_states_are_valid_and_terminal_failures_are_returned(self):
        for state in STATES:
            with self.subTest(state=state):
                reply = status(state, exit_code=0 if state in ("succeeded", "failed") else None)
                client, transport = self.client(reply)
                self.assertIs(client.development_status(JOB), reply)
                self.assertEqual(len(transport.calls), 1)
        for state in TERMINAL:
            with self.subTest(terminal=state):
                clock = Clock()
                reply = status(state, exit_code=0 if state in ("succeeded", "failed", "cancelled") else -3)
                client, transport = self.client(status("queued"), status("running"), reply, clock=clock)
                with patch("poima_client.development.time", clock):
                    self.assertIs(client.wait_for_development(JOB, timeout=1, poll_interval=.1), reply)
                self.assertEqual(len(transport.calls), 3)
                self.assertEqual(clock.sleeps, [.1, .1])

    def test_safe_id_extremes_and_numeric_time_bounds_are_accepted(self):
        for job in (1, 9007199254740991):
            reply = status("succeeded", job_id=job)
            client, transport = self.client(reply)
            self.assertIs(client.development_status(job, timeout=86400), reply)
            self.assertEqual(transport.calls[0][1], {"job_id": job})
        clock = Clock()
        reply = status("succeeded")
        client, transport = self.client(reply, clock=clock)
        with patch("poima_client.development.time", clock):
            self.assertIs(client.wait_for_development(JOB, timeout=86400, poll_interval=86400), reply)
        self.assertEqual(transport.calls[0][2], 86400)

    def test_invalid_arguments_reject_before_transport(self):
        class IntAlias(int):
            pass
        invalid_ids = (None, True, False, 0, -1, 9007199254740992, 17.0, "17", [], {}, IntAlias(17))
        invalid_times = (None, True, False, 0, -1, 86400.01, float("nan"), float("inf"), -float("inf"), "1", [], {})
        for method in ("development_status", "wait_for_development"):
            for job in invalid_ids:
                with self.subTest(method=method, job=job):
                    client, transport = self.client()
                    with self.assertRaises(ValueError):
                        getattr(client, method)(job)
                    self.assertEqual(transport.calls, [])
            for value in invalid_times:
                with self.subTest(method=method, timeout=value):
                    client, transport = self.client()
                    with self.assertRaises(ValueError):
                        getattr(client, method)(JOB, timeout=value)
                    self.assertEqual(transport.calls, [])
            for value in (None, 0, 1, "false", [], {}):
                with self.subTest(method=method, include_output=value):
                    client, transport = self.client()
                    with self.assertRaises(ValueError):
                        getattr(client, method)(JOB, include_output=value)
                    self.assertEqual(transport.calls, [])
        for value in invalid_times:
            with self.subTest(poll_interval=value):
                client, transport = self.client()
                with self.assertRaises(ValueError):
                    client.wait_for_development(JOB, poll_interval=value)
                self.assertEqual(transport.calls, [])

    def test_consumed_response_fields_are_strict_without_rewriting_unknown_fields(self):
        malformed = [None, [], "status", 1]
        for key in ("job_id", "state", "terminal", "exit_code"):
            reply = status()
            reply.pop(key)
            malformed.append(reply)
        for key, values in (("job_id", (True, 17.0, "17", 18, None)),
                            ("state", (None, 1, "complete", "RUNNING", [], {})),
                            ("terminal", (0, 1, None, "false", True)),
                            ("exit_code", (True, False, 0.0, "0", [], {}))):
            malformed.extend(status(**{key: value}) for value in values)
        malformed.extend(status(state, terminal=False) for state in TERMINAL)
        malformed.extend(status("succeeded", exit_code=value) for value in (None, -1, 1))
        for reply in malformed:
            for method in ("development_status", "wait_for_development"):
                with self.subTest(method=method, reply=reply):
                    client, transport = self.client(reply)
                    with self.assertRaises(DevelopmentJobError) as caught:
                        getattr(client, method)(JOB)
                    self.assertEqual(caught.exception.job_id, JOB)
                    self.assertIs(caught.exception.status, reply)
                    self.assertEqual(len(transport.calls), 1)
                    self.assertFalse(transport.closed)
        # Only consumed fields are constrained; arbitrary native metadata is retained.
        reply = status("failed", exit_code=0, future={"nested": [True, 1]}, error="verification failed")
        client, _ = self.client(reply)
        self.assertIs(client.wait_for_development(JOB), reply)

    def test_poll_budget_shrinks_including_rpc_elapsed_and_sleep(self):
        clock = Clock()
        final = status("succeeded")
        client, transport = self.client((status("queued"), .25), (status(), .5), final, clock=clock)
        with patch("poima_client.development.time", clock):
            self.assertIs(client.wait_for_development(JOB, timeout=3, poll_interval=.5), final)
        self.assertEqual(clock.sleeps, [.5, .5])
        for call, expected in zip(transport.calls, (3, 2.25, 1.25)):
            self.assertAlmostEqual(call[2], expected)
        self.assertTrue(all(call[1] == {"job_id": JOB, "include_output": False} for call in transport.calls))

    def test_expiry_after_partial_sleep_retains_last_validated_native_status(self):
        clock = Clock()
        first, last = status("queued"), status("running", untouched={"tick": 5})
        client, transport = self.client((first, .25), (last, .25), clock=clock)
        with patch("poima_client.development.time", clock):
            with self.assertRaises(JobWaitExpired) as caught:
                client.wait_for_development(JOB, timeout=1, poll_interval=.4)
        self.assertEqual(caught.exception.job_id, JOB)
        self.assertEqual(caught.exception.last_status, last)
        self.assertIsNot(caught.exception.last_status, last)
        last["untouched"]["tick"] = 99
        self.assertEqual(caught.exception.last_status["untouched"], {"tick": 5})
        self.assertEqual(len(transport.calls), 2)
        self.assertAlmostEqual(transport.calls[0][2], 1)
        self.assertAlmostEqual(transport.calls[1][2], .35)
        self.assertAlmostEqual(clock.sleeps[0], .4)
        self.assertAlmostEqual(clock.sleeps[1], .1)
        self.assertFalse(transport.closed)

    def test_nonterminal_reply_after_deadline_is_not_repolled(self):
        clock = Clock()
        last = status("running")
        client, transport = self.client((last, 2), clock=clock)
        with patch("poima_client.development.time", clock):
            with self.assertRaises(JobWaitExpired) as caught:
                client.wait_for_development(JOB, timeout=1)
        self.assertEqual(caught.exception.last_status, last)
        self.assertIsNot(caught.exception.last_status, last)
        last["untouched"]["result"].append("later mutation")
        self.assertEqual(caught.exception.last_status["untouched"]["result"], [1, None])
        self.assertEqual(len(transport.calls), 1)
        self.assertEqual(clock.sleeps, [])

    def test_late_trusted_terminal_reply_wins_over_local_wait_deadline(self):
        for state in TERMINAL:
            with self.subTest(state=state):
                clock = Clock()
                reply = status(state)
                client, transport = self.client((reply, 5), clock=clock)
                with patch("poima_client.development.time", clock):
                    self.assertIs(client.wait_for_development(JOB, timeout=.5), reply)
                self.assertEqual(len(transport.calls), 1)
                self.assertEqual(clock.sleeps, [])

    def test_late_malformed_reply_is_not_treated_as_wait_expiry(self):
        clock = Clock()
        reply = status("succeeded", terminal=1)
        client, transport = self.client((reply, 5), clock=clock)
        with patch("poima_client.development.time", clock):
            with self.assertRaises(DevelopmentJobError) as caught:
                client.wait_for_development(JOB, timeout=.5)
        self.assertIs(caught.exception.status, reply)
        self.assertEqual(len(transport.calls), 1)

    def test_rpc_transport_and_unknown_outcome_errors_propagate_without_repair(self):
        for error in (RpcError(-32004, "unknown job", {"detail": 7}),
                      TransportError("closed"), OutcomeUnknown("lost reply")):
            for method in ("development_status", "wait_for_development"):
                with self.subTest(method=method, error=type(error).__name__):
                    clock = Clock()
                    client, transport = self.client(status("running"), error, clock=clock) if method == "wait_for_development" else self.client(error)
                    with patch("poima_client.development.time", clock):
                        with self.assertRaises(type(error)) as caught:
                            getattr(client, method)(JOB)
                    self.assertIs(caught.exception, error)
                    self.assertEqual(len(transport.calls), 2 if method == "wait_for_development" else 1)
                    self.assertFalse(transport.closed)


if __name__ == "__main__":
    unittest.main()
