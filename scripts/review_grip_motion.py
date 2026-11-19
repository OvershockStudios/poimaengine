#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Report per-tick hand motion and fingertip contact from native RPC logs."""

import argparse
from itertools import islice, zip_longest
import json
import math
from pathlib import Path
import re


def frame(values):
    if len(values) != 16 or any(type(x) not in (int, float) or not math.isfinite(x) for x in values):
        raise ValueError("Observation requires a finite column-major world matrix.")
    if any(abs(values[i] - expected) > 1e-6 for i, expected in ((3, 0), (7, 0), (11, 0), (15, 1))):
        raise ValueError("World matrix is not affine.")
    columns = [values[i:i + 3] for i in (0, 4, 8)]
    lengths = [math.hypot(*column) for column in columns]
    if min(lengths) < 1e-9:
        raise ValueError("World matrix has a degenerate basis.")
    axes = tuple(tuple(x / length for x in column) for column, length in zip(columns, lengths))
    x, y, z = axes
    cross = (x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0])
    if abs(math.fsum(a * b for a, b in zip(x, y))) > 1e-5 or math.dist(cross, z) > 1e-5:
        raise ValueError("Motion review needs an orthogonal, right-handed frame.")
    return values[12:15], axes


def relative(reference, subject):
    origin, basis = reference
    position, axes = subject
    delta = [x - y for x, y in zip(position, origin)]
    return ([math.fsum(x * y for x, y in zip(axis, delta)) for axis in basis],
            [math.fsum(x * y for x, y in zip(axis, vector)) for vector in axes for axis in basis])


def angular_step(previous, next_frame):
    cosine = (math.fsum(a * b for a, b in zip(previous, next_frame)) - 1) * .5
    return math.degrees(math.acos(max(-1, min(1, cosine))))


def selected_samples(request, result, args):
    if request["method"] == "runtime.observe":
        values = {row["id"]: row["state"].get("world_matrix") for row in result["entities"]}
        selected = (args.camera, args.hand, args.item)
        if all(key in request["params"].get("ids", ()) and values.get(key) is not None for key in selected):
            return tuple(frame(values[key]) for key in selected)
    else:
        selectors = request["params"]["pairs"]
        observations = result["pairs"]
        if len(selectors) != len(observations):
            raise ValueError("Surface measurement count differs from the request.")
        probes = []
        for query, row in zip(selectors, observations):
            source = query["source"]
            target = query["target"]
            if (source["entity"], target["entity"]) != (args.finger_drawable, args.item):
                continue
            if (row["source"]["entity"], row["target"]["entity"]) != (source["entity"], target["entity"]):
                raise ValueError("Surface measurement identities differ from the requested probe.")
            distance = row["distance_m"]
            if type(distance) not in (float, int) or not math.isfinite(distance) or distance < 0:
                raise ValueError("Surface measurement must report a finite nonnegative distance.")
            probes.append((query, distance * 1000))
        if probes:
            return probes
    return None


def read_take(args):
    poses, contacts = {}, {}
    methods = {"runtime.observe": poses, "runtime.surface.measure": contacts}
    with args.requests.open(encoding="utf-8") as requests, args.responses.open(encoding="utf-8") as responses:
        for line, (sent, received) in enumerate(zip_longest(requests, responses), 1):
            if sent is None or received is None:
                raise ValueError("Request and response logs differ in length.")
            request, response = json.loads(sent), json.loads(received)
            if (not isinstance(request, dict) or not isinstance(response, dict) or
                    "id" not in request or request["id"] != response.get("id")
                    or request.get("jsonrpc") != "2.0" or response.get("jsonrpc") != "2.0"):
                raise ValueError(f"Unpaired JSON-RPC message on line {line}.")
            store = methods.get(request.get("method"))
            if store is None or "result" not in response:
                continue
            params, result = request["params"], response["result"]
            tick = params.get("tick")
            if params.get("session_id") != args.session or type(tick) is not int or not args.first <= tick <= args.last:
                continue
            keys = ("session_id", "tick", "structure_revision")
            if type(params.get("structure_revision")) is not int or any(params[k] != result.get(k) for k in keys):
                raise ValueError(f"Missing or mismatched observation guard on line {line}.")
            selected = selected_samples(request, result, args)
            if selected is None:
                continue
            guard = (tick, params["structure_revision"], result["authored_revision"])
            if store.setdefault(guard, selected) != selected:
                raise ValueError(f"Conflicting sample on line {line}.")
    joined = {}
    for guard in sorted(poses.keys() & contacts.keys()):
        tick = guard[0]
        if tick in joined:
            raise ValueError(f"Multiple complete revisions at tick {tick}. Select one phase in the input logs.")
        joined[tick] = (guard, poses[guard], contacts[guard])
    missing = list(islice((tick for tick in range(args.first, args.last + 1) if tick not in joined), 8))
    if missing:
        raise ValueError(f"Missing matched pose and contact observations at ticks {missing}.")
    return joined


def report(args, joined):
    rows = []
    selectors = None
    previous = initial_grip = previous_camera = None
    camera_travel = 0.
    for tick, (guard, (camera, hand, item), contacts) in joined.items():
        queries, distances = map(list, zip(*contacts))
        if selectors is not None and queries != selectors:
            raise ValueError(f"Surface probe definitions changed at tick {tick}.")
        selectors = queries
        hand_position, hand_axes = relative(camera, hand)
        grip_position, grip_axes = relative(hand, item)
        if initial_grip is None:
            initial_grip = (grip_position, grip_axes)
        if previous_camera is not None:
            camera_travel += math.dist(previous_camera, camera[0])
        rows.append(dict(tick=tick, structure_revision=guard[1], authored_revision=guard[2],
                         distances_mm=distances, hand_camera_position_m=hand_position,
                         hand_step_mm=math.dist(previous[0], hand_position) * 1000 if previous else None,
                         hand_step_degrees=angular_step(previous[1], hand_axes) if previous else None,
                         grip_drift_mm=math.dist(initial_grip[0], grip_position) * 1000,
                         grip_drift_degrees=angular_step(initial_grip[1], grip_axes)))
        previous, previous_camera = (hand_position, hand_axes), camera[0]
    worst = max(rows, key=lambda row: max(row["distances_mm"]))
    peak_translation = max(rows[1:], key=lambda row: row["hand_step_mm"])
    peak_rotation = max(rows[1:], key=lambda row: row["hand_step_degrees"])
    outside = [row["tick"] for row in rows if max(row["distances_mm"]) > args.contact_mm]
    return dict(format="poima.grip-motion.v1", session_id=args.session, tick_rate=60, probes=selectors,
                camera=args.camera, hand=args.hand, item=args.item,
                contact_limit_mm=args.contact_mm, ticks_outside_contact_limit=outside,
                worst_contact=dict(tick=worst["tick"], distance_mm=max(worst["distances_mm"])),
                peak_hand_step=dict(tick=peak_translation["tick"], distance_mm=peak_translation["hand_step_mm"]),
                peak_hand_rotation=dict(tick=peak_rotation["tick"], degrees=peak_rotation["hand_step_degrees"]),
                camera_travel_m=camera_travel, samples=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("requests", type=Path)
    parser.add_argument("responses", type=Path)
    for field in ("session", "camera", "hand", "item", "finger-drawable"):
        parser.add_argument("--" + field, required=True)
    parser.add_argument("--first", type=int, required=True)
    parser.add_argument("--last", type=int, required=True)
    parser.add_argument("--contact-mm", type=float, required=True)
    args = parser.parse_args()
    identities = (args.session, args.camera, args.hand, args.item, args.finger_drawable)
    if any(re.fullmatch("[0-9a-f]{32}", identity) is None for identity in identities):
        parser.error("Entity and session IDs must be 32 lowercase hexadecimal characters.")
    if args.first < 0 or args.last <= args.first or not math.isfinite(args.contact_mm) or args.contact_mm <= 0:
        parser.error("Supply at least two consecutive ticks and a positive finite contact limit.")
    try:
        result = report(args, read_take(args))
    except (AttributeError, KeyError, TypeError, ValueError, OSError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
