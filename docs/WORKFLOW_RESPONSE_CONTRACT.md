# Prospective runtime and save result checks

<!-- SPDX-License-Identifier: Apache-2.0 -->

The [candidate manifest](contracts/runtime-save-responses-v1.candidate.json)
describes successful results for the 21 methods selected by the
[workflow request contract](WORKFLOW_API_COMPATIBILITY.md). The Python validator
is **opt-in**. It checks the original result against consumer requirements and
the initiating request; it neither sends calls nor changes their results.
Existing authoring-core v1 remains unchanged.

This is a prospective contract, not a broader stable runtime API. Result
validation does not establish that a method is available, an operation completed
correctly, a rejection preserved state, or a native process closed normally.
Those need execution evidence. JSON-RPC envelope validation remains a transport
responsibility; native error codes depend on the method and failing condition.

## Use

```python
from poima_client.workflow_responses import validate_workflow_result

params = {"session_id": session_id, "tick": tick,
          "type": component_type_id, "limit": 16}
result = client.call("runtime.component.query", params)
validate_workflow_result("runtime.component.query", result, params)
```

`validate_workflow_result(method, result, params=None, context=None)` returns
the **same object** on success. It raises `ResponseContractError` on a detected
consumer-contract violation. Unknown methods are rejected by this selected
validator; use another contract for them. The normal `WorldClient.call` route
does not automatically apply these prospective checks.

A contract error means a terminal successful reply had an unexpected shape or
relationship. It does not mean the native mutation failed. Inspect the actual
state and recover a retained receipt deliberately. The validator never retries,
reconnects, repairs a result, or assumes that repeating a mutation is safe.

`workflow_response_contract()` returns a detached copy of the manifest. The
Python module carries its own contract data so an installed client does not
depend on a repository checkout. Tests compare that data with the JSON manifest.
Validation bounds nesting, node count and encoded bytes, and preserves JSON
integer/float/Boolean distinctions and lossless decimal-text integers.

## Request-dependent results

| Family | Consumer requirements |
| --- | --- |
| Runtime lifecycle | Inactive status uses null boundary fields; active status uses typed boundaries. Start/stop retries describe their retained outcomes. |
| Runtime entity/component reads | IDs, selected type, applicable supplied guards, sorted exclusive query pages, and nullable absent components remain distinct. Structure read guards are optional. |
| Joined observation | Exact selected state/component keys, sorted union membership, query cursor and schema-inclusion policy are checked. Explicit duplicate input IDs are invalid. |
| Gameplay inspection | Compact, full-schema, selected-field and absent-module results differ. An empty field selector means an empty values object. |
| Step and UI control | Initiating and current runtime boundaries remain separate, including save-driven replacement. Historical receipts are not compared with a later runtime's current status. |
| Save tickets | Kind/state enums, nonzero ticket identities, completion fields and restore metadata are checked. An unknown valid ticket can be expired after runtime stop. |
| External saves | Configuration, slot identity, nullable generation records, durable retries and load/replacement metadata are checked. Optional recovery and upgrade branches require their own execution coverage. |

Additional diagnostic fields may be present where the manifest allows them.
They do not compensate for missing required fields. Projected field sets and
compact gameplay modules are deliberately exact so an unexpected large schema
or unrelated value cannot silently enter a compact observation.

## Trusted schema context

Optional context accepts:

- `profile`: `source_world`, `source_shared_player`, or `readonly_game`.
- `component_schemas`: complete native schemas as an array or an ID-keyed map.
- `gameplay_schema`: a complete native gameplay schema.

Pass schemas obtained from a trusted, boundary-consistent native inspection.
With context, the validator checks dynamic component and compact gameplay
values against stable fields and their wire types. It does not hardcode sample
game names or recompute native fingerprints. Without an available schema,
compact values receive the documented wire-shape and selector checks; this is
weaker than typed validation and must not be described as equivalent.

Full gameplay schemas preserve their original component declarations. Those
declarations may omit unit/fingerprint metadata or use a different unique field
order. Canonical `runtime.components`, component-get and observation schemas
include that metadata and sorted fields. The checker preserves this distinction;
it does not rewrite a declaration into an invented native schema.

The profile labels the discovery scope. It does not authorize an operation or
infer an active runtime. Source authoring starts gameplay explicitly; a game
service starts its declared runtime/module automatically. Feature, trusted
image, playback, session and revision guards still apply.

## Qualification boundaries

The focused native capture is reproducible with
`tests/workflow_response_native.py` and the trusted Switchback source project
and exported game described in the [fixture instructions](evidence/fixtures/agent-switchback-fresh/README.md).
It uses the existing retained transport and requires a supervising Windows Job
such as `tests/switchback_ownership.py::gated`. It retains complete request and
response streams, tests selected error conditions, and preserves the original
source and immutable exported bundle.

The authored fixture covers a nonempty registered component schema in standalone
and shared source scopes. The compiled readonly fixture can have an empty
registry; it does not qualify nonempty readonly component workflows. Queued,
resolving and failed tickets, recovered/upgraded saves, physical input, renderer
output, production scale, native ABI compatibility and general upgrade behavior
remain separate gates. Prior successful game traces add retained-consumer
coverage; they do not fill unobserved branches by inference.

[Recorded qualification and remaining gaps](evidence/m2-workflow-response-contract.json)
join the native captures, portable tests and independent retained-result checks.
