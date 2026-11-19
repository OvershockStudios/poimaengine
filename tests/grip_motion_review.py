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
SHOULDER, ELBOW = (f"{n:032x}" for n in (6, 7))


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
    def run_cli(self, requests, responses, *, reach=False):
        with TemporaryDirectory() as temporary:
            inputs = [Path(temporary) / name for name in ("requests.jsonl", "responses.jsonl")]
            for path, messages in zip(inputs, (requests, responses)):
                path.write_text("".join(json.dumps(row) + "\n" for row in messages), encoding="utf-8")
            options = (["--reach", "--shoulder", SHOULDER, "--elbow", ELBOW] if reach else
                       ["--finger-drawable", SKIN, "--contact-mm", "5"])
            return subprocess.run([sys.executable, str(ROOT / "scripts/review_grip_motion.py"), *map(str, inputs),
                "--session", SESSION, "--camera", CAMERA, "--hand", HAND, "--item", ITEM,
                "--first", "12", "--last", "14", *options], text=True, capture_output=True, timeout=15)

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

    def reach_samples(self):
        requests, responses = samples()
        for index in (0, 2, 4):
            observe = requests[index]["params"]
            observe["ids"].extend([SHOULDER, ELBOW])
            poses = responses[index]["result"]["entities"]
            wrist = poses[1]["state"]["world_matrix"]
            for joint, height in ((SHOULDER, .5), (ELBOW, .2)):
                basis = wrist.copy()
                basis[13] += height
                poses.append(dict(id=joint, state=dict(world_matrix=basis)))
            receipt = requests[index + 1]
            receipt["method"] = "runtime.attachment.outcomes"
            del receipt["params"]["pairs"]
            row = responses[index + 1]["result"]
            del row["authored_revision"]
            del row["pairs"]
            row["outcomes"] = [dict(entity=ITEM, status="reach_blocked", pose="animation")] if index == 4 else []
        return requests, responses

    def test_reach_measures_arm_without_including_camera_turn(self):
        attempt = self.run_cli(*self.reach_samples(), reach=True)
        self.assertEqual(attempt.returncode, 0, attempt.stderr)
        result = json.loads(attempt.stdout)
        self.assertEqual(result["format"], "poima.reach-motion.v1")
        first, moved, blocked = result["samples"]
        self.assertIsNone(first["wrist_step_mm"])
        self.assertIsNone(moved["outcome"])
        self.assertEqual(moved["camera_step_mm"], 1000)
        self.assertAlmostEqual(moved["elbow_step_mm"], 0)
        self.assertAlmostEqual(moved["wrist_step_degrees"], 0)
        self.assertAlmostEqual(blocked["wrist_step_degrees"], 30)
        self.assertAlmostEqual(blocked["wrist_step_mm"], 25)
        self.assertAlmostEqual(blocked["forearm_mm"], 200)
        self.assertAlmostEqual(blocked["upper_arm_mm"], 300)
        self.assertEqual(blocked["outcome"]["pose"], "animation")
        self.assertEqual(result["peaks"]["elbow_step_mm"]["tick"], 14)

    def test_reach_rejects_missing_receipt_at_the_observed_structure(self):
        requests, responses = self.reach_samples()
        requests[3]["params"]["structure_revision"] = responses[3]["result"]["structure_revision"] = 4
        attempt = self.run_cli(requests, responses, reach=True)
        self.assertNotEqual(attempt.returncode, 0)
        self.assertIn("pickup outcome observations at ticks [13]", attempt.stderr)


if __name__ == "__main__":
    unittest.main()
