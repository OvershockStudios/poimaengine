#!/usr/bin/env python3
"""Negative and additive-input tests for the deliberately conservative schema gate."""
# SPDX-License-Identifier: Apache-2.0
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from check_authoring_api import FORMAT, MAX_FINDINGS, MAX_INPUT_BYTES, compare_discovery, schema_compare


def closed(properties, required=()):
    return dict(type="object", properties=properties, required=list(required), additionalProperties=False)


def branch(op, **properties):
    return closed(dict(op=dict(const=op), **properties), ["op", *properties])


class Compatibility(unittest.TestCase):
    def fails(self, old, new, contains=None):
        findings = schema_compare(old, new)
        self.assertTrue(findings, (old, new))
        if contains:
            self.assertTrue(any(contains in f["reason"] for f in findings), findings)
        self.assertTrue(all(f["path"].startswith("$") for f in findings))

    def test_required_removed_property_and_types(self):
        original = closed(dict(name=dict(type="string"), enabled=dict(type="boolean")), ["name"])
        changed = copy.deepcopy(original); del changed["properties"]["name"]
        self.fails(original, changed, "property was removed")
        changed = copy.deepcopy(original); changed["required"].append("enabled")
        self.fails(original, changed, "requires fields")
        self.fails(dict(type=["string", "null"]), dict(type="string"), "baseline type")
        self.fails(dict(type="string"), {}, "type information")
        self.fails(dict(type="string"), True, "type information")
        self.fails({}, dict(type="string"), "introduced")
        self.fails(dict(type="string"), dict(type="integer"), "baseline type")
        self.assertEqual(schema_compare(dict(type="integer"), dict(type="number")), [])
        self.assertEqual(schema_compare(original, dict(original, required=[])), [])

    def test_literals_defaults_and_annotations_are_not_data(self):
        self.fails(dict(enum=["a", "b"]), dict(enum=["a"]), "enum value")
        self.fails(dict(enum=[True]), dict(enum=[1]), "enum value")
        self.fails(dict(const="a"), dict(const="b"), "literal")
        self.assertEqual(schema_compare(dict(enum=["a"]), dict(enum=["b", "a"])), [])
        for old, new in ((dict(default=3), dict(default=4)), (dict(default=3), {}), ({}, dict(default=3)),
                         (dict(default=3), True),
                         (dict(default=True), dict(default=1)),
                         (dict(default=dict(description="old")), dict(default=dict(description="new")))):
            with self.subTest(old=old, new=new): self.fails(old, new, "Default")
        self.fails(dict(enum=[dict(title="old")]), dict(enum=[dict(title="new")]), "enum value")
        old = closed(dict(name=dict(type="string", description="Old")))
        new = copy.deepcopy(old); new["description"] = "New docs"; new["title"] = "Title"; new["$comment"] = "Comment"
        new["properties"]["name"]["description"] = "Updated field docs"
        self.assertEqual(schema_compare(old, new), [])

    def test_bounds_including_exclusive_cross_keyword_changes(self):
        for key, old, tighter, looser in (("minLength", 2, 3, 1), ("maxLength", 8, 7, 9),
                                          ("minItems", 2, 3, 1), ("maxItems", 8, 7, 9),
                                          ("minProperties", 2, 3, 1), ("maxProperties", 8, 7, 9),
                                          ("minimum", 2, 3, 1), ("maximum", 8, 7, 9)):
            with self.subTest(key=key):
                self.fails({key: old}, {key: tighter}, "bound")
                self.fails({}, {key: tighter}, "bound")
                self.assertEqual(schema_compare({key: old}, {key: looser}), [])
        self.fails(dict(minimum=2), dict(exclusiveMinimum=2), "lower")
        self.fails(dict(maximum=8), dict(exclusiveMaximum=8), "upper")
        self.assertEqual(schema_compare(dict(exclusiveMinimum=2), dict(minimum=2)), [])
        self.assertEqual(schema_compare(dict(exclusiveMaximum=8), dict(maximum=8)), [])
        self.fails(dict(minimum=0, exclusiveMinimum=-1), dict(minimum=-1, exclusiveMinimum=0), "lower")
        self.fails(dict(minItems=1), dict(minItems=True), "Invalid")

    def test_optional_additions_require_previously_closed_name_space(self):
        old = closed(dict(name=dict(type="string")), ["name"])
        new = copy.deepcopy(old); new["properties"]["optional"] = dict(type="boolean", default=False)
        self.assertEqual(schema_compare(old, new), [])
        self.fails(dict(type="object", properties={}), dict(type="object", properties=dict(optional=dict(type="boolean"))), "arbitrary")
        self.fails(dict(additionalProperties=True), dict(additionalProperties=False), "additional properties")
        self.assertEqual(schema_compare(dict(additionalProperties=False), dict(additionalProperties=True)), [])
        self.fails(dict(type="array", items=dict(type="string")), dict(type="array", items=dict(type="integer")), "baseline type")

    def test_changed_unknown_constraints_fail_closed(self):
        self.fails(dict(pattern="^a$"), dict(pattern=".*"), "Unsupported")
        self.fails(dict(uniqueItems=False), dict(uniqueItems=True), "Unsupported")
        self.fails(dict(not_=False), dict(not_=True), "Unsupported")
        old = closed(dict(name=dict(type="string"))); old["not"] = dict(const={}, description="Old docs")
        new = copy.deepcopy(old); new["properties"]["extra"] = dict(type="boolean"); new["not"]["description"] = "New docs"
        self.assertEqual(schema_compare(old, new), [])
        self.fails(dict(allOf=[dict(type="string")]), dict(allOf=[dict(type="string", minLength=2)]), "Unsupported")

    def test_unchanged_references_cannot_hide_changed_negated_constraints(self):
        # Local integer -> number widening looks safe, but the unchanged `not`
        # references it. Value 1.5 was accepted before and rejected afterward.
        for reference in ("$ref", "$dynamicRef", "$recursiveRef"):
            old = dict(properties=dict(x=dict(type="integer")),
                       **{"not": {reference: "#/properties/x"}})
            new = copy.deepcopy(old); new["properties"]["x"]["type"] = "number"
            self.fails(old, new, "references")
            cosmetic = copy.deepcopy(old); cosmetic["description"] = "Updated docs"
            self.assertEqual(schema_compare(old, cosmetic), [])

    def test_exclusive_branch_addition_requires_disjoint_required_consts(self):
        old = dict(oneOf=[branch("create", name=dict(type="string")), branch("delete", id=dict(type="string"))])
        new = copy.deepcopy(old); new["oneOf"].append(branch("rename", id=dict(type="string"), name=dict(type="string")))
        self.assertEqual(schema_compare(old, new), [])
        new["oneOf"].reverse(); self.assertEqual(schema_compare(old, new), [])
        unsafe = copy.deepcopy(old); unsafe["oneOf"].append(branch("create", another=dict(type="string")))
        self.fails(old, unsafe, "disjoint")
        unsafe = copy.deepcopy(old); unsafe["oneOf"].append(True)
        self.fails(old, unsafe, "discriminators")
        unsafe = copy.deepcopy(old); unsafe["oneOf"][0]["required"].remove("op")
        self.fails(old, unsafe, "discriminators")
        unsafe = copy.deepcopy(old); unsafe["oneOf"] = unsafe["oneOf"][1:]
        self.fails(old, unsafe, "removed")
        unsafe = copy.deepcopy(old); unsafe["oneOf"][0]["properties"]["name"]["minLength"] = 2
        self.fails(old, unsafe, "bound")
        plain = dict(oneOf=[dict(type="integer"), dict(type="string")])
        self.fails(plain, dict(oneOf=[dict(type="number"), dict(type="string")]), "discriminators")
        self.fails(plain, dict(oneOf=[dict(type="integer"), dict(type="string"), dict(type="number")]), "discriminators")

    def test_composite_const_discriminators_and_unprovable_custom_types(self):
        old = dict(oneOf=[branch("create"), branch("component.set", type=dict(const="Transform"))])
        new = copy.deepcopy(old); new["oneOf"].append(branch("component.set", type=dict(const="Camera")))
        self.assertEqual(schema_compare(old, new), [])
        new["oneOf"].append(branch("component.set", type=dict(type="string", pattern="^game:")))
        self.fails(old, new, "disjoint")

    def test_union_coverage_does_not_require_unrelated_builtins(self):
        old = dict(type="string", enum=["Transform"])
        new = dict(anyOf=[dict(type="string", enum=["Transform", "Camera"]), dict(type="string", pattern="^game:")])
        self.assertEqual(schema_compare(old, new), [])
        self.fails(old, dict(anyOf=[dict(type="string", enum=["Camera"]), dict(type="string", pattern="^game:")]), "No candidate")
        self.fails(dict(type="string"), dict(anyOf=[dict(type="string", enum=["a"]), dict(type="string", pattern="^b")]), "No candidate")
        old_union = dict(anyOf=[dict(type="string"), dict(type="null")])
        self.assertEqual(schema_compare(old_union, dict(anyOf=[dict(type="null"), dict(type="string"), dict(type="integer")])), [])
        # Identical outer/branch constraints remain mergeable despite docs edits.
        outer_items = dict(type="string", description="Outer docs")
        branch_items = dict(type="string", description="Branch docs")
        original = dict(type="array", items=outer_items,
                        anyOf=[dict(type="array", items=branch_items)])
        expanded = copy.deepcopy(original); expanded["anyOf"].append(dict(type="array", items=branch_items))
        self.assertEqual(schema_compare(original, expanded), [])

    def test_discovery_merge_removed_names_and_bounded_findings(self):
        schema = closed(dict(value=dict(type="string")), ["value"])
        baseline = dict(format=FORMAT, status="candidate", methods={"entity.get": schema}, components={"Transform": schema})
        candidate = dict(methods={"entity.get": schema}, components={"Transform": schema})
        self.assertEqual(compare_discovery(baseline, candidate), [])
        self.assertEqual(compare_discovery(baseline, [dict(result=dict(methods=candidate["methods"])), dict(components=candidate["components"])]), [])
        for missing in (dict(methods={}), dict(methods=candidate["methods"])):
            self.assertTrue(compare_discovery(baseline, missing))
        with self.assertRaises(ValueError):
            compare_discovery(baseline, [candidate, dict(methods={"entity.get": {}})])
        with self.assertRaises(ValueError): compare_discovery(dict(format=FORMAT), candidate)
        old = closed({"property" + str(i): dict(type="string") for i in range(300)})
        findings = schema_compare(old, closed({}))
        self.assertEqual(len(findings), MAX_FINDINGS + 1)
        self.assertIn("omitted", findings[-1]["reason"])

    def test_cli_exit_codes_and_strict_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); baseline = root / "baseline.json"; candidate = root / "candidate.json"; report = root / "report.json"
            baseline.write_text(json.dumps(dict(format=FORMAT, methods={"test": closed({})})), encoding="utf-8")
            candidate.write_text(json.dumps(dict(methods={"test": closed({})})), encoding="utf-8")
            command = [sys.executable, str(ROOT / "scripts/check_authoring_api.py"), "--baseline", str(baseline), "--candidate", str(candidate), "--report", str(report)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertTrue(json.loads(report.read_text())["passed"])
            candidate.write_text('{"methods": {}}', encoding="utf-8")
            self.assertEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 1)
            for invalid in ('{"methods":{},"methods":{}}', '{"methods": {"test": {"default": NaN}}}', '{"methods": {"test": {"default": 1e999}}}'):
                candidate.write_text(invalid, encoding="utf-8")
                self.assertEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 2)
            candidate.write_bytes(b" " * (MAX_INPUT_BYTES + 1))
            oversized = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertEqual(oversized.returncode, 2, oversized.stderr + oversized.stdout)
            self.assertIn("Input exceeds 8 MiB", json.loads(oversized.stdout)["error"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
