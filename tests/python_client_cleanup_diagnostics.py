#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""In-memory cleanup diagnostics: no child, native host or background thread."""
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

if 'poima_client' not in sys.modules:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools/python'))
from poima_client import JsonRpcProcess, RpcError, TransportError, WorldClient


class Embedded:
    def __init__(self, failures=()):
        self.failures = iter(failures)
        self.closed = False
        self.closes = 0

    def close(self):
        self.closes += 1
        failure = next(self.failures, None)
        if failure is not None:
            raise failure
        self.closed = True


class FrozenPrimary(Exception):
    def __setattr__(self, name, value):
        raise AssertionError('Do not decorate a foreign primary exception')


class CleanupDiagnostics(unittest.TestCase):
    def process(self):
        # Real constructor, close and __exit__, with no Popen or thread startup.
        process = Mock(pid=1, stdin=io.BytesIO(), stdout=io.BytesIO(), stderr=io.BytesIO())
        with patch('poima_client.transport.subprocess.Popen', return_value=process) as launch, \
             patch('poima_client.transport.threading.Thread.start') as start:
            transport = JsonRpcProcess(['synthetic-peer'], close_timeout=0.01)
            self.assertEqual(launch.call_count, 1)
            self.assertEqual(start.call_count, 3)
        transport._disconnect = Mock()
        transport._cleanup_done.set()
        return transport

    def test_initial_readonly_properties_and_successful_close(self):
        embedded = Embedded()
        client = WorldClient(embedded)
        transport = self.process()
        self.assertIsNone(client.cleanup_error)
        self.assertIsNone(transport.cleanup_error)
        with self.assertRaises(AttributeError):client.cleanup_error = TransportError('assign')
        with self.assertRaises(AttributeError):transport.cleanup_error = TransportError('assign')
        client.close(); transport.close()
        self.assertIsNone(client.cleanup_error)
        self.assertIsNone(transport.cleanup_error)
        self.assertEqual(embedded.closes, 1)

    def test_embedded_failure_retains_identity_and_primary_unchanged(self):
        cleanup = TransportError('cleanup detail')
        primary = FrozenPrimary('original')
        client = WorldClient(Embedded([cleanup]))
        with self.assertRaises(FrozenPrimary) as caught:
            with client:
                raise primary
        self.assertIs(caught.exception, primary)
        self.assertEqual(primary.args, ('original',))
        self.assertIsNone(primary.__context__, 'Cleanup must not be chained onto the propagating primary')
        self.assertIs(client.cleanup_error, cleanup)
        self.assertEqual(client.transport.closes, 1)

    def test_body_interrupt_kept_when_cleanup_raises_transport_error(self):
        for primary in (KeyboardInterrupt(), SystemExit(7)):
            cleanup = TransportError('separate cleanup failure')
            client = WorldClient(Embedded([cleanup]))
            with self.subTest(primary=type(primary).__name__):
                with self.assertRaises(type(primary)) as caught:
                    with client:raise primary
                self.assertIs(caught.exception, primary)
                self.assertIs(client.cleanup_error, cleanup)

    def test_embedded_successful_body_still_raises_cleanup_error(self):
        error = TransportError('close failed')
        client = WorldClient(Embedded([error]))
        with self.assertRaises(TransportError) as caught:
            with client:pass
        self.assertIs(caught.exception, error)
        self.assertIs(client.cleanup_error, error)

    def test_explicit_close_retains_latest_failure_after_later_success(self):
        first, last = TransportError('first'), TransportError('last')
        client = WorldClient(Embedded([first, None, last]))
        with self.assertRaises(TransportError) as caught:client.close()
        self.assertIs(caught.exception, first)
        client.close()
        self.assertIs(client.cleanup_error, first)
        with self.assertRaises(TransportError) as caught:client.close()
        self.assertIs(caught.exception, last)
        self.assertIs(client.cleanup_error, last)
        self.assertEqual(client.transport.closes, 3)

    def test_ordinary_close_does_not_swallow_interrupt_or_unexpected_error(self):
        for error in (KeyboardInterrupt(), SystemExit(9), ValueError('unexpected')):
            client = WorldClient(Embedded([error]))
            with self.subTest(error=type(error).__name__):
                with self.assertRaises(type(error)) as caught:client.close()
                self.assertIs(caught.exception, error)
                self.assertIsNone(client.cleanup_error)

    def test_real_transport_deadline_close_retains_public_error(self):
        transport = self.process()
        transport._cleanup_done = Mock()
        transport._cleanup_done.wait.return_value = False
        with self.assertRaises(TransportError) as caught:transport.close()
        self.assertIs(transport.cleanup_error, caught.exception)
        self.assertEqual(str(caught.exception), 'Owned process cleanup exceeded its bounded deadline')
        self.assertEqual(transport._disconnect.call_count, 1)
        transport._cleanup_done.wait.assert_called_once_with(0.11)

    def test_real_transport_context_primary_and_shared_wrapper_error(self):
        transport = self.process()
        transport._cleanup_error = OSError('low-level cleanup detail')
        client = WorldClient(transport)
        primary = RpcError(-32009, 'primary native error')
        with self.assertRaises(RpcError) as caught:
            with client:raise primary
        self.assertIs(caught.exception, primary)
        self.assertIsInstance(client.cleanup_error, TransportError)
        self.assertIs(client.cleanup_error, transport.cleanup_error)
        self.assertEqual(str(transport.cleanup_error), 'Owned process cleanup failed: low-level cleanup detail')

    def test_real_transport_direct_context_preserves_primary(self):
        transport = self.process()
        transport._cleanup_error = OSError('cleanup cause')
        primary = FrozenPrimary('unchanged')
        with self.assertRaises(FrozenPrimary) as caught:
            with transport:raise primary
        self.assertIs(caught.exception, primary)
        self.assertIsInstance(transport.cleanup_error, TransportError)

    def test_real_transport_successful_context_still_raises(self):
        transport = self.process()
        transport._cleanup_error = OSError('cleanup cause')
        with self.assertRaises(TransportError) as caught:
            with transport:pass
        self.assertIs(transport.cleanup_error, caught.exception)

    def test_real_transport_later_success_keeps_observed_diagnostic(self):
        transport = self.process()
        transport._cleanup_error = OSError('cleanup cause')
        with self.assertRaises(TransportError):transport.close()
        previous = transport.cleanup_error
        transport._cleanup_error = None
        transport.close()
        self.assertIs(transport.cleanup_error, previous)
        transport._cleanup_error = OSError('new cause')
        with self.assertRaises(TransportError) as caught:transport.close()
        self.assertIs(transport.cleanup_error, caught.exception)
        self.assertIsNot(transport.cleanup_error, previous)

    def test_real_transport_interrupt_during_close_still_propagates(self):
        for error in (KeyboardInterrupt(), SystemExit(4)):
            transport = self.process()
            transport._cleanup_done = Mock()
            transport._cleanup_done.wait.side_effect = error
            with self.subTest(error=type(error).__name__):
                with self.assertRaises(type(error)) as caught:transport.close()
                self.assertIs(caught.exception, error)
                self.assertIsNone(transport.cleanup_error)

    def test_read_and_default_repr_do_not_format_or_emit_diagnostic(self):
        class SecretError(TransportError):
            def __str__(self):raise AssertionError('Do not implicitly format diagnostic')
            def __repr__(self):raise AssertionError('Do not implicitly repr diagnostic')
        error = SecretError('private-token')
        client = WorldClient(Embedded([error]))
        try:client.close()
        except TransportError:pass
        transport = self.process()
        transport._close_error = error
        with patch('builtins.print') as output:
            self.assertIs(client.cleanup_error, error)
            self.assertIs(transport.cleanup_error, error)
            self.assertNotIn('private-token', repr(client))
            self.assertNotIn('private-token', repr(transport))
            output.assert_not_called()
        self.assertEqual(client.transport.closes, 1)
        self.assertEqual(transport._disconnect.call_count, 0)


if __name__ == '__main__':
    unittest.main()
