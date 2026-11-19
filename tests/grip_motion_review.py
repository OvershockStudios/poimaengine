#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the grip review CLI with known camera/hand motion and stale logs."""

import json
import math
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest

ROOT = Path(__file__).resolve().parents[1]
SESSION, CAMERA, HAND, ITEM, SKIN = (f"{n:032x}" for n in range(1, 6))


def matrix(position, degrees):
    angle = math.radians(degrees)
    c, s = math.cos(angle), math.sin(angle)
    return [c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, *position, 1]


def samples():
    requests, responses = [], []
    for tick, travel, camera_yaw, lateral, hand_yaw, distance in (
        (12, 0, 0, 0, 0, .002),
        (13, 1, 90, 0, 0, .003),
        (14, 1, 90, .025, 30, .006),
    ):
        guard = dict(session_id=SESSION, tick=tick, structure_revision=3)
        camera = matrix((travel, 0, 0), camera_yaw)
        cos = math.cos(math.radians(camera_yaw))
        sin = math.sin(math.radians(camera_yaw))
        hand = matrix((travel + lateral*cos, 0, -lateral*sin), camera_yaw + hand_yaw)
        item = matrix((hand[12], .1, hand[14]), camera_yaw + hand_yaw)
        ids = [CAMERA, HAND, ITEM]
        poses = [dict(id=key, state=dict(world_matrix=value)) for key, value in zip(ids, [camera, hand, item])]
        selector = dict(source=dict(entity=SKIN, vertex=7), target=dict(entity=ITEM))
        for method, params, result in (
            ("runtime.observe", dict(ids=ids, entity_fields=["world_matrix"]), dict(entities=poses)),
            ("runtime.surface.measure", dict(pairs=[selector]), dict(pairs=[
                dict(source=dict(entity=SKIN), target=dict(entity=ITEM), distance_m=distance)])),
        ):
            number = len(requests) + 1
            requests.append(dict(jsonrpc="2.0", id=number, method=method, params=dict(guard, **params)))
            responses.append(dict(jsonrpc="2.0", id=number, result=dict(guard, authored_revision=1, **result)))
    return requests, responses


class GripReview(unittest.TestCase):
    def run_cli(self, requests, responses):
        with TemporaryDirectory() as temporary:
            inputs = [Path(temporary) / name for name in ("requests.jsonl", "responses.jsonl")]
            for path, messages in zip(inputs, (requests, responses)):
                path.write_text("".join(json.dumps(row) + "\n" for row in messages), encoding="utf-8")
            return subprocess.run([sys.executable, str(ROOT / "scripts/review_grip_motion.py"), *map(str, inputs),
                "--session", SESSION, "--camera", CAMERA, "--hand", HAND, "--item", ITEM, "--finger-drawable", SKIN,
                "--first", "12", "--last", "14", "--contact-mm", "5"], text=True, capture_output=True, timeout=15)

    def test_camera_motion_is_removed_from_hand_step(self):
        attempt = self.run_cli(*samples())
        self.assertEqual(attempt.returncode, 0, attempt.stderr)
        result = json.loads(attempt.stdout)
        self.assertEqual(result["camera_travel_m"], 1)
        self.assertEqual(result["ticks_outside_contact_limit"], [14])
        self.assertEqual(result["samples"][1]["hand_step_mm"], 0)
        self.assertAlmostEqual(result["samples"][1]["hand_step_degrees"], 0)
        self.assertAlmostEqual(result["peak_hand_step"]["distance_mm"], 25)
        self.assertAlmostEqual(result["peak_hand_rotation"]["degrees"], 30)
        self.assertAlmostEqual(result["samples"][2]["grip_drift_mm"], 0)

    def test_same_tick_different_revisions_are_not_joined(self):
        requests, responses = samples()
        requests[3]["params"]["structure_revision"] = responses[3]["result"]["structure_revision"] = 4
        attempt = self.run_cli(requests, responses)
        self.assertNotEqual(attempt.returncode, 0)
        self.assertIn("ticks [13]", attempt.stderr)

    def test_probe_definitions_remain_fixed(self):
        requests, responses = samples()
        requests[3]["params"]["pairs"][0]["source"]["vertex"] = 8
        attempt = self.run_cli(requests, responses)
        self.assertNotEqual(attempt.returncode, 0)
        self.assertIn("probe definitions changed at tick 13", attempt.stderr)


if __name__ == "__main__":
    unittest.main()
