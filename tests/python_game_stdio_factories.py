#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Factory validation and existing client cleanup, with no native subprocesses.

These mocks qualify SDK argument/lifecycle behavior, not a native game, bundle
reader, EOF implementation or operating-system process ownership.
"""
import os
import copy
from pathlib import Path, PureWindowsPath
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/python"))
from poima_client import WorldClient, TransportError, RpcError, ResponseContractError
import poima_client.client as client_module
import poima_client.transport as transport_module


class MemoryTransport:
    def __init__(self, *, close_error=None, request_error=None):
        self.closed = False
        self.close_calls = 0
        self.close_error = close_error
        self.request_error = request_error
        self.requests = []

    def request(self, method, params, *, timeout):
        self.requests.append((method, params, timeout))
        if self.request_error is not None:
            raise self.request_error
        return {"echo": params}

    def close(self):
        self.close_calls += 1
        self.closed = True
        if self.close_error is not None:
            raise self.close_error


class GameFactories(unittest.TestCase):
    def capture(self, **options):
        return patch.object(client_module, "JsonRpcProcess", **options)

    def test_binary_factory_delegates_and_preserves_options(self):
        process = MemoryTransport()
        binary = PureWindowsPath("D:/Game Runtime/poima.exe")
        game = PureWindowsPath("D:/Game Content/game.json")
        with self.capture(return_value=process) as launch:
            with WorldClient.open_game(binary, game, cwd="C:/unrelated",
                                       close_timeout=0.25, max_pending=2,
                                       trace_directory="new traces", trace_limit_bytes=1024,
                                       validate_responses=False) as engine:
                self.assertIs(engine.transport, process)
                self.assertFalse(engine.validate_responses)
                self.assertEqual(engine.call("custom.read", {"all": [1, True]}, timeout=0.4),
                                 {"echo": {"all": [1, True]}})
        launch.assert_called_once_with([os.fspath(binary), "game", "stdio", os.fspath(game)],
                                       cwd="C:/unrelated", close_timeout=0.25, max_pending=2,
                                       trace_directory="new traces", trace_limit_bytes=1024)
        self.assertEqual(process.requests, [("custom.read", {"all": [1, True]}, 0.4)])
        self.assertEqual(process.close_calls, 1)

    def test_binary_factory_delegates_to_command_factory(self):
        sentinel = object()
        with patch.object(WorldClient, "open_game_command", return_value=sentinel) as command:
            self.assertIs(WorldClient.open_game(Path("poima"), Path("game.json"),
                                               save_root="saves", save_write_mode="background"), sentinel)
        command.assert_called_once_with(["poima"], Path("game.json"), save_root="saves",
                                        save_write_mode="background", cwd=None, validate_responses=True)

    def test_wrapper_prefix_is_literal_copied_and_uses_no_shell(self):
        prefix = ["owned wrapper", "", "--seconds", "20", "--", "native engine"]
        original = list(prefix)
        with self.capture(return_value=MemoryTransport()) as launch:
            engine = WorldClient.open_game_command(prefix, "unchanged/../game.json",
                                                   save_root="./--saves", save_write_mode="synchronous")
            prefix[:] = ["changed after construction"]
            engine.close()
        launch.assert_called_once_with(original + ["game", "stdio", "unchanged/../game.json",
                                                    "--save-root", "./--saves",
                                                    "--save-write-mode", "synchronous"], cwd=None)
        self.assertNotIn("shell", launch.call_args.kwargs)

    def test_optional_storage_argv_and_tuple_prefix(self):
        for mode in (None, "synchronous", "background"):
            with self.subTest(mode=mode), self.capture(return_value=MemoryTransport()) as launch:
                WorldClient.open_game_command(("native",), "./--game.json",
                                              save_root=Path("../external saves"), save_write_mode=mode).close()
                expected = ["native", "game", "stdio", "./--game.json", "--save-root", "../external saves"]
                if mode is not None:
                    expected += ["--save-write-mode", mode]
                launch.assert_called_once_with(expected, cwd=None)

    def rejected_before_launch(self, method, first, game, **options):
        with tempfile.TemporaryDirectory() as temp:
            trace = Path(temp) / "must-not-exist"
            with self.capture() as launch:
                with self.assertRaises((ValueError, TypeError)):
                    method(first, game, trace_directory=trace, **options)
            launch.assert_not_called()
            self.assertFalse(trace.exists())

    def test_invalid_game_and_storage_paths_never_create_transport_or_trace(self):
        invalid = (None, "", b"game.json", 1, True, [], "--game.json", "bad\0path")
        for method, first in ((WorldClient.open_game, "native"),
                              (WorldClient.open_game_command, ["owned", "--", "native"])):
            for value in invalid:
                with self.subTest(factory=method.__name__, game=repr(value)):
                    self.rejected_before_launch(method, first, value)
            for value in invalid[1:]:
                with self.subTest(factory=method.__name__, root=repr(value)):
                    self.rejected_before_launch(method, first, "game.json", save_root=value)

    def test_invalid_storage_modes_and_missing_root_reject_before_launch(self):
        for mode in ("sync", "Background", "", "synchronous\0", b"background", True, 1, []):
            with self.subTest(mode=repr(mode)):
                self.rejected_before_launch(WorldClient.open_game, "native", "game.json",
                                            save_root="saves", save_write_mode=mode)
        for mode in ("synchronous", "background"):
            with self.subTest(no_root=mode):
                self.rejected_before_launch(WorldClient.open_game_command, ["owned", "--"],
                                            "game.json", save_write_mode=mode)

    def test_invalid_prefix_and_response_validation_before_launch(self):
        for prefix in ("native --", [], (), [""], ["native", b"--"], ["native", None], ["native\0"]):
            with self.subTest(prefix=repr(prefix)):
                self.rejected_before_launch(WorldClient.open_game_command, prefix, "game.json")
        for method, first in ((WorldClient.open_game, "native"), (WorldClient.open_game_command, ["native"])):
            for value in (None, 0, 1, "true"):
                with self.subTest(factory=method.__name__, validation=repr(value)):
                    self.rejected_before_launch(method, first, "game.json", validate_responses=value)

    def test_unknown_transport_keyword_binds_before_any_process_or_trace(self):
        # Reproduce the original failure's keyword with the REAL constructor.
        # No independently launched host exists for this one-process factory.
        for method, first in ((WorldClient.open_game, "native"),
                              (WorldClient.open_game_command, ["owned", "--", "native"])):
            with self.subTest(factory=method.__name__), tempfile.TemporaryDirectory() as temp:
                trace = Path(temp) / "not-created"
                with patch.object(transport_module.subprocess, "Popen") as spawn:
                    with self.assertRaisesRegex(TypeError, "cleanup_timeout"):
                        method(first, "game.json", trace_directory=trace, cleanup_timeout=0.1)
                spawn.assert_not_called()
                self.assertFalse(trace.exists())

    def test_invalid_existing_transport_bounds_reject_before_process_or_trace(self):
        for options in ({"close_timeout": 0}, {"close_timeout": float("inf")}, {"max_pending": 0}):
            with self.subTest(options=options), tempfile.TemporaryDirectory() as temp:
                trace = Path(temp) / "not-created"
                with patch.object(transport_module.subprocess, "Popen") as spawn:
                    with self.assertRaises(ValueError):
                        WorldClient.open_game("native", "game.json", trace_directory=trace, **options)
                spawn.assert_not_called()
                self.assertFalse(trace.exists())

    def test_launch_error_propagates_without_shutdown_or_retry(self):
        failure = OSError("injected direct process launch failure")
        with self.capture(side_effect=failure) as launch:
            with self.assertRaises(OSError) as caught:
                WorldClient.open_game("native", "game.json")
        self.assertIs(caught.exception, failure)
        self.assertEqual(launch.call_count, 1)

    def test_context_preserves_body_error_and_closes_once(self):
        failure = RuntimeError("caller failed after opening")
        process = MemoryTransport()
        with self.capture(return_value=process):
            with self.assertRaises(RuntimeError) as caught:
                with WorldClient.open_game("native", "game.json"):
                    raise failure
        self.assertIs(caught.exception, failure)
        self.assertTrue(process.closed)
        self.assertEqual(process.close_calls, 1)
        self.assertEqual(process.requests, [])

    def test_primary_error_and_cleanup_failure_remain_separate(self):
        primary = RuntimeError("body")
        cleanup = TransportError("direct process failed to close")
        process = MemoryTransport(close_error=cleanup)
        with self.capture(return_value=process):
            with self.assertRaises(RuntimeError) as caught:
                with WorldClient.open_game_command(["owned", "--", "native"], "game.json") as engine:
                    raise primary
        self.assertIs(caught.exception, primary)
        self.assertIs(engine.cleanup_error, cleanup)
        self.assertEqual(process.close_calls, 1)
        self.assertEqual(process.requests, [])

    def test_cleanup_failure_on_success_is_not_hidden(self):
        cleanup = TransportError("direct close failure")
        process = MemoryTransport(close_error=cleanup)
        with self.capture(return_value=process):
            with self.assertRaises(TransportError) as caught:
                with WorldClient.open_game("native", "game.json") as engine:
                    pass
        self.assertIs(caught.exception, cleanup)
        self.assertIs(engine.cleanup_error, cleanup)
        self.assertEqual(process.close_calls, 1)

    def test_terminal_rpc_error_is_not_retried_or_rewritten(self):
        failure = RpcError(-32091, "original native diagnostic", {"preserved": True})
        process = MemoryTransport(request_error=failure)
        with self.capture(return_value=process):
            with self.assertRaises(RpcError) as caught:
                with WorldClient.open_game("native", "game.json") as engine:
                    engine.call("world.inspect", timeout=0.5)
        self.assertIs(caught.exception, failure)
        self.assertEqual(process.requests, [("world.inspect", {}, 0.5)])
        self.assertEqual(process.close_calls, 1)

    def test_default_validation_accepts_standalone_readonly_discovery(self):
        # Existing authoring-core schema permits standalone scope by omission;
        # the dedicated game host must not invent a new shared-scope enum.
        catalog = dict(protocol_version=1, schema_revision=1, mode="read_only_runtime",
                       read_only=True, runtime_available=True, partial=True, view="catalog",
                       methods=["runtime.status"], components=[], sections=[])
        focused = dict(catalog, view="method", methods={"runtime.status": {}})
        for name in ("components", "sections"):
            focused.pop(name)
        for view, name, response in (("catalog", None, catalog), ("method", "runtime.status", focused)):
            class DiscoveryTransport(MemoryTransport):
                def request(self, method, params, *, timeout):
                    self.requests.append((method, params, timeout))
                    return copy.deepcopy(response)
            process = DiscoveryTransport()
            with self.subTest(view=view), self.capture(return_value=process):
                with WorldClient.open_game("native", "game.json") as engine:
                    self.assertTrue(engine.validate_responses)
                    result = engine.discover(view, name)
                    self.assertEqual(result, response)
                    self.assertNotIn("session_scope", result)
            self.assertEqual(process.close_calls, 1)

    def test_default_validation_still_rejects_unknown_scope_and_readonly_mismatch(self):
        response = dict(protocol_version=1, schema_revision=1, mode="read_only_runtime",
                        read_only=True, runtime_available=True, partial=True, view="catalog",
                        methods=[], components=[], sections=[])
        for changes in ({"session_scope": "dedicated_game"}, {"read_only": False}):
            class InvalidDiscoveryTransport(MemoryTransport):
                def request(self, method, params, *, timeout):
                    return dict(response, **changes)
            process = InvalidDiscoveryTransport()
            with self.subTest(changes=changes), self.capture(return_value=process):
                with self.assertRaises(ResponseContractError):
                    with WorldClient.open_game("native", "game.json") as engine:
                        engine.discover()
            self.assertEqual(process.close_calls, 1)


if __name__ == "__main__":
    unittest.main()
