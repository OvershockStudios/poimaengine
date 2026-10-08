# SPDX-License-Identifier: Apache-2.0
"""Small conveniences over the authoritative native world API.

Helpers return native JSON results rather than maintaining another world model.
There are no automatic retries, conflict repairs, reconnects or schema caches.
"""
import json
import os
import re
import uuid

from .errors import OutcomeUnknown, TransportError
from .responses import ResponseContractError, validate_core_result
from .transport import JsonRpcProcess, REQUEST_LIMIT

_OMITTED = object()
_ENTITY_ID = re.compile(r"^[0-9a-f]{32}$")
_MAX_REVISION = 9007199254740991


def new_id():
    """Return a fresh native entity/request identifier (32 lowercase hex digits)."""
    return uuid.uuid4().hex


class PaginationError(RuntimeError):
    """A bounded traversal could not finish, or a page violated its snapshot."""


def _integer_bound(value, lower, upper, name):
    if isinstance(value, bool) or not isinstance(value, int) or not lower <= value <= upper:
        raise ValueError("{} must be an integer from {} to {}".format(name, lower, upper))


def _snapshot(params):
    # Bound the detached recovery context before sending anything. The transport
    # separately checks the complete envelope, including its correlation ID.
    chunks = []
    size = 0
    encoder = json.JSONEncoder(ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    try:
        for chunk in encoder.iterencode(params):
            size += len(chunk.encode("utf-8"))
            if size > REQUEST_LIMIT:
                raise ValueError("Request parameters exceed 1 MiB")
            chunks.append(chunk)
    except (TypeError, ValueError, UnicodeError, RecursionError) as error:
        raise ValueError("Cannot encode request parameters: " + str(error)) from error
    return "".join(chunks)


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Request parameters encode duplicate JSON object keys")
        result[key] = value
    return result


class WorldClient:
    """Own a native stdio client; closing it detaches without shutting down a host.

    ``WorldClient(transport)`` also accepts a request/close-compatible transport
    for explicit embedding. ``open`` owns a standalone world's writer; ``connect``
    attaches to a same-platform shared endpoint through the native CLI.
    """

    def __init__(self, transport, *, validate_responses=True):
        if not isinstance(validate_responses, bool):
            raise ValueError("validate_responses must be a boolean")
        self.transport = transport
        self.validate_responses = validate_responses

    @classmethod
    def open(cls, binary, world_path, *, cwd=None, validate_responses=True, **transport_options):
        """Launch ``binary world world_path`` with explicit paths and no shell.

        Paths use the executable's platform conventions; WSL does not translate
        Windows world paths automatically. Startup errors may surface on the
        first request. No discovery or extra mutation is performed implicitly.
        """
        if not isinstance(validate_responses, bool):
            raise ValueError("validate_responses must be a boolean")
        transport = JsonRpcProcess([os.fspath(binary), "world", os.fspath(world_path)],
                                   cwd=cwd, **transport_options)
        return cls(transport, validate_responses=validate_responses)

    @classmethod
    def connect(cls, binary, endpoint, *, timeout_ms=30000, cwd=None,
                validate_responses=True, **transport_options):
        """Launch an endpoint client; ``timeout_ms`` bounds connection and exchanges."""
        _integer_bound(timeout_ms, 100, 600000, "timeout_ms")
        if not isinstance(endpoint, str) or re.fullmatch(r"[A-Za-z0-9_-]{1,64}", endpoint) is None:
            raise ValueError("endpoint must be 1..64 ASCII letters, digits, underscores or hyphens")
        if not isinstance(validate_responses, bool):
            raise ValueError("validate_responses must be a boolean")
        transport = JsonRpcProcess([os.fspath(binary), "connect", endpoint,
                                    "--timeout-ms", str(timeout_ms)],
                                   cwd=cwd, **transport_options)
        return cls(transport, validate_responses=validate_responses)

    @property
    def closed(self):
        return self.transport.closed

    def close(self):
        """Close the owned transport, preserving the shared host and other clients."""
        self.transport.close()

    def __enter__(self):
        if self.closed:
            raise TransportError("World client transport is closed")
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        try:
            self.close()
        except TransportError:
            if exc_type is None:
                raise
        return False

    def call(self, method, params=None, *, timeout=30.0):
        """Call any discovered native method without rewriting its parameters.

        Core result validation reports an unexpected response shape after a
        terminal JSON reply; it does not undo a mutation or close the transport.
        Out-of-scope methods remain available without invented wrapper schemas.
        A bounded local JSON snapshot precedes the transport's request deadline.
        Validation and recovery use this immutable snapshot, never caller-owned
        objects that may have changed while the native response was pending.
        """
        actual = {} if params is None else params
        if not isinstance(actual, (dict, list)):
            raise ValueError("params must be an object, array, or None")
        return self._call_serialized(method, _snapshot(actual), timeout)

    def _call_serialized(self, method, serialized, timeout):
        detached = json.loads(serialized, object_pairs_hook=_unique_object)
        try:
            result = self.transport.request(method, detached, timeout=timeout)
            if self.validate_responses:
                # Even an embedding transport mutating its argument cannot
                # change the parameters used to validate the terminal result.
                validate_core_result(method, result, json.loads(serialized))
            return result
        except (OutcomeUnknown, ResponseContractError, KeyboardInterrupt, SystemExit) as error:
            # params_json remains authoritative if the convenient decoded copy
            # is edited later. Control-flow context does not assert transmission.
            error.method = method
            error.params_json = serialized
            error.params = json.loads(serialized)
            error.request_id = error.params.get("request_id") if isinstance(error.params, dict) else None
            raise

    def discover(self, view="catalog", name=None, *, timeout=30.0):
        """Fetch compact discovery by default; ``view='full'`` fetches everything."""
        if view not in ("full", "catalog", "method", "component", "section"):
            raise ValueError("Use discover_mutation for mutation discovery; unknown discovery view")
        named = view in ("method", "component", "section")
        if named and (not isinstance(name, str) or not name):
            raise ValueError("Named discovery requires a nonempty string name")
        if not named and name is not None:
            raise ValueError("Only method, component and section discovery accept name")
        params = {"view": view}
        if name is not None:
            params["name"] = name
        return self.call("world.describe", params, timeout=timeout)

    def discover_mutation(self, operation, type=None, *, timeout=30.0):
        """Discover an operation's transaction envelope, optionally by component.

        Availability remains authoritative in the native host. Enum, custom-type
        and conservative overlapping branches can remain in the returned union;
        this view does not restrict execution or guarantee one exact branch.
        """
        for label, value in (("operation", operation), ("type", type)):
            if label == "type" and value is None:
                continue
            if not isinstance(value, str) or not value:
                raise ValueError(label + " must be a nonempty UTF-8 string")
            try:
                size = len(value.encode("utf-8"))
            except UnicodeError as error:
                raise ValueError(label + " must be a valid UTF-8 string") from error
            if size > 128:
                raise ValueError(label + " must be at most 128 UTF-8 bytes")
        if type is not None and operation not in ("component.set", "component.remove"):
            raise ValueError("type is only supported for component.set and component.remove")
        params = {"view": "mutation", "operation": operation}
        if type is not None:
            params["type"] = type
        return self.call("world.describe", params, timeout=timeout)

    def inspect(self, *, timeout=30.0):
        return self.call("world.inspect", timeout=timeout)

    def history(self, *, timeout=30.0):
        return self.call("world.history", timeout=timeout)

    def get(self, entity_id, *, revision=None, component=None, timeout=30.0):
        params = {"id": entity_id}
        if revision is not None:
            params["revision"] = revision
        if component is not None:
            params["component"] = component
        return self.call("entity.get", params, timeout=timeout)

    def world_transform(self, entity_id, *, revision=None, timeout=30.0):
        params = {"id": entity_id}
        if revision is not None:
            params["revision"] = revision
        return self.call("entity.world_transform", params, timeout=timeout)

    def query(self, *, revision=None, parent=_OMITTED, component=None,
              after=None, limit=64, timeout=30.0):
        """Query one page; omitted parent means all entities, ``None`` means roots."""
        params = {"limit": limit}
        if revision is not None:
            params["revision"] = revision
        if parent is not _OMITTED:
            params["parent"] = parent
        if component is not None:
            params["component"] = component
        if after is not None:
            params["after"] = after
        return self.call("entity.query", params, timeout=timeout)

    def iter_query_pages(self, *, revision=None, parent=_OMITTED, component=None,
                         after=None, limit=256, max_pages=1024, timeout=30.0):
        """Yield bounded pages from one guarded revision, never a mixed snapshot.

        A later stale-revision error propagates; already yielded pages belonged
        to the original snapshot. Exhausting ``max_pages`` raises, never silently
        presents a truncated traversal as complete. Timeout applies per page.
        """
        _integer_bound(limit, 1, 256, "limit")
        _integer_bound(max_pages, 1, 65536, "max_pages")
        if revision is not None:
            _integer_bound(revision, 0, _MAX_REVISION, "revision")
        if after is not None:
            if revision is None:
                raise ValueError("after requires an explicit revision")
            if not isinstance(after, str) or _ENTITY_ID.fullmatch(after) is None:
                raise ValueError("after must be a native entity identifier")
        cursor = after
        for page_index in range(max_pages):
            page = self.query(revision=revision, parent=parent, component=component,
                              after=cursor, limit=limit, timeout=timeout)
            observed = page.get("revision") if isinstance(page, dict) else None
            if (isinstance(observed, bool) or not isinstance(observed, int)
                    or not 0 <= observed <= _MAX_REVISION):
                raise PaginationError("Page lacks a valid revision")
            if revision is None:
                revision = observed
            elif observed != revision:
                raise PaginationError("Page changed the guarded revision")
            rows = page.get("entities")
            if not isinstance(rows, list) or len(rows) > limit:
                raise PaginationError("Page has an invalid entity list")
            previous = cursor
            for row in rows:
                identifier = row.get("id") if isinstance(row, dict) else None
                if (not isinstance(identifier, str) or _ENTITY_ID.fullmatch(identifier) is None
                        or (previous is not None and identifier <= previous)):
                    raise PaginationError("Page entity identifiers are not strictly increasing")
                previous = identifier
            if "next_after" not in page:
                raise PaginationError("Page lacks its completion cursor")
            next_cursor = page["next_after"]
            if next_cursor is not None and (not rows or next_cursor != previous):
                raise PaginationError("Page cursor does not identify its final entity")
            yield page
            if next_cursor is None:
                return
            cursor = next_cursor
        raise PaginationError("Traversal exceeded max_pages before completion")

    def iter_entities(self, **query_options):
        """Yield native query rows using the same bounded, revision-pinned traversal."""
        for page in self.iter_query_pages(**query_options):
            yield from page["entities"]

    def _mutation(self, method, params, timeout):
        # Share the exact original snapshot with generic calls rather than
        # serializing twice. The common path also rebinds ResponseContractError
        # to the helper-generated receipt ID and the original sent parameters.
        return self._call_serialized(method, _snapshot(params), timeout)

    def transact(self, ops, base_revision, *, request_id=None, preview=False, timeout=30.0):
        """Submit one guarded transaction; retain an explicit ID for later exact retry."""
        return self._mutation("world.transact", {
            "ops": ops, "base_revision": base_revision,
            "request_id": new_id() if request_id is None else request_id,
            "preview": preview}, timeout)

    def undo(self, base_revision, *, request_id=None, timeout=30.0):
        return self._mutation("world.undo", {
            "base_revision": base_revision,
            "request_id": new_id() if request_id is None else request_id}, timeout)

    def redo(self, base_revision, *, request_id=None, timeout=30.0):
        return self._mutation("world.redo", {
            "base_revision": base_revision,
            "request_id": new_id() if request_id is None else request_id}, timeout)
