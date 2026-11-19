#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Report first-person grip or reach motion from guarded native RPC logs."""

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


def guarded_results(args, methods):
    with args.requests.open(encoding="utf-8") as requests, args.responses.open(encoding="utf-8") as responses:
        for line, (sent, received) in enumerate(zip_longest(requests, responses), 1):
            if sent is None or received is None:
                raise ValueError("Request and response logs differ in length.")
            request, response = json.loads(sent), json.loads(received)
            if (not isinstance(request, dict) or not isinstance(response, dict) or
                    "id" not in request or request["id"] != response.get("id")
                    or request.get("jsonrpc") != "2.0" or response.get("jsonrpc") != "2.0"):
                raise ValueError(f"Unpaired JSON-RPC message on line {line}.")
            method = request.get("method")
            if method not in methods or "result" not in response:
                continue
            params, result = request["params"], response["result"]
            tick = params.get("tick")
            if params.get("session_id") != args.session or type(tick) is not int or not args.first <= tick <= args.last:
                continue
            keys = ("session_id", "tick", "structure_revision")
            if type(params.get("structure_revision")) is not int or any(params[k] != result.get(k) for k in keys):
                raise ValueError(f"Missing or mismatched observation guard on line {line}.")
            yield line, request, result


def read_take(args):
    poses, contacts = {}, {}
    methods = {"runtime.observe": poses, "runtime.surface.measure": contacts}
    for line, request, result in guarded_results(args, methods):
        selected = selected_samples(request, result, args)
        if selected is None:
            continue
        params = request["params"]
        guard = (params["tick"], params["structure_revision"], result["authored_revision"])
        if methods[request["method"]].setdefault(guard, selected) != selected:
            raise ValueError(f"Conflicting sample on line {line}.")
    return match_ticks(args, poses, contacts, "pose and contact")


def match_ticks(args, poses, observations, phase):
    joined = {}
    for guard in sorted(poses.keys() & observations.keys()):
        tick = guard[0]
        if tick in joined:
            raise ValueError(f"Multiple complete revisions at tick {tick}. Select one phase in the input logs.")
        joined[tick] = (guard, poses[guard], observations[guard])
    missing = list(islice((tick for tick in range(args.first, args.last + 1) if tick not in joined), 8))
    if missing:
        raise ValueError(f"Missing matched {phase} observations at ticks {missing}.")
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


def read_reach(args):
    poses, outcomes = {}, {}
    joints = (args.camera, args.shoulder, args.elbow, args.hand)
    methods = {"runtime.observe": poses, "runtime.attachment.outcomes": outcomes}
    for line, request, result in guarded_results(args, methods):
        params = request["params"]
        method = request["method"]
        if method == "runtime.observe":
            if not all(joint in params.get("ids", ()) for joint in joints):
                continue
            matrices = {row["id"]: row["state"].get("world_matrix") for row in result["entities"]}
            selected = tuple(frame(matrices[joint]) for joint in joints)
            selected = (result["authored_revision"], *selected)
        else:
            matches = [row for row in result["outcomes"] if row["entity"] == args.item]
            if len(matches) > 1:
                raise ValueError(f"Duplicate item outcome on line {line}.")
            selected = matches[0] if matches else None
        guard = (params["tick"], params["structure_revision"])
        if methods[method].setdefault(guard, selected) != selected:
            raise ValueError(f"Conflicting sample on line {line}.")
    return match_ticks(args, poses, outcomes, "pose and pickup outcome")


def reach_report(args, joined):
    rows = []
    previous = None
    for tick, (guard, (authored_revision, camera, shoulder, elbow, hand), outcome) in joined.items():
        joints = [relative(camera, joint) for joint in (shoulder, elbow, hand)]
        positions = [position for position, _ in joints]
        wrist_axes = joints[-1][1]
        measured = [camera[0], *positions]
        if previous is None:
            camera_step, shoulder_step, elbow_step, wrist_step = (None,) * 4
            wrist_rotation = None
        else:
            previous_positions, previous_wrist_axes = previous
            camera_step, shoulder_step, elbow_step, wrist_step = (
                math.dist(before, after) * 1000 for before, after in zip(previous_positions, measured))
            wrist_rotation = angular_step(previous_wrist_axes, wrist_axes)
        rows.append(dict(tick=tick, structure_revision=guard[1], authored_revision=authored_revision,
                         outcome=outcome, joints_camera_position_m=positions,
                         upper_arm_mm=math.dist(positions[0], positions[1]) * 1000,
                         forearm_mm=math.dist(positions[1], positions[2]) * 1000,
                         camera_step_mm=camera_step, shoulder_step_mm=shoulder_step,
                         elbow_step_mm=elbow_step, wrist_step_mm=wrist_step,
                         wrist_step_degrees=wrist_rotation))
        previous = measured, wrist_axes
    peaks = {}
    for metric in ("camera_step_mm", "elbow_step_mm", "wrist_step_mm", "wrist_step_degrees"):
        peak = max(rows[1:], key=lambda row: row[metric])
        peaks[metric] = dict(tick=peak["tick"], value=peak[metric])
    return dict(format="poima.reach-motion.v1", session_id=args.session, tick_rate=60,
                camera=args.camera, shoulder=args.shoulder, elbow=args.elbow, hand=args.hand,
                item=args.item, peaks=peaks, samples=rows)


def read_clearance(args, joined):
    samples = {}
    for line, request, result in guarded_results(args, {"runtime.pose.clearance"}):
        params = request["params"]
        guard = (params["tick"], params["structure_revision"])
        selected = params["segments"], params.get("ignore", []), result["segments"]
        if samples.setdefault(guard, selected) != selected:
            raise ValueError(f"Conflicting clearance sample on line {line}.")

    rows = []
    baseline = previous = None
    previous_structure = None
    for tick, (guard, _, _) in joined.items():
        if guard not in samples:
            raise ValueError(f"No matched clearance observation at tick {tick}.")
        selectors, ignore, observed = samples[guard]
        definitions = [{key: row[key] for key in ("endpoints", "radius")} for row in selectors]
        if baseline is None:
            baseline = definitions, ignore
        if baseline != (definitions, ignore) or not 1 <= len(observed) == len(definitions) <= 16:
            raise ValueError(f"Clearance segment definitions changed or are invalid at tick {tick}.")
        if previous_structure is not None:
            if previous_structure != guard[1]:
                raise ValueError(f"Clearance review crosses a structure revision at tick {tick}.")
            if [row.get("previous") for row in selectors] != previous:
                raise ValueError(f"Clearance does not use the preceding committed points at tick {tick}.")
        blockers = []
        statuses = []
        for index, (selector, row) in enumerate(zip(selectors, observed), 1):
            sweep = row["sweep_blocker"]
            start, end = row["previous_hit"], row["committed_hit"]
            expected = "clear" if "previous" in selector else "endpoint_only"
            if start or end:
                expected = "blocked_endpoint"
            elif sweep:
                expected = "blocked_sweep"
            if row["status"] != expected or ((start or sweep) and "previous" not in selector) or (sweep and (start or end)):
                raise ValueError(f"Inconsistent clearance status at tick {tick}, segment {index}.")
            statuses.append(expected)
            for phase, hit in (("previous", start), ("committed", end), ("sweep", sweep["hit"] if sweep else None)):
                if hit is None:
                    continue
                depth = hit["penetration_depth"]
                if type(depth) not in (int, float) or not math.isfinite(depth) or depth < 0:
                    raise ValueError(f"Invalid clearance depth at tick {tick}, segment {index}.")
                contact = dict(segment=index, phase=phase, obstacle=hit["entity"], penetration_mm=depth * 1000)
                if phase == "sweep":
                    contact.update(interval=sweep["interval"], interval_fraction=hit["fraction"])
                blockers.append(contact)
        rows.append(dict(tick=tick, statuses=statuses, blockers=blockers))
        previous = [row["committed"] for row in observed]
        previous_structure = guard[1]
    return dict(segments=baseline[0], ignore=baseline[1], samples=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("requests", type=Path)
    parser.add_argument("responses", type=Path)
    for field in ("session", "camera", "hand", "item"):
        parser.add_argument("--" + field, required=True)
    parser.add_argument("--finger-drawable")
    parser.add_argument("--reach", action="store_true", help="Pair an arm's pose with same-tick pickup outcomes.")
    parser.add_argument("--clearance", action="store_true", help="Join pose-clearance segment observations to a reach.")
    parser.add_argument("--shoulder")
    parser.add_argument("--elbow")
    parser.add_argument("--first", type=int, required=True)
    parser.add_argument("--last", type=int, required=True)
    parser.add_argument("--contact-mm", type=float)
    args = parser.parse_args()
    if args.reach and (args.finger_drawable is not None or args.contact_mm is not None):
        parser.error("Reach review accepts arm observations and pickup outcomes.")
    if not args.reach and (args.shoulder is not None or args.elbow is not None):
        parser.error("Shoulder and elbow require --reach.")
    if args.clearance and not args.reach:
        parser.error("--clearance requires --reach.")
    selected = (args.shoulder, args.elbow) if args.reach else (args.finger_drawable,)
    identities = (args.session, args.camera, args.hand, args.item, *selected)
    if any(identity is None or re.fullmatch("[0-9a-f]{32}", identity) is None for identity in identities):
        parser.error("Entity and session IDs must be 32 lowercase hexadecimal characters.")
    if args.reach and len({args.camera, args.shoulder, args.elbow, args.hand}) != 4:
        parser.error("A reach requires distinct camera, shoulder, elbow and hand entities.")
    if args.first < 0 or args.last <= args.first:
        parser.error("Supply at least two consecutive ticks.")
    if not args.reach and (args.contact_mm is None or not math.isfinite(args.contact_mm) or args.contact_mm <= 0):
        parser.error("Supply a positive finite contact limit.")
    try:
        if args.reach:
            joined = read_reach(args)
            result = reach_report(args, joined)
            if args.clearance:
                result["clearance"] = read_clearance(args, joined)
        else:
            result = report(args, read_take(args))
    except (AttributeError, KeyError, TypeError, ValueError, OSError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
