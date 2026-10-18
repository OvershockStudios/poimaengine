# Authored source and running state

`source_stale` reports whether the active runtime's frozen authored baseline
differs from the current in-memory authored world. It appears in
`runtime.inspect`, `runtime.observe`, `runtime.material.inspect` and
`runtime.texture.inspect`. It does not report whether gameplay has moved an
entity away from its authored position.

## Meaning

The existing comparison remains authoritative:

1. Different authored revisions mean stale source.
2. With equal revisions, compare authored entities, asset provenance,
   navigation, component schemas, templates and UI using their existing JSON
   comparison rules. Matching revision numbers alone are insufficient.

The comparison treats absent and empty component-schema, template and UI
catalogs as equivalent. Navigation and provenance retain their existing
presence-sensitive comparisons. Request receipts and retired-identity ledgers
are not part of this authored-content comparison.

An authored edit can make the source stale while gameplay continues against its
previous baseline. Undo can restore the same content while advancing the
authored revision; the result remains stale. Restarting freezes current authoring.
Supported [material](LIVE_MATERIALS.md) and [texture](LIVE_TEXTURES.md) adoption
can update the active baseline without restarting the game.

Loading a save restores its frozen definition rather than replacing current
authoring. Its revision can equal current authoring while its content differs.
The load reply compares the prepared definition directly; subsequent inspection
must agree after activation. See [save replacement](RUNTIME.md#durable-save-slots).

## Repeated observation

From 0.0.188, the private world owner caches this exact Boolean until either
committed document changes. Invalidation follows authored persistence, runtime
start, successful save activation and successful live material/texture adoption.
Actual runtime stop also clears the cache. Initialization begins without a
cached result. Invalidation adds no allocation or throwing work to publication.

Only the source comparison is reused. Ticks, counts, gameplay/UI/material/
structure revisions and control sequence are still read from current state.
Simulation, component edits and gameplay reload do not themselves change the
two authored baseline documents. The cache is absent from schemas, receipts,
content hashes, saves and the gameplay ABI. Existing serialized world ownership
still applies; this change does not make a world concurrently thread-safe.

These observations compare committed in-memory state. They do not silently
reload an externally edited world file or detect arbitrary cooked-file changes.
The authored write guard still rejects external file changes; reopen the world
before editing that new source. A new owner starts with a new comparison cache.

## Checks

`world_source_stale_native` exercises actual native sessions, all four read
surfaces, atomic rejection, history and same-revision saved definitions. It
retains complete before/after save operands and compares every byte. Run the
executable with `--output FRESH_DIRECTORY` to keep its fixture and observations;
the parent directory must already exist. Its default CTest mode removes only
its successful temporary fixture; failures retain their fixture.

Simulation must be enabled. A simulation-disabled build explicitly skips this
runtime check; a zero exit from that branch is not runtime qualification.
Existing material and texture tests separately exercise adoption and its guards.
The [batching investigation](RUNTIME_BATCHING.md) explains why comparisons must
keep the same stepping and observation schedule when measuring this change.

## Recorded workload

The Windows 0.0.188 fixture passes 1,939 checks and nine complete saved-byte
pairs. Its retained observations report 1,937 checks before the final two
fixture-publication checks; the terminal output includes both. Twenty original
native save records retain the baseline operands. The tiny fixture uses the
original nine-member v1 runtime snapshot; it is separate from Performance Yard.

The matched Performance Yard run compares sealed native 0.0.186 and 0.0.188
executables with the same qualified 0.0.185 bridge, original compiled gameplay,
2,394 authored entities, 2,397 live entities, 24 animated characters and 64 lights.
Fifty globals, four component schemas and every one of fourteen v7 snapshot
payload members remain included.

Six fresh owners run serially in old/new, new/old, old/new order. Every owner
warms up for 120 ticks, then records three 120-tick windows. Every step request
advances one tick and follows the same guard inspection. Each engine contributes
1,080 measured ticks and 1,080 invocations of each listed native region. The
external helper row combines nine 120-tick loop brackets per engine. Native scopes measure inclusive
elapsed wall time, including scheduling and waits; nested rows overlap.

| Observation | Native 0.0.186 | Native 0.0.188 |
| --- | ---: | ---: |
| `runtime.inspect` mean, ms/call | 5.329 | 2.188 |
| `runtime.inspect` p50, ms/call | 3.982 | 1.450 |
| `runtime.inspect` p95, ms/call | 10.388 | 4.953 |
| `runtime.step` mean, ms/call | 4.260 | 4.360 |
| `runtime.batch` mean, ms/tick | 2.457 | 2.363 |
| External helper loop, ms/tick | 19.315 | 16.058 |

Inspection and helper-loop means improve in all three pairs. Step mean rises
slightly in the pooled measurement. The helper row includes Python, guard and
step transport, validation and retained writes. These are warm observations
after initial source comparison, not cache-miss costs or rendered frame times.
The laptop is on AC power with 100% battery at both recorded brackets; both
engines use two configured workers. Jolt uses its single-threaded job system.
Thermals, caches, scheduling and ordinary GC are not isolated.

The 75.302-second worker retains 6,048 original RPC pairs, eighteen captures
containing 159,840 events, twenty-four complete v7 checkpoint references and
twelve prospective complete-state forecasts. Source-derived forecasts replace
only the two observed host-epoch halves in a separate expected operand before
the counterpart's first tick. Every original actual value and byte remains
retained. Original cross-owner payload equality is false; complete typed and
lexical forecast agreement is true. This is no general determinism proof.

All six native owners and their seven-process Job close normally without
forced termination. Independent readers verify source/artifact identities,
original RPC joins, full snapshots and profiler statistics. Exact reproduction
requires the retained world/assets, original compiled game closure, selected
bridge/SDK closure, hostfxr and original traces; this is not a clean-checkout
benchmark. The original game SDK and selected bridge SDK have distinct pinned
hashes and separately qualified actual native-host consumption.

The [selected evidence](evidence/m2-runtime-inspection.json) separates native
tests, platform skips, full-state forecasts and timing scopes. The result does
not establish rendered FPS, input latency, memory-growth causes, thirty-minute
reliability or Alpha completion. The subsequent 0.0.189
[authored asset-reference cache](ASSETS.md#authoring-cache-and-fresh-verification)
addresses repeated reference scans before ordinary dispatch. Its separate
comparison retains the same command cadence; these earlier timings remain the
original 0.0.186/0.0.188 cohort.
