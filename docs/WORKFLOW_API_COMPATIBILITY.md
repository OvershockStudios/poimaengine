# Prospective workflow request compatibility

<!-- SPDX-License-Identifier: Apache-2.0 -->

The [runtime/save selection](contracts/runtime-save-workflow-v1.candidate.json)
tracks 21 selected request schemas beyond [authoring-core v1](AUTHORING_API_COMPATIBILITY.md).
It is a **prospective candidate**, with no stability promise. This tool checks
request compatibility and selected discovery context; results, errors, native
behavior, C# source compatibility and binary ABI require separate gates.
The [gameplay/content/save profile](ALPHA_GAMEPLAY_PROFILE.md) defines those
boundaries. Existing authoring-core v1 remains unchanged.

## Selected surface

The candidate covers runtime start/status/inspect/stop/step; entity and joined
component observation; native gameplay load/inspection; logical UI inspection
and activation; gameplay save-ticket status/result; and external save
configuration/status/inspection/write/load. The manifest lists every selected
method. No component schemas are separately selected, although selected method
and context nodes retain their complete nested schemas.

Three profile IDs describe the discovery owner:

| Profile | Expected context |
| --- | --- |
| `source_world` | Standalone authoring: `mode:authoring`, `read_only:false`, absent `session_scope`. |
| `source_shared_player` | Shared authoring: `mode:authoring`, `read_only:false`, `session_scope:shared_headless`. The profile name does not qualify player methods. |
| `readonly_game` | Shared exported game: `mode:read_only_runtime`, `read_only:true`, `session_scope:shared_headless`. |

All 21 selected methods must be advertised for a projection. That does not mean
every operation is usable in every state: an exported game starts its declared
Runtime and gameplay automatically, whereas source authoring starts them
explicitly. Feature availability, active session, trusted image, guards and
playback restrictions still apply. Method discovery is not execution evidence.

## Project and compare

Use a **full** native `world.describe` snapshot from the appropriate scope,
either its result object or JSON-RPC envelope. Focused/partial snapshots are
rejected. Keep the original snapshot and trusted baseline with their identities.
The tool uses the existing conservative request checker; it executes no engine.

```sh
python scripts/project_workflow_api.py --profile source_world \
  --discovery discovery-before.json --output workflow-before.json
python scripts/project_workflow_api.py --profile source_world \
  --discovery discovery-after.json --baseline workflow-before.json \
  --output workflow-comparison.json
```

The default manifest is the candidate linked above and the default checker is
`scripts/check_authoring_api.py`. `--manifest` and `--checker` can select explicit
trusted local files. Exit 0 means projection succeeded or no comparison findings
were produced; exit 1 means the comparison found changed or unproven scope;
invalid input/usage exits 2. An empty finding list remains request-only evidence.

Projection deep-copies every selected method node, including closed branches,
constraints and defaults. It does not rewrite schemas to force compatibility.
Selected context retains explicit presence records: an omitted value differs
from null, including standalone `session_scope`. Profile membership and policy
must match exactly. Full limits and non-schema metadata are compared exactly;
changed capacities or wording can therefore require review even when an engine
change is otherwise useful. This broad context is candidate drift detection,
not a released promise to freeze every capacity or diagnostic string.

Only two reviewed context paths are compared as schemas:
`spawn_templates.components` and `spawn_templates.hierarchy`. Their complete
nodes survive; sibling metadata remains exact. Optional request-schema additions
can pass while changed defaults, tighter constraints or unknown changes produce
findings. Unknown, overlapping or missing semantic paths reject rather than
treating arbitrary metadata as JSON Schema.

Adding a previously undeclared optional field can preserve request acceptance
even when its default changes omitted-input behavior. This checker cannot prove
that behavior; it needs a separate native test. Changes to an already declared
default produce findings.

References, resource identifiers, dialects, anchors and vocabularies are not
resolved. The projection retains the complete source when these dependencies
could affect the selected scope, and comparison stays unproven—even if the two
inputs are identical. Removing dependency context from a referenced projection
is rejected. A projection is not a signature or independent proof of its source;
retain the original capture and trusted baseline provenance.

## Portable tests and optional capture integration

```sh
python tests/workflow_api_projection.py
```

Portable fixtures are small explicit consumer schemas for guards, nullability,
command bounds, defaults and schema/context policy. They are not native catalog
copies or execution evidence. Tests cover required-field/type/default changes,
open requests and ambiguous unions, exact metadata siblings, policy/membership,
references/dialects, strict JSON and candidate relabelling. They need no renderer,
compiled gameplay, build output, downloaded content or engine process.

Actual capture integration is opt-in. Supply directories containing
`standalone-authoring.json`, `shared-authoring.json` and
`shared-readonly-game.json`; both directories must represent the declared scope.

```sh
python tests/workflow_api_projection.py \
  --discovery-dir path/to/current-captures \
  --previous-discovery-dir path/to/previous-captures \
  --evidence path/to/projection-tests.json
```

This reads captures only. It validates complete selected nodes and contexts,
compares old accepted requests with the new candidate and records capture
identities. It does not launch the hosts or establish that their operations ran.
The broader Alpha gates still require independent response contracts, native
negative/state-retention tests and retained-game continuation across supported
builds, alongside performance, physical input, listening and installation.

## Recorded qualification

The [0.0.142 evidence](evidence/m2-workflow-request-compatibility.json) records
36 portable checks on each of Linux and Windows, including three actual
Windows-native discovery scopes compared from 0.0.134 to 0.0.142. The new
readonly scope uses a genuine matching runtime installation and export; the
failed attempt to use an older export remains failed. These comparisons read
request schemas and metadata, rather than executing every selected operation.

Separate Windows native checks qualify integer retry-guard rejection with
retained Native AOT gameplay and current-SDK CoreCLR components. The latter
passes 463 RPCs, rollback/save continuation and 100 collectible reloads. Those
bounded consumer checks do not release a response/behavior contract for all
21 methods. The released authoring-core v1 and remaining Alpha gates retain
their existing boundaries.
