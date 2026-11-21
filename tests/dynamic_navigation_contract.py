#!/usr/bin/env python3
"""Guarded native route observations over two physical doors and a saved pose."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/python"))
from poima_client import RpcError, WorldClient


def entity_id(number):
    return f"{number:032x}"


ACTOR = entity_id(1)
DOORS = {-1: entity_id(6), 1: entity_id(7)}
STATIC_BOUNDS = entity_id(8)
GOAL = [9, .2, 0]
ROTATION = [0, 0, 0, 1]


def box(half_extents, motion="static", **options):
    return dict(half_extents=half_extents, motion=motion, mass=1, friction=.5,
                restitution=0, **options)


def set_component(identity, kind, value):
    return dict(op="component.set", id=identity, type=kind, value=value)


def fixture():
    operations = []

    def add(identity, name, position, kind, value):
        operations.extend([
            dict(op="entity.create", id=identity, name=name, parent=None),
            set_component(identity, "Transform", dict(position=position, rotation=ROTATION, scale=[1, 1, 1])),
            set_component(identity, kind, value),
        ])

    add(ACTOR, "Route observer", [-9, .2, 0], "CharacterController",
        dict(radius=.3, height=1.8, speed=4, jump_speed=5, camera=None))
    # Two broad pads joined by two separate, narrow bridge floors.
    for number, name, x, z, half in [
        (2, "West room", -8, 0, [4, .5, 7]),
        (3, "East room", 8, 0, [4, .5, 7]),
        (4, "South passage", 0, -4, [5, .5, 1.4]),
        (5, "North passage", 0, 4, [5, .5, 1.4]),
    ]:
        add(entity_id(number), name, [x, -.5, z], "BoxCollider", box(half))
    for sign, door in DOORS.items():
        add(door, f"Lift gate {sign:+}", [0, 4, sign * 4], "BoxCollider",
            box([.35, .9, 1.4], "kinematic", navigation_obstacle=True))
    add(STATIC_BOUNDS, "Static authored obstacle", [18, 1.25, 18], "BoxCollider",
        box([.5, 1.25, .5], navigation_obstacle=True))
    return operations


def corridor_side(path):
    assert path["status"] == "complete" and path["complete"], path
    corners = [point for point in path["corners"] if abs(point[0]) < 5.5 and abs(point[2]) > 2.5]
    assert corners, path
    side = 1 if corners[0][2] > 0 else -1
    assert all(point[2] * side > 2.5 for point in corners), path
    return side


def run(args, directory, record):
    owner = None
    world = directory / "Gate Room.json"
    save_root = directory / "Saves"
    save_root.mkdir()

    def native(path):
        value = str(path.resolve())
        if args.windows_interop:
            return subprocess.check_output(["wslpath", "-w", value], text=True, timeout=10).strip()
        return value

    def open_owner():
        return WorldClient.open(str(args.binary.resolve()), native(world))

    def close_owner(client):
        client.close()
        exit_code = client.transport.returncode
        record["owner_exit_codes"].append(exit_code)
        assert exit_code == 0, exit_code

    def rpc(method, error=None, **params):
        call = dict(method=method, params=params)
        record["calls"].append(call)
        try:
            result = owner.call(method, params, timeout=30)
        except RpcError as failure:
            call["error"] = dict(code=failure.code, message=failure.message)
            assert failure.code == error, call
            return call["error"]
        call["result"] = result
        assert error is None, call
        return result

    def configure_saves():
        return rpc("save.configure", request_id=uuid.uuid4().hex, expected_generation=0,
                   root=native(save_root))["generation"]

    def query(session, tick, *, error=None, **changes):
        params = dict(session_id=session, tick=tick, structure_revision=0, entity=ACTOR,
                      goal=GOAL, extents=[.4, .55, .4])
        return rpc("runtime.navigation.path", error=error, **(params | changes))

    def step(session, tick, count, **changes):
        result = rpc("runtime.step", request_id=uuid.uuid4().hex, session_id=session,
                     expected_tick=tick, ticks=count, **changes)
        assert result["current_session_id"] == session and result["current_tick"] == tick + count, result
        return tick + count

    def close_gate(session, tick, side):
        return step(session, tick, 24, motions=[dict(entity=DOORS[side], position=[0, .9, side * 4],
                   rotation=ROTATION, duration_ticks=24)])

    def obstacles(result):
        assert [item["entity"] for item in result["obstacles"]] == sorted([*DOORS.values(), STATIC_BOUNDS]), result
        return {item["entity"]: item for item in result["obstacles"]}

    try:
        owner = open_owner()
        capabilities = rpc("world.describe")
        assert capabilities["navigation"]["runtime_obstacles"]["available"], capabilities["navigation"]
        assert "runtime.navigation.path" in capabilities["methods"]
        rpc("world.transact", request_id=uuid.uuid4().hex, base_revision=0, ops=fixture())
        rpc("world.transact", error=-32602, request_id=uuid.uuid4().hex, base_revision=1,
            ops=[set_component(DOORS[1], "BoxCollider", box([.35, .9, 1.4], "dynamic", navigation_obstacle=True))])
        baked = rpc("world.navigation.bake", revision=1)
        assert baked["ignored_moving_colliders"] == 2 and baked["ignored_runtime_obstacles"] == 3, baked
        asset = baked["asset"]
        package = Path(str(world) + ".assets") / (asset + ".pnav")
        assert hashlib.sha256(package.read_bytes()).hexdigest() == asset
        # An explicit false must give the same static package as omission.
        rpc("world.transact", request_id=uuid.uuid4().hex, base_revision=1,
            ops=[set_component(entity_id(2), "BoxCollider", box([4, .5, 7], navigation_obstacle=False))])
        rebaked = rpc("world.navigation.bake", revision=2)
        assert rebaked["reused"] and rebaked["asset"] == asset, rebaked
        rpc("world.transact", request_id=uuid.uuid4().hex, base_revision=2,
            ops=[dict(op="navigation.set", asset=asset)])
        authored = world.read_bytes()
        static_params = dict(revision=3, asset=asset, start=[-9, .2, 0], end=GOAL, extents=[.4, .55, .4])
        authored_route = rpc("world.navigation.path", **static_params)
        primary = corridor_side(authored_route["path"])

        session = uuid.uuid4().hex
        rpc("runtime.start", session_id=session, revision=3)
        tick = step(session, 0, 12)
        initial = query(session, tick)
        assert corridor_side(initial["path"]) == primary and initial["asset"] == asset, initial
        assert not initial["obstacle_filtered"] and not initial["blocked"], initial
        assert obstacles(initial)[DOORS[primary]]["minimum"][1] > 3, initial
        assert all(value > 17 for value in (obstacles(initial)[STATIC_BOUNDS]["minimum"][0],
                                           obstacles(initial)[STATIC_BOUNDS]["minimum"][2])), initial

        tick = close_gate(session, tick, primary)
        query(session, tick - 1, error=-32009)
        query(session, tick, error=-32009, structure_revision=1)
        query(uuid.uuid4().hex, tick, error=-32030)
        detour = query(session, tick)
        assert detour == query(session, tick)
        assert corridor_side(detour["path"]) == -primary and not detour["blocked"], detour
        assert detour["obstacle_filtered"] and obstacles(detour)[DOORS[primary]]["maximum"][1] < 2, detour
        # Projected endpoints should remain at their static landing positions.
        for key in ("projected_start", "projected_end"):
            assert detour["path"][key] == initial["path"][key], detour
        assert rpc("world.navigation.path", **static_params) == authored_route

        generation = configure_saves()
        rpc("save.write", request_id=uuid.uuid4().hex, configuration_generation=generation, slot="closed-gate",
            expected_generation=0, session_id=session, expected_tick=tick, expected_gameplay_revision=0)
        tick = close_gate(session, tick, -primary)
        closed = query(session, tick)
        assert closed["blocked"] and closed["obstacle_filtered"] and not closed["path"]["complete"], closed
        assert closed["path"]["status"] in ("partial", "unreachable"), closed
        assert all(obstacles(closed)[door]["maximum"][1] < 2 for door in DOORS.values()), closed

        close_owner(owner)
        owner = None
        owner = open_owner()
        generation = configure_saves()
        restored_session = uuid.uuid4().hex
        saved = rpc("save.load", request_id=uuid.uuid4().hex, configuration_generation=generation, slot="closed-gate",
                    expected_generation=1, revision=3, expected_session_id=None, expected_tick=None,
                    expected_gameplay_revision=None, new_session_id=restored_session)
        assert saved["tick"] == detour["tick"], saved
        assert query(restored_session, saved["tick"]) == (detour | {"session_id": restored_session})
        assert world.read_bytes() == authored
        assert hashlib.sha256(package.read_bytes()).hexdigest() == asset
    finally:
        if owner is not None:
            close_owner(owner)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="Existing directory for temporary test files")
    parser.add_argument("--windows-interop", action="store_true")
    parser.add_argument("--evidence", type=Path, help="Write original native calls and results as JSON")
    args = parser.parse_args()
    record = dict(calls=[], owner_exit_codes=[], passed=False)
    try:
        with tempfile.TemporaryDirectory(prefix="poima-dynamic-navigation-", dir=args.output) as scratch:
            run(args, Path(scratch), record)
        record["passed"] = True
    finally:
        if args.evidence:
            args.evidence.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(f"Verified dynamic navigation through {len(record['calls'])} native requests and two clean owners.")


if __name__ == "__main__":
    main()
