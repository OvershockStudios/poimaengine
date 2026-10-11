# SPDX-License-Identifier: Apache-2.0
"""Pure registered-component pagination contracts using the public Python client.

Run: python3 tests/python_runtime_observation.py
Scripted native replies exercise client boundaries; this does not qualify a
native runtime, arbitrary custom field values, or a complete game checkpoint.
"""
import copy
import math
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "python"))
from poima_client import ObservationError, OutcomeUnknown, RpcError, TransportError, WorldClient


def uid(number):
    return format(number, "032x")


SESSION, TYPE, OTHER, FIELD, OTHER_FIELD = (uid(n) for n in range(500, 505))
PINS = ("structure_revision", "component_revision", "gameplay_revision",
        "ui_revision", "control_sequence")
SOURCE = ("authored_revision", "current_authored_revision", "source_stale")
HEADER = dict(session_id=SESSION, tick=9, authored_revision=3,
              current_authored_revision=4, source_stale=True,
              **{name: index + 1 for index, name in enumerate(PINS)})
STATE = dict(world_matrix=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
             layout="column_major", velocity=[0, 0, 0], has_body=False,
             is_character=False, ground=None, yaw=0, pitch=0, animation=None)
DEFAULT_FIELDS = ("world_matrix", "layout", "velocity", "has_body",
                  "is_character", "ground", "yaw", "pitch")


class ScriptedTransport:
    """Record actual WorldClient requests and return detached scripted replies."""
    def __init__(self, responder):
        self.responder = responder
        self.calls = []
        self.closed = False

    def request(self, method, params=None, timeout=30.0):
        self.calls.append((method, copy.deepcopy(params), timeout))
        return self.responder(method, params, len(self.calls))

    def close(self):
        self.closed = True


class Pages:
    def __init__(self, count, mutate=None):
        self.ids = [uid(n) for n in range(1, count + 1)]
        self.mutate = mutate

    def __call__(self, method, params, ordinal):
        if method != "runtime.observe":
            raise AssertionError("Unexpected hidden client call: " + method)
        query = params["query"]
        remaining = [entity for entity in self.ids if entity > query.get("after", "")]
        selected = remaining[:query["limit"]]
        types = []
        for component in sorted(params["components"], key=lambda item: item["type"]):
            item = dict(type=component["type"], fingerprint="a" * 64)
            if params["include_schemas"]:
                item["schema"] = {"revision": 1}
            types.append(item)
        rows = []
        for entity in selected:
            values = {}
            for component in params["components"]:
                fields = component.get("fields", [FIELD])
                values[component["type"]] = {field: "9223372036854775807" for field in fields}
            rows.append(dict(id=entity,
                             state={field: copy.deepcopy(STATE[field])
                                    for field in params.get("entity_fields", DEFAULT_FIELDS)},
                             components=values))
        reply = dict(copy.deepcopy(HEADER), component_types=types, entities=rows,
                     query=dict(type=query["type"], entities=selected,
                                next_after=selected[-1] if len(remaining) > len(selected) else None))
        if self.mutate is not None:
            self.mutate(reply, ordinal)
        return reply


class ObservationContracts(unittest.TestCase):
    def client(self, count=1, mutate=None):
        transport = ScriptedTransport(Pages(count, mutate))
        return WorldClient(transport), transport

    def iterator(self, client, **kwargs):
        return client.iter_component_observations(SESSION, HEADER["tick"], TYPE, **kwargs)

    def rejected_reply(self, mutate, *, count=1, **kwargs):
        client, transport = self.client(count, mutate)
        with self.assertRaises(ObservationError):
            list(self.iterator(client, **kwargs))
        return transport

    def test_zero_single_and_many_complete_pages(self):
        for count in (0, 1, 32, 33, 64, 65, 129):
            with self.subTest(count=count):
                client, transport = self.client(count)
                pages = list(self.iterator(client, components=[dict(type=TYPE, fields=[FIELD])],
                                           include_schemas=True, timeout=2.5))
                self.assertEqual(len(transport.calls), max(1, math.ceil(count / 32)))
                self.assertEqual([row["id"] for page in pages for row in page["entities"]],
                                 [uid(n) for n in range(1, count + 1)])
                self.assertIsNone(pages[-1]["query"]["next_after"])
                for index, (method, params, timeout) in enumerate(transport.calls):
                    self.assertEqual(method, "runtime.observe")
                    self.assertEqual(timeout, 2.5)
                    self.assertEqual(params["session_id"], SESSION)
                    self.assertEqual(params["tick"], HEADER["tick"])
                    self.assertFalse(set(SOURCE) & set(params))
                    if index == 0:
                        self.assertFalse(set(PINS) & set(params))
                        self.assertNotIn("after", params["query"])
                    else:
                        self.assertEqual({key: params[key] for key in PINS},
                                         {key: HEADER[key] for key in PINS})
                        self.assertEqual(params["query"]["after"], uid(index * 32))
                if count:
                    self.assertEqual(pages[0]["entities"][0]["components"][TYPE][FIELD],
                                     "9223372036854775807")

    def test_initial_partial_and_complete_pins_are_forwarded(self):
        for pins in ({PINS[0]: HEADER[PINS[0]]}, {key: HEADER[key] for key in PINS}):
            with self.subTest(pins=pins):
                client, transport = self.client(33)
                list(self.iterator(client, pins=pins))
                first = transport.calls[0][1]
                self.assertEqual({key: first[key] for key in PINS if key in first}, pins)
                self.assertEqual({key: transport.calls[1][1][key] for key in PINS},
                                 {key: HEADER[key] for key in PINS})

    def test_supported_page_size_extremes(self):
        for count, limit in ((3, 1), (65, 64)):
            with self.subTest(count=count, limit=limit):
                client, transport = self.client(count)
                pages = list(self.iterator(client, limit=limit))
                self.assertEqual(len(transport.calls), math.ceil(count / limit))
                self.assertEqual([row["id"] for page in pages for row in page["entities"]],
                                 [uid(n) for n in range(1, count + 1)])
                self.assertTrue(all(call[1]["query"]["limit"] == limit for call in transport.calls))

    def test_initial_pin_mismatch_rejects_first_page(self):
        client, transport = self.client(1)
        with self.assertRaises(ObservationError):
            list(self.iterator(client, pins={PINS[0]: HEADER[PINS[0]] + 1}))
        self.assertEqual(len(transport.calls), 1)

    def test_every_runtime_and_source_boundary_drift_rejects_continuation(self):
        for key in ("session_id", "tick", *PINS, *SOURCE):
            with self.subTest(key=key):
                def mutate(reply, ordinal):
                    if ordinal == 2:
                        reply[key] = (uid(999) if key == "session_id" else
                                      False if key == "source_stale" else HEADER[key] + 1)
                client, transport = self.client(65, mutate)
                stream = self.iterator(client)
                first = next(stream)
                self.assertEqual(len(first["entities"]), 32)
                with self.assertRaises(ObservationError):
                    next(stream)
                self.assertEqual(len(transport.calls), 2)
                self.assertEqual(list(stream), [])

    def test_strict_header_wire_types_and_missing_tokens(self):
        for key in ("tick", *PINS, *SOURCE[:2]):
            for alias in (True, float(HEADER[key]), -1, 9007199254740992, "1"):
                with self.subTest(key=key, alias=alias):
                    self.rejected_reply(lambda reply, _: reply.__setitem__(key, alias))
        for alias in (1, 0, None, "true"):
            self.rejected_reply(lambda reply, _: reply.__setitem__("source_stale", alias))
        for key in ("session_id", "tick", *PINS, *SOURCE):
            with self.subTest(missing=key):
                self.rejected_reply(lambda reply, _: reply.pop(key))

    def test_caller_page_and_selector_mutations_do_not_change_continuation(self):
        fields, selected = ["yaw"], [dict(type=TYPE, fields=[FIELD])]
        pins = {PINS[0]: HEADER[PINS[0]]}
        client, transport = self.client(33)
        stream = self.iterator(client, pins=pins, entity_fields=fields,
                               components=selected, include_schemas=True)
        first = next(stream)
        first[PINS[0]] = 999
        first["source_stale"] = False
        first["query"]["next_after"] = uid(999)
        first["component_types"][0]["schema"]["revision"] = 999
        first["entities"].clear()
        fields.append("pitch")
        selected[0]["fields"].append(OTHER_FIELD)
        selected.append(dict(type=OTHER))
        pins[PINS[0]] = 999
        second = next(stream)
        self.assertEqual(second["query"]["entities"], [uid(33)])
        self.assertEqual(transport.calls[1][1]["entity_fields"], ["yaw"])
        self.assertEqual(transport.calls[1][1]["components"], [dict(type=TYPE, fields=[FIELD])])
        self.assertEqual(transport.calls[1][1][PINS[0]], HEADER[PINS[0]])
        self.assertEqual(list(stream), [])

    def test_empty_projections_and_nonquery_absent_component_are_valid(self):
        client, _ = self.client(1)
        page = list(self.iterator(client, entity_fields=[], components=[]))[0]
        self.assertEqual(page["entities"][0]["state"], {})
        self.assertEqual(page["entities"][0]["components"], {})
        def absent(reply, _):
            reply["entities"][0]["components"][OTHER] = None
        client, _ = self.client(1, absent)
        page = list(self.iterator(client, entity_fields=[], components=[
            dict(type=TYPE, fields=[]), dict(type=OTHER, fields=[OTHER_FIELD])]))[0]
        self.assertEqual(page["entities"][0]["components"], {TYPE: {}, OTHER: None})
        self.rejected_reply(lambda reply, _: reply["entities"][0]["components"].__setitem__(TYPE, None),
                            components=[dict(type=TYPE)])

    def test_metadata_type_schema_and_projection_consistency(self):
        selected = [dict(type=TYPE, fields=[FIELD])]
        mutations = [
            lambda r: r.__setitem__("component_types", None),
            lambda r: r["component_types"].clear(),
            lambda r: r["component_types"].append(copy.deepcopy(r["component_types"][0])),
            lambda r: r["component_types"][0].__setitem__("type", OTHER),
            lambda r: r["component_types"][0].__setitem__("fingerprint", ""),
            lambda r: r["component_types"][0].__setitem__("fingerprint", 123),
            lambda r: r["component_types"][0].pop("fingerprint"),
            lambda r: r["component_types"][0].pop("schema"),
            lambda r: r["component_types"][0].__setitem__("schema", []),
        ]
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                self.rejected_reply(lambda reply, _, fn=mutation: fn(reply),
                                    components=selected, include_schemas=True)
        for changed in (1.0, True, 2):
            def drift(reply, ordinal):
                if ordinal == 2:
                    reply["component_types"][0]["schema"]["revision"] = changed
            transport = self.rejected_reply(drift, count=65, components=selected,
                                             include_schemas=True)
            self.assertEqual(len(transport.calls), 2)

    def test_component_metadata_order_is_canonical(self):
        selected = [dict(type=OTHER, fields=[]), dict(type=TYPE, fields=[])]
        client, _ = self.client(1)
        page = list(self.iterator(client, components=selected))[0]
        self.assertEqual([item["type"] for item in page["component_types"]], [TYPE, OTHER])
        self.rejected_reply(lambda reply, _: reply["component_types"].reverse(), components=selected)

    def test_nonobject_reply_and_invalid_ids_are_rejected(self):
        for reply in (None, [], "unexpected", 1):
            transport = ScriptedTransport(lambda method, params, ordinal, value=reply: value)
            with self.assertRaises(ObservationError):
                list(self.iterator(WorldClient(transport)))
            self.assertEqual(len(transport.calls), 1)
        self.rejected_reply(lambda reply, _: reply.__setitem__("session_id", "A" * 32))
        self.rejected_reply(lambda reply, _: reply["query"]["entities"].__setitem__(0, "bad"))

    def test_query_rows_and_cursor_must_match_exact_membership(self):
        mutations = [
            lambda r: r.pop("query"), lambda r: r.__setitem__("entities", None),
            lambda r: r["query"].__setitem__("type", OTHER),
            lambda r: r["query"].pop("next_after"),
            lambda r: r["query"].__setitem__("entities", None),
            lambda r: r["entities"].pop(),
            lambda r: r["entities"][0].__setitem__("id", OTHER),
            lambda r: r["query"]["entities"].reverse(),
            lambda r: r["query"]["entities"].__setitem__(1, r["query"]["entities"][0]),
            lambda r: r["query"].__setitem__("next_after", uid(999)),
            lambda r: r["entities"][0].__setitem__("state", None),
            lambda r: r["entities"][0].__setitem__("components", None),
        ]
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                self.rejected_reply(lambda reply, _, fn=mutation: fn(reply), count=33)
        self.rejected_reply(lambda r, _: r["query"].__setitem__("next_after", uid(1)), count=1)
        self.rejected_reply(lambda r, _: r["query"].__setitem__("next_after", uid(1)), count=0)
        self.rejected_reply(lambda r, _: r["query"].__setitem__("next_after", False), count=33)
        self.rejected_reply(lambda r, _: r.__setitem__("entities", r["entities"] + r["entities"]), count=32)
        def oversized(reply, _):
            reply["query"]["entities"].append(uid(999))
            extra = copy.deepcopy(reply["entities"][0])
            extra["id"] = uid(999)
            reply["entities"].append(extra)
        self.rejected_reply(oversized, count=32)

    def test_continuation_cannot_repeat_prior_membership(self):
        def repeat(reply, ordinal):
            if ordinal == 2:
                reply["query"]["entities"][0] = uid(32)
                reply["entities"][0]["id"] = uid(32)
        transport = self.rejected_reply(repeat, count=33)
        self.assertEqual(len(transport.calls), 2)

    def test_projection_omissions_additions_and_field_id_validation(self):
        mutations = [
            lambda r: r["entities"][0]["state"].clear(),
            lambda r: r["entities"][0]["state"].__setitem__("animation", None),
            lambda r: r["entities"][0]["components"].clear(),
            lambda r: r["entities"][0]["components"].__setitem__(OTHER, {}),
            lambda r: r["entities"][0]["components"][TYPE].clear(),
            lambda r: r["entities"][0]["components"][TYPE].__setitem__(OTHER_FIELD, 0),
            lambda r: r["entities"][0]["components"].__setitem__(TYPE, []),
            lambda r: r["entities"][0]["components"].__setitem__(TYPE, {"bad": 0}),
        ]
        for index, mutation in enumerate(mutations):
            with self.subTest(mutation=index):
                self.rejected_reply(lambda reply, _, fn=mutation: fn(reply),
                                    components=[dict(type=TYPE, fields=[FIELD])])

    def test_max_pages_discards_completion_without_hidden_calls(self):
        client, transport = self.client(65)
        stream = self.iterator(client, max_pages=1)
        partial = next(stream)
        self.assertEqual(len(partial["entities"]), 32)
        with self.assertRaises(ObservationError):
            next(stream)
        self.assertEqual(len(transport.calls), 1)
        self.assertEqual(list(stream), [])
        client, transport = self.client(32)
        self.assertEqual(len(list(self.iterator(client, max_pages=1))), 1)
        self.assertEqual(len(transport.calls), 1)

    def test_native_and_transport_error_identity_after_partial_page(self):
        for error in (RpcError(-32009, "changed"), RpcError(-32030, "session ended"),
                      RpcError(-32004, "missing"), TransportError("closed"),
                      OutcomeUnknown("read timed out")):
            with self.subTest(error=type(error).__name__, message=str(error)):
                pages = Pages(65)
                def responder(method, params, ordinal):
                    if ordinal == 2:
                        raise error
                    return pages(method, params, ordinal)
                transport = ScriptedTransport(responder)
                stream = self.iterator(WorldClient(transport))
                self.assertEqual(len(next(stream)["entities"]), 32)
                with self.assertRaises(type(error)) as caught:
                    next(stream)
                self.assertIs(caught.exception, error)
                self.assertEqual(len(transport.calls), 2)
                self.assertEqual(list(stream), [])
                self.assertFalse(transport.closed)

    def test_invalid_inputs_never_reach_transport(self):
        cases = [dict(limit=True), dict(limit=0), dict(limit=65), dict(limit=1.0),
                 dict(max_pages=0), dict(max_pages=True), dict(max_pages=65537),
                 dict(include_schemas=1), dict(timeout=True), dict(timeout=0),
                 dict(timeout=float("nan")), dict(timeout=float("inf")), dict(timeout=86401),
                 dict(pins=[]), dict(pins={"unknown": 1}), dict(pins={PINS[0]: True}),
                 dict(pins={PINS[0]: 1.0}), dict(pins={PINS[0]: -1}),
                 dict(entity_fields="yaw"), dict(entity_fields=["yaw", "yaw"]),
                 dict(entity_fields=["unknown"]), dict(components={}),
                 dict(components=[dict(type=TYPE)] * 2),
                 dict(components=[dict(type=uid(n)) for n in range(5)]),
                 dict(components=[dict(type=TYPE, fields=None)]),
                 dict(components=[dict(type=TYPE, fields=[FIELD] * 2)]),
                 dict(components=[dict(type=TYPE, fields=[uid(n) for n in range(33)])]),
                 dict(components=[dict(type=TYPE, fields=["bad"])]),
                 dict(components=[dict(type="game:" + TYPE)]),
                 dict(components=[dict(type=TYPE, extra=float("nan"))]),
                 dict(components=[dict(type=TYPE, fields=["\ud800"])]),
                 dict(components=[dict(type=TYPE, extra="x" * (1024 * 1024))])]
        for options in cases:
            with self.subTest(options=str(options)[:160]):
                client, transport = self.client()
                with self.assertRaises(ValueError):
                    list(self.iterator(client, **options))
                self.assertEqual(transport.calls, [])
        for session, tick, component in (("bad", 9, TYPE), ("A" * 32, 9, TYPE),
                                         (SESSION, True, TYPE), (SESSION, 9.0, TYPE),
                                         (SESSION, -1, TYPE), (SESSION, 9007199254740992, TYPE),
                                         (SESSION, 9, "game:" + TYPE)):
            client, transport = self.client()
            with self.assertRaises(ValueError):
                list(client.iter_component_observations(session, tick, component))
            self.assertEqual(transport.calls, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
