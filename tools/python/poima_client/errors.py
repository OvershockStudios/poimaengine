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
    """A validated terminal error, retaining its native code/message/data.

    Calls through JsonRpcProcess and WorldClient also retain detached request
    context. rpc_id correlates one transport response; request_id is an optional
    native operation receipt selector. Neither is permission to retry.
    Context is deliberately excluded from args, str and repr. Native messages
    themselves are preserved, not redacted. Manually constructed errors have
    unknown request context until raised by a client call.
    """

    def __init__(self, code, message, data=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.data = data
        self.method = None
        self.rpc_id = None
        self.params_json = None
        self.params = None
        self.request_id = None
