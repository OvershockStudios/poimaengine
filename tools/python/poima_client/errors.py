# SPDX-License-Identifier: Apache-2.0
"""Errors from the native Poima call-only automation transport."""


class TransportError(RuntimeError):
    """The transport failed, or a request was rejected before it could be sent."""


class OutcomeUnknown(TransportError):
    """A request may have reached the engine without a trusted final response.

    This is not permission to retry a mutation. Inspect authoritative state or
    its receipt from a new, explicitly created connection before deciding what
    to do next. The transport never retries or reconnects automatically.
    """


class RpcError(RuntimeError):
    """A validated JSON-RPC error returned by the authoritative engine."""

    def __init__(self, code, message, data=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.data = data
