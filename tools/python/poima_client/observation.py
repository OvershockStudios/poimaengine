# SPDX-License-Identifier: Apache-2.0
"""Bounded registered-component observation over the authoritative runtime API.

No discovery, retry, reconnect, tick advancement or mutation is implicit.
Pages share the first observation's guards; discard partial pages after a later
failure when a complete snapshot is required. Native values and omitted
component-field sets remain governed by the native schema. These reads do not
retain saves, gameplay state or UI contents.
"""
import json
import re

PINS = ('structure_revision', 'component_revision', 'gameplay_revision',
        'ui_revision', 'control_sequence')
SOURCE = ('authored_revision', 'current_authored_revision', 'source_stale')
HEADER = ('session_id', 'tick', *PINS, *SOURCE)
DEFAULT_FIELDS = ('world_matrix', 'layout', 'velocity', 'has_body', 'is_character', 'ground', 'yaw', 'pitch')
FIELDS = set(DEFAULT_FIELDS) | {'local_transform', 'animation', 'motion', 'kinematic_target', 'motion_remaining_ticks'}
MAX_INTEGER = 9007199254740991
ID = re.compile(r'[0-9a-f]{32}')


class ObservationError(RuntimeError):
    """A successful read could not form the promised observation boundary."""


def identity(value):
    return isinstance(value, str) and ID.fullmatch(value) is not None


def integer(value):
    return type(value) is int and 0 <= value <= MAX_INTEGER


def encoded(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False,
                      sort_keys=True, separators=(',', ':'))


def iter_component_observations(client, session_id, tick, type_id, *, pins=None,
                                limit=32, max_pages=1024, entity_fields=None,
                                components=None, include_schemas=False, timeout=30.0):
    """Yield complete native pages under all five first-read pins and source tokens.

    ``type_id`` is a registered custom-component ID, without ``game:``. Starting
    pins are optional; omitted ones come from the first observation. Every next
    request pins all five. No cross-page reordering or synthesized rows. Timeout
    is per native call; max_pages bounds traversal, not a whole-task deadline.
    Caller-selected state/component schemas remain the native API's authority.
    """
    if not identity(session_id) or not integer(tick) or not identity(type_id):
        raise ValueError('Expected native session/type IDs and a safe integer tick')
    if type(limit) is not int or not 1 <= limit <= 64:
        raise ValueError('limit must be an integer from 1 to 64')
    if type(max_pages) is not int or not 1 <= max_pages <= 65536:
        raise ValueError('max_pages must be an integer from 1 to 65536')
    if type(include_schemas) is not bool:
        raise ValueError('include_schemas must be boolean')
    if type(timeout) not in (int, float) or not 0 < timeout <= 86400:
        raise ValueError('timeout must be finite, positive and at most 86400')
    pins = {} if pins is None else pins
    if (not isinstance(pins, dict) or set(pins) - set(PINS)
            or any(not integer(value) for value in pins.values())):
        raise ValueError('Starting pins must be known safe integer guards')
    # Detach selectors before the first RPC, including nested stable-field IDs.
    selectors = {'components': [] if components is None else components,
                 'include_schemas': include_schemas}
    if entity_fields is not None:
        selectors['entity_fields'] = entity_fields
    try:
        selectors_json = encoded(selectors)
        if len(selectors_json.encode('utf-8')) > 1024 * 1024:
            raise ValueError('Selectors exceed the request parameter bound')
        selectors = json.loads(selectors_json)
    except (TypeError, ValueError, UnicodeError, RecursionError) as error:
        raise ValueError('Selectors must be bounded UTF-8 JSON') from error
    fields = selectors.get('entity_fields', list(DEFAULT_FIELDS))
    if (not isinstance(fields, list) or any(not isinstance(field, str) or field not in FIELDS for field in fields)
            or len(fields) != len(set(fields))):
        raise ValueError('Select unique known entity fields')
    selected_components = selectors['components']
    if not isinstance(selected_components, list) or len(selected_components) > 4:
        raise ValueError('Select at most four component types')
    component_fields = {}
    for component in selected_components:
        if (not isinstance(component, dict) or set(component) - {'type', 'fields'}
                or not identity(component.get('type')) or component['type'] in component_fields):
            raise ValueError('Select unique native component IDs')
        selected = component.get('fields')
        if 'fields' in component and (not isinstance(selected, list) or len(selected) > 32
                or any(not identity(field) for field in selected) or len(selected) != len(set(selected))):
            raise ValueError('Select unique stable component field IDs')
        component_fields[component['type']] = None if selected is None else set(selected)
    component_ids = sorted(component_fields)
    expected = {'session_id': session_id, 'tick': tick, **dict(pins)}
    metadata = None
    after = None
    for page_index in range(max_pages):
        query = {'type': type_id, 'limit': limit}
        if after is not None:
            query['after'] = after
        params = {key: expected[key] for key in ('session_id', 'tick', *PINS) if key in expected}
        params.update(json.loads(selectors_json), query=query)
        reply = client.call('runtime.observe', params, timeout=timeout)
        if not isinstance(reply, dict):
            raise ObservationError('Observation is not an object')
        header = {key: reply.get(key) for key in HEADER}
        if (not identity(header['session_id']) or not integer(header['tick'])
                or any(not integer(header[key]) for key in (*PINS, *SOURCE[:2]))
                or type(header['source_stale']) is not bool):
            raise ObservationError('Observation boundary fields have invalid wire types')
        if any(header[key] != value for key, value in expected.items()):
            raise ObservationError('Observation crossed a runtime or authored-source boundary')
        if not isinstance(reply.get('component_types'), list):
            raise ObservationError('Missing component projection metadata')
        types = reply['component_types']
        if (any(not isinstance(item, dict) or not identity(item.get('type'))
                or not isinstance(item.get('fingerprint'), str) or not item['fingerprint']
                or (include_schemas and not isinstance(item.get('schema'), dict)) for item in types)
                or [item['type'] for item in types] != component_ids):
            raise ObservationError('Component projection metadata omits selected types')
        try:
            projection = encoded(reply['component_types'])
        except (TypeError, ValueError, UnicodeError, RecursionError) as error:
            raise ObservationError('Invalid component projection metadata') from error
        if metadata is not None and projection != metadata:
            raise ObservationError('Component projection metadata changed between pages')
        page = reply.get('query')
        rows = reply.get('entities')
        if (not isinstance(page, dict) or page.get('type') != type_id
                or not isinstance(page.get('entities'), list)
                or 'next_after' not in page or not isinstance(rows, list)):
            raise ObservationError('Missing query membership or completion cursor')
        ids = page['entities']
        if (len(ids) > limit or any(not identity(value) for value in ids)
                or ids != sorted(set(ids)) or (after is not None and any(value <= after for value in ids))
                or len(rows) != len(ids)):
            raise ObservationError('Query page is not bounded, sorted exclusive membership')
        if (any(not isinstance(row, dict) or not identity(row.get('id'))
                or not isinstance(row.get('state'), dict) or not isinstance(row.get('components'), dict)
                for row in rows) or [row['id'] for row in rows] != ids):
            raise ObservationError('Observation rows differ from query membership')
        for row in rows:
            if set(row['state']) != set(fields) or set(row['components']) != set(component_ids):
                raise ObservationError('Observation omitted or added projected fields')
            for component_id, values in row['components'].items():
                selected = component_fields[component_id]
                if values is None:
                    if component_id == type_id:
                        raise ObservationError('Queried membership is absent from its selected projection')
                    continue
                if (not isinstance(values, dict) or any(not identity(field) for field in values)
                        or (selected is not None and set(values) != selected)):
                    raise ObservationError('Component payload differs from selected stable fields')
        cursor = page['next_after']
        if cursor is not None and (not ids or not identity(cursor) or cursor != ids[-1] or len(ids) != limit):
            raise ObservationError('Query cursor does not end a full page')
        # Keep internal guard/metadata strings detached from caller-mutated pages.
        expected = header
        metadata = projection
        yield reply
        if cursor is None:
            return
        after = cursor
    raise ObservationError('Traversal exceeded max_pages before completion')
