#!/usr/bin/env python3
"""Conservative compatibility gate for selected native authoring input schemas.

This is deliberately not general JSON Schema subsumption. It does not verify
results, errors, retry behavior, runtime defaults, binary ABI, or performance.
Changed unsupported constraints and unprovable unions fail closed. Behavioral
qualification is still required before any API stability claim.
"""
# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

FORMAT = "poima.authoring-api-baseline.v1"
COSMETIC = {"description", "title", "$comment"}
MAX_FINDINGS = 128
MAX_INPUT_BYTES = 8 * 1024 * 1024
TYPES = {"null", "boolean", "integer", "number", "string", "object", "array"}
BOUNDS = {"minLength": True, "maxLength": False, "minItems": True,
          "maxItems": False, "minProperties": True, "maxProperties": False}
NUMERIC = {"minimum", "exclusiveMinimum", "maximum", "exclusiveMaximum"}
SUPPORTED = {"type", "properties", "required", "additionalProperties", "items",
             "enum", "const", "default", "oneOf", "anyOf", *BOUNDS, *NUMERIC}
MISSING = object()


def _equal(left: Any, right: Any) -> bool:
    """JSON literals retain type, including bool versus integer and defaults."""
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(_equal(left[k], right[k]) for k in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(_equal(a, b) for a, b in zip(left, right))
    return left == right


def _clean(schema: Any) -> Any:
    """Ignore annotations only in schema positions, never enum/default data."""
    if not isinstance(schema, dict):
        return schema
    out = {k: v for k, v in schema.items() if k not in COSMETIC}
    for key in ("properties", "patternProperties", "$defs", "definitions"):
        if isinstance(out.get(key), dict):
            out[key] = {k: _clean(v) for k, v in out[key].items()}
    for key in ("oneOf", "anyOf", "allOf", "prefixItems"):
        if isinstance(out.get(key), list):
            out[key] = [_clean(v) for v in out[key]]
    for key in ("items", "additionalProperties", "not", "if", "then", "else", "contains"):
        if isinstance(out.get(key), (dict, bool)):
            out[key] = _clean(out[key])
    return out


def _path(path: str, key: str) -> str:
    return path + "[" + json.dumps(key, ensure_ascii=True) + "]"


class _Findings:
    def __init__(self) -> None:
        self.values: list[dict[str, str]] = []
        self.omitted = False

    def add(self, path: str, reason: str) -> None:
        if len(self.values) < MAX_FINDINGS:
            self.values.append({"path": path[:1024], "reason": reason})
        elif not self.omitted:
            self.omitted = True
            self.values.append({"path": "$", "reason": "Additional findings omitted by bounded report."})


def _types(value: Any) -> set[str] | None:
    if isinstance(value, str):
        values = [value]
    elif isinstance(value, list):
        values = value
    else:
        return None
    if not values or any(not isinstance(v, str) or v not in TYPES for v in values):
        return None
    return set(values)


def _number(value: Any) -> bool:
    return type(value) is int or (type(value) is float and math.isfinite(value))


def _keyword_equal(key: str, left: Any, right: Any) -> bool:
    if key in ("items", "additionalProperties", "not", "if", "then", "else", "contains", "propertyNames", "unevaluatedProperties", "unevaluatedItems"):
        return _equal(_clean(left), _clean(right))
    if key in ("oneOf", "anyOf", "allOf", "prefixItems") and isinstance(left, list) and isinstance(right, list):
        return _equal([_clean(s) for s in left], [_clean(s) for s in right])
    if key in ("properties", "$defs", "definitions", "patternProperties") and isinstance(left, dict) and isinstance(right, dict):
        return _equal({k: _clean(s) for k, s in left.items()}, {k: _clean(s) for k, s in right.items()})
    return _equal(left, right)


def _contains_reference(value: Any) -> bool:
    """References can observe changes outside the recursively compared node.

    Scan conservatively, including opaque keyword data. A reference-like literal
    can therefore require review too; resolving schema/resource scopes is outside
    this gate's contract.
    """
    pending = [value]
    while pending:
        node = pending.pop()
        if isinstance(node, dict):
            if any(key in node for key in ("$ref", "$dynamicRef", "$recursiveRef")):
                return True
            pending.extend(node.values())
        elif isinstance(node, list):
            pending.extend(node)
    return False


def _effective_bound(schema: dict, lower: bool) -> tuple[float | int, bool] | None:
    inclusive, exclusive = ("minimum", "exclusiveMinimum") if lower else ("maximum", "exclusiveMaximum")
    values = [(schema[k], k == exclusive) for k in (inclusive, exclusive) if k in schema]
    if not values:
        return None
    selected = max(v for v, _ in values) if lower else min(v for v, _ in values)
    return selected, any(is_exclusive for v, is_exclusive in values if v == selected)


def _discriminators(branch: Any) -> dict[str, Any] | None:
    if not isinstance(branch, dict) or branch.get("type") != "object":
        return None
    props, required = branch.get("properties"), branch.get("required", [])
    if not isinstance(props, dict) or not isinstance(required, list):
        return None
    values = {}
    for name in ("op", "kind", "type"):
        prop = props.get(name)
        if name in required and isinstance(prop, dict) and "const" in prop:
            value = prop["const"]
            if isinstance(value, (str, int, bool)) or value is None:
                values[name] = value
    return values or None


def _disjoint(left: dict, right: dict) -> bool:
    return any(name in right and not _equal(value, right[name]) for name, value in left.items())


def _object_branch(branch: Any, closed: bool) -> tuple[dict, set[str]] | None:
    """Only direct, reference-free object constraints can witness exclusion.

    In particular patternProperties can admit names absent from properties, and
    references/resource scopes are intentionally not resolved by this checker.
    """
    if not isinstance(branch, dict) or branch.get("type") != "object":
        return None
    if _contains_reference(branch) or set(branch) - SUPPORTED - COSMETIC:
        return None
    if closed and branch.get("additionalProperties") is not False:
        return None
    properties, required = branch.get("properties", {}), branch.get("required", [])
    if not isinstance(properties, dict) or any(not isinstance(k, str) for k in properties):
        return None
    if not isinstance(required, list) or any(not isinstance(k, str) for k in required):
        return None
    if len(required) != len(set(required)) or not set(required).issubset(properties):
        return None
    return properties, set(required)


def _closed_object_extension(before: list, after: list) -> bool:
    """Preserve old oneOf acceptances using a forbidden-property witness.

    Retain every old closed branch unchanged (including multiplicity). Each
    added object must directly require a property forbidden by *each* old
    branch. It therefore cannot add a second match for any old accepted value.
    New branches may overlap one another: this proves only old acceptance, not
    acceptance of new caller shapes. Arbitrary union/branch rewrites are refused.
    """
    old_shapes = [_object_branch(branch, closed=True) for branch in before]
    if any(shape is None for shape in old_shapes):
        return False
    remaining = list(after)
    for branch in before:
        match = next((i for i, candidate in enumerate(remaining)
                      if _equal(_clean(branch), _clean(candidate))), None)
        if match is None:
            return False
        del remaining[match]
    if not remaining:
        return True
    for branch in remaining:
        shape = _object_branch(branch, closed=False)
        if shape is None:
            return False
        _, required = shape
        if not required or any(not required.difference(old_properties)
                               for old_properties, _ in old_shapes):
            return False
    return True


def _merge_schema(outer: dict, branch: Any) -> dict | bool | None:
    """Only combine syntactically nonconflicting constraints; no allOf solver."""
    if branch is False:
        return False
    if branch is True:
        return outer
    if not isinstance(branch, dict):
        return None
    merged = dict(outer)
    for key, value in branch.items():
        if key in COSMETIC:
            continue
        if key in merged and not _keyword_equal(key, merged[key], value):
            return None
        merged[key] = value
    return merged


def _compatible(old: Any, new: Any, depth: int) -> bool:
    found = _Findings()
    _compare(old, new, "$", found, depth)
    return not found.values


def _compare(old: Any, new: Any, path: str, found: _Findings, depth: int = 0) -> None:
    if depth > 64:
        found.add(path, "Schema nesting exceeds conservative comparison limit.")
        return
    if not isinstance(old, (dict, bool)) or not isinstance(new, (dict, bool)):
        found.add(path, "Schema must be an object or boolean.")
        return
    if _equal(_clean(old), _clean(new)):
        return
    if depth == 0 and (_contains_reference(old) or _contains_reference(new)):
        found.add(path, "Changed schema contains references; referenced constraint preservation is unproven.")
        return
    if old is False:
        return
    if new is True and isinstance(old, dict):
        # Acceptances widen, but discarding explicit field/type/default metadata
        # still breaks the selected authoring contract this gate preserves.
        _compare(old, {}, path, found, depth + 1)
        return
    if new is True:
        return
    if new is False or old is True:
        if old is True and isinstance(new, dict) and not _clean(new):
            return
        found.add(path, "An unconstrained/accepted schema became restricted or impossible.")
        return
    assert isinstance(old, dict) and isinstance(new, dict)

    # Defaults are metadata with observable authoring behavior, not constraints.
    if not _equal(old.get("default", MISSING), new.get("default", MISSING)):
        found.add(_path(path, "default"), "Default was added, removed, or changed.")

    if "anyOf" in new:
        branches = new["anyOf"]
        old_branches = old.get("anyOf", [old])
        if not isinstance(branches, list) or not branches or not isinstance(old_branches, list) or not old_branches:
            found.add(_path(path, "anyOf"), "Invalid union schema.")
            return
        outer_new = {k: v for k, v in new.items() if k not in COSMETIC | {"anyOf"}}
        outer_old = {k: v for k, v in old.items() if k not in COSMETIC | {"anyOf"}} if "anyOf" in old else {}
        for index, branch in enumerate(old_branches):
            target = _merge_schema(outer_old, branch)
            candidates = [_merge_schema(outer_new, b) for b in branches]
            if target is None or not any(c is not None and _compatible(target, c, depth + 1) for c in candidates):
                found.add(_path(path, "anyOf") + f"[{index}]", "No candidate union branch provably preserves the complete baseline branch.")
        return
    if "anyOf" in old:
        found.add(_path(path, "anyOf"), "Union was removed/replaced; preservation is unproven.")
        return

    for key in sorted((old.keys() | new.keys()) - SUPPORTED - COSMETIC):
        if not _keyword_equal(key, old.get(key, MISSING), new.get(key, MISSING)):
            found.add(_path(path, key), "Unsupported keyword changed; compatibility is unproven.")

    old_types = _types(old["type"]) if "type" in old else None
    new_types = _types(new["type"]) if "type" in new else None
    if ("type" in old and old_types is None) or ("type" in new and new_types is None):
        found.add(_path(path, "type"), "Invalid type declaration.")
    elif old_types is None and new_types is not None:
        found.add(_path(path, "type"), "Type constraint was introduced for previously unconstrained values.")
    elif old_types is not None and new_types is None:
        found.add(_path(path, "type"), "Baseline type information was removed.")
    elif old_types is not None and new_types is not None:
        removed = old_types - new_types
        if "number" in new_types:
            removed.discard("integer")
        if removed:
            found.add(_path(path, "type"), "Candidate no longer accepts every baseline type.")

    for key in ("enum", "const"):
        if key not in old:
            if key in new:
                found.add(_path(path, key), "Literal constraint was introduced.")
        elif key not in new:
            found.add(_path(path, key), "Baseline literal declaration was removed.")
        elif key == "const" and not _equal(old[key], new[key]):
            found.add(_path(path, key), "Required literal value changed.")
        elif key == "enum":
            if not isinstance(old[key], list) or not old[key] or not isinstance(new[key], list) or not new[key]:
                found.add(_path(path, key), "Invalid enum declaration.")
            elif any(not any(_equal(value, n) for n in new[key]) for value in old[key]):
                found.add(_path(path, key), "Candidate removed at least one baseline enum value.")

    for key, lower in BOUNDS.items():
        if any(key in value and (not isinstance(value[key], int) or isinstance(value[key], bool) or value[key] < 0) for value in (old, new)):
            found.add(_path(path, key), "Invalid nonnegative integral bound.")
        elif key in new and (key not in old or (new[key] > old[key] if lower else new[key] < old[key])):
            found.add(_path(path, key), "Candidate tightened or introduced an acceptance bound.")
    numeric_valid = True
    for key in NUMERIC:
        if any(key in value and not _number(value[key]) for value in (old, new)):
            found.add(_path(path, key), "Invalid finite numeric bound.")
            numeric_valid = False
    if numeric_valid:
        for lower in (True, False):
            before, after = _effective_bound(old, lower), _effective_bound(new, lower)
            if after is not None and (before is None or (after[0] > before[0] if lower else after[0] < before[0]) or (after[0] == before[0] and after[1] and not before[1])):
                found.add(path, "Candidate tightened or introduced a numeric " + ("lower" if lower else "upper") + " bound.")

    old_required, new_required = old.get("required", []), new.get("required", [])
    if any(not isinstance(v, list) or any(not isinstance(k, str) for k in v) for v in (old_required, new_required)):
        found.add(_path(path, "required"), "Invalid required-field declaration.")
        old_required, new_required = [], []
    elif set(new_required) - set(old_required):
        found.add(_path(path, "required"), "Candidate requires fields baseline callers could omit.")
    old_props, new_props = old.get("properties", {}), new.get("properties", {})
    if not isinstance(old_props, dict) or not isinstance(new_props, dict):
        found.add(_path(path, "properties"), "Invalid object properties declaration.")
    else:
        for name, schema in old_props.items():
            prop_path = _path(_path(path, "properties"), name)
            if name not in new_props:
                found.add(prop_path, "Baseline property was removed.")
            else:
                _compare(schema, new_props[name], prop_path, found, depth + 1)
        for name in new_props.keys() - old_props.keys():
            if old.get("additionalProperties", True) is not False and _clean(new_props[name]) not in ({}, True):
                found.add(_path(_path(path, "properties"), name), "New property constrains a name previously accepted as an arbitrary additional property.")

    old_extra, new_extra = old.get("additionalProperties", True), new.get("additionalProperties", True)
    if old_extra is not False and new_extra is False:
        found.add(_path(path, "additionalProperties"), "Previously accepted additional properties are now rejected.")
    elif old_extra is not False and new_extra is not True:
        _compare(old_extra, new_extra, _path(path, "additionalProperties"), found, depth + 1)
    if "items" in old and "items" in new:
        _compare(old["items"], new["items"], _path(path, "items"), found, depth + 1)
    elif "items" in old:
        found.add(_path(path, "items"), "Baseline item schema was removed.")
    elif "items" in new and _clean(new["items"]) not in ({}, True):
        found.add(_path(path, "items"), "Previously unconstrained array items are now restricted.")

    if "oneOf" in old or "oneOf" in new:
        before, after = old.get("oneOf"), new.get("oneOf")
        union_path = _path(path, "oneOf")
        if not isinstance(before, list) or not before or not isinstance(after, list) or not after:
            found.add(union_path, "Exclusive union was added, removed, or malformed.")
            return
        if _equal([_clean(b) for b in before], [_clean(b) for b in after]):
            return
        if _closed_object_extension(before, after):
            return
        old_tags, new_tags = [_discriminators(b) for b in before], [_discriminators(b) for b in after]
        if any(t is None for t in old_tags + new_tags):
            found.add(union_path, "Changed exclusive union lacks required stable object const discriminators.")
            return
        if any(not _disjoint(a, b) for i, a in enumerate(new_tags) for b in new_tags[i + 1:]):
            found.add(union_path, "Candidate exclusive branches are not provably disjoint by stable const discriminators.")
            return
        for index, (branch, tags) in enumerate(zip(before, old_tags)):
            matches = [b for b, t in zip(after, new_tags) if _equal(tags, t)]
            if len(matches) != 1:
                found.add(union_path + f"[{index}]", "Baseline discriminator branch was removed or changed.")
            else:
                _compare(branch, matches[0], union_path + f"[{index}]", found, depth + 1)


def schema_compare(old: Any, new: Any, path: str = "$") -> list[dict[str, str]]:
    found = _Findings()
    _compare(old, new, path, found)
    return found.values


def compare_discovery(baseline: dict, candidates: list[dict] | dict) -> list[dict[str, str]]:
    if not isinstance(baseline, dict) or baseline.get("format") != FORMAT:
        raise ValueError("Expected portable authoring API baseline format " + FORMAT)
    selected = {key: baseline.get(key, {}) for key in ("methods", "components")}
    if any(not isinstance(v, dict) for v in selected.values()) or not any(selected.values()):
        raise ValueError("Baseline needs selected method/component schema maps.")
    merged: dict[str, dict] = {"methods": {}, "components": {}}
    for document in [candidates] if isinstance(candidates, dict) else candidates:
        if not isinstance(document, dict):
            raise ValueError("Candidate discovery must be an object.")
        if "result" in document:
            document = document["result"]
        if not isinstance(document, dict) or not any(k in document for k in merged):
            raise ValueError("Candidate lacks method/component discovery maps.")
        for category in merged:
            values = document.get(category, {})
            if not isinstance(values, dict):
                raise ValueError("Candidate schema category must be an object.")
            for name, schema in values.items():
                if name in merged[category] and not _equal(_clean(merged[category][name]), _clean(schema)):
                    raise ValueError("Conflicting candidate schema for " + category + "." + name)
                merged[category][name] = schema
    found = _Findings()
    for category, entries in selected.items():
        for name, schema in entries.items():
            path = _path(_path("$", category), name)
            if name not in merged[category]:
                found.add(path, "Selected baseline operation/component was removed or omitted.")
            else:
                _compare(schema, merged[category][name], path, found)
    return found.values


def _load(path: Path) -> Any:
    with path.open("rb") as source:
        data = source.read(MAX_INPUT_BYTES + 1)
    if len(data) > MAX_INPUT_BYTES:
        raise ValueError("Input exceeds 8 MiB: " + str(path))
    def invalid(value: str) -> None:
        raise ValueError("Non-finite JSON number: " + value)
    def floating(value: str) -> float:
        number = float(value)
        if not math.isfinite(number):
            raise ValueError("Non-finite JSON number: " + value)
        return number
    def object_pairs(pairs: list[tuple[str, Any]]) -> dict:
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate JSON key: " + key)
            result[key] = value
        return result
    return json.loads(data.decode("utf-8"), parse_constant=invalid, parse_float=floating, object_pairs_hook=object_pairs)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True, action="append")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    try:
        findings = compare_discovery(_load(args.baseline), [_load(p) for p in args.candidate])
        report = {"passed": not findings, "findings": findings,
                  "scope": "Selected input schemas only; conservative recognized rules, no general JSON Schema subsumption or runtime behavior/ABI guarantee."}
        code = 0 if report["passed"] else 1
    except (OSError, ValueError, TypeError, RecursionError) as error:
        report = {"passed": False, "error": str(error)[:4096]}
        code = 2
    text = json.dumps(report, indent=2, ensure_ascii=True) + "\n"
    print(text, end="")
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(text, encoding="utf-8")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
