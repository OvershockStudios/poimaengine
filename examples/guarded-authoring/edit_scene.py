#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the guarded-authoring playbook against a fresh native world."""
import argparse
import json
from pathlib import Path

from poima_client import RpcError, WorldClient, new_id


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(binary, world):
    require(binary.is_file(), "Build the native engine first.")
    require(not world.exists(), "Use a new world path; this example creates content.")
    world.parent.mkdir(parents=True, exist_ok=True)
    room, prop, receipt = new_id(), new_id(), new_id()
    operations = [
        {"op": "entity.create", "id": room, "name": "Workshop"},
        {"op": "entity.create", "id": prop, "parent": room, "name": "Crate"},
    ]
    with WorldClient.open(binary, world) as engine:
        engine.discover_mutation("entity.create")
        engine.discover_mutation("component.set", "Transform")
        initial = engine.inspect()["revision"]
        preview = engine.transact(operations, initial, request_id=receipt, preview=True)
        require(preview["committed"] is False, "Preview unexpectedly committed.")
        require(engine.inspect()["revision"] == initial, "Preview changed the world.")
        created = engine.transact(operations, initial, request_id=receipt)
        base = created["revision"]

        # Simulate an intervening edit through the same owner. Shared clients
        # use these same guards; this example does not launch a second client.
        renamed = engine.transact(
            [{"op": "entity.rename", "id": prop, "name": "Supply crate"}], base)
        state = engine.inspect()
        stale_operations = [{
            "op": "component.set", "id": prop, "type": "Transform",
            "value": {"position": [2, 0, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]},
        }]
        try:
            engine.transact(stale_operations, base)
        except RpcError as error:
            require(error.code == -32009, "Expected the stale-revision error.")
        else:
            raise RuntimeError("Stale edit unexpectedly committed.")
        require(engine.inspect() == state, "Rejected edit changed the world.")

        # This terminal conflict is known not to have committed. Inspect the
        # target and deliberately accept the earlier rename before a new edit.
        require(engine.get(prop, revision=renamed["revision"])["value"]["name"] == "Supply crate",
                "The intervening name change was lost.")
        moved = engine.transact(stale_operations, renamed["revision"])
        final = moved["revision"]
        replayed = engine.transact(operations, initial, request_id=receipt)
        require(replayed["replayed"] is True, "Retained receipt did not replay.")
        require(replayed["revision"] == base, "Replay did not return its original revision.")
        require(engine.inspect()["revision"] == final, "Replay changed the current revision.")
        expected = engine.get(prop, revision=final)
        require(expected["value"]["parent"] == room, "The hierarchy was lost.")
        require(engine.get(prop, revision=final, component="Transform")["value"]["position"] == [2, 0, 0],
                "The reconciled Transform was not applied.")
    require(engine.transport.returncode == 0, "The first owner did not exit cleanly.")

    with WorldClient.open(binary, world) as engine:
        require(engine.inspect()["revision"] == final, "Reopen changed the revision.")
        require(engine.get(prop, revision=final) == expected, "Reopen changed authored content.")
        require(engine.transact(operations, initial, request_id=receipt)["replayed"] is True,
                "The retained receipt did not survive reopening.")
        require(engine.inspect()["revision"] == final, "Reopened replay changed the world.")
    require(engine.transport.returncode == 0, "The reopened owner did not exit cleanly.")
    print(json.dumps({"passed": True, "revision": final, "entities": [room, prop],
                      "owners_exit_codes": [0, 0],
                      "checks": ["preview", "atomic creation", "stale rejection",
                                 "deliberate reconciliation", "receipt replay", "fresh-owner persistence"]}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("world", type=Path)
    args = parser.parse_args()
    run(args.binary.resolve(), args.world.resolve())
